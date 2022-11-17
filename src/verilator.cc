#include <iostream>

#include <verilated.h>
#include "Vcomposer.h"
#include <cinttypes>
#include <queue>
#include <pthread.h>
#include "../include/data_server.h"
#include "../include/cmd_server.h"

#include <composer_allocator_declaration.h>
#include "verilated_vcd_c.h"
#include "../include/verilator.h"
#include "../include/ddr_macros.h"

#ifdef USE_DRAMSIM
// dramsim3
#include "dram_system.h"

using namespace dramsim3;
#endif

// in bytes
#define DATA_BUS_WIDTH 64

Vcomposer *top;
uint64_t main_time = 0;

data_server *d_server;
cmd_server *c_server;

pthread_mutex_t main_lock = PTHREAD_MUTEX_INITIALIZER;
extern data_server *d_server;
bool kill_sig = false;

#ifdef USE_DRAMSIM
std::vector<dramsim3::JedecDRAMSystem *> mem_sys;
Config dramsim3config("../DRAMsim3/configs/DDR4_8Gb_x16_2666.ini", "./");
std::map<uint64_t, std::queue<memory_transaction *> *> in_flight_reads;
std::map<uint64_t, std::queue<memory_transaction *> *> in_flight_writes;
#endif

void enqueue_transaction(v_address_channel<QData> &chan, std::queue<memory_transaction *> &lst) {
  if (*chan.valid && *chan.ready) {
    char *addr = (char *) d_server->at.translate(*chan.addr);
    auto tx = new memory_transaction(addr, 1 << *chan.size,
                                     *chan.len + 1, 0, *chan.burst == 0, *chan.id, *chan.addr);
    lst.push(tx);
  }
}

enum cmd_transfer_state {
  CMD_INACTIVE,
  CMD_BITS_WRITE_ADDR,
  CMD_BITS_WRITE_DAT,
  CMD_BITS_WRITE_B,
  CMD_VALID_ADDR,
  CMD_VALID_DAT,
  CMD_VALID_WRITE_B,
  CMD_RECHECK_READY_ADDR,
  CMD_RECHECK_READY_DAT,

};

enum resp_transfer_state {
  RESPT_INACTIVE,
  RESPT_BITS_ADDR,
  RESPT_BITS_READ,
  RESPT_READY_ADDR,
  RESPT_READY_WRITE,
  RESPT_READY_WRITE_B,
  RESPT_RECHECK_VALID_ADDR,
  RESPT_RECHECK_VALID_READ,
};

enum update_state {
  UPDATE_IDLE_RESP,
  UPDATE_IDLE_CMD,
  UPDATE_CMD_ADDR,
  UPDATE_CMD_WAIT,
  UPDATE_RESP_ADDR,
  UPDATE_RESP_WAIT
};

struct command_transaction {
  uint32_t *cmdbuf = nullptr;
  int8_t progress = 0;
  cmd_transfer_state state = CMD_INACTIVE;
  bool ready_for_command = false;

  static const int payload_length = 5;
};

struct response_transaction {
  static const int payload_length = 3;
  uint32_t resbuf[payload_length]{};
  uint8_t progress = 0;

  resp_transfer_state state = RESPT_INACTIVE;
};

void run_verilator(int argc, char **argv) {
  // start servers to communicate with user programs
  Verilated::commandArgs(argc, argv);
  top = new Vcomposer;

  auto c = composer::rocc_cmd::start_cmd(ALUSystem_ID, 0, 0, true, composer::R0, 0, 0,0, 32, 64);
  auto p = c.pack(pack_cfg);
  for (int i = 0; i < 5; ++i) {
    printf("%x\n", p[i]);
  }

  Verilated::traceEverOn(true);
  auto tfp = new VerilatedVcdC;
  top->trace(tfp, 100);
  tfp->open("trace.vcd");

  mem_interface<QData> axi4_mems[NUM_DDR_CHANNELS];
#ifdef USE_DRAMSIM
  for (auto &axi4_mem: axi4_mems) {
    auto q = new JedecDRAMSystem(
            dramsim3config,
            "",
            [&axi4_mem](uint64_t addr) {
              auto tx = in_flight_reads[addr]->front();
              tx->progress--;
              if (tx->progress == 0) {
                axi4_mem.read_transactions.push(tx);
              }
              in_flight_reads[addr]->pop();
              printf("read transaction finish %x\n", tx->id);
            },
            [&axi4_mem](uint64_t addr) {
              auto tx = in_flight_writes[addr]->front();
              tx->progress--;
              if (tx->progress == 0) {
                axi4_mem.b->to_enqueue = tx->id;
                delete tx;
              }
              in_flight_writes[addr]->pop();
//              axi4_mem.write_transactions.push(new memory_transaction(ad, size, len, 0, false, id, addr));
              printf("write transaction finish\n");
            });
    mem_sys.push_back(q);
  }
#endif
  printf("There are %d DDR Channels\n", NUM_DDR_CHANNELS);
#if NUM_DDR_CHANNELS >= 1
  init_ddr_interface(0)
#if NUM_DDR_CHANNELS >= 2
  init_ddr_interface(1)
#if NUM_DDR_CHANNELS >= 3
  init_ddr_interface(2)
#if NUM_DDR_CHANNELS >= 4
  init_ddr_interface(3)
#endif
#endif
#endif
#endif
  // reset circuit
  top->reset = 1;

  for (auto &mem: axi4_mems) {
#ifndef USE_DRAMSIM
    *mem.ar->ready = 1;
    *mem.w->ready = 0;
#endif
    *mem.r->valid = 0;
    *mem.b->valid = 0;
    *mem.aw->ready = 1;
  }

  top->clock = 1;

  for (int i = 0; i < 10; ++i) {
    top->clock = top->clock ^ 1;
    top->eval();
    top->clock = top->clock ^ 1;
    top->eval();
  }
  top->reset = 0;

  command_transaction ongoing_cmd;
  response_transaction ongoing_rsp;
  update_state ongoing_update = UPDATE_IDLE_CMD;
  int cmds_in_flight = 0;
  bool bus_occupied = false;
  int check_freq = 50;
  printf("main time %lld\n", main_time);
  while (not kill_sig) {
    // clock is high after posedge - changes now are taking place after posedge,
    // and will take effect on negedge
    main_time++;
    if (main_time % 10000 <= 1) {
      printf("main time: %lld\n", main_time);
    }

    // ------------ HANDLE COMMAND INTERFACE ----------------

    tfp->dump(main_time);
    top->clock = top->clock ^ 1; // negedge
    top->eval();
    main_time++;

    // ------------ HANDLE MEMORY INTERFACES ----------------
    for (int inter_idx = 0; inter_idx < NUM_DDR_CHANNELS; ++inter_idx) {
      mem_interface<QData> &inter = axi4_mems[inter_idx];
      if (not inter.read_transactions.empty() && not *inter.r->valid) {
        auto tx = inter.read_transactions.front();
        int start = (tx->progress * tx->size) % DATA_BUS_WIDTH;
        char *dest = (char *) inter.r->data->m_storage + start;
        fflush(stdout);
        memcpy(dest, tx->addr, tx->size);
        fflush(stdout);
        bool am_done = tx->len == (tx->progress + 1);
        *inter.r->valid = 1;
        *inter.r->last = am_done;
        *inter.r->id = tx->id;
        printf("performing read %x, %d/%d\n", tx->id, tx->progress, tx->len);
        fflush(stdout);
        if (*inter.r->ready) {
          tx->progress++;
          if (not tx->fixed) {
            tx->addr += tx->size;
          }
          if (am_done) {
            inter.read_transactions.pop();
            delete tx;
          }
        }
      } else {
        *inter.r->valid = 0;
      }

      // update all channels with new information now that we've updated states
      if (not inter.b->send_ids.empty()) {
        *inter.b->valid = 1;
        *inter.b->id = inter.b->send_ids.front();
        if (*inter.b->ready) {
          inter.b->send_ids.pop();
        }
      } else {
        *inter.b->valid = 0;
      }

      if (inter.b->to_enqueue != -1) {
        inter.b->send_ids.push(inter.b->to_enqueue);
        inter.b->to_enqueue = -1;
      }

      // slave response to valid transaction and write valid
      // write can respond before write address so just ahve to wait for address
      // to be recieved. ready and valid have to be high at the same time for this
      // to work
      if (not inter.write_transactions.empty() and not*inter.w->ready) {
#ifdef USE_DRAMSIM
        *inter.w->ready = inter.to_enqueue_write == nullptr;
#else
        *inter.w->ready = 1;
#endif
        if (*inter.w->valid && *inter.w->ready) {
          auto trans = inter.write_transactions.front();
          printf("Handling write transaction: addr %16llx, id: %d\n", (uint64_t) trans->addr, trans->id);
          // refer to https://developer.arm.com/documentation/ihi0022/e/AMBA-AXI3-and-AXI4-Protocol-Specification/Single-Interface-Requirements/Transaction-structure/Data-read-and-write-structure?lang=en#CIHIJFAF
          char *src = (char *) inter.w->data->m_storage;
          uint64_t strobe = *inter.w->strobe;
          uint32_t off = 0;
          // for writes, we need to account for alignment and strobe,so we're re-aligning address here
          // align to 64B - zero out bottom 6b
          auto addr = (char *) ((uint64_t) trans->addr & 0xFFFFFFFFFFFFFFC0);
          while (strobe != 0) {
            if (strobe & 1) {
              addr[off] = src[off];
            }
            off += 1;
            strobe >>= 1;
          }
          trans->progress++;

          if (*inter.w->last) {
            printf("Enqueuing %d to be succeeded \n", trans->id);
            inter.write_transactions.pop();
#ifdef USE_DRAMSIM
            trans->dram_tx_progress = 0;
            trans->dram_tx_len = trans->len * trans->size >> 3;
            trans->progress = trans->dram_tx_len;
            inter.to_enqueue_write = trans;
#else
            inter.b->to_enqueue = trans->id;
            delete trans;
#endif
          }
          if (not trans->fixed) {
            trans->addr += trans->size;
          }
        }
      } else {
        *inter.w->ready = 0;
      }
      enqueue_transaction(*inter.aw, inter.write_transactions);

#ifdef USE_DRAMSIM
      if (*axi4_mems[inter_idx].ar->ready && *axi4_mems[inter_idx].ar->valid) {
        uint64_t addr = *axi4_mems[inter_idx].ar->addr;
        char *ad = (char *) d_server->at.translate(addr);
        auto txsize = (int) pow(2, *axi4_mems[inter_idx].ar->size);
        auto txlen = *axi4_mems[inter_idx].ar->len + 1;
        auto dram_txlen = txsize * txlen >> 3;
        if (dram_txlen == 0) dram_txlen = 1;
        auto tx = new memory_transaction(ad, txsize, txlen, dram_txlen, false,
                                         *axi4_mems[inter_idx].ar->id, addr);
        // 64b per DRAM transaction
        tx->dram_tx_len = dram_txlen;
        inter.to_enqueue_read = tx;
        tx->dram_tx_progress = 0;
        printf("enqueue ID %x\n", *axi4_mems[inter_idx].ar->id);
      }

      if (inter.to_enqueue_read != nullptr) {
        auto &r = *inter.to_enqueue_read;
        auto fpga_init = r.fpga_addr;
        auto fpga_addr = fpga_init + 8 * r.dram_tx_progress;
        if (mem_sys[inter_idx]->WillAcceptTransaction(fpga_addr, false)) {
          mem_sys[inter_idx]->AddTransaction(fpga_addr, false);
          if (in_flight_reads.find(fpga_addr) == in_flight_reads.end())
            in_flight_reads[fpga_addr] = new std::queue<memory_transaction*>();
          auto &q = *in_flight_reads[fpga_addr];
          q.push(&r);
          printf("enqueueing %x\n", fpga_addr);
          r.dram_tx_progress++;
        }
        if (r.dram_tx_len == r.dram_tx_progress) {
          inter.to_enqueue_read = nullptr;
        }
      }

      if (inter.to_enqueue_write != nullptr) {
        auto &w = *inter.to_enqueue_write;
        auto fpga_addr = w.fpga_addr + w.dram_tx_progress * 8;
        if (mem_sys[inter_idx]->WillAcceptTransaction(fpga_addr, true)) {
          w.dram_tx_progress++;
          mem_sys[inter_idx]->AddTransaction(fpga_addr, true);
          if (in_flight_writes.find(fpga_addr) == in_flight_writes.end())
            in_flight_writes[fpga_addr] = new std::queue<memory_transaction*>;
          in_flight_writes[fpga_addr]->push(&w);
        }
        if (w.dram_tx_progress == w.dram_tx_len) {
          inter.to_enqueue_write = nullptr;
        }
      }

      // to signify that the AXI4 DDR Controller is busy enqueueing another transaction in the DRAM
      *inter.ar->ready = inter.to_enqueue_read == nullptr;
#else
      enqueue_transaction(*inter.ar, inter.read_transactions);
#endif

    }

    // start queueing up a new command if one is available
    top->ocl_0_aw_valid = top->ocl_0_w_valid = top->ocl_0_r_ready = top->ocl_0_ar_valid = top->ocl_0_b_ready = 0;
    switch (ongoing_cmd.state) {
      // tell the composer that we're going to send 32-bits of a command over the PCIE bus
      case CMD_BITS_WRITE_ADDR:
        ongoing_cmd.ready_for_command = false;
        top->ocl_0_aw_valid = 1;
        top->ocl_0_aw_bits_len = 0;
        top->ocl_0_aw_bits_id = 0;
        top->ocl_0_aw_bits_addr = CMD_BITS;
        if (top->ocl_0_aw_ready) {
          ongoing_cmd.state = CMD_BITS_WRITE_DAT;
          printf("to write dat\n");
        }
        break;
        // send the command over the PCIE bus
      case CMD_BITS_WRITE_DAT:
        top->ocl_0_w_valid = 1;
        top->ocl_0_w_bits_data = ongoing_cmd.cmdbuf[ongoing_cmd.progress];
        if (top->ocl_0_w_ready) {
          ongoing_cmd.state = CMD_BITS_WRITE_B;
          printf("wrote %d, going to bits write response. BITS: %08x\n", ongoing_cmd.progress,
                 ongoing_cmd.cmdbuf[ongoing_cmd.progress]);
        }
        break;
      case CMD_BITS_WRITE_B:
        top->ocl_0_b_ready = 1;
        if (top->ocl_0_b_valid) {
          if (top->ocl_0_b_bits_resp == 0) {
            ongoing_cmd.state = CMD_VALID_ADDR;
            printf("to valid addr\n");
          } else {
            fprintf(stderr, "Recieved error from write response!");
            exit(1);
          }
        }
        // We just send the 32-bits, now "simulate" the decoupled interface by toggling to the CMD_VALID bit to 1
        // This bit is visible from the CMD_VALID bit so we need to perform an AXI transaction
      case CMD_VALID_ADDR:
        top->ocl_0_aw_valid = 1;
        top->ocl_0_aw_bits_addr = CMD_VALID;
        top->ocl_0_aw_bits_len = 0; // length is actually one - see AXI spec
        top->ocl_0_aw_bits_id = 0;
        if (top->ocl_0_aw_ready) {
          printf("to valid dat\n");
          ongoing_cmd.state = CMD_VALID_DAT;
        }
        break;
        // send the CMD_VALID bit
      case CMD_VALID_DAT:
        top->ocl_0_w_valid = 1;
        top->ocl_0_w_bits_data = 1;
        if (top->ocl_0_w_ready) {
          printf("to valid b\n");
          ongoing_cmd.state = CMD_VALID_WRITE_B;
        }
        break;
      case CMD_VALID_WRITE_B:
        top->ocl_0_b_ready = 1;
        if (top->ocl_0_b_valid) {
          if (top->ocl_0_b_bits_resp == 0) {
            ongoing_cmd.progress++;
            // send last thing, yield bus
            if (ongoing_cmd.progress == command_transaction::payload_length) {
              printf("Command finished. To CMD_INACTIVE\n");
              ongoing_cmd.state = CMD_INACTIVE;
              bus_occupied = false;
            } else {
              printf("Progress %d/%d. Continuing to CMD_RECHECK_READY_ADDR\n", ongoing_cmd.progress,
                     command_transaction::payload_length);
              // else, need to send the next 32-bit chunk and see that the channel is "ready"
              ongoing_cmd.state = CMD_RECHECK_READY_ADDR;
            }
          } else {
            fprintf(stderr, "Recieved error from write response!");
            exit(1);
          }
        }
        break;
        // We just send 32-bits over the interface, check if it's ready for another 32b by requesting ready from the
        // CMD_READY bit
      case CMD_RECHECK_READY_ADDR:
        top->ocl_0_ar_valid = 1;
        top->ocl_0_ar_bits_addr = CMD_READY;
        top->ocl_0_ar_bits_len = 0;
        top->ocl_0_ar_bits_id = 0;
        if (top->ocl_0_ar_ready) {
          ongoing_cmd.state = CMD_RECHECK_READY_DAT;
        }
        break;
        // read the CMD_READY bit
      case CMD_RECHECK_READY_DAT:
        top->ocl_0_r_ready = 1;
        if (top->ocl_0_r_valid) {
          // if it's ready for another command
          if (top->ocl_0_r_bits_data) {
            ongoing_cmd.state = CMD_BITS_WRITE_ADDR;
          } else {
            ongoing_cmd.state = CMD_RECHECK_READY_ADDR;
          }
        }
        break;
      case CMD_INACTIVE:
        if (ongoing_cmd.ready_for_command && !bus_occupied) {
          pthread_mutex_lock(&c_server->cmdserverlock);
          if (not c_server->cmds.empty()) {
            printf("\tGot command from cmd_server!\n");
            bus_occupied = true;
            ongoing_cmd.state = CMD_BITS_WRITE_ADDR;
            ongoing_cmd.cmdbuf = c_server->cmds.front().pack(pack_cfg);
            kill_sig = c_server->cmds.front().getOpcode() == ROCC_OP_FLUSH;
            std::cout << c_server->cmds.front() << std::endl << std::endl;
            ongoing_cmd.progress = 0;
            if (c_server->cmds.front().getXd() == 1)
              cmds_in_flight++;
            c_server->cmds.pop();
          }
          pthread_mutex_unlock(&c_server->cmdserverlock);
        }
        break;
    }
    switch (ongoing_rsp.state) {
      case RESPT_INACTIVE:
        break;
      case RESPT_BITS_ADDR:
        top->ocl_0_ar_bits_addr = RESP_BITS;
        top->ocl_0_ar_bits_len = 0;
        top->ocl_0_ar_valid = 1;
        top->ocl_0_ar_bits_id = 0;
        if (top->ocl_0_ar_ready) {
          printf("respt-bits-addr -> respt-bits-read\n");
          ongoing_rsp.state = RESPT_BITS_READ;
        }
      case RESPT_BITS_READ:
        top->ocl_0_r_ready = 1;
        if (top->ocl_0_r_valid) {
          ongoing_rsp.resbuf[ongoing_rsp.progress++] = top->ocl_0_r_bits_data;
          printf("respt-bits-read -> respt-ready-addr\n");
          ongoing_rsp.state = RESPT_READY_ADDR;
        }
        break;
      case RESPT_READY_ADDR:
        top->ocl_0_aw_valid = 1;
        top->ocl_0_aw_bits_len = 0;
        top->ocl_0_aw_bits_addr = RESP_READY;
        top->ocl_0_aw_bits_id = 0;
        if (top->ocl_0_aw_ready) {
          printf("respt-ready-addr -> respt-ready-write\n");
          ongoing_rsp.state = RESPT_READY_WRITE;
        }
        break;
      case RESPT_READY_WRITE:
        top->ocl_0_w_valid = 1;
        top->ocl_0_w_bits_data = 1;
        if (top->ocl_0_w_ready) {
          printf("respt-ready-write -> respt-ready-b\n");
          ongoing_rsp.state = RESPT_READY_WRITE_B;
        }
      case RESPT_READY_WRITE_B:
        top->ocl_0_b_ready = 1;
        if (top->ocl_0_b_valid) {
          if (top->ocl_0_b_bits_resp == 0) {
            if (ongoing_rsp.progress == response_transaction::payload_length) {
              c_server->register_reponse(ongoing_rsp.resbuf);
              cmds_in_flight--;
              bus_occupied = false;
              printf("respt-ready-b -> respt-inactive\n");
              ongoing_rsp.state = RESPT_INACTIVE;
            } else {
              printf("Not done yet %d/%d. respt-ready-b -> respt-recheck-valid-address\n", ongoing_rsp.progress,
                     response_transaction::payload_length);
              ongoing_rsp.state = RESPT_RECHECK_VALID_ADDR;
            }
          } else {
            fprintf(stderr, "Recieved error from write response!");
            exit(1);
          }
        }
        break;
      case RESPT_RECHECK_VALID_ADDR:
        top->ocl_0_ar_valid = 1;
        top->ocl_0_ar_bits_addr = RESP_VALID;
        top->ocl_0_ar_bits_len = 0;
        top->ocl_0_ar_bits_id = 0;
        if (top->ocl_0_ar_ready) {
          ongoing_rsp.state = RESPT_RECHECK_VALID_READ;
        }
        break;
      case RESPT_RECHECK_VALID_READ:
        top->ocl_0_r_ready = 1;
        if (top->ocl_0_r_valid) {
          if (top->ocl_0_r_bits_data) {
            ongoing_rsp.state = RESPT_BITS_ADDR;
          } else {
            ongoing_rsp.state = RESPT_RECHECK_VALID_ADDR;
          }
        }
        break;
    }
    switch (ongoing_update) {
      case UPDATE_IDLE_RESP:
        if (!bus_occupied && (main_time % check_freq == 0)) {
          bus_occupied = true;
          ongoing_update = UPDATE_RESP_ADDR;
        }
        break;
      case UPDATE_RESP_ADDR:
        top->ocl_0_ar_valid = 1;
        top->ocl_0_ar_bits_len = 0;
        top->ocl_0_ar_bits_id = 0;
        top->ocl_0_ar_bits_addr = RESP_VALID;
        if (top->ocl_0_ar_ready) {
          ongoing_update = UPDATE_RESP_WAIT;
        }
        break;
      case UPDATE_RESP_WAIT:
        top->ocl_0_r_ready = 1;
        if (top->ocl_0_r_valid) {
          ongoing_rsp.progress = 0;
          if (top->ocl_0_r_bits_data == 1) {
            ongoing_rsp.state = RESPT_BITS_ADDR;
          } else {
            bus_occupied = false;
          }
          ongoing_update = UPDATE_IDLE_CMD;
        }
        break;
      case UPDATE_IDLE_CMD:
        if (!bus_occupied && (main_time % check_freq == 0)) {
          bus_occupied = true;
          ongoing_update = UPDATE_CMD_ADDR;
        }
        break;
      case UPDATE_CMD_ADDR:
        top->ocl_0_ar_valid = 1;
        top->ocl_0_ar_bits_id = 0;
        top->ocl_0_ar_bits_addr = CMD_READY;
        top->ocl_0_ar_bits_len = 0;
        if (top->ocl_0_ar_ready) {
          ongoing_update = UPDATE_CMD_WAIT;
        }
        break;
      case UPDATE_CMD_WAIT:
        top->ocl_0_r_ready = 1;
        if (top->ocl_0_r_valid) {
          ongoing_cmd.ready_for_command = top->ocl_0_r_bits_data;
          if (cmds_in_flight > 0) {
            ongoing_update = UPDATE_IDLE_RESP;
          } else {
            ongoing_update = UPDATE_IDLE_CMD;
          }
        }
        bus_occupied = false;
        break;
    }

    tfp->dump(main_time);
#ifdef USE_DRAMSIM
    for (auto &sys: mem_sys) {
      sys->ClockTick();
    }
#endif
    top->clock = top->clock ^ 1; // posedge
    top->eval();
  }

  printf("trying to close data\n");
  fflush(stdout);
  d_server->stop();
  printf("trying to close cmd\n");
  fflush(stdout);
  c_server->stop();
  printf("printing traces\n");
  fflush(stdout);
  tfp->close();
  exit(0);
}

int main(int argc, char **argv) {
  d_server = new data_server;
  c_server = new cmd_server;
  d_server->start();
  c_server->start();
  printf("Entering verilator\n");
  run_verilator(argc, argv);
  pthread_mutex_lock(&main_lock);
  pthread_mutex_lock(&main_lock);
  printf("Main thread exiting\n");
  exit(0);
}

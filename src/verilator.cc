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

uint64_t main_time = 0;

extern pthread_mutex_t cmdserverlock;
extern std::queue<composer::rocc_cmd> cmds;
extern std::unordered_map<system_core_pair, std::queue<int> *> in_flight;

pthread_mutex_t main_lock = PTHREAD_MUTEX_INITIALIZER;
bool kill_sig = false;

#ifdef USE_DRAMSIM
Config dramsim3config("../DRAMsim3/configs/DDR4_8Gb_x16_2666.ini", "./");
#endif

void enqueue_transaction(v_address_channel<ComposerMemIDDtype> &chan, std::queue<memory_transaction *> &lst) {
  if (chan.getValid() && chan.getValid()) {
    char *addr = (char *) at.translate(chan.getAddr());
    int sz = 1 << chan.getSize();
    int len = 1 + chan.getLen(); // per axi
    bool is_fixed = chan.getBurst() == 0;
    int id = chan.getId();
    uint64_t fpga_addr = chan.getAddr();
    auto tx = new memory_transaction(addr, sz, len, 0, is_fixed, id, fpga_addr);
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
  int id = 0;
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

static uint64_t get_dimm_address(uint64_t addr) {
  uint64_t acc = 0;
  uint64_t cursor = 1;
  int real = 0;
  for (int i = 0; i < 64; ++i) {
    if (addrMask & cursor) {
      if (addr & cursor) {
        acc |= 1 << real;
      }
      ++real;
    }
    cursor <<= 1;
  }
  return acc;
}

#ifdef TRACE
VerilatedVcdC *tfp;
#endif

//#define TRACE

void run_verilator() {
  // start servers to communicate with user programs
  const char *v[3] = {"", "+verilator+seed+14934534", "+verilator+rand+reset+2"};

  Verilated::commandArgs(3, v);

  Vcomposer top;

  Verilated::traceEverOn(true);
#if defined(TRACE)
  tfp = new VerilatedVcdC;
  top.trace(tfp, 100);
  tfp->open("trace.vcd");
#endif

  mem_interface<ComposerMemIDDtype> axi4_mems[NUM_DDR_CHANNELS];
  for (int i = 0; i < NUM_DDR_CHANNELS; ++i) {
    axi4_mems[i].id = i;
  }

#if defined(COMPOSER_HAS_DMA)
  mem_interface<ComposerDMAIDtype> dma;
  int dma_tx_progress = 0;
  int dma_tx_length = 0;
  dma.aw = new v_address_channel<ComposerDMAIDtype>(top.dma_aw_ready, top.dma_aw_valid, top.dma_aw_bits_id,
                                                    top.dma_aw_bits_size, top.dma_aw_bits_burst,
                                                    top.dma_aw_bits_addr, top.dma_aw_bits_len);
  dma.ar = new v_address_channel<ComposerDMAIDtype>(top.dma_ar_ready, top.dma_ar_valid, top.dma_ar_bits_id,
                                                    top.dma_ar_bits_size, top.dma_ar_bits_burst,
                                                    top.dma_ar_bits_addr, top.dma_ar_bits_len);
  dma.w = new data_channel<ComposerDMAIDtype>(top.dma_w_ready, top.dma_w_valid,
                                              &top.dma_w_bits_strb, top.dma_w_bits_last, nullptr);
  dma.r = new data_channel<ComposerDMAIDtype>(top.dma_r_ready, top.dma_r_valid,
                                              nullptr, top.dma_r_bits_last, &top.dma_r_bits_id);
  dma.w->setData((char *) top.dma_r_bits_data.m_storage);
  dma.r->setData((char *) top.dma_w_bits_data.m_storage);
  dma.b = new response_channel<ComposerDMAIDtype>(top.dma_b_ready, top.dma_b_valid, top.dma_b_bits_id);
#endif
#ifdef USE_DRAMSIM
  for (auto &axi4_mem: axi4_mems) {
    axi4_mem.mem_sys = new JedecDRAMSystem(
            dramsim3config,
            "",
            [&axi4_mem](uint64_t addr) {
              auto tx = axi4_mem.in_flight_reads[addr]->front();
              tx->progress--;
              if (tx->progress == 0) {
                axi4_mem.read_transactions.push(tx);
              }
              axi4_mem.in_flight_reads[addr]->pop();
            },
            [&axi4_mem](uint64_t addr) {
              auto tx = axi4_mem.in_flight_writes[addr]->front();
              tx->progress--;
              if (tx->progress == 0) {
                axi4_mem.b->to_enqueue = tx->id;
                delete tx;
              }
              axi4_mem.in_flight_writes[addr]->pop();
//              axi4_mem.write_transactions.push(new memory_transaction(ad, size, len, 0, false, id, addr));
//              printf("write transaction finish\n");
            });

  }
#endif
#ifdef VERBOSE
  printf("There are %d DDR Channels\n", NUM_DDR_CHANNELS);
#endif
#if NUM_DDR_CHANNELS >= 1
  init_ddr_interface(0)
  axi4_mems[0].w->setData((char *) &top.mem_0_w_bits_data.at(0));
  axi4_mems[0].r->setData((char *) &top.mem_0_r_bits_data.at(0));
#if NUM_DDR_CHANNELS >= 2
  init_ddr_interface(1)
#if NUM_DDR_CHANNELS >= 4
  init_ddr_interface(2)
  init_ddr_interface(3)
#endif
#endif
#endif
  // reset circuit
  top.reset = 1;
  for (auto &mem: axi4_mems) {
#ifndef USE_DRAMSIM
    mem.ar->setReady(1);
    mem.w->setReady(0);
#endif
    mem.r->setValid(0);
    mem.b->setValid(0);
    mem.aw->setReady(1);
  }
#ifdef COMPOSER_HAS_DMA
  dma.b->setReady(0);
  dma.ar->setValid(0);
  dma.aw->setValid(0);
  dma.w->setValid(0);
  dma.r->setReady(0);
#endif

  std::map<std::tuple<int, int>, unsigned long long> start_times;

  for (int i = 0; i < 50; ++i) {
    top.clock = 0;
    top.eval();
#ifdef TRACE
    tfp->dump(main_time);
#endif
    main_time += 4;
    top.clock = 1;
    top.eval();
#ifdef TRACE
    tfp->dump(main_time);
#endif
    main_time += 4;
  }
  top.reset = 0;
  top.clock = 0;

  command_transaction ongoing_cmd;
  response_transaction ongoing_rsp;
  update_state ongoing_update = UPDATE_IDLE_CMD;
  int cmds_in_flight = 0;
  bool bus_occupied = false;
  int check_freq = 50;
#ifdef VERBOSE
  printf("main time %lld\n", main_time);
#endif
  while (not kill_sig) {
    // clock is high after posedge - changes now are taking place after posedge,
    // and will take effect on negedge
    if (main_time % 1000000 <= 1) {
#ifdef VERBOSE
      printf("main time: %lld\n", main_time);
#endif
    }

    // ------------ HANDLE COMMAND INTERFACE ----------------


    // start queueing up a new command if one is available
    top.ocl_0_aw_valid = top.ocl_0_w_valid = top.ocl_0_r_ready = top.ocl_0_ar_valid = top.ocl_0_b_ready = 0;
    switch (ongoing_cmd.state) {
      // tell the composer that we're going to send 32-bits of a command over the PCIE bus
      case CMD_BITS_WRITE_ADDR:
        ongoing_cmd.ready_for_command = false;
        top.ocl_0_aw_valid = 1;
        top.ocl_0_aw_bits_len = 0;
        top.ocl_0_aw_bits_id = 0;
        top.ocl_0_aw_bits_addr = CMD_BITS;
        if (top.ocl_0_aw_ready) {
          ongoing_cmd.state = CMD_BITS_WRITE_DAT;
//          printf("to write dat\n");
        }
        break;
        // send the command over the PCIE bus
      case CMD_BITS_WRITE_DAT:
        top.ocl_0_w_valid = 1;
        top.ocl_0_w_bits_data = ongoing_cmd.cmdbuf[ongoing_cmd.progress];
        if (top.ocl_0_w_ready) {
          ongoing_cmd.state = CMD_BITS_WRITE_B;
//          printf("wrote %d, going to bits write response. BITS: %08x\n", ongoing_cmd.progress,
//                 ongoing_cmd.cmdbuf[ongoing_cmd.progress]);
        }
        break;
      case CMD_BITS_WRITE_B:
        top.ocl_0_b_ready = 1;
        if (top.ocl_0_b_valid) {
          if (top.ocl_0_b_bits_resp == 0) {
            ongoing_cmd.state = CMD_VALID_ADDR;
//            printf("to valid addr\n");
          } else {
            fprintf(stderr, "Recieved error from write response!");
            exit(1);
          }
        }
        // We just send the 32-bits, now "simulate" the decoupled interface by toggling to the CMD_VALID bit to 1
        // This bit is visible from the CMD_VALID bit, so we need to perform an AXI transaction
      case CMD_VALID_ADDR:
        top.ocl_0_aw_valid = 1;
        top.ocl_0_aw_bits_addr = CMD_VALID;
        top.ocl_0_aw_bits_len = 0; // length is actually one - see AXI spec
        top.ocl_0_aw_bits_id = 0;
        if (top.ocl_0_aw_ready) {
//          printf("to valid dat\n");
          ongoing_cmd.state = CMD_VALID_DAT;
        }
        break;
        // send the CMD_VALID bit
      case CMD_VALID_DAT:
        top.ocl_0_w_valid = 1;
        top.ocl_0_w_bits_data = 1;
        if (top.ocl_0_w_ready) {
//          printf("to valid b\n");
          ongoing_cmd.state = CMD_VALID_WRITE_B;
        }
        break;
      case CMD_VALID_WRITE_B:
        top.ocl_0_b_ready = 1;
        if (top.ocl_0_b_valid) {
          if (top.ocl_0_b_bits_resp == 0) {
            ongoing_cmd.progress++;
            // send last thing, yield bus
            if (ongoing_cmd.progress == command_transaction::payload_length) {
              ongoing_cmd.state = CMD_INACTIVE;
              bus_occupied = false;
            } else {
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
        top.ocl_0_ar_valid = 1;
        top.ocl_0_ar_bits_addr = CMD_READY;
        top.ocl_0_ar_bits_len = 0;
        top.ocl_0_ar_bits_id = 0;
        if (top.ocl_0_ar_ready) {
          ongoing_cmd.state = CMD_RECHECK_READY_DAT;
        }
        break;
        // read the CMD_READY bit
      case CMD_RECHECK_READY_DAT:
        top.ocl_0_r_ready = 1;
        if (top.ocl_0_r_valid) {
          // if it's ready for another command
          if (top.ocl_0_r_bits_data) {
            ongoing_cmd.state = CMD_BITS_WRITE_ADDR;
          } else {
            ongoing_cmd.state = CMD_RECHECK_READY_ADDR;
          }
        }
        break;
      case CMD_INACTIVE:
        if (ongoing_cmd.ready_for_command &&
            !bus_occupied &&
            (ongoing_update == UPDATE_IDLE_CMD || ongoing_update == UPDATE_IDLE_RESP)) {
          pthread_mutex_lock(&cmdserverlock);
          if (not cmds.empty()) {
            bus_occupied = true;
            ongoing_cmd.state = CMD_BITS_WRITE_ADDR;
            if (cmds.front().getXd()) {
              auto id = std::tuple<int, int>(cmds.front().getSystemId(), cmds.front().getCoreId());
              start_times[id] = main_time;
            }
            ongoing_cmd.cmdbuf = cmds.front().pack(pack_cfg);
            kill_sig = cmds.front().getOpcode() == ROCC_OP_FLUSH;
            ongoing_cmd.progress = 0;
            if (cmds.front().getXd() == 1)
              cmds_in_flight++;
            cmds.pop();
          }
          pthread_mutex_unlock(&cmdserverlock);
        }
        break;
    }
    switch (ongoing_rsp.state) {
      case RESPT_INACTIVE:
        break;
      case RESPT_BITS_ADDR:
        top.ocl_0_ar_bits_addr = RESP_BITS;
        top.ocl_0_ar_bits_len = 0;
        top.ocl_0_ar_valid = 1;
        top.ocl_0_ar_bits_id = 0;
        if (top.ocl_0_ar_ready) {
          ongoing_rsp.state = RESPT_BITS_READ;
        }
      case RESPT_BITS_READ:
        top.ocl_0_r_ready = 1;
        if (top.ocl_0_r_valid) {
          ongoing_rsp.resbuf[ongoing_rsp.progress++] = top.ocl_0_r_bits_data;
          ongoing_rsp.state = RESPT_READY_ADDR;
        }
        break;
      case RESPT_READY_ADDR:
        top.ocl_0_aw_valid = 1;
        top.ocl_0_aw_bits_len = 0;
        top.ocl_0_aw_bits_addr = RESP_READY;
        top.ocl_0_aw_bits_id = 0;
        if (top.ocl_0_aw_ready) {
          ongoing_rsp.state = RESPT_READY_WRITE;
        }
        break;
      case RESPT_READY_WRITE:
        top.ocl_0_w_valid = 1;
        top.ocl_0_w_bits_data = 1;
        if (top.ocl_0_w_ready) {
          ongoing_rsp.state = RESPT_READY_WRITE_B;
        }
      case RESPT_READY_WRITE_B:
        top.ocl_0_b_ready = 1;
        if (top.ocl_0_b_valid) {
          if (top.ocl_0_b_bits_resp == 0) {
            if (ongoing_rsp.progress == response_transaction::payload_length) {
              composer::rocc_response r(ongoing_rsp.resbuf, pack_cfg);
              auto id = std::tuple<int, int>(r.system_id, r.core_id);
              auto start = start_times[id];
#ifdef VERBOSE
              printf("Command took %f us\n", float((main_time - start)) / 1000);
#endif
              register_reponse(ongoing_rsp.resbuf);
              cmds_in_flight--;
              bus_occupied = false;
//              printf("respt-ready-b -> respt-inactive\n");
              ongoing_rsp.state = RESPT_INACTIVE;
            } else {
//              printf("Not done yet %d/%d. respt-ready-b -> respt-recheck-valid-address\n", ongoing_rsp.progress,
//                     response_transaction::payload_length);
              ongoing_rsp.state = RESPT_RECHECK_VALID_ADDR;
            }
          } else {
            fprintf(stderr, "Received error from write response!");
            exit(1);
          }
        }
        break;
      case RESPT_RECHECK_VALID_ADDR:
        top.ocl_0_ar_valid = 1;
        top.ocl_0_ar_bits_addr = RESP_VALID;
        top.ocl_0_ar_bits_len = 0;
        top.ocl_0_ar_bits_id = 0;
        if (top.ocl_0_ar_ready) {
          ongoing_rsp.state = RESPT_RECHECK_VALID_READ;
        }
        break;
      case RESPT_RECHECK_VALID_READ:
        top.ocl_0_r_ready = 1;
        if (top.ocl_0_r_valid) {
          if (top.ocl_0_r_bits_data) {
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
        top.ocl_0_ar_valid = 1;
        top.ocl_0_ar_bits_len = 0;
        top.ocl_0_ar_bits_id = 0;
        top.ocl_0_ar_bits_addr = RESP_VALID;
        if (top.ocl_0_ar_ready) {
          ongoing_update = UPDATE_RESP_WAIT;
        }
        break;
      case UPDATE_RESP_WAIT:
        top.ocl_0_r_ready = 1;
        if (top.ocl_0_r_valid) {
          ongoing_rsp.progress = 0;
          if (top.ocl_0_r_bits_data == 1) {
#ifdef VERBOSE
            printf("Found valid response on cycle %lu!!! %d %d\n", main_time, top.ocl_0_r_valid, top.ocl_0_r_bits_data);
#endif
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
        top.ocl_0_ar_valid = 1;
        top.ocl_0_ar_bits_id = 0;
        top.ocl_0_ar_bits_addr = CMD_READY;
        top.ocl_0_ar_bits_len = 0;
        if (top.ocl_0_ar_ready) {
          ongoing_update = UPDATE_CMD_WAIT;
        }
        break;
      case UPDATE_CMD_WAIT:
        top.ocl_0_r_ready = 1;
        if (top.ocl_0_r_valid) {
          ongoing_cmd.ready_for_command = top.ocl_0_r_bits_data;
          if (cmds_in_flight > 0) {
            ongoing_update = UPDATE_IDLE_RESP;
          } else {
            ongoing_update = UPDATE_IDLE_CMD;
          }
        }
        bus_occupied = false;
        break;
    }

#ifdef USE_DRAMSIM
    // approx clock diff
    for (int i = 0; i < 13; ++i) {
      for (auto &mem: axi4_mems) {
        mem.mem_sys->ClockTick();
      }
    }
#endif
    for (auto &inter: axi4_mems) {
      if (inter.r->getValid() && inter.r->getReady()) {
        auto tx = inter.read_transactions.front();
        tx->progress++;
        if (not tx->fixed) {
          tx->addr += tx->size;
        }
        if (inter.r->getLast()) {
          inter.read_transactions.pop();
          delete tx;
        }
      }

#ifdef USE_DRAMSIM
      if (inter.ar->getReady() && inter.ar->getValid()) {
        uint64_t addr = inter.ar->getAddr();
        char *ad = (char *) at.translate(addr);
        auto txsize = (int) pow(2, inter.ar->getSize());
        auto txlen = inter.ar->getLen() + 1;
        auto dram_txlen = txsize * txlen >> 3;
        if (dram_txlen == 0) dram_txlen = 1;
        auto tx = new memory_transaction(ad, txsize, txlen, dram_txlen, false,
                                         inter.ar->getId(), addr);
        // 64b per DRAM transaction
        tx->dram_tx_len = dram_txlen;
        inter.to_enqueue_read = tx;
        tx->dram_tx_progress = 0;
      }
#endif
    }

    top.clock = 1; // posedge
    main_time += 4;
    top.eval();
#ifdef TRACE
    tfp->dump(main_time);
#endif

    // ------------ HANDLE MEMORY INTERFACES ----------------
    for (mem_interface<ComposerMemIDDtype> &inter: axi4_mems) {
      if (inter.b->getReady() && inter.b->getValid()) {
        inter.b->send_ids.pop();
      }
      if (not inter.read_transactions.empty() && not inter.r->getValid()) {
        auto tx = inter.read_transactions.front();
        int start = (tx->progress * tx->size) % (DATA_BUS_WIDTH / 8);
        char *dest = (char *) inter.r->getData() + start;
        memcpy(dest, tx->addr, tx->size);
        bool am_done = tx->len == (tx->progress + 1);
        inter.r->setValid(1);
        inter.r->setLast(am_done);
        inter.r->setId(tx->id);
      } else {
        inter.r->setValid(0);
      }

      // update all channels with new information now that we've updated states
      if (not inter.b->send_ids.empty()) {
        inter.b->setValid(1);
        inter.b->setId(inter.b->send_ids.front());
      } else {
        inter.b->setValid(0);
      }

      if (inter.b->to_enqueue != -1) {
        inter.b->send_ids.push(inter.b->to_enqueue);
        inter.b->to_enqueue = -1;
      }

      // slave response to valid transaction and write valid
      // write can respond before write address so just ahve to wait for address
      // to be recieved. ready and valid have to be high at the same time for this
      // to work
      if (not inter.write_transactions.empty() and not inter.w->getReady()) {
#ifdef USE_DRAMSIM
        inter.w->setReady(inter.to_enqueue_write == nullptr);
#else
        inter.w->setReady(1);
#endif
        if (inter.w->getValid() && inter.w->getReady()) {
          auto trans = inter.write_transactions.front();
          // refer to https://developer.arm.com/documentation/ihi0022/e/AMBA-AXI3-and-AXI4-Protocol-Specification/Single-Interface-Requirements/Transaction-structure/Data-read-and-write-structure?lang=en#CIHIJFAF
          char *src = inter.w->getData();
          ComposerStrobeSimDtype strobe = inter.w->getStrobe();
          uint32_t off = 0;
          // for writes, we need to account for alignment and strobe,so we're re-aligning address here
          // align to 64B - zero out bottom 6b
          auto addr = trans->addr;
          while (strobe != 0) {
            if (strobe & 1) {
#ifdef COMPOSER_HAS_DMA
              auto curr_ptr = uintptr_t(addr + off);
              auto base_ptr = uintptr_t(dma_ptr);
              auto end_ptr = uintptr_t(dma_ptr + dma_len);
              if (dma_in_progress and dma_write and curr_ptr < end_ptr and curr_ptr >= base_ptr) {
                // we're performing a DMA into the same address space so just make sure that the values match up...
                if (addr[off] != src[off]) {
                  std::cerr << "DMA write was not a no-op. " << off << " " << int(addr[off]) << " " << int(src[off])
                            << std::endl;
                  tfp->close();
                  throw std::exception();
                }
              } else {
                addr[off] = src[off];
              }
#else
              addr[off] = src[off];
#endif
            }
            off += 1;
            strobe >>= 1;
          }
          trans->progress++;

          if (not trans->fixed) {
            trans->addr += trans->size;
          }

          if (inter.w->getLast()) {
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
        }
      } else {
        inter.w->setReady(0);
      }
      enqueue_transaction(*inter.aw, inter.write_transactions);

#ifdef USE_DRAMSIM

      if (inter.to_enqueue_read != nullptr) {
        auto &r = *inter.to_enqueue_read;
        auto dimm_base = get_dimm_address(r.fpga_addr);
        auto dimm_addr = dimm_base + 8 * r.dram_tx_progress;
        if (inter.mem_sys->WillAcceptTransaction(dimm_addr, false)) {
          inter.mem_sys->AddTransaction(dimm_addr, false);
          if (inter.in_flight_reads.find(dimm_addr) == inter.in_flight_reads.end())
            inter.in_flight_reads[dimm_addr] = new std::queue<memory_transaction *>();
          auto &q = *inter.in_flight_reads[dimm_addr];
          q.push(&r);
          r.dram_tx_progress++;
        }
        if (r.dram_tx_len == r.dram_tx_progress) {
          inter.to_enqueue_read = nullptr;
        }
      }

      if (inter.to_enqueue_write != nullptr) {
        auto &w = *inter.to_enqueue_write;
        auto dimm_addr = get_dimm_address(w.fpga_addr) + w.dram_tx_progress * 8;
        if (inter.mem_sys->WillAcceptTransaction(dimm_addr, true)) {
          w.dram_tx_progress++;
          inter.mem_sys->AddTransaction(dimm_addr, true);
          if (inter.in_flight_writes.find(dimm_addr) == inter.in_flight_writes.end())
            inter.in_flight_writes[dimm_addr] = new std::queue<memory_transaction *>;
          inter.in_flight_writes[dimm_addr]->push(&w);
        }
        if (w.dram_tx_progress == w.dram_tx_len) {
          inter.to_enqueue_write = nullptr;
        }
      }

      // to signify that the AXI4 DDR Controller is busy enqueueing another transaction in the DRAM
      inter.ar->setReady(inter.to_enqueue_read == nullptr);
#else
      enqueue_transaction(*inter.ar, inter.read_transactions);
#endif

    }

#ifdef COMPOSER_HAS_DMA
    pthread_mutex_lock(&dma_lock);
    // enqueue dma transaction into dma axi interface
    dma.aw->setValid(0);
    dma.ar->setValid(0);
    dma.b->setReady(0);
    dma.w->setValid(0);
    dma.r->setReady(0);
    if (dma_valid && not dma_in_progress) {
      dma_tx_progress = 0;
      dma_tx_length = dma_len >> 6;
      if (dma_write) {
        dma.aw->setValid(1);
        dma.aw->setAddr(dma_fpga_addr);
        dma.aw->setId(0);
        dma.aw->setSize(6);
        dma.aw->setLen((dma_len / 64) - 1);
        dma.aw->setBurst(1);
        if (dma.aw->fire()) {
          dma_in_progress = true;
        }
      } else {
        dma.ar->setValid(1);
        dma.ar->setAddr(dma_fpga_addr);
        dma.ar->setId(0);
        dma.ar->setSize(6);
        dma.ar->setLen((dma_len / 64) - 1);
        dma.ar->setBurst(1);
        if (dma.ar->fire()) {
          dma_in_progress = true;
        }
      }
    }

    if (dma_in_progress) {
      if (dma_write) {
        if (dma_tx_progress < dma_tx_length) {
          dma.w->setValid(1);
          memcpy(dma.w->getData(), dma_ptr + 64 * dma_tx_progress, 64);
          dma.w->setLast(dma_tx_progress + 1 == dma_tx_length);
          dma.w->setStrobe(0xFFFFFFFFFFFFFFFFL);
          if (dma.w->getReady()) {
            dma_tx_progress++;
          }
        } else {
          dma.b->setReady(1);
          if (dma.b->fire()) {
            dma_valid = false;
            dma_in_progress = false;
            pthread_mutex_unlock(&dma_wait_lock);
          }
        }
      } else {
        dma.r->setReady(1);
        if (dma.r->fire()) {
          char *rval = dma.r->getData();
          char *src_val = dma_ptr + 64 * dma_tx_progress;
          for (int i = 0; i < 64; ++i) {
            if (rval[i] != src_val[i]) {
              std::cerr << "Got an unexpected value from the DMA... " << i << " " << dma_tx_progress << " " << rval[i]
                        << " " << src_val[i] << std::endl;
              throw std::exception();
            }
          }
          dma_tx_progress++;
          if (dma_tx_progress == dma_tx_length) {
            dma_valid = false;
            dma_in_progress = false;
            pthread_mutex_unlock(&dma_wait_lock);
          }
        }
      }
    }
    pthread_mutex_unlock(&dma_lock);
#endif
    top.clock = 0; // negedge
    top.eval();
    main_time += 4;
#ifdef TRACE
    tfp->dump(main_time);
#endif
  }
#ifdef TRACE
#ifdef VERBOSE
  printf("printing traces\n");
#endif
  fflush(stdout);
  tfp->close();
#endif
  exit(0);
}

int main() {
  data_server::start();
  cmd_server::start();
#ifdef VERBOSE
  printf("Entering verilator\n");
#endif
  run_verilator();
  pthread_mutex_lock(&main_lock);
  pthread_mutex_lock(&main_lock);
#ifdef VERBOSE
  printf("Main thread exiting\n");
#endif
  exit(0);
}

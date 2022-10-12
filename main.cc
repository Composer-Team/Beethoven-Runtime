#include <iostream>

#include <verilated.h>
#include <Vcomposer.h>
#include <cinttypes>
#include <composer_verilator_server.h>
#include "data_server.h"
#include "cmd_server.h"
#include <yaml-cpp/yaml.h>

#include "verilated_vcd_c.h"

// in bytes
#define DATA_BUS_WIDTH 64

Vcomposer *top;
uint64_t main_time = 0;

#define ASSERT(x) if (!(x)) {fprintf(stderr, "Condition " # x " was not met!\n"); exit(1); }

void enqueue_transaction(address_channel<QData> &chan, std::queue<memory_transaction *> &lst) {
  ASSERT(*chan.id < 8)
  if (*chan.valid && *chan.ready) {
    printf("\tEnqueued transaction!\n");
    lst.push(
            new memory_transaction((char *) (*chan.addr), (int) pow(2, *chan.size),
                                   *chan.len + 1, 0, *chan.burst == 0, *chan.id));
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

int main(int argc, char **argv) {
  std::string config_path;
  auto cfg_path_c = std::getenv("COMPOSER_HARDWARE_DIR");
  if (cfg_path_c == nullptr) {
    auto second = std::getenv("COMPOSER_ROOT");
    if (second == nullptr) {
      std::cerr << "COMPOSER_HARDWARE_DIR not set!" << std::endl;
      exit(1);
    }
    config_path = std::string(second) + "/Composer-Hardware/composer.yaml";
  } else {
    config_path = std::string(cfg_path_c) + "/composer.yaml";
  }
  auto cfg = YAML::LoadFile(config_path);
  auto pack_cfg = composer_pack_info(cfg["system_id_bits"].as<int>(),
                                     cfg["core_id_bits"].as<int>());
  // start servers to communicate with user programs
  data_server dataServer{};
  cmd_server cmdServer;
  dataServer.start();
  cmdServer.start();
  Verilated::commandArgs(argc, argv);
  top = new Vcomposer;

  Verilated::traceEverOn(true);
  auto tfp = new VerilatedVcdC;
  top->trace(tfp, 100);
  tfp->open("trace.vcd");

  mem_interface<QData> axi4_mems[1];
  // ugly initializations
  {
    axi4_mems[0].aw = new address_channel(&top->axi4_mem_0_aw_ready,
                                          &top->axi4_mem_0_aw_valid,
                                          &top->axi4_mem_0_aw_bits_id,
                                          &top->axi4_mem_0_aw_bits_size,
                                          &top->axi4_mem_0_aw_bits_burst,
                                          &top->axi4_mem_0_aw_bits_addr);
    axi4_mems[0].ar = new address_channel(&top->axi4_mem_0_ar_ready,
                                          &top->axi4_mem_0_ar_valid,
                                          &top->axi4_mem_0_ar_bits_id,
                                          &top->axi4_mem_0_ar_bits_size,
                                          &top->axi4_mem_0_ar_bits_burst,
                                          &top->axi4_mem_0_ar_bits_addr);
    axi4_mems[0].w = new data_channel(&top->axi4_mem_0_w_ready,
                                      &top->axi4_mem_0_w_valid,
                                      &top->axi4_mem_0_w_bits_data,
                                      &top->axi4_mem_0_w_bits_strb,
                                      &top->axi4_mem_0_w_bits_last);
    axi4_mems[0].r = new data_channel(&top->axi4_mem_0_r_ready,
                                      &top->axi4_mem_0_r_valid,
                                      &top->axi4_mem_0_r_bits_data,
                                      nullptr,
                                      &top->axi4_mem_0_r_bits_last);
    axi4_mems[0].b = new response_channel(&top->axi4_mem_0_b_ready,
                                          &top->axi4_mem_0_b_valid,
                                          &top->ocl_0_b_bits_id);
  }
  // reset circuit
  top->reset = 1;
  *axi4_mems->ar->ready = 1;
  *axi4_mems->aw->ready = 1;
  *axi4_mems->r->valid = 0;
  *axi4_mems->w->ready = 0;
  *axi4_mems->b->valid = 0;
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
  int check_freq = 10000;

  while (main_time < check_freq * 20) {
    // clock is high after posedge - changes now are taking place after posedge,
    // and will take effect on negedge
    main_time++;
    if (((main_time >> 1)+50) % check_freq == 0) {
      // clear current line
      printf("\33[2K\r");
      printf("\rClock cycle: %lld", main_time);
      printf("\tCMDReady: %d\tRCU %d%d%d\tBusOccupied: %d\tCmdsInFlight: %d\tCProg %d",
             ongoing_cmd.ready_for_command, ongoing_rsp.state, ongoing_cmd.state, ongoing_update, bus_occupied, cmds_in_flight, ongoing_cmd.progress);
      fflush(stdout);
    }

    // ------------ HANDLE COMMAND INTERFACE ----------------

    tfp->dump(main_time);
    top->clock = top->clock ^ 1; // negedge
    top->eval();
    main_time++;

    // ------------ HANDLE MEMORY INTERFACES ----------------
    for (mem_interface<QData> &i: axi4_mems) {
      enqueue_transaction(*i.ar, i.read_transactions);
      enqueue_transaction(*i.aw, i.write_transactions);

      if (*i.r->valid || *i.w->valid) {
        printf("%d , %d!!!!", *i.ar->valid, *i.aw->valid);
      }
      // handle all advancements of channels before updating them because a posedge
      // just finished
      if (*i.b->ready && *i.b->valid) {
        i.b->send_ids.pop();
      }
      // if master is ready, advance state for read
      if (*i.r->ready && *i.r->valid) {
        auto trans = i.read_transactions.front();
        trans->progress++;
        if (not trans->fixed) {
          trans->addr += trans->size;
        }
        if (*i.r->last) {
          i.read_transactions.pop();
          i.current_read_channel_contents = -1;
        }
        delete trans;
      }

      // update all channels with new information now that we've updated states

      if (not i.b->send_ids.empty()) {
        *i.b->valid = 1;
        *i.b->id = i.b->send_ids.front();
      } else {
        *i.b->valid = 0;
      }

      // load data into bus but don't advance state
      // must use ready bit directly because we need it to be high at the posedge
      // for us to obey protocol
      if (!i.read_transactions.empty()) {
        auto trans = i.read_transactions.front();
        // don't copy data if it's already there (optimization)
        if (i.current_read_channel_contents != trans->progress) {
          // consider narrow transfers. Refer to....
          // https://developer.arm.com/documentation/ihi0022/e/AMBA-AXI3-and-AXI4-Protocol-Specification/Single-Interface-Requirements/Transaction-structure/Data-read-and-write-structure?lang=en#CIHIJFAF
          int start = (trans->progress * trans->size) % DATA_BUS_WIDTH;
          char *dest = (char *) i.r->data->m_storage + start;
          memcpy(dest, trans->addr, trans->size);
          i.current_read_channel_contents = trans->progress;
          *i.r->last = trans->progress == trans->len - 1;
          *i.r->id = trans->id;
        }
      }

      // slave response to valid transaction and write valid
      // write can respond before write address so just ahve to wait for address
      // to be recieved. ready and valid have to be high at the same time for this
      // to work
      if (*i.w->ready && *i.w->valid) {
        auto trans = i.write_transactions.front();
        // refer to https://developer.arm.com/documentation/ihi0022/e/AMBA-AXI3-and-AXI4-Protocol-Specification/Single-Interface-Requirements/Transaction-structure/Data-read-and-write-structure?lang=en#CIHIJFAF
        int start = (trans->progress * trans->size) % DATA_BUS_WIDTH;
        char *src = (char *) i.w->data->m_storage + start;
        uint64_t strobe = *i.w->strobe >> start;
        for (int byte = 0; byte < trans->size; ++byte) {
          if (strobe & 1) {
            trans->addr[byte] = src[byte];
          }
          strobe >>= 1;
        }
        if (not trans->fixed)
          trans->addr += trans->size;
        trans->progress++;
        if (*i.w->last) {
          i.b->send_ids.push(trans->id);
          i.write_transactions.pop();
        }
      }
    }

    // start queueing up a new command if one is available
    top->ocl_0_aw_valid = top->ocl_0_w_valid = top->ocl_0_r_ready = top->ocl_0_ar_valid = top->ocl_0_b_ready = 0;
    switch (ongoing_cmd.state) {
      // tell the composer that we're going to send 32-bits of a command over the PCIE bus
      case CMD_BITS_WRITE_ADDR:
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
          printf("wrote %d, going to bits write response\n", ongoing_cmd.progress);
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
              printf("Progress %d/%d. Continuing to CMD_RECHECK_READY_ADDR\n", ongoing_cmd.progress, command_transaction::payload_length);
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
          pthread_mutex_lock(&cmdServer.cmdserverlock);
          if (not cmdServer.cmds.empty()) {
            printf("\tGot command from cmd_server!\n\n");
            bus_occupied = true;
            ongoing_cmd.state = CMD_BITS_WRITE_ADDR;
            ongoing_cmd.cmdbuf = cmdServer.cmds.front().pack();
            ongoing_cmd.progress = 0;
            cmdServer.cmds.pop();
            cmds_in_flight++;
          }
          pthread_mutex_unlock(&cmdServer.cmdserverlock);
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
              rocc_response r(ongoing_rsp.resbuf, pack_cfg);
              system_core_pair pr(r.system_id, r.core_id);
              pthread_mutex_lock(&cmdServer.cmdserverlock);
              auto q = cmdServer.in_flight[pr];
              int id = q->front();
              csf->responses[id] = r;
              // allow client thread to access response
              pthread_mutex_unlock(&csf->wait_for_response[id]);
              q->pop();
              pthread_mutex_unlock(&cmdServer.cmdserverlock);
              cmds_in_flight--;
              bus_occupied = false;
              printf("respt-ready-b -> respt-inactive\n");
              ongoing_rsp.state = RESPT_INACTIVE;
            } else {
              printf("Not done yet %d/%d. respt-ready-b -> respt-recheck-valid-address\n", ongoing_rsp.progress, response_transaction::payload_length);
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
          printf("Checked Resp(%d)\n", top->ocl_0_r_bits_data);
          if (top->ocl_0_r_bits_data == 1) {
            printf("Response detected, starting response handling\n");
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
            printf("\nCmd checked, checking response now\n");
            ongoing_update = UPDATE_IDLE_RESP;
          } else {
            printf("\nCmd checked, no commands in flight\n");
            ongoing_update = UPDATE_IDLE_CMD;
          }
        }
        bus_occupied = false;
        break;
    }

    for (mem_interface<QData> &i: axi4_mems) {
      *i.w->ready = not i.write_transactions.empty();
      *i.r->valid = not i.read_transactions.empty();
    }

    tfp->dump(main_time);
    top->clock = top->clock ^ 1; // posedge
    top->eval();
  }

  printf("trying to close data\n"); fflush(stdout);
  dataServer.stop();
  printf("trying to close cmd\n"); fflush(stdout);
  cmdServer.stop();
  printf("printing traces\n"); fflush(stdout);
  tfp->close();
}
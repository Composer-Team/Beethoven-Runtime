#include <iostream>

#include <verilated.h>
#include <Vcomposer.h>
#include <cinttypes>
#include <composer_verilator_server.h>
#include "data_server.h"
#include "cmd_server.h"
#include <yaml-cpp/yaml.h>

// in bytes
#define DATA_BUS_WIDTH 64

Vcomposer *top;
uint64_t main_time = 0;

#define ASSERT(x) if (!(x)) {fprintf(stderr, "Condition " # x " was not met!\n"); exit(1); }

void enqueue_transaction(address_channel<QData> &chan, std::queue<memory_transaction *> &lst) {
  ASSERT(*chan.id < 8)
  if (*chan.valid && *chan.ready) {
    lst.push(
            new memory_transaction((char *) (*chan.addr), (int) pow(2, *chan.size),
                                   *chan.len + 1, 0, *chan.burst == 0, *chan.id));
  }
}

enum cmd_transfer_state {
  CMD_INACTIVE,
  CMD_BITS_WRITE_ADDR,
  CMD_BITS_WRITE_DAT,
  CMD_VALID_ADDR,
  CMD_VALID_DAT
};

enum resp_transfer_state {
  RESPT_INACTIVE,
  RESPT_BITS_ADDR,
  RESPT_BITS_READ,
  RESPT_READY_ADDR,
  RESPT_READY_WRITE
};

enum update_state {
  UPDATE_CMD_ADDR,
  UPDATE_CMD_WAIT,
  UPDATE_RESP_ADDR,
  UPDATE_RESP_WAIT,
  UPDATE_STALL // need the channel to service a response read
};

struct command_transaction {
  uint32_t *cmdbuf = nullptr;
  int8_t progress = 0;
  cmd_transfer_state state = CMD_INACTIVE;
  bool ready_for_command = false;
  bool can_commence = true;

  static const int payload_length = 5;
};

struct response_transaction {
  uint32_t resbuf[4]{};
  uint8_t progress = 0;
  resp_transfer_state state = RESPT_INACTIVE;
  bool can_commence = true;

  static const int payload_length = 4;
};

int main(int argc, char **argv) {
  auto cfg_path_c = std::getenv("COMPOSER_HARDWARE_DIR");
  if (cfg_path_c == nullptr) {
    std::cerr << "COMPOSER_HARDWARE_DIR not set!" << std::endl;
    exit(1);
  }
  std::string config_path = std::string(cfg_path_c) + "/composer.yaml";
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
  top->reset = 0;
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
  top->reset = 1;

  command_transaction ongoing_cmd;
  response_transaction ongoing_rsp;
  update_state ongoing_update = UPDATE_CMD_ADDR;

  while (!Verilated::gotFinish()) {
    // clock is high after posedge - changes now are taking place after posedge,
    // and will take effect on negedge
    main_time++;

    // ------------ HANDLE COMMAND INTERFACE ----------------


    top->clock = top->clock ^ 1; // negedge
    top->eval();

    // ------------ HANDLE MEMORY INTERFACES ----------------
    for (mem_interface<QData> &i: axi4_mems) {
      enqueue_transaction(*i.ar, i.read_transactions);
      enqueue_transaction(*i.aw, i.write_transactions);

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
    top->ocl_0_aw_valid = top->ocl_0_w_valid = top->ocl_0_r_ready = top->ocl_0_ar_valid = 0;
    switch (ongoing_cmd.state) {
      case CMD_BITS_WRITE_ADDR:
        top->ocl_0_aw_valid = 1;
        top->ocl_0_aw_bits_len = command_transaction::payload_length - 1;
        top->ocl_0_aw_bits_id = 0;
        top->ocl_0_aw_bits_addr = CMD_BITS;
        if (top->ocl_0_aw_ready)
          ongoing_cmd.state = CMD_BITS_WRITE_DAT;
        break;
      case CMD_BITS_WRITE_DAT:
        top->ocl_0_w_valid = 1;
        top->ocl_0_w_bits_data = ongoing_cmd.cmdbuf[ongoing_cmd.progress];
        if (top->ocl_0_w_ready) {
          ongoing_cmd.progress++;
          if (ongoing_cmd.progress == command_transaction::payload_length) {
            ongoing_cmd.state = CMD_VALID_ADDR;
          }
        }
        break;
      case CMD_VALID_ADDR:
        top->ocl_0_aw_valid = 1;
        top->ocl_0_aw_bits_addr = CMD_VALID;
        top->ocl_0_aw_bits_len = 0; // length is actually one - see AXI spec
        top->ocl_0_aw_bits_id = 0;
        if (top->ocl_0_aw_ready) {
          ongoing_cmd.state = CMD_VALID_DAT;
        }
        break;
      case CMD_VALID_DAT:
        top->ocl_0_w_valid = 1;
        top->ocl_0_w_bits_data = 1;
        if (top->ocl_0_w_ready) {
          ongoing_cmd.state = CMD_INACTIVE;
          ongoing_cmd.can_commence = true;
        }
        break;
      case CMD_INACTIVE:
        if (ongoing_cmd.ready_for_command && ongoing_cmd.can_commence) {
          pthread_mutex_lock(&cmdServer.cmdserverlock);
          if (not cmdServer.cmds.empty()) {
            ongoing_rsp.can_commence = false;
            ongoing_cmd.state = CMD_BITS_WRITE_ADDR;
            ongoing_cmd.cmdbuf = cmdServer.cmds.front().pack();
            ongoing_cmd.progress = 0;
            cmdServer.cmds.pop();
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
          ongoing_rsp.state = RESPT_BITS_READ;
        }
      case RESPT_BITS_READ:
        top->ocl_0_r_ready = 1;
        if (top->ocl_0_r_valid) {
          ongoing_rsp.resbuf[ongoing_rsp.progress++] = top->ocl_0_r_bits_data;
          ongoing_rsp.state = RESPT_READY_ADDR;
        }
        break;
      case RESPT_READY_ADDR:
        top->ocl_0_aw_valid = 1;
        top->ocl_0_aw_bits_len = 0;
        top->ocl_0_aw_bits_addr = RESP_READY;
        top->ocl_0_aw_bits_id = 0;
        if (top->ocl_0_aw_ready) {
          ongoing_rsp.state = RESPT_READY_WRITE;
        }
        break;
      case RESPT_READY_WRITE:
        top->ocl_0_w_valid = 1;
        top->ocl_0_w_bits_data = 1;
        if (top->ocl_0_w_ready) {
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
          }
        }
    }

    for (mem_interface<QData> &i: axi4_mems) {
      *i.w->ready = not i.write_transactions.empty();
      *i.r->valid = not i.read_transactions.empty();
    }

    top->clock = top->clock ^ 1; // posedge
    top->eval();
  }

  dataServer.stop();
  cmdServer.stop();
}
//
// Created by Chris Kjellqvist on 8/9/23.
//

#include "sim/front_bus_ctrl_axi.h"
#include "composer_allocator_declaration.h"
#include "sim/mem_ctrl.h"
#include "util.h"
#include <cmd_server.h>
#include <csignal>
#include <verilated_fst_c.h>
#include <verilated_vcd_c.h>

static bool bus_occupied = false;
extern pthread_mutex_t cmdserverlock;
extern std::queue<composer::rocc_cmd> cmds;
extern std::unordered_map<system_core_pair, std::queue<int> *> in_flight;
static std::map<std::tuple<int, int>, unsigned long long> start_times;
extern uint64_t main_time;
static int cmds_inflight = 0;
static int check_freq = 50;
extern bool kill_sig;
extern mem_ctrl::mem_interface<ComposerMemIDDtype> axi4_mems[NUM_DDR_CHANNELS];
#ifdef USE_VCD
extern VerilatedVcdC *tfp;
#else
extern VerilatedFstC *tfp;
#endif

static void sig_handle(int sig) {
  for (auto q: axi4_mems) {
    q.mem_sys->PrintEpochStats();
  }
  tfp->close();
  fprintf(stderr, "FST written!\n");
  fflush(stderr);
  exit(sig);
}

command_transaction ongoing_cmd;
response_transaction ongoing_rsp;
update_state ongoing_update = UPDATE_IDLE_CMD;


void update_command_state(VComposerTop &top){
  switch (ongoing_cmd.state) {
    // tell the composer that we're going to send 32-bits of a command over the PCIE bus
    case CMD_BITS_WRITE_ADDR:
      ongoing_cmd.ready_for_command = false;
      top.S00_AXI_awvalid = 1;
#ifndef CONTROL_LITE
      top.S00_AXI_awlen = 0;
      top.S00_AXI_awid = 0;
#endif
      top.S00_AXI_awaddr = CMD_BITS;
      if (top.S00_AXI_awready) {
        ongoing_cmd.state = CMD_BITS_WRITE_DAT;
        //          printf("to write dat\n");
      }
      break;
      // send the command over the PCIE bus
    case CMD_BITS_WRITE_DAT:
      top.S00_AXI_wvalid = 1;
#if AXIL_BUS_WIDTH <= 64
      top.S00_AXI_wdata = ongoing_cmd.cmdbuf[ongoing_cmd.progress];
#else
      top.S00_AXI_wdata.at(0) = ongoing_cmd.cmdbuf[ongoing_cmd.progress];
#endif
      LOG(printf("Writing %x to %x\n", top.S00_AXI_wdata, CMD_BITS));
      if (top.S00_AXI_wready) {
        ongoing_cmd.state = CMD_BITS_WRITE_B;
      }
      break;
    case CMD_BITS_WRITE_B:
      top.S00_AXI_bready = 1;
      if (top.S00_AXI_bvalid) {
        if (top.S00_AXI_bresp == 0) {
          ongoing_cmd.state = CMD_VALID_ADDR;
        } else {
          fprintf(stderr, "Recieved error from write response!");
          sig_handle(1);
        }
      }
      break;
      // We just send the 32-bits, now "simulate" the decoupled interface by toggling to the CMD_VALID bit to 1
      // This bit is visible from the CMD_VALID bit, so we need to perform an AXI transaction
    case CMD_VALID_ADDR:
      top.S00_AXI_awvalid = 1;
      top.S00_AXI_awaddr = CMD_VALID;
#ifndef CONTROL_LITE
      top.S00_AXI_awlen = 0;// length is actually one - see AXI spec
      top.S00_AXI_awid = 0;
#endif
      if (top.S00_AXI_awready) {
        //          printf("to valid dat\n");
        ongoing_cmd.state = CMD_VALID_DAT;
      }
      break;
      // send the CMD_VALID bit
    case CMD_VALID_DAT:
      top.S00_AXI_wvalid = 1;
#if AXIL_BUS_WIDTH <= 64
      top.S00_AXI_wdata = 1;
#else
      top.S00_AXI_wdata.at(0) = 1;
#endif
      if (top.S00_AXI_wready) {
        ongoing_cmd.state = CMD_VALID_WRITE_B;
      }
      break;
    case CMD_VALID_WRITE_B:
      top.S00_AXI_bready = 1;
      if (top.S00_AXI_bvalid) {
        if (top.S00_AXI_bresp == 0) {
          ongoing_cmd.progress++;
          // send last thing, yield bus
          if (ongoing_cmd.progress == command_transaction::payload_length) {
            for (auto &axi_mem: axi4_mems) {
              axi_mem.mem_sys->ResetStats();
            }
            ongoing_cmd.state = CMD_INACTIVE;
            bus_occupied = false;
          } else {
            // else, need to send the next 32-bit chunk and see that the channel is "ready"
            ongoing_cmd.state = CMD_RECHECK_READY_ADDR;
          }
        } else {
          fprintf(stderr, "Recieved error from write response!");
          sig_handle(1);
        }
      }
      break;
      // We just send 32-bits over the interface, check if it's ready for another 32b by requesting ready from the
      // CMD_READY bit
    case CMD_RECHECK_READY_ADDR:
      top.S00_AXI_arvalid = 1;
      top.S00_AXI_araddr = CMD_READY;
#ifndef CONTROL_LITE
      top.S00_AXI_arlen = 0;
      top.S00_AXI_arid = 0;
#endif
      if (top.S00_AXI_arready) {
        ongoing_cmd.state = CMD_RECHECK_READY_DAT;
      }
      break;
      // read the CMD_READY bit
    case CMD_RECHECK_READY_DAT:
      top.S00_AXI_rready = 1;
      if (top.S00_AXI_rvalid) {
        // if it's ready for another command
        if (top.S00_AXI_rdata) {
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
            cmds_inflight++;
          cmds.pop();
        }
        pthread_mutex_unlock(&cmdserverlock);
      }
      break;
  }

}

void update_resp_state(VComposerTop &top) {
  switch (ongoing_rsp.state) {
    case RESPT_INACTIVE:
      break;
    case RESPT_BITS_ADDR:
      top.S00_AXI_araddr = RESP_BITS;
      top.S00_AXI_arvalid = 1;
#ifndef CONTROL_LITE
      top.S00_AXI_arlen = 0;
      top.S00_AXI_arid = 0;
#endif
      if (top.S00_AXI_arready) {
        ongoing_rsp.state = RESPT_BITS_READ;
      }
    case RESPT_BITS_READ:
      top.S00_AXI_rready = 1;
      if (top.S00_AXI_rvalid) {
#if AXIL_BUS_WIDTH <= 64
        ongoing_rsp.resbuf[ongoing_rsp.progress++] = top.S00_AXI_rdata;
#else
        ongoing_rsp.resbuf[ongoing_rsp.progress++] = top.S00_AXI_rdata.at(0);
#endif
        ongoing_rsp.state = RESPT_READY_ADDR;
      }
      break;
    case RESPT_READY_ADDR:
      top.S00_AXI_awvalid = 1;
      top.S00_AXI_awaddr = RESP_READY;
#ifndef CONTROL_LITE
      top.S00_AXI_awlen = 0;
      top.S00_AXI_awid = 0;
#endif
      if (top.S00_AXI_awready) {
        ongoing_rsp.state = RESPT_READY_WRITE;
      }
      break;
    case RESPT_READY_WRITE:
      top.S00_AXI_wvalid = 1;
#if AXIL_BUS_WIDTH <= 64
      top.S00_AXI_wdata = 1;
#else
      top.S00_AXI_wdata.at(0) = 1;
#endif
      if (top.S00_AXI_wready) {
        ongoing_rsp.state = RESPT_READY_WRITE_B;
      }
    case RESPT_READY_WRITE_B:
      top.S00_AXI_bready = 1;
      if (top.S00_AXI_bvalid) {
        if (top.S00_AXI_bresp == 0) {
          if (ongoing_rsp.progress == response_transaction::payload_length) {
            composer::rocc_response r(ongoing_rsp.resbuf, pack_cfg);
            auto id = std::tuple<int, int>(r.system_id, r.core_id);
            auto start = start_times[id];
            LOG(printf("Command took %f ms\n", float((main_time - start)) / 1000 / 1000 / 1000));
            register_reponse(ongoing_rsp.resbuf);
            cmds_inflight--;
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
          sig_handle(1);
        }
      }
      break;
    case RESPT_RECHECK_VALID_ADDR:
      top.S00_AXI_arvalid = 1;
      top.S00_AXI_araddr = RESP_VALID;
#ifndef CONTROL_LITE
      top.S00_AXI_arlen = 0;
      top.S00_AXI_arid = 0;
#endif
      if (top.S00_AXI_arready) {
        ongoing_rsp.state = RESPT_RECHECK_VALID_READ;
      }
      break;
    case RESPT_RECHECK_VALID_READ:
      top.S00_AXI_rready = 1;
      if (top.S00_AXI_rvalid) {
        if (top.S00_AXI_rdata) {
          ongoing_rsp.state = RESPT_BITS_ADDR;
        } else {
          ongoing_rsp.state = RESPT_RECHECK_VALID_ADDR;
        }
      }
      break;
  }
}

void update_update_state(VComposerTop &top) {
  switch (ongoing_update) {
    case UPDATE_IDLE_RESP:
      if (!bus_occupied && (main_time % check_freq == 0)) {
        bus_occupied = true;
        ongoing_update = UPDATE_RESP_ADDR;
      }
      break;
    case UPDATE_RESP_ADDR:
      top.S00_AXI_arvalid = 1;
#ifndef CONTROL_LITE
      top.S00_AXI_arlen = 0;
      top.S00_AXI_arid = 0;
#endif
      top.S00_AXI_araddr = RESP_VALID;
      if (top.S00_AXI_arready) {
        ongoing_update = UPDATE_RESP_WAIT;
      }
      break;
    case UPDATE_RESP_WAIT:
      top.S00_AXI_rready = 1;
      if (top.S00_AXI_rvalid) {
        ongoing_rsp.progress = 0;
        if (
#if AXIL_BUS_WIDTH <= 64
            top.S00_AXI_rdata == 1
#else
            top.S00_AXI_rdata.at(0) == 1
#endif
        ) {
          LOG(printf("Found valid response on cycle %lu!!! %d %d\n", main_time, top.S00_AXI_rvalid, top.S00_AXI_rdata));
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
      top.S00_AXI_arvalid = 1;
      top.S00_AXI_araddr = CMD_READY;
#ifndef CONTROL_LITE
      top.S00_AXI_arid = 0;
      top.S00_AXI_arlen = 0;
#endif
      if (top.S00_AXI_arready) {
        ongoing_update = UPDATE_CMD_WAIT;
      }
      break;
    case UPDATE_CMD_WAIT:
      top.S00_AXI_rready = 1;
      if (top.S00_AXI_rvalid) {
        ongoing_cmd.ready_for_command = top.S00_AXI_rdata;
        if (cmds_inflight > 0) {
          ongoing_update = UPDATE_IDLE_RESP;
        } else {
          ongoing_update = UPDATE_IDLE_CMD;
        }
      }
      bus_occupied = false;
      break;
  }
}
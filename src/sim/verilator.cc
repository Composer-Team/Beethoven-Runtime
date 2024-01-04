#include <iostream>

#include "VComposerTop.h"
#include "cmd_server.h"
#include "data_server.h"
#include <csignal>
#include <pthread.h>
#include <queue>
#include <verilated.h>

#include "sim/mem_ctrl.h"
#include "sim/ddr_macros.h"
#include "sim/verilator.h"
#include "sim/front_bus_ctrl.h"

#ifdef USE_VCD
#include "verilated_vcd_c.h"
#else
#include "verilated_fst_c.h"
#endif
#include <composer_allocator_declaration.h>

uint64_t main_time = 0;

extern std::queue<composer::rocc_cmd> cmds;
extern std::unordered_map<system_core_pair, std::queue<int> *> in_flight;
mem_ctrl::mem_interface<ComposerMemIDDtype> axi4_mems[NUM_DDR_CHANNELS];

pthread_mutex_t main_lock = PTHREAD_MUTEX_INITIALIZER;
bool kill_sig = false;

#ifndef DEFAULT_PL_CLOCK
#define FPGA_CLOCK 100
#else
#define FPGA_CLOCK DEFAULT_PL_CLOCK
#endif

#ifdef USE_VCD
VerilatedVcdC *tfp;
#else
VerilatedFstC *tfp;
#endif

static void sig_handle(int sig) {
  tfp->close();
  fprintf(stderr, "FST written!\n");
  fflush(stderr);
  exit(sig);
}

void tick(VComposerTop *top) {
  try {
    top->eval();
  } catch (std::exception &e) {
    tfp->dump(main_time);
    tfp->close();
    std::cerr << "Emergency dump!" << std::endl;
    throw e;
  }
}


void run_verilator() {
#if 500000 % FPGA_CLOCK != 0
    fprintf(stderr, "Provided FPGA clock rate (%d MHz) does not evenly divide 500. This may result in some inaccuracies in precise simulation measurements.", FPGA_CLOCK);
#endif

#ifdef USE_DRAMSIM
  mem_ctrl::init("../DRAMsim3/configs/DDR4_8Gb_x8_3200.ini");
  // using this to estimate AWS bandwidth
  // KRIA has much slower memory!
  // Config dramsim3config("../DRAMsim3/configs/Kria.ini", "./");
  const float DDR_CLOCK = 1000.0 / dramsim3config->tCK; // NOLINT
#else
  auto DDR_CLOCK = 1333;
  axi_ddr_bus_multiplicity = (DATA_BUS_WIDTH / 8) / DDR_BUS_WIDTH_BYTES;
  DDR_BUS_BURST_LENGTH = 8;
#endif


  auto fpga_clock_inc = 500000 / FPGA_CLOCK;
  std::cout << "FPGA CLOCK RATE (MHz): " << FPGA_CLOCK << std::endl;
  int ddr_clock_inc = DDR_CLOCK / FPGA_CLOCK; // NOLINT

  // start servers to communicate with user programs
  const char *v[3] = {"", "+verilator+seed+14934534", "+verilator+rand+reset+2"};
  Verilated::commandArgs(3, v);
  VComposerTop top;
  Verilated::traceEverOn(true);
#ifdef USE_VCD
  tfp = new VerilatedVcdC;
  top.trace(tfp, 30);
  tfp->open("trace.vcd");
#else
  tfp = new VerilatedFstC;
  top.trace(tfp, 30);
  tfp->open("trace.fst");
#endif

  std::cout << "Tracing!" << std::endl;

  for (int i = 0; i < NUM_DDR_CHANNELS; ++i) {
    axi4_mems[i].id = i;
  }

#if defined(COMPOSER_HAS_DMA)

  mem_ctrl::mem_interface<ComposerDMAIDtype> dma;
  int dma_txprogress = 0;
  int dma_txlength = 0;
  dma.aw = new mem_ctrl::v_address_channel<ComposerDMAIDtype>(top.dma_awready, top.dma_awvalid, top.dma_awid,
                                                    top.dma_awsize, top.dma_awburst,
                                                    top.dma_awaddr, top.dma_awlen);
  dma.ar = new mem_ctrl::v_address_channel<ComposerDMAIDtype>(top.dma_arready, top.dma_arvalid, top.dma_arid,
                                                    top.dma_arsize, top.dma_arburst,
                                                    top.dma_araddr, top.dma_arlen);
  dma.w = new mem_ctrl::data_channel<ComposerDMAIDtype>(top.dma_wready, top.dma_wvalid,
                                              &top.dma_wstrb, top.dma_wlast, nullptr);
  dma.r = new mem_ctrl::data_channel<ComposerDMAIDtype>(top.dma_rready, top.dma_rvalid,
                                              nullptr, top.dma_rlast, &top.dma_rid);
  dma.w->setData((char *) top.dma_rdata.m_storage);
  dma.r->setData((char *) top.dma_wdata.m_storage);
  dma.b = new mem_ctrl::response_channel<ComposerDMAIDtype>(top.dma_bready, top.dma_bvalid, top.dma_bid);
#endif
#ifdef USE_DRAMSIM
  for (auto &axi4_mem: axi4_mems) {
    axi4_mem.init_dramsim3();
  }
#endif

#if NUM_DDR_CHANNELS >= 1
  init_ddr_interface(0)
  axi4_mems[0].r->setData((char *) &top.M00_AXI_rdata.at(0));
  axi4_mems[0].w->setData((char *) &top.M00_AXI_wdata.at(0));
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


  for (int i = 0; i < 50; ++i) {
    top.clock = 0;
    tick(&top);
    tfp->dump(main_time);
    main_time += fpga_clock_inc;
    top.clock = 1;
    tick(&top);
    tfp->dump(main_time);
    main_time += fpga_clock_inc;
  }
  top.reset = 0;
  top.clock = 0;

  command_transaction ongoing_cmd;
  response_transaction ongoing_rsp;
  update_state ongoing_update = UPDATE_IDLE_CMD;
#ifdef VERBOSE
  printf("main time %lld\n", main_time);
#endif
  unsigned long ms = 1;
  while (not kill_sig) {
    // clock is high after posedge - changes now are taking place after posedge,
    // and will take effect on negedge
    if (main_time > ms * 1000 * 1000 * 1000) {
      printf("main time: %lu ms\n", ms);
      fflush(stdout);
      ++ms;
    }

#ifdef KILL_SIM
    if (ms >= KILL_SIM) {
      kill_sig = true;
    }
#endif
    top.clock = 1;// posedge
    main_time += fpga_clock_inc;
    {

      // ------------ HANDLE COMMAND INTERFACE ----------------
      // start queueing up a new command if one is available
      top.S00_AXI_awvalid = top.S00_AXI_wvalid = top.S00_AXI_rready = top.S00_AXI_arvalid = top.S00_AXI_bready = 0;
      update_command_state(ongoing_cmd, ongoing_rsp, ongoing_update, top);
      update_resp_state(ongoing_cmd, ongoing_rsp, ongoing_update, top);
      update_update_state(ongoing_cmd, ongoing_rsp, ongoing_update, top);

#ifdef USE_DRAMSIM
      // approx clock diff
      for (int i = 0; i < ddr_clock_inc; ++i) {
        for (auto &axi4_mem: axi4_mems) {
          axi4_mem.mem_sys->ClockTick();
          try_to_enqueue_ddr(axi4_mem);
        }
      }
#endif
      for (auto &axi4_mem: axi4_mems) {
        if (axi4_mem.r->getValid() && axi4_mem.r->getReady()) {
          RLOCK
          auto tx = axi4_mem.read_transactions.front();
          tx->axi_bus_beats_progress++;
          if (not tx->fixed) {
            tx->addr += tx->size;
          }
          if (axi4_mem.r->getLast() || !tx->can_be_last) {
            axi4_mem.read_transactions.pop();
          }
          RUNLOCK
        }

#ifdef USE_DRAMSIM
        if (axi4_mem.ar->getReady() && axi4_mem.ar->getValid()) {
          uint64_t addr = axi4_mem.ar->getAddr();
          char *ad = (char *) at.translate(addr);
          auto txsize = (int) 1 << axi4_mem.ar->getSize();
          auto txlen = axi4_mem.ar->getLen() + 1;
          auto tx = std::make_shared<mem_ctrl::memory_transaction>(ad, txsize, txlen, 0, false,
                                                                   axi4_mem.ar->getId(), addr);
          // 64b per DRAM transaction
          RLOCK
          axi4_mem.ddr_read_q.push_back(tx);
          RUNLOCK
        }
#endif
      }


      // ------------ HANDLE MEMORY INTERFACES ----------------
      for (mem_ctrl::mem_interface<ComposerMemIDDtype> &axi4_mem: axi4_mems) {
        if (axi4_mem.b->getReady() && axi4_mem.b->getValid()) {
          //#ifdef VERBOSE
          //        fprintf(stderr, "Write response %d\n", axi4_mem.b->send_ids.front());
          //#endif
          axi4_mem.b->send_ids.pop();
        }

        RLOCK
        if (not axi4_mem.read_transactions.empty()) {
          auto tx = axi4_mem.read_transactions.front();
          int start = (tx->axi_bus_beats_progress * tx->size) % (DATA_BUS_WIDTH / 8);
          char *dest = (char *) axi4_mem.r->getData() + start;
          memcpy(dest, tx->addr, tx->size);
          bool am_done = tx->len == (tx->axi_bus_beats_progress + 1);
          axi4_mem.r->setValid(1);
          axi4_mem.r->setLast(am_done && tx->can_be_last);
          axi4_mem.r->setId(tx->id);
        } else {
          axi4_mem.r->setValid(0);
          axi4_mem.r->setLast(false);
        }
        RUNLOCK

        WLOCK
        // update all channels with new information now that we've updated states
        if (not axi4_mem.b->send_ids.empty()) {
          axi4_mem.b->setValid(1);
          axi4_mem.b->setId(axi4_mem.b->send_ids.front());
        } else {
          axi4_mem.b->setValid(0);
        }

        if (!axi4_mem.b->to_enqueue.empty()) {
          axi4_mem.b->send_ids.push(axi4_mem.b->to_enqueue.front());
          axi4_mem.b->to_enqueue.pop();
        }

        mem_ctrl::enqueue_transaction(*axi4_mem.aw, axi4_mem.write_transactions);

        if (not axi4_mem.write_transactions.empty()) {
          if (axi4_mem.w->getValid() && axi4_mem.w->getReady()) {
            auto trans = axi4_mem.write_transactions.front();
            // refer to https://developer.arm.com/documentation/ihi0022/e/AMBA-AXI3-and-AXI4-Protocol-Specification/Single-Interface-Requirements/Transaction-structure/Data-read-and-write-structure?lang=en#CIHIJFAF
            char *src = axi4_mem.w->getData();
            ComposerStrobeSimDtype strobe = axi4_mem.w->getStrobe();
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
            trans->axi_bus_beats_progress++;

            if (not trans->fixed) {
              trans->addr += trans->size;
            }

            if (axi4_mem.w->getLast()) {
              axi4_mem.write_transactions.pop();

#ifdef USE_DRAMSIM
              trans->dram_tx_axi_enqueue_progress = 0;
              trans->dram_tx_len_bus_beats = trans->len * trans->size >> 3;
              trans->axi_bus_beats_progress = 1;
              axi4_mem.ddr_write_q.push_back(trans);
#else
              axi4_mem.b->to_enqueue.push(trans->id);
#endif
            }
          }
        } else {
          axi4_mem.w->setReady(0);
        }
        WUNLOCK

#ifdef USE_DRAMSIM
        RLOCK
        // to signify that the AXI4 DDR Controller is busy enqueueing another transaction in the DRAM
        axi4_mem.ar->setReady(axi4_mem.can_accept_read());
        RUNLOCK

        WLOCK

#ifdef USE_DRAMSIM
        axi4_mem.w->setReady(axi4_mem.can_accept_write());
        axi4_mem.aw->setReady(axi4_mem.can_accept_write());
#else
        axi4_mem.w->setReady(1);
#endif
        WUNLOCK
#else
        enqueue_transaction(*axi4_mem.ar, axi4_mem.read_transactions);
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
        dma_txprogress = 0;
        dma_txlength = int(dma_len >> 6);
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
          if (dma_txprogress < dma_txlength) {
            dma.w->setValid(1);
            memcpy(dma.w->getData(), dma_ptr + 64 * dma_txprogress, 64);
            dma.w->setLast(dma_txprogress + 1 == dma_txlength);
            dma.w->setStrobe(0xFFFFFFFFFFFFFFFFL);
            if (dma.w->getReady()) {
              dma_txprogress++;
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
            char *src_val = dma_ptr + 64 * dma_txprogress;
            for (int i = 0; i < 64; ++i) {
              if (rval[i] != src_val[i]) {
                std::cerr << "Got an unexpected value from the DMA... " << i << " " << dma_txprogress << " " << rval[i]
                          << " " << src_val[i] << std::endl;
                throw std::exception();
              }
            }
            dma_txprogress++;
            if (dma_txprogress == dma_txlength) {
              dma_valid = false;
              dma_in_progress = false;
              pthread_mutex_unlock(&dma_wait_lock);
            }
          }
        }
      }
      pthread_mutex_unlock(&dma_lock);
#endif

    }
    tick(&top);
    tfp->dump(main_time);
    top.clock = 0;// negedge
    tick(&top);
    main_time += fpga_clock_inc;
    tfp->dump(main_time);
  }
#ifdef VERBOSE
  printf("printing traces\n");
#endif
  fflush(stdout);
  tfp->close();
#ifdef USE_DRAMSIM
  for (auto &axi_mem: axi4_mems) {
    axi_mem.mem_sys->PrintStats();
  }
#endif
  sig_handle(0);
}


int main() {
  signal(SIGTERM, sig_handle);
  signal(SIGABRT, sig_handle);
  signal(SIGINT, sig_handle);
  signal(SIGKILL, sig_handle);

  data_server::start();
  cmd_server::start();
#ifdef VERBOSE
  printf("Entering verilator\n");
#endif
  try {
    run_verilator();
    pthread_mutex_lock(&main_lock);
    pthread_mutex_lock(&main_lock);
#ifdef VERBOSE
    printf("Main thread exiting\n");
#endif
  } catch (std::exception &e) {
    sig_handle(1);
  }
  sig_handle(0);
}


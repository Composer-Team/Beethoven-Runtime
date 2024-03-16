#include <iostream>

#include "VComposerTop.h"
#include "cmd_server.h"
#include <csignal>
#include <pthread.h>
#include <queue>
#include <verilated.h>

#include "sim/ddr_macros.h"
#include "sim/front_bus_ctrl_uart.h"
#include "sim/mem_ctrl.h"
#include "sim/verilator.h"
#include "trace/trace_read.h"

#include "util.h"
#include <composer_allocator_declaration.h>


#ifndef DEFAULT_PL_CLOCK
#define FPGA_CLOCK 100
#else
#define FPGA_CLOCK DEFAULT_PL_CLOCK
#endif

uint64_t main_time = 0;

bool active_reset = true;

extern std::queue<composer::rocc_cmd> cmds;
extern std::unordered_map<system_core_pair, std::queue<int> *> in_flight;
mem_ctrl::mem_interface<ComposerMemIDDtype> axi4_mems[NUM_DDR_CHANNELS];

pthread_mutex_t main_lock = PTHREAD_MUTEX_INITIALIZER;

waveTrace *tfp;

void sig_handle(int sig) {
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

void read_program(std::queue<unsigned char> &vec, const std::string &fname) {
  FILE *f = fopen(fname.c_str(), "r");
  char buf[256];
  char c;
  int addr = 0;

  while ((c = getc(f)) != EOF) {
    if (isspace(c)) while (isspace(c=getc(f)));
    if (c != '@') ungetc(c, f);
    switch (c) {
      case '@': {
        fscanf(f, "%s", buf);
        auto naddr = strtol(buf, nullptr, 16);
        while (addr < naddr) {
          vec.push('\0');
          addr++;
        }
        break;
      }
      default:
      {
        auto q = getc(f);
        ungetc(q, f);
        if (q == '@') continue;
        fscanf(f, "%s", buf);
        vec.push(strtol(buf, nullptr, 16));
        addr++;
        break;
      }
    }
  }
}

bool kill_sig = false;


void run_verilator(std::optional<std::string> trace_file, const std::string &dram_config_file) {
#if 500000 % FPGA_CLOCK != 0
  fprintf(stderr, "Provided FPGA clock rate (%d MHz) does not evenly divide 500. This may result in some inaccuracies in precise simulation measurements.", FPGA_CLOCK);
#endif

  mem_ctrl::init(dram_config_file);
  bool use_trace = false;

  std::queue<unsigned char> program, stdout_strm;
  read_program(program, *trace_file);

  // using this to estimate AWS bandwidth
  // KRIA has much slower memory!
  // Config dramsim3config("../DRAMsim3/configs/Kria.ini", "./");
  const float DDR_CLOCK = 1000.0 / dramsim3config->tCK;// NOLINT


  auto fpga_clock_inc = 500000 / FPGA_CLOCK;
  std::cout << "FPGA CLOCK RATE (MHz): " << FPGA_CLOCK << std::endl;
  float ddr_clock_inc = DDR_CLOCK / FPGA_CLOCK;// NOLINT
  printf("%f\n", ddr_clock_inc);
  float ddr_acc = 0;

  // start servers to communicate with user programs
  const char *v[3] = {"", "+verilator+seed+14934534", "+verilator+rand+reset+2"};
  Verilated::commandArgs(3, v);
  VComposerTop top;
  Verilated::traceEverOn(true);
  tfp = new waveTrace;
  top.trace(tfp, 30);
  tfp->open("trace" TRACE_FILE_ENDING);

  LOG(printf("TESTING THIS"));

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
  for (auto &axi4_mem: axi4_mems) {
    axi4_mem.init_dramsim3();
  }

#if NUM_DDR_CHANNELS >= 1
  init_ddr_interface(0)
#if NUM_DDR_CHANNELS >= 2
      init_ddr_interface(1)
#if NUM_DDR_CHANNELS >= 4
          init_ddr_interface(2)
              init_ddr_interface(3)
#endif
#endif
#endif
      // reset circuit
      top.reset = active_reset;
//  top.CHIP_FESEL = 0;
//  top.CHIP_SCEN = 0;
//  top.CHIP_SCLK1 = top.CHIP_SCLK2 = top.CHIP_SHIFTIN = top.CHIP_SHIFTOUT = 0;
//  top.CHIP_UART_M_BAUD_SEL = 0x7;
//  top.CHIP_UART_M_RXD = 1;
//  top.CHIP_UART_M_CTS = 1;
  for (auto &mem: axi4_mems) {
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
  top.reset = !active_reset;
  top.clock = 0;

  LOG(printf("main time %lld\n", main_time));
  unsigned long ms = 1;

  int loops = 0;
  int last = program.size();
  while (program.size() > 0) {
    loops ++;
    if (last != program.size() && (program.size() % 100 == 0)) {
      printf("\r%d %zu %llu", loops, program.size(), main_time);
      fflush(stdout);
      last = program.size();
    }
    top.clock = 1;
    main_time += fpga_clock_inc;
    tick(&top);
    tfp->dump(main_time);

    top.clock = 0;
    main_time += fpga_clock_inc;
    tick(&top);
    queue_uart(program, stdout_strm, top.STDUART_program_uart_rxd, top.STDUART_uart_txd, 1);
    tfp->dump(main_time);

  }

  printf("--program written--\n\n"); fflush(stdout);

  top.reset = active_reset;
  for (int i =0; i < 6; ++i) {
    top.clock = 1;
    main_time += fpga_clock_inc;
    tick(&top);
    tfp->dump(main_time);
    top.clock = 0;
    main_time += fpga_clock_inc;
    tick(&top);
    tfp->dump(main_time);
  }
  top.reset = !active_reset;


  int last_size = stdout_strm.size();

  int count = 1000000;
  while (not kill_sig
  && (--count > 0)
  ) {
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
      printf("killing\n");
      fflush(stdout);
    }
#endif
    top.clock = 1;// posedge
    main_time += fpga_clock_inc;
    {

      // ------------ HANDLE COMMAND INTERFACE ----------------
      queue_uart(program, stdout_strm, top.STDUART_program_uart_rxd, top.STDUART_uart_txd, 1);
      if (last_size != stdout_strm.size()) {
        printf("%c", stdout_strm.back());
        last_size = stdout_strm.size();
        if (stdout_strm.back() == 0x4) {
          // this is the kill signal defined by the arm source (see uart_stdout.h)
          kill_sig = true;

        }
      }
      // approx clock diff
      ddr_acc += ddr_clock_inc;

      while (ddr_acc >= 1) {
        for (auto &axi4_mem: axi4_mems) {
          axi4_mem.mem_sys->ClockTick();
          try_to_enqueue_ddr(axi4_mem);
        }
        ddr_acc -= 1;
      }

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
      }

      // ------------ HANDLE MEMORY INTERFACES ----------------
      for (mem_ctrl::mem_interface<ComposerMemIDDtype> &axi4_mem: axi4_mems) {
        if (axi4_mem.b->getReady() && axi4_mem.b->getValid()) {
          axi4_mem.b->send_ids.pop();
        }

        RLOCK
        if (not axi4_mem.read_transactions.empty()) {
          auto tx = axi4_mem.read_transactions.front();
          int start = (tx->axi_bus_beats_progress * tx->size) % (DATA_BUS_WIDTH / 8);
          char *dest = (char *) axi4_mem.r->getData() + start;
          if (SANITY) {
            memcpy(dest, tx->addr, tx->size);
          }
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
              if (strobe & 1 && SANITY) {
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
              trans->dram_tx_axi_enqueue_progress = 0;
              trans->dram_tx_len_bus_beats = trans->len * trans->size >> 3;
              trans->axi_bus_beats_progress = 1;
              axi4_mem.ddr_write_q.push_back(trans);
            }
          }
        } else {
          axi4_mem.w->setReady(0);
        }
        WUNLOCK

        RLOCK
        // to signify that the AXI4 DDR Controller is busy enqueueing another transaction in the DRAM
        axi4_mem.ar->setReady(axi4_mem.can_accept_read());
        RUNLOCK

        WLOCK

        axi4_mem.w->setReady(axi4_mem.can_accept_write());
        axi4_mem.aw->setReady(axi4_mem.can_accept_write());
        WUNLOCK
      }
    }
    tick(&top);
    tfp->dump(main_time);
    top.clock = 0;// negedge
    tick(&top);
    main_time += fpga_clock_inc;
    tfp->dump(main_time);
  }
  printf("Final stdout print:\n");
  fflush(stdout);
  LOG(printf("printing traces\n"));
  fflush(stdout);
  tfp->close();
  for (auto &axi_mem: axi4_mems) {
    axi_mem.mem_sys->PrintStats();
  }
  sig_handle(0);
}


int main(int argc, char **argv) {
  signal(SIGTERM, sig_handle);
  signal(SIGABRT, sig_handle);
  signal(SIGINT, sig_handle);
  signal(SIGKILL, sig_handle);

  std::optional<std::string> dram_file = {};
  std::optional<std::string> trace_file = {};
  for (int i = 1; i < argc; ++i) {
    assert(argv[i][0] == '-');
    if (strcmp(argv[i] + 1, "dramconfig") == 0) {
      dram_file = std::string(argv[i + 1]);
    } else if (strcmp(argv[i] + 1, "tracefile") == 0) {
      trace_file = std::string(argv[i + 1]);
    }
    ++i;
  }

  if (!dram_file.has_value()) {
    dram_file = std::string("../custom_dram_configs/DDR4_8Gb_x16_3200.ini");
  }
  assert(trace_file.has_value());

  LOG(printf("Entering verilator\n"));
  try {
    run_verilator(trace_file, *dram_file);
    pthread_mutex_lock(&main_lock);
    pthread_mutex_lock(&main_lock);
    LOG(printf("Main thread exiting\n"));
  } catch (std::exception &e) {
    tfp->close();
    throw(e);
  }
  sig_handle(0);
}

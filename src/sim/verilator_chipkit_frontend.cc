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

void push_val(const char buf[4], std::queue<unsigned char> &vec) {
  for (int i = 0; i < 4; ++i) {
    vec.push(buf[i]);
  }
}

void push_val(const uint32_t val, std::queue<unsigned char> &vec) {
  auto buf = (uint8_t *) &val;
  for (int i = 0; i < 4; ++i) {
    vec.push(buf[i]);
  }
}

static void readMemFile2ChipkitDMA(std::queue<unsigned char> &vec, const std::string &fname) {
  FILE *f = fopen(fname.c_str(), "r");
  char buf[256];
  char c;
  int32_t addr = 0;

  char inst_buf[4];
  char inst_idx = 0;
  while ((c = getc(f)) != EOF) {
    if (isspace(c)) while (isspace(c = getc(f)));
    if (c != '@') ungetc(c, f);
    if (c == EOF) break;
    if (c == -1) break;
    switch (c) {
      case '@':
        fscanf(f, "%s", buf);
        addr = strtol(buf, nullptr, 16);
        break;
      default: {
        auto q = getc(f);
        ungetc(q, f);
        if (q == '@') continue;
        fscanf(f, "%s", buf);
        inst_buf[inst_idx] = (char) strtol(buf, nullptr, 16);
        if (++inst_idx == 4) {
          vec.push('w');
          push_val(addr, vec);
          push_val(inst_buf, vec);

          printf("pushing %x %02x %02x %02x %02x\n", addr, (char)inst_buf[0], (char)inst_buf[1], (char)inst_buf[2], (char)inst_buf[3]);
          vec.push(0xa);
          inst_idx = 0;
          addr+=4;
        }
        break;
      }
    }
  }
  assert(inst_idx == 0);
}

bool kill_sig = false;

std::vector<char> memory_array;

size_t smallestpow2over(size_t n) {
  size_t p = 1;
  while (p <= n) {
    p <<= 1;
  }
  return p;
}

static char mem_get(uintptr_t addr) {
  // if array is not big enough size it up to the first power of 2 over addr length
  if (addr >= memory_array.size()) {
    memory_array.resize(smallestpow2over(addr + 1));
  }
  return memory_array[addr];
}

static void mem_set(uintptr_t addr, char val) {
  if (addr >= memory_array.size()) {
    memory_array.resize(smallestpow2over(addr + 1));
  }
  memory_array[addr] = val;
}

void run_verilator(std::optional<std::string> trace_file,
                   const std::string &dram_config_file,
                   std::optional<std::string> dma_file) {
#if 500000 % FPGA_CLOCK != 0
  fprintf(stderr, "Provided FPGA clock rate (%d MHz) does not evenly divide 500. This may result in some inaccuracies in precise simulation measurements.", FPGA_CLOCK);
#endif

  mem_ctrl::init(dram_config_file);
  std::queue<unsigned char> program, stdout_strm;
  readMemFile2ChipkitDMA(program, *trace_file);
  const float DDR_CLOCK = 1000.0 / dramsim3config->tCK;// NOLINT
  auto fpga_clock_inc = 500000 / FPGA_CLOCK;
  float ddr_clock_inc = DDR_CLOCK / FPGA_CLOCK;// NOLINT
  float ddr_acc = 0;

  // 0 is the slowest, 14 is the fastest. For whatever reason, 15 isn't working
  set_baud(14);

  // start servers to communicate with user programs
  const char *v[1] = {""};
  Verilated::commandArgs(0, v);
  VComposerTop top;
  Verilated::traceEverOn(true);
  tfp = new waveTrace;
  top.trace(tfp, 30);
  tfp->open("trace" TRACE_FILE_ENDING);
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
  axi4_mems[0].w->setData((char *) &top.M00_AXI_wdata);
  axi4_mems[0].r->setData((char *) &top.M00_AXI_rdata);
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
  top.CHIP_UART_M_RXD = 1;
  top.CHIP_UART_M_CTS = 0; // clear to send
  // DO NOT MESS WITH THIS
  top.CHIP_UART_M_BAUD_SEL = get_baud_sel();
  top.CHIP_FESEL = 0; // uart
  // disable scan
  top.CHIP_SCEN = 0;
  top.CHIP_SCLK1 = top.CHIP_SCLK2 = top.CHIP_SHIFTIN = top.CHIP_SHIFTOUT = 0;

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


  printf("initial reset\n");
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
  int loops = 1000;

  printf("steady1\n");
  while (loops > 0) {
    loops--;
    top.clock = 1;
    main_time += fpga_clock_inc;
    tick(&top);
    tfp->dump(main_time);

    top.clock = 0;
    main_time += fpga_clock_inc;
    tick(&top);
    tfp->dump(main_time);
  }

  if (dma_file.has_value()) {
    printf("memory contents\n");
    std::queue<unsigned char> dma_program;
    readMemFile2ChipkitDMA(dma_program, *dma_file);
    top.CHIP_ASPSEL = 0;
    auto dma_last = dma_program.size();
    while (!dma_program.empty() || top.CHIP_UART_M_RXD == 0) {
      if (dma_last != dma_program.size() && (dma_program.size() % 100 == 0)) {
        printf("\r%d %zu %llu", loops, program.size(), main_time);
        fflush(stdout);
        dma_last = dma_program.size();
      }

      top.clock = 1;
      main_time += fpga_clock_inc;
      tick(&top);
      tfp->dump(main_time);
      top.clock = 0;
      main_time += fpga_clock_inc;
      tick(&top);
      tfp->dump(main_time);
      queue_uart(dma_program, stdout_strm, top.CHIP_UART_M_RXD, top.STDUART_uart_txd);

      {
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
            auto tx = std::make_shared<mem_ctrl::memory_transaction>(uintptr_t(ad), txsize, txlen, 0, false,
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
              for (int i = 0; i < tx->size; ++i) {
                dest[i] = mem_get(tx->addr + i);
              }
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
                  printf("writing addr(%x) dat(%x)\n", addr + off, src[off]);
                  mem_set(addr + off, src[off]);
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

    }
  }

  top.CHIP_ASPSEL = 1;
  printf("loading program\n\n");
  auto last = program.size();
  while (!program.empty() || top.CHIP_UART_M_RXD == 0) {
    loops++;
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
    queue_uart(program, stdout_strm, top.CHIP_UART_M_RXD, top.STDUART_uart_txd);
    tfp->dump(main_time);
  }
  printf("--program written--\n\n");
  fflush(stdout);

  loops = 1000;
  printf("steady2\n");
  while (loops > 0) {
    loops--;
    top.clock = 1;
    main_time += fpga_clock_inc;
    tick(&top);
    tfp->dump(main_time);

    top.clock = 0;
    main_time += fpga_clock_inc;
    tick(&top);
    tfp->dump(main_time);
  }

  printf("reset2\n");

  top.reset = active_reset;
  for (int i = 0; i < 6; ++i) {
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

  printf("run program\n");
  int last_size = stdout_strm.size();

  int count = 10000;
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
      // ------------ HANDLE COMMAND INTERFACE ----------------
      assert(program.empty());
      queue_uart(program, stdout_strm, top.CHIP_UART_M_RXD, top.STDUART_uart_txd);
      queue_uart(program, stdout_strm, top.STDUART_uart_rxd, top.STDUART_uart_txd, 1);
      if (last_size != stdout_strm.size()) {
        printf("%c", stdout_strm.back());
        last_size = stdout_strm.size();
        if (stdout_strm.back() == 0x4) {
          // this is the kill signal defined by the arm source (see uart_stdout.h)
          kill_sig = true;
        }
      }
    {
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
          auto tx = std::make_shared<mem_ctrl::memory_transaction>(uintptr_t(ad), txsize, txlen, 0, false,
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
            for (int i = 0; i < tx->size; ++i) {
              dest[i] = mem_get(tx->addr + i);
            }
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
                printf("writing addr(%x) dat(%x)\n", addr + off, src[off]);
                mem_set(addr + off, src[off]);
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
  std::optional<std::string> dma_file = {};
  for (int i = 1; i < argc; ++i) {
    assert(argv[i][0] == '-');
    if (strcmp(argv[i] + 1, "dramconfig") == 0) {
      dram_file = std::string(argv[i + 1]);
    } else if (strcmp(argv[i] + 1, "tracefile") == 0) {
      trace_file = std::string(argv[i + 1]);
    } else if (strcmp(argv[i] + 1, "dmafile") == 0) {
      dma_file = std::string(argv[i + 1]);
    }
    ++i;
  }

  if (!dram_file.has_value()) {
    dram_file = std::string("../custom_dram_configs/DDR4_8Gb_x16_3200.ini");
  }
  assert(trace_file.has_value());

  LOG(printf("Entering verilator\n"));
  try {
    run_verilator(trace_file, *dram_file, dma_file);
    pthread_mutex_lock(&main_lock);
    pthread_mutex_lock(&main_lock);
    LOG(printf("Main thread exiting\n"));
  } catch (std::exception &e) {
    tfp->close();
    throw (e);
  }
  sig_handle(0);
}

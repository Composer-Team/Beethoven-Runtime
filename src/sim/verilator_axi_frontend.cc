#include <iostream>

#include "BeethovenTop.h"
#include "cmd_server.h"
#include "data_server.h"
#include <csignal>
#include <pthread.h>
#include <queue>
#include <verilated.h>

#include "sim/ddr_macros.h"
#include "sim/front_bus_ctrl_axi.h"
#include "sim/mem_ctrl.h"
#include "sim/verilator.h"
#include "trace/trace_read.h"

#include <beethoven_allocator_declaration.h>
#include "util.h"



#ifndef DEFAULT_PL_CLOCK
#define FPGA_CLOCK 100
#else
#define FPGA_CLOCK DEFAULT_PL_CLOCK
#endif

uint64_t main_time = 0;

bool active_reset = true;

extern std::queue<beethoven::rocc_cmd> cmds;
extern std::unordered_map<system_core_pair, std::queue<int> *> in_flight;
mem_ctrl::mem_interface<BeethovenMemIDDtype> axi4_mems[NUM_DDR_CHANNELS];

pthread_mutex_t main_lock = PTHREAD_MUTEX_INITIALIZER;
bool kill_sig = false;

waveTrace *tfp;

void sig_handle(int sig) {
  tfp->close();
  fprintf(stderr, "FST written!\n");
  fflush(stderr);
  exit(sig);
}

void tick(BeethovenTop *top) {
  try {
    top->eval();
  } catch (std::exception &e) {
    tfp->dump(main_time);
    tfp->close();
    std::cerr << "Emergency dump!" << std::endl;
    throw e;
  }
}

//
// Created by Christopher Kjellqvist on 2/2/24.
//

#include "trace/trace_read.h"
#include "sim/verilator.h"

Trace *trace = nullptr;

void init_trace(const std::string &fname) {
  FILE *f = fopen(fname.c_str(), "r");
  // read f line by line
  if (f == nullptr) throw std::runtime_error("Could not open file");

  trace = new Trace {};

  char * line = nullptr;
  size_t huh;
  int nchar = getline(&line, &huh, f);
  // valid operations
  // read <address> <comparison_value>
  //    read should return true if you want the trace to continue
  // write <address> <value>
  uint32_t payloads[4];
  int payload_id;
  std::optional<TraceType> mode;
  while (nchar != -1) {
    line = strtok(line, " ");
    payload_id = 0;
    mode = {};
    bool comment = false;
    while (line && !comment) {
      if (strcmp(line, "read") == 0) {
        mode = ReadConditionType;
      } else if (strcmp(line, "write") == 0) {
        mode = WriteType;
      } else if (strcmp(line, "#") == 0) {
        comment = true;
      } else {
        payloads[payload_id] = std::strtol(line, nullptr, 0);
        payload_id++;
      }
      line = strtok(nullptr, " ");
    }
    if (mode.has_value()) {
      assert(payload_id == 2);
      trace->push(TraceUnit(*mode, payloads[0], payloads[1]));
    }
    nchar = getline(&line, &huh, f);
  }
  return;
}

enum TraceMachineState {
  IdleState,
  ReadAddressState,
  ReadState,
  WriteAddressState,
  WriteState,
  WriteResponse
};

static TraceMachineState state = IdleState;

int time_in_idle = 0;

void trace_rising_edge_pre(BeethovenTop &top) {
  if (top.reset == active_reset) return;
  switch (state) {
    case IdleState:
      time_in_idle++;
      if (time_in_idle == 100) {
        if (trace->empty()) {
          sig_handle(0);
        } else {
          if (trace->front().ty == ReadConditionType)
            state = ReadAddressState;
          else
            state = WriteAddressState;
        }
      }
      break;
    case ReadAddressState:
      if (top.S00_AXI_arready && top.S00_AXI_arvalid) {
        state = ReadState;
      }
      break;
    case ReadState:
      if (top.S00_AXI_rready && top.S00_AXI_rvalid) {
        state = IdleState;
        time_in_idle = 0;
        if (trace->front().payload == top.S00_AXI_rdata) {
          trace->pop();
        }
      }
      break;
    case WriteAddressState:
      if (top.S00_AXI_awready && top.S00_AXI_awvalid) {
        state = WriteState;
      }
      break;
    case WriteState:
      if (top.S00_AXI_wready && top.S00_AXI_wvalid) {
        state = WriteResponse;
      }
      break;
    case WriteResponse:
      if (top.S00_AXI_bvalid && top.S00_AXI_bready) {
        trace->pop();
        state = IdleState;
        time_in_idle = 0;
      }
      break;
  }
}

void trace_rising_edge_post(BeethovenTop &top) {
  if (top.reset == active_reset) return;
  top.S00_AXI_bready = false;
  top.S00_AXI_awvalid = false;
  top.S00_AXI_arvalid = false;
  top.S00_AXI_wvalid = false;
  top.S00_AXI_rready = false;
  switch (state) {
    case IdleState:
      break;
    case WriteAddressState:
      top.S00_AXI_awvalid = true;
      top.S00_AXI_awaddr = trace->front().address;
      break;
    case WriteState:
      top.S00_AXI_wdata = trace->front().payload;
      top.S00_AXI_wstrb = 0xF;
      top.S00_AXI_wvalid = true;
      break;
    case ReadAddressState:
      top.S00_AXI_arvalid = true;
      top.S00_AXI_araddr = trace->front().address;
      break;
    case ReadState:
      top.S00_AXI_rready = true;
      break;
    case WriteResponse:
      top.S00_AXI_bready = true;
      break;
  }
}

TraceUnit::TraceUnit(TraceType ty, uint64_t address, uint32_t payload) : ty(ty), address(address), payload(payload) {}

void print_state(uint64_t mem, uint64_t time) {
  std::string time_string;
  // in seconds
  double time_d = (double)time / 1e12;
  if (time < 1000 * 1000 * 1000) {
    time_string = std::to_string(double(time) / 1000 / 1000) + "µs";
  } else {
    time_string = std::to_string(double(time) / 1000 / 1000 / 1000) + "ms";
  }

  std::string mem_unit;
  double mem_norm;
  if (mem < 1024) {
    mem_unit = "B";
    mem_norm = (double)mem;
  } else if (mem < 1024 * 1024) {
    mem_unit = "KB";
    mem_norm = (double)mem / 1024;
  } else if (mem < 1024 * 1024 * 1024) {
    mem_unit = "MB";
    mem_norm = (double)mem / 1024 / 1024;
  } else {
    mem_unit = "GB";
    mem_norm = (double)mem / 1024 / 1024 / 1024;
  }

  std::string mem_rate;
  // compute bytes per second and normalize
  double mem_rate_norm = (double)mem / time_d;
  if (mem_rate_norm < 1024) {
    mem_rate = std::to_string(mem_rate_norm) + "B/s";
  } else if (mem_rate_norm < 1024 * 1024) {
    mem_rate = std::to_string(mem_rate_norm / 1024) + "KB/s";
  } else if (mem_rate_norm < 1024 * 1024 * 1024) {
    mem_rate = std::to_string(mem_rate_norm / 1024 / 1024) + "MB/s";
  } else {
    mem_rate = std::to_string(mem_rate_norm / 1024 / 1024 / 1024) + "GB/s";
  }

  std::cout << "\rTime: " << time_string << " | Memory: " << mem_norm << mem_unit << " | Rate: " << mem_rate << " | ";
}

void run_verilator(std::optional<std::string> trace_file, const std::string &dram_config_file) {
#if 500000 % FPGA_CLOCK != 0
  fprintf(stderr, "Provided FPGA clock rate (%d MHz) does not evenly divide 500. This may result in some inaccuracies in precise simulation measurements.", FPGA_CLOCK);
#endif

  mem_ctrl::init(dram_config_file);
  bool use_trace = false;
  if (trace_file.has_value()) {
    init_trace(*trace_file);
    use_trace = true;
  }
  // using this to estimate AWS bandwidth
  // KRIA has much slower memory!
  // Config dramsim3config("../DRAMsim3/configs/Kria.ini", "./");
  const float DDR_CLOCK = 1000.0 / dramsim3config->tCK;// NOLINT


  auto fpga_clock_inc = 500000 / FPGA_CLOCK;
  std::cout << "FPGA CLOCK RATE (MHz): " << FPGA_CLOCK << std::endl;
  float ddr_clock_inc = DDR_CLOCK / FPGA_CLOCK;// NOLINT
  printf("%f\n", ddr_clock_inc);
  float ddr_acc = 0;
  uint64_t memory_transacted = 0;
  uint64_t cycle_count = 0;

  // start servers to communicate with user programs
//  const char *v[3] = {"", "+verilator+seed+14934534", "+verilator+rand+reset+2"};
//  const int cv = 3;
const char *v[1] = {""};
const int cv = 1;
  Verilated::commandArgs(cv, v);
  BeethovenTop top;
  Verilated::traceEverOn(true);
  tfp = new waveTrace;
  top.trace(tfp, 30);
  tfp->open("trace" TRACE_FILE_ENDING );

  std::cout << "Tracing!" << std::endl;

  for (int i = 0; i < NUM_DDR_CHANNELS; ++i) {
    axi4_mems[i].id = i;
  }

#if defined(BEETHOVEN_HAS_DMA)

  mem_ctrl::mem_interface<BeethovenDMAIDtype> dma;
  int dma_txprogress = 0;
  int dma_txlength = 0;
  dma.aw = new mem_ctrl::v_address_channel<BeethovenDMAIDtype>(top.dma_awready, top.dma_awvalid, top.dma_awid,
                                                              top.dma_awsize, top.dma_awburst,
                                                              top.dma_awaddr, top.dma_awlen);
  dma.ar = new mem_ctrl::v_address_channel<BeethovenDMAIDtype>(top.dma_arready, top.dma_arvalid, top.dma_arid,
                                                              top.dma_arsize, top.dma_arburst,
                                                              top.dma_araddr, top.dma_arlen);
  dma.w = new mem_ctrl::data_channel<BeethovenDMAIDtype>(top.dma_wready, top.dma_wvalid,
                                                        &top.dma_wstrb, top.dma_wlast, nullptr);
  dma.r = new mem_ctrl::data_channel<BeethovenDMAIDtype>(top.dma_rready, top.dma_rvalid,
                                                        nullptr, top.dma_rlast, &top.dma_rid);
  dma.w->setData((char *) top.dma_rdata.m_storage);
  dma.r->setData((char *) top.dma_wdata.m_storage);
  dma.b = new mem_ctrl::response_channel<BeethovenDMAIDtype>(top.dma_bready, top.dma_bvalid, top.dma_bid);
#endif
  for (auto &axi4_mem: axi4_mems) {
    axi4_mem.init_dramsim3();
  }

#if NUM_DDR_CHANNELS >= 1
  init_ddr_interface(0)
#if DATA_BUS_WIDTH <= 64
  axi4_mems[0].r->setData((char *) &top.M00_AXI_rdata);
  axi4_mems[0].w->setData((char *) &top.M00_AXI_wdata);
#else
  axi4_mems[0].r->setData((char *) &top.M00_AXI_rdata.at(0));
  axi4_mems[0].w->setData((char *) &top.M00_AXI_wdata.at(0));
#endif
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
  for (auto &mem: axi4_mems) {
    mem.r->setValid(0);
    mem.b->setValid(0);
    mem.aw->setReady(1);
  }
#ifdef BEETHOVEN_HAS_DMA
  dma.b->setReady(0);
  dma.ar->setValid(0);
  dma.aw->setValid(0);
  dma.w->setValid(0);
  dma.r->setReady(0);
#endif

  top.S00_AXI_awvalid = top.S00_AXI_wvalid = top.S00_AXI_rready = top.S00_AXI_arvalid = top.S00_AXI_bready = 0;

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
  while (not kill_sig) {
    // clock is high after posedge - changes now are taking place after posedge,
    // and will take effect on negedge

#ifdef KILL_SIM
    if (ms >= KILL_SIM) {
      kill_sig = true;
      printf("killing\n"); fflush(stdout);
    }
#endif
    top.clock = 1;// posedge
    main_time += fpga_clock_inc;
    cycle_count++;
    if ((cycle_count & 1024) == 0) {
      print_state(memory_transacted, main_time);
      fflush(stdout);
    }
    {

      // ------------ HANDLE COMMAND INTERFACE ----------------
      // start queueing up a new command if one is available

      if (!use_trace) {
        top.S00_AXI_awvalid = top.S00_AXI_wvalid = top.S00_AXI_rready = top.S00_AXI_arvalid = top.S00_AXI_bready = 0;
        update_command_state(top);
        update_resp_state(top);
        update_update_state(top);
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
	  memory_transacted += (DATA_BUS_WIDTH >> 3);
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
          auto txlen = (int)(axi4_mem.ar->getLen()) + 1;
          auto tx = std::make_shared<mem_ctrl::memory_transaction>((uintptr_t) ad, txsize, txlen, 0, false,
                                                                   axi4_mem.ar->getId(), addr, false);
          // 64b per DRAM transaction
          RLOCK
          axi4_mem.ddr_read_q.push_back(tx);
          RUNLOCK
        }
      }


      // ------------ HANDLE MEMORY INTERFACES ----------------
      for (mem_ctrl::mem_interface<BeethovenMemIDDtype> &axi4_mem: axi4_mems) {
        if (axi4_mem.b->getReady() && axi4_mem.b->getValid()) {
          axi4_mem.b->send_ids.pop();
          axi4_mem.num_in_flight_writes--;
        }

        RLOCK
        if (not axi4_mem.read_transactions.empty()) {
          auto tx = axi4_mem.read_transactions.front();
          int start = (tx->axi_bus_beats_progress * tx->size) % (DATA_BUS_WIDTH / 8);
          char *dest = (char *) axi4_mem.r->getData() + start;
          memcpy(dest, reinterpret_cast<char *>(tx->addr), tx->size);
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

        if (axi4_mem.aw->getValid() && axi4_mem.aw->getReady()) {
          try {
#ifdef BAREMETAL_RUNTIME
            uintptr_t addr = axi4_mem.aw->getAddr();
#else
            char *addr = (char *) at.translate(axi4_mem.aw->getAddr());
#endif
            int sz = 1 << axi4_mem.aw->getSize();
            int len = 1 + int(axi4_mem.aw->getLen());// per axi
            bool is_fixed = axi4_mem.aw->getBurst() == 0;
            int id = axi4_mem.aw->getId();
            uint64_t fpga_addr = axi4_mem.aw->getAddr();
            auto tx = std::make_shared<mem_ctrl::memory_transaction>(uintptr_t(addr), sz, len, 0, is_fixed, id, fpga_addr, false);
            axi4_mem.write_transactions.push(tx);
            axi4_mem.num_in_flight_writes++;
          } catch (std::exception &e) {
            tfp->dump(main_time);
            tfp->close();
            throw e;
          }
        }

//        mem_ctrl::enqueue_transaction(*axi4_mem.aw, axi4_mem.write_transactions);

        if (not axi4_mem.write_transactions.empty()) {
          if (axi4_mem.w->getValid() && axi4_mem.w->getReady()) {
            auto trans = axi4_mem.write_transactions.front();
            // refer to https://developer.arm.com/documentation/ihi0022/e/AMBA-AXI3-and-AXI4-Protocol-Specification/Single-Interface-Requirements/Transaction-structure/Data-read-and-write-structure?lang=en#CIHIJFAF
            char *src = axi4_mem.w->getData();
//            auto strobe = axi4_mem.w->getStrobe();
            uint32_t off = 0;
            // for writes, we need to account for alignment and strobe,so we're re-aligning address here
            // align to 64B - zero out bottom 6b
            auto addr = trans->addr;
	    /*
            while (strobe != 0) {
              if (strobe & 1) {
                reinterpret_cast<char *>(addr)[off] = src[off];
                memory_transacted++;
              }
              off += 1;
              strobe >>= 1;
            }
	    */
            trans->axi_bus_beats_progress++;

            if (not trans->fixed) {
              trans->addr += trans->size;
            }

            if (axi4_mem.w->getLast()) {
              axi4_mem.write_transactions.pop();
              trans->dram_tx_axi_enqueue_progress = 0;
              trans->dram_tx_n_enqueues = std::max((trans->len * (trans->size >> 3))/DDR_ENQUEUE_SIZE_BYTES, 1);
              trans->axi_bus_beats_progress = 1;
              axi4_mem.ddr_write_q.push_back(trans);
              axi4_mem.w->setReady(!axi4_mem.write_transactions.empty() || axi4_mem.num_in_flight_writes < axi4_mem.max_in_flight_writes);
            } else {
              axi4_mem.w->setReady(1);
            }
          }
        } else {
          axi4_mem.w->setReady(axi4_mem.num_in_flight_writes < axi4_mem.max_in_flight_writes);
        }
        WUNLOCK

        RLOCK
        // to signify that the AXI4 DDR Controller is busy enqueueing another transaction in the DRAM
        axi4_mem.ar->setReady(axi4_mem.can_accept_read());
        RUNLOCK

        WLOCK

        axi4_mem.aw->setReady(axi4_mem.num_in_flight_writes < axi4_mem.max_in_flight_writes);
        WUNLOCK
      }

#ifdef BEETHOVEN_HAS_DMA
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
//            dma.w->setStrobe(0xFFFFFFFFFFFFFFFFL);
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
    if (use_trace) {
      if (main_time > fpga_clock_inc * 200)
        trace_rising_edge_pre(top);
    }
    tick(&top);
    if (use_trace) {
      if (main_time > fpga_clock_inc * 200)
        trace_rising_edge_post(top);
    }
    tfp->dump(main_time);
    top.clock = 0;// negedge
    tick(&top);
    main_time += fpga_clock_inc;
    tfp->dump(main_time);
  }
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
      std::cerr << "dramconfig is " << *dram_file << std::endl;
    } else if (strcmp(argv[i] + 1, "tracefile") == 0) {
      trace_file = std::string(argv[i + 1]);
    }
    ++i;
  }

  if (!dram_file.has_value()) {
    dram_file = std::string("../custom_dram_configs/DDR4_8Gb_x16_3200.ini");
  }

  data_server::start();
  cmd_server::start();
  LOG(printf("Entering verilator\n"));
  try {
    run_verilator(trace_file, *dram_file);
    pthread_mutex_lock(&main_lock);
    pthread_mutex_lock(&main_lock);
    LOG(printf("Main thread exiting\n"));
  } catch (std::exception &e) {
    tfp->close();
    throw (e);
  }
  sig_handle(0);
}

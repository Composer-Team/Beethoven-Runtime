#include <iostream>

#include "BeethovenTop.h"
#include "cmd_server.h"
#include "data_server.h"
#include <csignal>
#include <chrono>
#include <pthread.h>
#include <queue>
#include <verilated.h>

#include "sim/ddr_macros.h"
#include "sim/axi/front_bus_ctrl_axi.h"
#include "sim/mem_ctrl.h"
#include "sim/verilator.h"
#include "sim/tick.h"
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


BeethovenTop top;

pthread_mutex_t main_lock = PTHREAD_MUTEX_INITIALIZER;
bool kill_sig = false;

waveTrace *tfp;


void sig_handle(int sig) {
#if NUM_DDR_CHANNELS >= 1
  for (auto &q: axi4_mems) {
    q.mem_sys->PrintStats();
  }
#endif
#ifdef VERILATOR
  tfp->close();
#endif
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
#include "sim/axi/state_machine.h"

/*
void init_trace(const std::string &fname) {
  FILE *f = fopen(fname.c_str(), "r");
  // read f line by line
  if (f == nullptr) throw std::runtime_error("Could not open file");

  trace = new Trace{};

  char *line = nullptr;
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
*/

TraceUnit::TraceUnit(TraceType ty, uint64_t address, uint32_t payload) : ty(ty), address(address), payload(payload) {}

#if NUM_DDR_CHANNELS >= 1
extern int writes_emitted;
extern int reads_emitted;
uint8_t dummy;
#endif

void print_state(uint64_t mem, uint64_t time, uint64_t time_since) {
  std::string time_string;
  // in seconds
  if (time < 1000 * 1000 * 1000) {
    time_string = std::to_string(double(time) / 1000 / 1000) + "µs";
  } else {
    time_string = std::to_string(double(time) / 1000 / 1000 / 1000) + "ms";
  }

  std::string mem_unit;
  double mem_norm;
  if (mem < 1024) {
    mem_unit = "B";
    mem_norm = (double) mem;
  } else if (mem < 1024 * 1024) {
    mem_unit = "KB";
    mem_norm = (double) mem / 1024;
  } else if (mem < 1024 * 1024 * 1024) {
    mem_unit = "MB";
    mem_norm = (double) mem / 1024 / 1024;
  } else {
    mem_unit = "GB";
    mem_norm = (double) mem / 1024 / 1024 / 1024;
  }

#if NUM_DDR_CHANNELS >= 1
  std::string mem_rate;
  // compute bytes per second and normalize
  double time_d = (double) time_since / 1e12;
  double mem_rate_norm = (double) mem / time_d;
  if (mem_rate_norm < 1024) {
    mem_rate = std::to_string(mem_rate_norm) + "B/s";
  } else if (mem_rate_norm < 1024 * 1024) {
    mem_rate = std::to_string(mem_rate_norm / 1024) + "KB/s";
  } else if (mem_rate_norm < 1024 * 1024 * 1024) {
    mem_rate = std::to_string(mem_rate_norm / 1024 / 1024) + "MB/s";
  } else {
    mem_rate = std::to_string(mem_rate_norm / 1024 / 1024 / 1024) + "GB/s";
  }

  std::cout << "\rTime: " << time_string << " | Memory: " << mem_norm << mem_unit << " | Rate: " << mem_rate << " | w("
            << writes_emitted << ") r(" << reads_emitted << ")";
#else
  std::cout << "\rTime: " << time_string;
#endif
}

uint64_t time_last_command = 0;
uint64_t memory_transacted = 0;
bool use_trace = false;
float ddr_clock_inc;

void run_verilator(std::optional<std::string> trace_file, const std::string &dram_config_file) {
#if 500000 % FPGA_CLOCK != 0
  fprintf(stderr, "Provided FPGA clock rate (%d MHz) does not evenly divide 500. This may result in some inaccuracies in precise simulation measurements.", FPGA_CLOCK);
#endif
  /*
  if (trace_file.has_value()) {
    init_trace(*trace_file);
    use_trace = true;
  }
  */

  auto fpga_clock_inc = 500000 / FPGA_CLOCK;

#if NUM_DDR_CHANNELS >= 1
  mem_ctrl::init(dram_config_file);
  const float DDR_CLOCK = 1000.0 / dramsim3config->tCK;// NOLINT
  std::cout << "FPGA CLOCK RATE (MHz): " << FPGA_CLOCK << std::endl;
  ddr_clock_inc = DDR_CLOCK / FPGA_CLOCK;// NOLINT
  printf("%f\n", ddr_clock_inc);

#endif
  // using this to estimate AWS bandwidth
  // KRIA has much slower memory!
  // Config dramsim3config("../DRAMsim3/configs/Kria.ini", "./");


  uint64_t cycle_count = 0;

  const char *v[1] = {""};
  const int cv = 1;
  Verilated::commandArgs(cv, v);
  Verilated::traceEverOn(true);
  tfp = new waveTrace;
  top.trace(tfp, 30);
  tfp->open("trace" TRACE_FILE_ENDING);

  std::cout << "Tracing!" << std::endl;
  top.reset = active_reset;

#if NUM_DDR_CHANNELS >= 1
  for (int i = 0; i < NUM_DDR_CHANNELS; ++i) {
    axi4_mems[i].id = i;
  }

#if defined(BEETHOVEN_HAS_DMA)

  dma_intf_t dma;
  int dma_txprogress = 0;
  int dma_txlength = 0;
  dma.aw.init(top.dma_awready, top.dma_awvalid, top.dma_awid, top.dma_awsize, top.dma_awburst, top.dma_awaddr, top.dma_awlen);
  dma.ar.init(top.dma_arready, top.dma_arvalid, top.dma_arid, top.dma_arsize, top.dma_arburst, top.dma_araddr, top.dma_arlen);
  dma.w.init(top.dma_wready, top.dma_wvalid, top.dma_wlast, nullptr, &top.dma_wstrb);
  dma.r.init(top.dma_rready, top.dma_rvalid, top.dma_rlast, &top.dma_rlast, nullptr);
  dma.b.init(top.dma_bready, top.dma_bvalid, &top.dma_bid);
#endif
  for (auto &axi4_mem: axi4_mems) {
    axi4_mem.init_dramsim3();
  }
/**
 * This absolutely sucks to do it this way, but I can't think of a better
 * way to do it that doesn't invoke even uglier and less flexible C macros
 */

  axi4_mems[0].ar.init(GetSetWrapper(top.M00_AXI_arready),
      GetSetWrapper(top.M00_AXI_arvalid),
      GetSetWrapper(top.M00_AXI_arid),
      GetSetWrapper(top.M00_AXI_arsize),
      GetSetWrapper(top.M00_AXI_arburst),
      GetSetWrapper(top.M00_AXI_araddr),
      GetSetWrapper(top.M00_AXI_arlen));
  axi4_mems[0].aw.init(GetSetWrapper(top.M00_AXI_awready),
      GetSetWrapper(top.M00_AXI_awvalid),
      GetSetWrapper(top.M00_AXI_awid),
      GetSetWrapper(top.M00_AXI_awsize),
      GetSetWrapper(top.M00_AXI_awburst),
      GetSetWrapper(top.M00_AXI_awaddr),
      GetSetWrapper(top.M00_AXI_awlen));
  axi4_mems[0].w.init(GetSetWrapper(top.M00_AXI_wready),
      GetSetWrapper(top.M00_AXI_wvalid),
      GetSetWrapper(top.M00_AXI_wlast),
      GetSetWrapper(dummy),
      GetSetWrapper(top.M00_AXI_wstrb),
      GetSetDataWrapper<uint8_t, DATA_BUS_WIDTH / 8>(&top.M00_AXI_wdata.at(0)));
  axi4_mems[0].r.init(GetSetWrapper(top.M00_AXI_rready),
      GetSetWrapper(top.M00_AXI_rvalid),
      GetSetWrapper(top.M00_AXI_rlast),
      GetSetWrapper(top.M00_AXI_rid),
      GetSetWrapper(dummy),
      GetSetDataWrapper<uint8_t, DATA_BUS_WIDTH / 8> (&top.M00_AXI_rdata.at(0)));
  axi4_mems[0].b.init(GetSetWrapper(top.M00_AXI_bready),
      GetSetWrapper(top.M00_AXI_bvalid),
      GetSetWrapper(top.M00_AXI_bid));
#if NUM_DDR_CHANNELS >= 2
#error "Can't do 2 yet"
#if NUM_DDR_CHANNELS >= 4
#error "Can't do other
#endif
#endif
  // reset circuit
  for (auto &mem: axi4_mems) {
    mem.r.setValid(0);
    mem.b.setValid(0);
    mem.aw.setReady(1);
  }
#ifdef BEETHOVEN_HAS_DMA
  dma.b.setReady(0);
  dma.ar.setValid(0);
  dma.aw.setValid(0);
  dma.w.setValid(0);
  dma.r.setReady(0);
#endif
#endif


  top.S00_AXI_awvalid = top.S00_AXI_wvalid = top.S00_AXI_rready = top.S00_AXI_arvalid = top.S00_AXI_bready = 0;
  auto time_last_print = std::chrono::high_resolution_clock::now();
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

  auto ctrl = new AXIControlIntf<GetSetWrapper<uint8_t>, GetSetWrapper<BeethovenFrontBusAddr_t>, GetSetWrapper<uint32_t>>();
  ctrl->set_aw(
      GetSetWrapper(top.S00_AXI_awvalid),
      GetSetWrapper(top.S00_AXI_awready),
      GetSetWrapper(top.S00_AXI_awaddr));
  ctrl->set_ar(
      GetSetWrapper(top.S00_AXI_arvalid),
      GetSetWrapper(top.S00_AXI_arready),
      GetSetWrapper(top.S00_AXI_araddr));
  ctrl->set_w(
      GetSetWrapper(top.S00_AXI_wvalid),
      GetSetWrapper(top.S00_AXI_wready),
      GetSetWrapper(top.S00_AXI_wdata));
  ctrl->set_r(
      GetSetWrapper(top.S00_AXI_rvalid),
      GetSetWrapper(top.S00_AXI_rready),
      GetSetWrapper(top.S00_AXI_rdata));
  ctrl->set_b(
      GetSetWrapper(top.S00_AXI_bready),
      GetSetWrapper(top.S00_AXI_bvalid));

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

    if ((cycle_count & 1024) == 0 && std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - time_last_print).count() > 500) {
      time_last_print = std::chrono::high_resolution_clock::now();
      print_state(memory_transacted, main_time, main_time - time_last_command);
      fflush(stdout);
    }
    tick_signals(ctrl);
//    if (use_trace) {
//      if (main_time > fpga_clock_inc * 200)
//        trace_rising_edge_pre(top);
//    }
    tick(&top);
//    if (use_trace) {
//      if (main_time > fpga_clock_inc * 200)
//        trace_rising_edge_post(top);
//    }
    tfp->dump(main_time);
    top.clock = 0;// negedge
    tick(&top);
    main_time += fpga_clock_inc;
    tfp->dump(main_time);
  }
  LOG(printf("printing traces\n"));
  fflush(stdout);
  tfp->close();
#if NUM_DDR_CHANNELS >= 1
  for (auto &axi_mem: axi4_mems) {
    axi_mem.mem_sys->PrintStats();
  }
#endif
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

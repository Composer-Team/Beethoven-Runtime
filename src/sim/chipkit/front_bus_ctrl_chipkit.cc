//
// Created by Chris Kjellqvist on 8/9/23.
//

#include "beethoven_allocator_declaration.h"
#include "sim/axi/front_bus_ctrl_axi.h"
#include "sim/mem_ctrl.h"
#include <verilated_fst_c.h>

extern mem_ctrl::mem_interface<BeethovenMemIDDtype> axi4_mems[NUM_DDR_CHANNELS];
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


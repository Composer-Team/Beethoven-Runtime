//
// Created by Christopher Kjellqvist on 8/6/24.
//

#include "vcs_vpi_user.h"
#include "sim/mem_ctrl.h"
#include "sim/axi/state_machine.h"
#include "sim/tick.h"
#include "sim/axi/vcs_handle.h"
#include "cmd_server.h"
#include "data_server.h"
#include <pthread.h>

#include "beethoven_allocator_declaration.h"

#include <cstring>
#include <vector>

std::vector<vpiHandle> inputs, outputs;
AXIControlIntf<VCSShortHandle, VCSShortHandle, VCSShortHandle> ctrl;
uint64_t memory_transacted = 0;
uint64_t main_time = 0;
pthread_mutex_t main_lock = PTHREAD_MUTEX_INITIALIZER;
float ddr_clock_inc;
bool kill_sig;
extern "C" PLI_INT32 init_input_signals_calltf(PLI_BYTE8 *user_data);
extern "C" PLI_INT32 init_output_signals_calltf(PLI_BYTE8 *user_data);
extern "C" PLI_INT32 init_structures_calltf(PLI_BYTE8 *user_data);
extern "C" PLI_INT32 tick_calltf(PLI_BYTE8 *user_data);


PLI_INT32 init_input_signals_calltf(PLI_BYTE8 * /*user_data*/) {
	std::cout << "init inputs" << std::endl;
  vpiHandle syscall_handle = vpi_handle(vpiSysTfCall, nullptr);
  vpiHandle arg_iter = vpi_iterate(vpiArgument, syscall_handle);
  // Cache Inputs
  if (arg_iter != nullptr) {
    while (vpiHandle arg_handle = vpi_scan(arg_iter)) {
      inputs.push_back(arg_handle);
    }
  }
  return 0;
}

PLI_INT32 init_output_signals_calltf(PLI_BYTE8 * /*user_data*/) {
	std::cout << "init outputs" << std::endl;
  vpiHandle syscall_handle = vpi_handle(vpiSysTfCall, nullptr);
  vpiHandle arg_iter = vpi_iterate(vpiArgument, syscall_handle);
  // Cache Inputs
  if (arg_iter != nullptr) {
    while (vpiHandle arg_handle = vpi_scan(arg_iter)) {
      outputs.push_back(arg_handle);
    }
  }
  return 0;
}

vpiHandle getHandle(const std::string &name) {
  s_vpi_value value;
  value.format = vpiStringVal;
  char *str = (char *) malloc(name.size() + 1);
  strcpy(str, name.c_str());
  auto handle = vpi_handle_by_name(str, nullptr);
  if (handle == nullptr) {
    std::cerr << "Could not find handle for " << name << std::endl;
    exit(1);
  }
  free(str);
  return handle;
}

PLI_INT32 init_structures_calltf(PLI_BYTE8 *) {
	std::cout << "init structures: " << std::endl;
  // at this point, we have all the inputs and outputs, and we have to tie them into the interfaces
#if NUM_DDR_CHANNELS >= 1
  mem_ctrl::init("custom_dram_configs/DDR4_8Gb_x8_2400.ini");
  ddr_clock_inc = (1000.0 / dramsim3config->tCK) / DEFAULT_PL_CLOCK;
  axi4_mems[0].ar.init(VCSShortHandle(getHandle("M00_AXI_arready")),
                       VCSShortHandle(getHandle("M00_AXI_arvalid")),
                       VCSShortHandle(getHandle("M00_AXI_arid")),
                       VCSShortHandle(getHandle("M00_AXI_arsize")),
                       VCSShortHandle(getHandle("M00_AXI_arburst")),
                       VCSShortHandle(getHandle("M00_AXI_araddr")),
                       VCSShortHandle(getHandle("M00_AXI_arlen")));
  axi4_mems[0].aw.init(VCSShortHandle(getHandle("M00_AXI_awready")),
                       VCSShortHandle(getHandle("M00_AXI_awvalid")),
                       VCSShortHandle(getHandle("M00_AXI_awid")),
                       VCSShortHandle(getHandle("M00_AXI_awsize")),
                       VCSShortHandle(getHandle("M00_AXI_awburst")),
                       VCSShortHandle(getHandle("M00_AXI_awaddr")),
                       VCSShortHandle(getHandle("M00_AXI_awlen")));
  VCSShortHandle dummy;
  axi4_mems[0].w.init(VCSShortHandle(getHandle("M00_AXI_wready")),
                      VCSShortHandle(getHandle("M00_AXI_wvalid")),
                      VCSShortHandle(getHandle("M00_AXI_wlast")),
                      dummy,
                      VCSLongHandle(getHandle("M00_AXI_wstrb")),
                      VCSLongHandle(getHandle("M00_AXI_wdata")));
  axi4_mems[0].r.init(VCSShortHandle(getHandle("M00_AXI_rready")),
                      VCSShortHandle(getHandle("M00_AXI_rvalid")),
                      VCSShortHandle(getHandle("M00_AXI_rlast")),
                      VCSShortHandle(getHandle("M00_AXI_rid")),
                      dummy,
                      VCSLongHandle(getHandle("M00_AXI_rdata")));
  axi4_mems[0].b.init(VCSShortHandle(getHandle("M00_AXI_bready")),
                      VCSShortHandle(getHandle("M00_AXI_bvalid")),
                      VCSShortHandle(getHandle("M00_AXI_bid")));
#if NUM_DDR_CHANNELS >= 2
#error "not implemented yet"
#endif
#endif

  ctrl.set_ar(
          VCSShortHandle(getHandle("S00_AXI_arvalid")),
          VCSShortHandle(getHandle("S00_AXI_arready")),
          VCSShortHandle(getHandle("S00_AXI_araddr")));
  ctrl.set_aw(
          VCSShortHandle(getHandle("S00_AXI_awvalid")),
          VCSShortHandle(getHandle("S00_AXI_awready")),
          VCSShortHandle(getHandle("S00_AXI_awaddr")));
  ctrl.set_w(
          VCSShortHandle(getHandle("S00_AXI_wvalid")),
          VCSShortHandle(getHandle("S00_AXI_wready")),
          VCSShortHandle(getHandle("S00_AXI_wdata")));
  ctrl.set_r(
          VCSShortHandle(getHandle("S00_AXI_rvalid")),
          VCSShortHandle(getHandle("S00_AXI_rready")),
          VCSShortHandle(getHandle("S00_AXI_rdata")));
  ctrl.set_b(
          VCSShortHandle(getHandle("S00_AXI_bvalid")),
          VCSShortHandle(getHandle("S00_AXI_bready")));


  std::cout << "start servers" << std::endl;

  cmd_server::start();
  data_server::start();

  std:: cout << "finished init structures" << std::endl;
  return 0;
}

PLI_INT32 tick_calltf(PLI_BYTE8 * /*user_data*/) {
	printf(".");
	fflush(stdout);

  tick_signals(&ctrl);
  return 0;
}


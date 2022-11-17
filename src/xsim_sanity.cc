//
// Created by Chris Kjellqvist on 11/17/22.
//
#include "composer_allocator_declaration.h"
#include "fpga_pci_sv.h"
#include "sh_dpi_tasks.h"
#include <composer/rocc_cmd.h>

extern "C" void test_main_hook(int *rc) {
  cosim_printf("enter function\n");
  fpga_mgmt_init();
  cosim_printf("initialized\n");

  auto co = composer::rocc_cmd::start_cmd(ALUSystem_ID, 0, 0, true, composer::R0, 0, 0, 0, 32,  64);
  auto p = co.pack(pack_cfg);
  cosim_printf("packed command\n");
  pci_bar_handle_t pci_bar_handle = PCI_BAR_HANDLE_INIT;
  for (int i = 0; i < 5; ++i) {
    cosim_printf("testing %d\n", i);
    uint32_t ready = 0;
    while (!ready) {
      fpga_pci_peek(pci_bar_handle, CMD_READY, &ready);
      cosim_printf("READY: %d\n", ready);
    }
    fpga_pci_poke(pci_bar_handle, CMD_BITS, p[i]);
    fpga_pci_poke(pci_bar_handle, CMD_VALID, 1);
  }

  fpga_mgmt_close();
  *rc = 0;
}
//
// Created by Chris Kjellqvist on 11/17/22.
//
#include "composer_allocator_declaration.h"
extern "C" {
#include "fpga_pci_sv.h"
#include "sh_dpi_tasks.h"
};
#include <composer/rocc_cmd.h>

extern "C" void test_main_hook(int *rc) {
  cosim_printf("enter function\n");
  fflush(stdout);
  fpga_mgmt_init();
  cosim_printf("initialized\n");
  fflush(stdout);

  uint32_t p[] = {0x2200407b, 0x0, 0x20, 0x0, 0x40};

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

  for (int i = 0; i < 3; ++i) {
    uint32_t ready = 0;
    while (!ready) fpga_pci_peek(pci_bar_handle, RESP_VALID, &ready);
    uint32_t dat;
    fpga_pci_peek(pci_bar_handle, RESP_BITS, &dat);
    cosim_printf("dat %x\n", dat);
    fpga_pci_poke(pci_bar_handle, RESP_READY, 1);
  }


  fpga_mgmt_close();
  *rc = 0;
}

//
// Created by Chris Kjellqvist on 11/17/22.
//
#include "composer_allocator_declaration.h"

#include <composer/rocc_cmd.h>
extern "C" {
#ifdef FPGA
#include <fpga_pci.h>
#include <fpga_mgmt.h>
#else
#include "fpga_pci_sv.h"
#include "sh_dpi_tasks.h"

#endif
};
pci_bar_handle_t pci_bar_handle = PCI_BAR_HANDLE_INIT;

void write_command(uint32_t *p) {
  for (int i = 0; i < 5; ++i) {
    uint32_t ready = 0;
    while (!ready) fpga_pci_peek(pci_bar_handle, CMD_READY, &ready);
    fpga_pci_poke(pci_bar_handle, CMD_BITS, p[i]);
    fpga_pci_poke(pci_bar_handle, CMD_VALID, 1);
  }
}

uint32_t * get_response() {
  auto * v = new uint32_t[3];
  for (int i = 0; i < 3; ++i) {
    uint32_t ready = 0;
    while (!ready) fpga_pci_peek(pci_bar_handle, RESP_VALID, &ready);
    fpga_pci_peek(pci_bar_handle, RESP_BITS, &ready);
    v[i] = ready;
    fpga_pci_poke(pci_bar_handle, RESP_READY, 1);
  }
  return v;
}

#ifdef FPGA
#define cosim_printf printf
int main()
#else
extern "C" void test_main_hook(int *rc)
#endif
{

  // read addr: 1ff000-1ff800
  //write addr: 1fe000-1fe800
  //Sending addresses           1ff000 and           1fe000 to fpga
  //Sent data
  //command in file is function: 0 system_id: 1 opcode: { rs1_num:   rs2_num:  xd:   rd: 0 xs1:   xs2:   core_id:   rs1: 2048 rs2: 2093056
  //detected addr command
  //addr_read: 1010007b	0	4001	0	1ff000
  //command in file is function: 0 system_id: 1 opcode: { rs1_num:   rs2_num:   xd:   rd: 0 xs1:   xs2:   core_id:   rs1: 2048 rs2: 2088960
  //detected addr command
  //addr_write: 1000007b	0	4000	0	1fe000
  //start_cmd: 1200407b	0	400	0	f

  cosim_printf("enter function\n");
  fflush(stdout);
  fpga_mgmt_init();
  cosim_printf("initialized\n");
  fflush(stdout);

  uint32_t addr1[] = {0x1010007b, 0, 0x4001, 0x0, 0x1ff000};
  uint32_t addr2[] = {0x1000007b, 0, 0x4000, 0x0, 0x1fe000};
  uint32_t start[] = {0x1200407b, 0, 0, 0x400, 0x0, 0xf};

#ifdef FPGA
  int rc = fpga_pci_attach(0, FPGA_APP_PF, APP_PF_BAR0, 0, &pci_bar_handle);
  if (rc) {
    printf("Fail\n");
    exit(1);
  }
#endif
  write_command(addr1);
  write_command(addr2);
  write_command(start);
  auto dat = get_response();
  for (int i = 0; i < 3; ++i) {
    cosim_printf("%ux\n", dat[i]);
  }
  fpga_mgmt_close();
#ifndef FPGA
  *rc = 0;
#endif
}

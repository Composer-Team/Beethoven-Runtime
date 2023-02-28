//
// Created by Chris Kjellqvist on 12/2/22.
//

#include "mmio.h"
#include <iostream>

#ifdef Kria
#include <sys/mman.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>

volatile uint32_t *ptr_RESP_BITS;
volatile uint32_t *ptr_RESP_VALID;
volatile uint32_t *ptr_RESP_READY;
volatile uint32_t *ptr_CMD_BITS;
volatile uint32_t *ptr_CMD_VALID;
volatile uint32_t *ptr_CMD_READY;
volatile uint32_t *ptr_AXIL_DEBUG;
int devmem_fd;
volatile char *devmem_map;

#endif

void setup_mmio() {
  // Kria specific initialization
#ifdef Kria
  devmem_fd = open("/dev/mem", O_SYNC | O_RDWR);
  if (devmem_fd < 0) {
    std::cerr << "Error opening /dev/mem. Errno: " << strerror(errno) << std::endl;
    exit(errno);
  }
  void *dvm = mmap(nullptr, 0x100, PROT_READ | PROT_WRITE, MAP_SYNC, devmem_fd, 0x2000000000L);
  if (dvm == MAP_FAILED) {
    std::cerr << "Failed to map devmem using mmap. Errno: " << strerror(errno) << std::endl;
    exit(errno);
  }
  devmem_map = (char *) dvm;
#endif
}


void poke_mmio(uint64_t addr, uint32_t val) {
#ifdef F1
  int rc = fpga_pci_poke(pci_bar_handle, addr, val);
  if (rc) {
    std::cerr << "Failed to peek PCI " << rc << std::endl;
    throw std::exception();
  }
#elif defined(Kria)
  *((volatile uint32_t *)(devmem_map + addr)) = val;
#endif
}


uint32_t peek_mmio(uint32_t addr) {
#ifdef F1
  uint32_t v;
  int rc = fpga_pci_peek(pci_bar_handle, addr, &v);
  if (rc) {
    std::cerr << "Failed to peek PCI " << rc << std::endl;
    throw std::exception();
  }
  return v;
#elif defined(Kria)
  return *((volatile uint32_t *)(devmem_map + addr));
#endif
}

//
// Created by Chris Kjellqvist on 12/2/22.
//

#include "mmio.h"
#include "fpga_utils.h"
#include <iostream>
#include <atomic>

void poke_mmio(uint64_t addr, int val) {
#ifdef F1
  int rc = fpga_pci_poke(pci_bar_handle, addr, val);
  if (rc) {
    std::cerr << "Failed to peek PCI " << rc << std::endl;
    throw std::exception();
  }
#elif defined(Kria)
  *(int*)(ComposerMMIOOffset | addr) = val;
#endif
}
uint32_t peek_mmio(uint64_t addr) {
#ifdef F1
  uint32_t v;
  int rc = fpga_pci_peek(pci_bar_handle, addr, &v);
  if (rc) {
    std::cerr << "Failed to peek PCI " << rc << std::endl;
    throw std::exception();
  }
  return v;
#elif defined(Kria)
  return *(int*)(ComposerMMIOOffset | addr);
#endif

}
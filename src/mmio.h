//
// Created by Chris Kjellqvist on 12/2/22.
//

#ifndef COMPOSER_VERILATOR_MMIO_H
#define COMPOSER_VERILATOR_MMIO_H

#include <composer_allocator_declaration.h>
#include <cinttypes>

template<typename t>
void poke_mmio(uint64_t addr, t val) {
#ifdef F1
  int rc = fpga_pci_poke(pci_bar_handle, addr, val);
if (rc) {
  std::cerr << "Failed to peek PCI " << rc << std::endl;
  throw std::exception();
}
#elif defined(Kria)
  *(t *) (ComposerMMIOOffset | addr) = val;
#endif
}


template<typename t>
t peek_mmio(uint64_t addr) {
#ifdef F1
  uint32_t v;
int rc = fpga_pci_peek(pci_bar_handle, addr, &v);
if (rc) {
  std::cerr << "Failed to peek PCI " << rc << std::endl;
  throw std::exception();
}
return v;
#elif defined(Kria)
  return *(t *) (ComposerMMIOOffset | addr);
#endif
}

#endif //COMPOSER_VERILATOR_MMIO_H

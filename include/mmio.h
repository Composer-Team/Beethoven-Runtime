//
// Created by Chris Kjellqvist on 12/2/22.
//

#ifndef COMPOSER_VERILATOR_MMIO_H
#define COMPOSER_VERILATOR_MMIO_H

#include <composer_allocator_declaration.h>
#include <cinttypes>


void setup_mmio();

void poke_mmio(uint64_t addr, uint32_t val);

uint32_t peek_mmio(uint32_t addr);

#endif //COMPOSER_VERILATOR_MMIO_H

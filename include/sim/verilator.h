//
// Created by Chris Kjellqvist on 10/29/22.
//

#ifndef COMPOSER_VERILATOR_VERILATOR_H
#define COMPOSER_VERILATOR_VERILATOR_H
#include <composer_allocator_declaration.h>
#include <verilated.h>
#include "dram_system.h"

#ifdef USE_VCD
extern VerilatedVcdC *tfp;
#else
extern VerilatedFstC *tfp;
#endif

void run_verilator();


#endif//COMPOSER_VERILATOR_VERILATOR_H

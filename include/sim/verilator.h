//
// Created by Chris Kjellqvist on 10/29/22.
//

#ifndef COMPOSER_VERILATOR_VERILATOR_H
#define COMPOSER_VERILATOR_VERILATOR_H
#include "dram_system.h"
#include <composer_allocator_declaration.h>
#include <verilated.h>
#include <optional>

#ifdef USE_VCD
#include "verilated_vcd_c.h"
using waveTrace = VerilatedVcdC;
#define TRACE_FILE_ENDING ".vcd"
#else
#include "verilated_fst_c.h"
using waveTrace = VerilatedFstC;
#define TRACE_FILE_ENDING ".fst"
#endif

extern waveTrace *tfp;

void run_verilator(std::optional<std::string> trace_file, const std::string &dram_config_file);

void sig_handle(int sig);

extern bool active_reset;

#endif//COMPOSER_VERILATOR_VERILATOR_H

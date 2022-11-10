//
// Created by Chris Kjellqvist on 10/29/22.
//

#ifndef COMPOSER_VERILATOR_FPGA_UTILS_H
#define COMPOSER_VERILATOR_FPGA_UTILS_H

#ifdef FPGA

#include "fpga_mgmt.h"
#include "fpga_pci.h"
extern "C" {
  #include "fpga_dma.h"
};
#include <string>

extern pthread_mutex_t bus_lock;
extern int slot_id;
extern int pci_bar_handle;
extern int xdma_write_fd;
extern int xdma_read_fd;
extern pthread_mutex_t main_lock;



void check_rc(int rc, const std::string &message);

void fpga_setup(int id);

void fpga_shutdown();


#endif

#endif //COMPOSER_VERILATOR_FPGA_UTILS_H

//
// Created by Chris Kjellqvist on 10/29/22.
//

#ifndef COMPOSER_VERILATOR_FPGA_UTILS_H
#define COMPOSER_VERILATOR_FPGA_UTILS_H

#include "fpga_mgmt.h"
#include "fpga_pci.h"

#ifdef __cplusplus
extern "C" {
#endif

extern pthread_mutex_t bus_lock;
extern int slot_id;
extern int pci_bar_handle;
extern int xdma_write_fd;
extern int xdma_read_fd;
extern pthread_mutex_t main_lock;


void check_rc(int rc, const char *message);

void fpga_setup(int id);

void fpga_shutdown();

int wrapper_fpga_dma_burst_write(int fd, uint8_t *buffer, size_t xfer_sz,
                                 size_t address);

int wrapper_fpga_dma_burst_read(int fd, uint8_t *buffer, size_t xfer_sz,
                                size_t address);
#ifdef __cplusplus
}
#endif


#endif //COMPOSER_VERILATOR_FPGA_UTILS_H

//
// Created by Chris Kjellqvist on 10/29/22.
//

#ifdef FPGA

#include <iostream>
#include "fpga_utils.h"
#include <string>
#include <sys/fcntl.h>

pthread_mutex_t bus_lock;
int slot_id;
int pci_bar_handle;
int xdma_write_fd;
int xdma_read_fd;

void check_rc(int rc, const std::string &message) {
  if (rc) {
    std::cerr << "Failure: '" << message << "' " << rc << std::endl;
    exit(rc);
  }
}

void fpga_setup(int id) {
  uint16_t pci_vendor_id = 0x1D0F; /* Amazon PCI Vendor ID */
  uint16_t pci_device_id = 0xF000; /* PCI Device ID preassigned by Amazon for F1 applications */

  slot_id = id;
  int rc = fpga_pci_init();
  check_rc(rc, "fpga_pci_init FAILED");

  /* check AFI status */
  struct fpga_mgmt_image_info info = {0};

  /* get local image description, contains status, vendor id, and device id. */
  rc = fpga_mgmt_describe_local_image(slot_id, &info, 0);
  check_rc(rc, "Unable to get AFI information from slot. Are you running as root?");

  /* check to see if the slot is ready */
  if (info.status != FPGA_STATUS_LOADED) {
    rc = 1;
    check_rc(rc, "AFI in Slot is not in READY state !");
  }

  fprintf(stderr, "AFI PCI  Vendor ID: 0x%x, Device ID 0x%x\n",
          info.spec.map[FPGA_APP_PF].vendor_id,
          info.spec.map[FPGA_APP_PF].device_id);

  /* confirm that the AFI that we expect is in fact loaded */
  if (info.spec.map[FPGA_APP_PF].vendor_id != pci_vendor_id ||
      info.spec.map[FPGA_APP_PF].device_id != pci_device_id) {
    fprintf(stderr, "AFI does not show expected PCI vendor id and device ID. If the AFI "
                    "was just loaded, it might need a rescan. Rescanning now.\n");

    rc = fpga_pci_rescan_slot_app_pfs(slot_id);
    check_rc(rc, "Unable to update PF for slot");
    /* get local image description, contains status, vendor id, and device id. */
    rc = fpga_mgmt_describe_local_image(slot_id, &info, 0);
    check_rc(rc, "Unable to get AFI information from slot");

    fprintf(stderr, "AFI PCI  Vendor ID: 0x%x, Device ID 0x%x\n",
            info.spec.map[FPGA_APP_PF].vendor_id,
            info.spec.map[FPGA_APP_PF].device_id);

    /* confirm that the AFI that we expect is in fact loaded after rescan */
    if (info.spec.map[FPGA_APP_PF].vendor_id != pci_vendor_id ||
        info.spec.map[FPGA_APP_PF].device_id != pci_device_id) {
      rc = 1;
      check_rc(rc, "The PCI vendor id and device of the loaded AFI are not "
                   "the expected values.");
    }
  }

  /* attach to BAR0 */
  pci_bar_handle = PCI_BAR_HANDLE_INIT;
  rc = fpga_pci_attach(slot_id, FPGA_APP_PF, APP_PF_BAR0, 0, &pci_bar_handle);
  check_rc(rc, "fpga_pci_attach FAILED");
  xdma_read_fd = fpga_dma_open_queue(FPGA_DMA_XDMA, id, 0, true);
  xdma_write_fd = fpga_dma_open_queue(FPGA_DMA_XDMA, id, 1, false);
  if (xdma_write_fd < 0) {
    std::cerr << "error opening xdma write fd" << std::endl;
    exit(1);
  }

  if (xdma_read_fd < 0) {
    std::cerr << "error opening xdma read fd" << std::endl;
    exit(1);
  }
}


void fpga_shutdown() {

#ifndef SIMULATION_XSIM
  int rc = fpga_pci_detach(pci_bar_handle);
  // don't call check_rc because of fpga_shutdown call. do it manually:
  check_rc(rc, "Failure while detaching from the fpga");
#endif
}


#endif

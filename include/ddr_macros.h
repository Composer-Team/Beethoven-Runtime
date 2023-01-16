//
// Created by Chris Kjellqvist on 11/10/22.
//

#ifndef COMPOSER_VERILATOR_DDR_MACROS_H
#define COMPOSER_VERILATOR_DDR_MACROS_H
#include <composer_allocator_declaration.h>

#if DATA_BUS_WIDTH > 64
#define access_w(dnum) ((char*)top.M0 ## dnum ## _AXI_wdata.m_storage)
#define access_r(dnum) ((char*)top.M0 ## dnum ## _AXI_rdata.m_storage)
#else
#define access_w(dnum) ((char*)&top.M0 ## dnum ## _AXI_wdata)
#define access_r(dnum) ((char*)&top.M0 ## dnum ## _AXI_rdata)
#endif

#define init_ddr_interface(DDR_NUM) \
axi4_mems[DDR_NUM].aw = new v_address_channel<ComposerMemIDDtype>(top.M0 ## DDR_NUM ## _AXI_awready, \
                                      top.M0 ## DDR_NUM ## _AXI_awvalid, \
                                      top.M0 ## DDR_NUM ## _AXI_awid, \
                                      top.M0 ## DDR_NUM ## _AXI_awsize, \
                                      top.M0 ## DDR_NUM ## _AXI_awburst, \
                                      top.M0 ## DDR_NUM ## _AXI_awaddr, \
                                      top.M0 ## DDR_NUM ## _AXI_awlen); \
axi4_mems[DDR_NUM].ar = new v_address_channel<ComposerMemIDDtype>(top.M0 ## DDR_NUM ## _AXI_arready, \
                                      top.M0 ## DDR_NUM ## _AXI_arvalid, \
                                      top.M0 ## DDR_NUM ## _AXI_arid, \
                                      top.M0 ## DDR_NUM ## _AXI_arsize, \
                                      top.M0 ## DDR_NUM ## _AXI_arburst, \
                                      top.M0 ## DDR_NUM ## _AXI_araddr, \
                                      top.M0 ## DDR_NUM ## _AXI_arlen); \
axi4_mems[DDR_NUM].w = new data_channel<ComposerMemIDDtype>(top.M0 ## DDR_NUM ## _AXI_wready, \
                                  top.M0 ## DDR_NUM ## _AXI_wvalid, \
                                  &top.M0 ## DDR_NUM ## _AXI_wstrb, \
                                  top.M0 ## DDR_NUM ## _AXI_wlast, \
                                  nullptr); \
axi4_mems[DDR_NUM].r = new data_channel<ComposerMemIDDtype>(top.M0 ## DDR_NUM ## _AXI_rready, \
                                  top.M0 ## DDR_NUM ## _AXI_rvalid, \
                                  nullptr, \
                                  top.M0 ## DDR_NUM ## _AXI_rlast, \
                                  &top.M0 ## DDR_NUM ## _AXI_rid); \
axi4_mems[DDR_NUM].b = new response_channel<ComposerMemIDDtype>(top.M0 ## DDR_NUM ## _AXI_bready, \
                                      top.M0 ## DDR_NUM ## _AXI_bvalid, \
                                      top.M0 ## DDR_NUM ## _AXI_bid); \

#endif //COMPOSER_VERILATOR_DDR_MACROS_H

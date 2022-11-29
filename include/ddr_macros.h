//
// Created by Chris Kjellqvist on 11/10/22.
//

#ifndef COMPOSER_VERILATOR_DDR_MACROS_H
#define COMPOSER_VERILATOR_DDR_MACROS_H

#define init_ddr_interface(DDR_NUM) \
axi4_mems[DDR_NUM].aw = new v_address_channel<QData>(top->mem_ ## DDR_NUM ## _aw_ready, \
                                      top->mem_ ## DDR_NUM ## _aw_valid, \
                                      top->mem_ ## DDR_NUM ## _aw_bits_id, \
                                      top->mem_ ## DDR_NUM ## _aw_bits_size, \
                                      top->mem_ ## DDR_NUM ## _aw_bits_burst, \
                                      top->mem_ ## DDR_NUM ## _aw_bits_addr, \
                                      top->mem_ ## DDR_NUM ## _aw_bits_len); \
axi4_mems[DDR_NUM].ar = new v_address_channel<QData>(top->mem_ ## DDR_NUM ## _ar_ready, \
                                      top->mem_ ## DDR_NUM ## _ar_valid, \
                                      top->mem_ ## DDR_NUM ## _ar_bits_id, \
                                      top->mem_ ## DDR_NUM ## _ar_bits_size, \
                                      top->mem_ ## DDR_NUM ## _ar_bits_burst, \
                                      top->mem_ ## DDR_NUM ## _ar_bits_addr, \
                                      top->mem_ ## DDR_NUM ## _ar_bits_len); \
axi4_mems[DDR_NUM].w = new data_channel(top->mem_ ## DDR_NUM ## _w_ready, \
                                  top->mem_ ## DDR_NUM ## _w_valid, \
                                  top->mem_ ## DDR_NUM ## _w_bits_data, \
                                  &top->mem_ ## DDR_NUM ## _w_bits_strb, \
                                  top->mem_ ## DDR_NUM ## _w_bits_last, \
                                  nullptr); \
axi4_mems[DDR_NUM].r = new data_channel(top->mem_ ## DDR_NUM ## _r_ready, \
                                  top->mem_ ## DDR_NUM ## _r_valid, \
                                  top->mem_ ## DDR_NUM ## _r_bits_data, \
                                  nullptr, \
                                  top->mem_ ## DDR_NUM ## _r_bits_last, \
                                  &top->mem_ ## DDR_NUM ## _r_bits_id); \
axi4_mems[DDR_NUM].b = new response_channel(top->mem_ ## DDR_NUM ## _b_ready, \
                                      top->mem_ ## DDR_NUM ## _b_valid, \
                                      top->mem_ ## DDR_NUM ## _b_bits_id); \

#endif //COMPOSER_VERILATOR_DDR_MACROS_H

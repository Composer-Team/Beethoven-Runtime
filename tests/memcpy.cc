//
// Created by Christopher Kjellqvist on 11/14/24.
//

#include "fpga_utils.h"
#include <mmio.h>

int main() {
  fpga_setup(0);
  mmio_poke();
}
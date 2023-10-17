//
// Created by Chris Kjellqvist on 10/29/22.
//
#include "../include/cmd_server.h"
#include "../include/data_server.h"
#include <pthread.h>
#include "fpga_utils.h"

pthread_mutex_t main_lock = PTHREAD_MUTEX_INITIALIZER;


#ifdef VSIM
extern "C" {
#include <sh_dpi_tasks.h>
};
extern "C" void test_main_hook(uint32_t *exit_code)
#else

#ifdef Kria
#include <cmsis_gcc.h>
#include <cmsis_cp15.h>
void kria_setup() {
  uint32_t cbar = __get_CBAR();
  uint64_t cbar_long = (uint64_t)cbar;
  void *ptr = reinterpret_cast<void *>(cbar_long);
  printf("CBAR is %p\n", ptr); fflush(stdout);
}
#endif


int main()
#endif
{
#ifdef F1
  fpga_setup(0);
#endif
#ifdef Kria
  kria_setup();
#endif
  // Kria does local allocations only
  cmd_server::start();
  data_server::start();
  pthread_mutex_lock(&main_lock);
  pthread_mutex_lock(&main_lock);
#ifdef F1
  fpga_shutdown();
#endif
#ifdef VSIM
  *exit_code = 0;
#endif
}

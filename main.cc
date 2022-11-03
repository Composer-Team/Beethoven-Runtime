//
// Created by Chris Kjellqvist on 10/29/22.
//
#include "cmd_server.h"
#include "data_server.h"
#include <pthread.h>
#include "verilator.h"
#include "fpga_utils.h"

data_server *d_server;
cmd_server *c_server;


int main(int argc, char **argv) {
  d_server = new data_server;
  c_server = new cmd_server;
  d_server->start();
  c_server->start();
#ifdef SIM
  run_verilator(argc, argv);
#endif
#ifdef FPGA
  fpga_setup(0);
#endif
  pthread_mutex_lock(&main_lock);
  pthread_mutex_lock(&main_lock);
#ifdef FPGA
  fpga_shutdown();
#endif
  exit(0);
}

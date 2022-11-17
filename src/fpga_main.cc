//
// Created by Chris Kjellqvist on 10/29/22.
//
#include "../include/cmd_server.h"
#include "../include/data_server.h"
#include <pthread.h>
#include "fpga_utils.h"
#include <response_poller.h>
#include <unistd.h>
#include <stdlib.h>

//data_server *d_server;
cmd_server *c_server;

pthread_mutex_t main_lock = PTHREAD_MUTEX_INITIALIZER;

#include <ctime>

#ifdef VSIM
extern "C" {
#include <sh_dpi_tasks.h>
};
extern "C" void test_main_hook(uint32_t *exit_code)
#else

int main(int argc, char **argv)
#endif
{
  fpga_setup(0);

//  d_server = new data_server;
  c_server = new cmd_server;
  response_poller poller;

//  d_server->start();
  c_server->start();
//  poller.start_poller();
  pthread_mutex_lock(&main_lock);
  pthread_mutex_lock(&main_lock);
  fpga_shutdown();
#ifdef VSIM
  *exit_code = 0;
#endif
}

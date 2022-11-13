//
// Created by Chris Kjellqvist on 10/29/22.
//
#include "../include/cmd_server.h"
#include "../include/data_server.h"
#include <pthread.h>
#include "fpga_utils.h"
#include <response_poller.h>

data_server *d_server;
cmd_server *c_server;

pthread_mutex_t main_lock = PTHREAD_MUTEX_INITIALIZER;


int main(int argc, char **argv) {
  d_server = new data_server;
  c_server = new cmd_server;
  d_server->start();
  c_server->start();
  fpga_setup(0);
  response_poller poller;
  poller.start_poller();
  pthread_mutex_lock(&main_lock);
  pthread_mutex_lock(&main_lock);
  printf("Main thread exiting\n");
  fpga_shutdown();
  exit(0);
}

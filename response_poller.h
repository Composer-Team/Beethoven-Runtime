//
// Created by Chris Kjellqvist on 11/6/22.
//

#ifndef COMPOSER_VERILATOR_RESPONSE_POLLER_H
#define COMPOSER_VERILATOR_RESPONSE_POLLER_H

#include <pthread.h>
#include <verilator_server.h>
#include <thread>

extern composer::cmd_server_file *csf;

struct response_poller {
  pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
  int n_waiting = 0;

  // no easy way to do this without interrupts from FPGA and without race conditions
  std::timed_mutex poller_release_mutex;

  void start_poller();

public:
  explicit response_poller();
};

#endif //COMPOSER_VERILATOR_RESPONSE_POLLER_H

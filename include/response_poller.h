//
// Created by Chris Kjellqvist on 11/6/22.
//

#ifndef COMPOSER_VERILATOR_RESPONSE_POLLER_H
#define COMPOSER_VERILATOR_RESPONSE_POLLER_H

#include <pthread.h>
#include <composer/verilator_server.h>
#include <thread>

extern composer::cmd_server_file *csf;

struct response_poller {
  static void start_poller();
};

#endif //COMPOSER_VERILATOR_RESPONSE_POLLER_H

//
// Created by Chris Kjellqvist on 9/29/22.
//
#ifndef COMPOSER_VERILATOR_CMD_SERVER_H
#define COMPOSER_VERILATOR_CMD_SERVER_H

#include <rocc.h>
#include <queue>
#include <unordered_map>
#include <composer_verilator_server.h>

extern composer::cmd_server_file *csf;

struct system_core_pair {
  int system;
  int core;
  system_core_pair(int system, int core);
  bool operator==(const system_core_pair &other) const {
    return system == other.system and core == other.core;
  }
};

template <>
struct std::hash<system_core_pair> {
  std::size_t operator()(const system_core_pair& k) const
  {
    return k.core << 8 | k.system;
  }

};

struct cmd_server {
  bool stop_cond;

  pthread_mutex_t cmdserverlock = PTHREAD_MUTEX_INITIALIZER;
  std::queue<composer::rocc_cmd> cmds;
  std::unordered_map<system_core_pair, std::queue<int>*> in_flight;
  void start();
  void stop();
private:
  pthread_t thread;
};


#endif //COMPOSER_VERILATOR_CMD_SERVER_H

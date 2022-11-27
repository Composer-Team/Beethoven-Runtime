//
// Created by Chris Kjellqvist on 9/27/22.
//

#include <composer/verilator_server.h>
#include <composer_allocator_declaration.h>
#include "../include/cmd_server.h"
#include "../include/data_server.h"
#include "fpga_utils.h"

#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>
#include <tuple>
#include <cassert>
#include <cerrno>
#include <cstring>

#include "response_poller.h"

// for shared memory
#include <fcntl.h>

#ifdef FPGA
#include "fpga_utils.h"
#include <composer_allocator_declaration.h>
#endif
#ifdef SIM
extern bool kill_sig;
#endif
#include <ctime>

system_core_pair::system_core_pair(int system, int core) {
  this->system = system;
  this->core = core;
}

using namespace composer;

cmd_server_file *csf;

pthread_mutex_t cmdserverlock = PTHREAD_MUTEX_INITIALIZER;
std::queue<composer::rocc_cmd> cmds;
std::unordered_map<system_core_pair, std::queue<int>*> in_flight;


static void* cmd_server_f(void* _) {
  // map in the shared file
  int fd_composer = shm_open(cmd_server_file_name.c_str(), O_CREAT | O_RDWR, file_access_flags);
  if (fd_composer < 0) {
    printf("Failed to initialize cmd_file\n%s\n", strerror(errno));
    exit(errno);
  }
  ftruncate(fd_composer, sizeof(cmd_server_file));
  auto &addr = *(cmd_server_file*)mmap(nullptr, sizeof(cmd_server_file), file_access_prots,
                                 MAP_SHARED, fd_composer, 0);
  csf = &addr;
  // we need to initialize it! This used to be a race condition, where the cmd_server thread was racing against the
  // poller thread to get to the file. The poller often won, found old dat anad mucked everything up :(
  cmd_server_file::init(addr);
#ifndef SIM
  response_poller::start_poller();
#endif

  std::vector<std::pair<int, FILE*>> alloc;
  pthread_mutex_lock(&addr.server_mut);
  pthread_mutex_lock(&addr.server_mut);
  while(true) {
    // allocate space for response
    int id;
    if (addr.cmd.getXd()) {
      pthread_mutex_lock(&addr.free_list_lock);
      id = addr.free_list[addr.free_list_idx];
      addr.free_list_idx--;
      pthread_mutex_unlock(&addr.free_list_lock);

      // return response handle to client
      addr.pthread_wait_id = id;
      // end return response handle to client

      // enqueue command for main simulation thread to handle
      addr.pthread_wait_id = id;
    } else {
      addr.pthread_wait_id = id = 0xffff;
    }
    pthread_mutex_lock(&cmdserverlock);
    if (addr.quit) {
      pthread_mutex_unlock(&main_lock);
#ifdef SIM
      kill_sig = true;
#endif
      return nullptr;
    }
#if defined(FPGA) || defined(VSIM)
    pthread_mutex_lock(&bus_lock);
    auto *pack = addr.cmd.pack(pack_cfg);
    for (int i = 0; i < 5; ++i) { // command is 5 32-bit payloads
      uint32_t ready = false;
      while(!ready) {
        int rc = fpga_pci_peek(pci_bar_handle, CMD_READY, &ready);
        assert(rc == 0);
      }

      if (fpga_pci_poke(pci_bar_handle, CMD_BITS, pack[i])) {
        fprintf(stderr, "error in CMD_BITS poke\n");
        exit(1);
      }

      if (fpga_pci_poke(pci_bar_handle, CMD_VALID, 1)) {
        fprintf(stderr, "error in CMD_VALID poke\n");
        exit(1);
      }
    }
    free(pack);
    pthread_mutex_unlock(&bus_lock);
#else
    cmds.push(addr.cmd);
#endif
    // let main thread know how to return result
    if (addr.cmd.getXd()) {
      const auto key = system_core_pair(addr.cmd.getSystemId(), addr.cmd.getCoreId());
      auto &m = in_flight;
      std::queue<int> *q;
      auto iterator = m.find(key);
      if (iterator == m.end()) {
        q = new std::queue<int>;
        m[key] = q;
      } else
        q = iterator->second;
      assert(id != 0xffff);
      q->push(id);
    }
    pthread_mutex_unlock(&addr.cmd_recieve_server_resp_lock);
    pthread_mutex_unlock(&cmdserverlock);
    // re-lock self to stall
    pthread_mutex_lock(&addr.server_mut);
  }
  munmap(&addr, sizeof(cmd_server_file));
  shm_unlink(cmd_server_file_name.c_str());
  return nullptr;
}

void cmd_server::start() {
  pthread_t thread;
  pthread_create(&thread, nullptr, cmd_server_f, nullptr);
}

void register_reponse(uint32_t *r_buffer) {
  composer::rocc_response r(r_buffer, pack_cfg);
  system_core_pair pr(r.system_id, r.core_id);
  pthread_mutex_lock(&cmdserverlock);
  auto it = in_flight.find(pr);
  if (it == in_flight.end()) {
    fprintf(stderr, "Error: Got bad response from HW: %x %x %x\n", r_buffer[0], r_buffer[1], r_buffer[2]);
    pthread_mutex_unlock(&cmdserverlock);
    pthread_mutex_unlock(&main_lock);
  } else {
    int id = in_flight[pr]->front();
    csf->responses[id] = r;
    // allow client thread to access response
    pthread_mutex_unlock(&csf->wait_for_response[id]);
    in_flight[pr]->pop();
    pthread_mutex_unlock(&cmdserverlock);
  }
}

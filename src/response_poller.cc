//
// Created by Chris Kjellqvist on 11/6/22.
//

#include "response_poller.h"
#include "fpga_utils.h"
#include "mmio.h"
#ifdef VSIM
#include "sh_dpi_tasks.h"
#endif

#include "cmd_server.h"
#include <composer_allocator_declaration.h>
#include <cinttypes>
#include <thread>

#include <cerrno>
#include <cstring>

using namespace std::chrono_literals;

static void *poll_thread(void *in) {
  int flights;
  int tries = 0;
  while (true) {
    tries++;
    pthread_mutex_lock(&csf->process_waiting_count_lock);
    flights = csf->processes_waiting;
    pthread_mutex_unlock(&csf->process_waiting_count_lock);

    if (flights) {
      uint32_t buf[3];
      for (unsigned int &i: buf) {
        uint32_t resp_ready = 0;
        while (!resp_ready) {
          resp_ready = peek_mmio(RESP_VALID);
          if (not resp_ready) {
            std::this_thread::sleep_for(1us);
          }
          tries++;
        }
        i = peek_mmio(RESP_BITS);
        poke_mmio(RESP_READY, 1);
      }
      register_reponse(buf);
      pthread_mutex_lock(&csf->process_waiting_count_lock);
      csf->processes_waiting--;
      pthread_mutex_unlock(&csf->process_waiting_count_lock);
    } else {
      std::this_thread::sleep_for(10ms);
    }
  }
  pthread_mutex_unlock(&main_lock);
  return nullptr;
}

void response_poller::start_poller() {
  pthread_t thread;
  pthread_create(&thread, nullptr, poll_thread, nullptr);
}

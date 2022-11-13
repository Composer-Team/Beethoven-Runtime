//
// Created by Chris Kjellqvist on 11/6/22.
//

#include "response_poller.h"
#include <fpga_pci.h>
#include "fpga_utils.h"
#include <composer_allocator_declaration.h>
#include <cinttypes>

using namespace std::chrono_literals;

response_poller::response_poller() {
  this->poller_release_mutex.lock();
}

[[noreturn]] static void* poll_thread(void * in) {
  auto *p = (response_poller*) in;
  int pause_length = *(int*)in; // UNUSED
  int flights;
  while(true) {
    pthread_mutex_lock(&p->queue_mutex);
    flights = p->n_waiting;
    pthread_mutex_unlock(&p->queue_mutex);

    if (flights) {
      uint32_t buf[3];
      for (unsigned int & i : buf) {
        uint32_t resp_ready = 0;
        while (!resp_ready) {
          fpga_pci_peek(pci_bar_handle, RESP_READY, &resp_ready);
          if (not resp_ready) {
            std::this_thread::sleep_for(1ms);
          }
        }
        fpga_pci_peek(pci_bar_handle, RESP_BITS, &i);
        fpga_pci_poke(pci_bar_handle, RESP_VALID, 1);
      }
      pthread_mutex_lock(&p->queue_mutex);
      p->n_waiting--;
      pthread_mutex_unlock(&p->queue_mutex);
    } else {
      p->poller_release_mutex.try_lock_for(100ms);
    }
  }
}
void response_poller::start_poller() {
  pthread_t thread;
  pthread_create(&thread, nullptr, poll_thread, this);
}

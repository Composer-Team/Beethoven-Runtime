//
// Created by Chris Kjellqvist on 11/6/22.
//

#include "response_poller.h"
#include "fpga_utils.h"
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

response_poller::response_poller() {
}

[[noreturn]] static void* poll_thread(void * in) {
  int flights;
  int tries = 0;
  while(tries < 20) {
	  tries ++;
    pthread_mutex_lock(&csf->process_waiting_count_lock);
    flights = csf->processes_waiting;
    pthread_mutex_unlock(&csf->process_waiting_count_lock);
    printf("Polling! In-flight: %d\n", flights);

    if (flights) {
      uint32_t buf[3];
      int rc = 0;
      for (unsigned int & i : buf) {
        uint32_t resp_ready = 0;
        while (!resp_ready) {
          rc |= fpga_pci_peek(pci_bar_handle, RESP_READY, &resp_ready);
          if (not resp_ready) {
            std::this_thread::sleep_for(1ms);
          }
        }
        rc |= fpga_pci_peek(pci_bar_handle, RESP_BITS, &i);
        rc |= fpga_pci_poke(pci_bar_handle, RESP_VALID, 1);
      }
      if (rc) {
        fprintf(stderr, "Error in fpga pci peek/poke\t%s\n", strerror(errno));
        throw std::exception();
      }
      c_server->register_reponse(buf);
      pthread_mutex_lock(&csf->process_waiting_count_lock);
      csf->processes_waiting--;
      pthread_mutex_unlock(&csf->process_waiting_count_lock);
    } else {
      std::this_thread::sleep_for(300ms);
    }
  }
  pthread_mutex_unlock(&main_lock);
}
void response_poller::start_poller() {
  pthread_t thread;
  pthread_create(&thread, nullptr, poll_thread, nullptr);
}

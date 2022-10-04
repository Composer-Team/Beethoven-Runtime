//
// Created by Chris Kjellqvist on 9/27/22.
//

#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <tuple>
#include <string>
#include <composer_verilator_server.h>
#include "data_server.h"


static void* data_server_f(void* stop) {
  FILE* f = fopen(data_server_file_name.c_str(), "w+");
  int fd_composer = fileno(f);
  bool *stop_cond = (bool*)stop;
  ftruncate(fd_composer, sizeof(comm_file));
  auto &addr = *(comm_file*)mmap(nullptr, sizeof(comm_file), PROT_READ | PROT_WRITE,
                    MAP_SHARED, fd_composer, 0);

  pthread_mutexattr_t attrs;
  pthread_mutexattr_init(&attrs);
  pthread_mutexattr_setpshared(&attrs, true);

  pthread_mutex_init(&addr.server_mut, &attrs);
  pthread_mutex_init(&addr.wait_for_request_process, &attrs);
  memset(addr.fname, 0, 512);
  addr.addr = 0;
  addr.fsize = 0;

  int req_num = 0;
  std::vector<std::pair<int, FILE*>> alloc;

  pthread_mutex_lock(&addr.server_mut);
  while(addr.fname[0] != 0 && !*stop_cond) {
    // get file name, descriptor, expand the file, and map it to address space
    auto fname = "/tmp/composer_file_" + std::to_string(req_num);
    FILE *nf = fopen(fname.c_str(), "w+");
    int nfd = fileno(nf);
    ftruncate(nfd, (off_t)addr.fsize);
    void *naddr = mmap(nullptr, addr.fsize, PROT_READ | PROT_WRITE,
                       MAP_SHARED, nfd, 0);
    //write response
    memcpy(addr.fname, fname.c_str(), fname.length());
    addr.addr = (uint64_t)naddr;
    // un-lock client to read response
    pthread_mutex_unlock(&addr.wait_for_request_process);
    // re-lock self to stall
    pthread_mutex_lock(&addr.server_mut);
  }

  return nullptr;
}

void data_server::start() {
  pthread_create(&thread, nullptr, data_server_f, &stop_cond);
}

void data_server::stop() {
  stop_cond = true;
  pthread_join(thread, nullptr);
}
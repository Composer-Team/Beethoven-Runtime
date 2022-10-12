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

#include <fcntl.h>

static comm_file *cf;

static void* data_server_f(void* stop) {
  int fd_composer = shm_open(data_server_file_name.c_str(), O_CREAT | O_RDWR, S_IWUSR | S_IRUSR);
  bool *stop_cond = (bool*)stop;
  ftruncate(fd_composer, sizeof(comm_file));
  auto &addr = *(comm_file*)mmap(nullptr, sizeof(comm_file), PROT_READ | PROT_WRITE,
                    MAP_SHARED, fd_composer, 0);
  cf = &addr;

  pthread_mutexattr_t attrs;
  pthread_mutexattr_init(&attrs);
  pthread_mutexattr_setpshared(&attrs, PTHREAD_PROCESS_SHARED);

  pthread_mutex_init(&addr.server_mut, &attrs);
  pthread_mutex_init(&addr.wait_for_request_process, &attrs);
  memset(addr.fname, 0, 512);
  addr.addr = 0;
  addr.fsize = 0;

  int req_num = 0;
  pthread_mutex_lock(&addr.server_mut);
  pthread_mutex_lock(&addr.server_mut);
  while(addr.fname[0] != 0 && !*stop_cond) {
    // get file name, descriptor, expand the file, and map it to address space
    auto fname = "/tmp/composer_file_" + std::to_string(req_num);
    int nfd = shm_open(fname.c_str(), O_CREAT | O_RDWR, S_IWUSR | S_IRUSR);
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
  pthread_mutex_unlock(&cf->server_mut);
  pthread_join(thread, nullptr);
}
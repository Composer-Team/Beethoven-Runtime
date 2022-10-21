//
// Created by Chris Kjellqvist on 9/27/22.
//

#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <string>

#include <composer_allocator_declaration.h>
#include <composer/verilator_server.h>

#include "data_server.h"

#include <fcntl.h>

using namespace composer;

static composer::data_server_file *cf;

static void* data_server_f(void* server) {
  auto *ds = (data_server*)server;

  int fd_composer = shm_open(data_server_file_name.c_str(), O_CREAT | O_RDWR, S_IWUSR | S_IRUSR);
  ftruncate(fd_composer, sizeof(data_server_file));
  auto &addr = *(data_server_file*)mmap(nullptr, sizeof(data_server_file), PROT_READ | PROT_WRITE,
                    MAP_SHARED, fd_composer, 0);
  cf = &addr;

  pthread_mutexattr_t attrs;
  pthread_mutexattr_init(&attrs);
  pthread_mutexattr_setpshared(&attrs, PTHREAD_PROCESS_SHARED);

  pthread_mutex_init(&addr.server_mut, &attrs);
  pthread_mutex_init(&addr.data_cmd_recieve_resp_lock, &attrs);
  pthread_mutex_init(&addr.data_cmd_send_lock, &attrs);
  memset(addr.fname, 0, 512);
  addr.op_argument = 0;

  fprintf(stderr, "Constructing allocator\n");
  auto allocator = new composer_allocator();
  fprintf(stderr, "Constructed allocator\n");

  int req_num = 0;
  pthread_mutex_lock(&addr.data_cmd_recieve_resp_lock);
  pthread_mutex_lock(&addr.server_mut);
  pthread_mutex_lock(&addr.server_mut);
  while(!ds->isStopCond()) {
    // get file name, descriptor, expand the file, and map it to address space
    printf("recieved dserver command\n"); fflush(stdout);
    switch (addr.operation) {
      case data_server_op::ALLOC: {
        auto fname = "/tmp/composer_file_" + std::to_string(req_num);
        int nfd = shm_open(fname.c_str(), O_CREAT | O_RDWR, S_IWUSR | S_IRUSR);
        ftruncate(nfd, (off_t) addr.op_argument);
        void *naddr = mmap(nullptr, addr.op_argument, PROT_READ | PROT_WRITE,
                           MAP_SHARED, nfd, 0);
        fprintf(stderr, "Got data cmd and made new filed\n");
        //write response
        // copy file name to response field
        strcpy(addr.fname, fname.c_str());
        printf("fname is '%s'\n", addr.fname); fflush(stdout);
        // allocate memory
        auto fpga_addr = allocator->remote_alloc(addr.op_argument);
        // add mapping in server
        printf("completed allocation\n");fflush(stdout);
        ds->at.add_mapping(fpga_addr.getFpgaAddr(), addr.op_argument, naddr);
        // return fpga address
        printf("added mapping\n"); fflush(stdout);
        addr.op_argument = fpga_addr.getFpgaAddr();
        printf("added arg to addr: %llu \n", addr.op_argument); fflush(stdout);
        break;
      }
      case data_server_op::FREE: {
        allocator->remote_free(composer::remote_ptr(addr.op_argument, 0));
        ds->at.remove_mapping(addr.op_argument);
        break;
      }
    }
    // un-lock client to read response
    pthread_mutex_unlock(&addr.data_cmd_recieve_resp_lock);
    printf("unlocked recieve\n"); fflush(stdout);
    // re-lock self to stall
    pthread_mutex_lock(&addr.server_mut);
  }

  delete allocator;
  return nullptr;
}

void data_server::start() {
  pthread_create(&thread, nullptr, data_server_f, this);
}

void data_server::stop() {
  stop_cond = true;
  pthread_mutex_unlock(&cf->server_mut);
  pthread_join(thread, nullptr);
}

bool data_server::isStopCond() const {
  return stop_cond;
}

void *address_translator::translate(uint64_t fp_addr) {
  printf("translating %llx\n", fp_addr);
  auto it = mappings.begin();
  while (it != mappings.end()) {
    if (it->fpga_addr <= fp_addr and it->fpga_addr + it->mapping_length > fp_addr) {
      break;
    }
    it ++;
  }
  if (it == mappings.end()) {
    fprintf(stderr, "BAD ADDRESS IN TRANSLATION FROM FPGA -> CPU: %llx\n", fp_addr);
    exit(1);
  }
  if (it->fpga_addr + it->mapping_length <= fp_addr) {
    fprintf(stderr, "ADDRESS IS OUT OF BOUNDS FROM FPGA -> CPU\n");
    exit(1);
  }
  return (char *) it->cpu_addr + (fp_addr - it->fpga_addr);
}

void address_translator::add_mapping(uint64_t fpga_addr, uint64_t mapping_length, void *cpu_addr) {
  mappings.emplace(fpga_addr, cpu_addr, mapping_length);
}

void address_translator::remove_mapping(uint64_t fpga_addr) {
  addr_pair a(fpga_addr, nullptr, 0);
  auto it = mappings.find(a);
  if (it == mappings.end()) {
    printf("ERROR - could not remove mapping in data server because could not find address...\n");
    exit(1);
  }
  mappings.erase(it);
}

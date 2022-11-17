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

#include "../include/data_server.h"

#include <fcntl.h>

#ifdef SIM
extern bool kill_sig;
#endif

#ifdef FPGA
#include "fpga_utils.h"
#endif

using namespace composer;

static composer::data_server_file *cf;
address_translator at;

[[noreturn]] static void *data_server_f(void *server) {
  auto *ds = (data_server *) server;

  int fd_composer = shm_open(data_server_file_name.c_str(), O_CREAT | O_RDWR, S_IROTH | S_IWOTH);
  ftruncate(fd_composer, sizeof(data_server_file));
  auto &addr = *(data_server_file *) mmap(nullptr, sizeof(data_server_file), PROT_READ | PROT_WRITE,
                                          MAP_SHARED, fd_composer, 0);
  cf = &addr;

  fprintf(stderr, "Constructing allocator\n");
  auto allocator = new composer_allocator();
  fprintf(stderr, "Constructed allocator\n");

  int req_num = 0;
  pthread_mutex_lock(&addr.data_cmd_recieve_resp_lock);
  pthread_mutex_lock(&addr.server_mut);
  pthread_mutex_lock(&addr.server_mut);
  data_server_file::init(addr);
  while (true) {
    printf("data server got cmd\n"); fflush(stdout);
    // get file name, descriptor, expand the file, and map it to address space
    switch (addr.operation) {
      case data_server_op::ALLOC: {
        auto fname = "/composer_file_" + std::to_string(req_num);
        req_num++;
        int nfd = shm_open(fname.c_str(), O_CREAT | O_RDWR, S_IWUSR | S_IRUSR);
        ftruncate(nfd, (off_t) addr.op_argument);
        void *naddr = mmap(nullptr, addr.op_argument, PROT_READ | PROT_WRITE,
                           MAP_SHARED, nfd, 0);
        //write response
        // copy file name to response field
        strcpy(addr.fname, fname.c_str());
        // allocate memory
        auto fpga_addr = allocator->malloc(addr.op_argument);
        // add mapping in server
        at.add_mapping(fpga_addr.getFpgaAddr(), addr.op_argument, naddr);
        // return fpga address
        addr.op_argument = fpga_addr.getFpgaAddr();
        break;
      }
      case data_server_op::FREE:
        allocator->free(composer::remote_ptr(addr.op_argument, 0));
        at.remove_mapping(addr.op_argument);
        break;
#ifdef SIM
        case data_server_op::MOVE_TO_FPGA:
        case data_server_op::MOVE_FROM_FPGA:
          // noop
          break;
#endif
#ifdef FPGA
      case data_server_op::MOVE_FROM_FPGA: {
        auto *dst = (uint8_t *) addr.op_argument;
        wrapper_fpga_dma_burst_read(xdma_read_fd, dst, addr.op3_argument, addr.op2_argument);
        break;
      }
      case data_server_op::MOVE_TO_FPGA: {
        auto *src = (uint8_t *) addr.op2_argument;
        printf("trying to transfer\n"); fflush(stdout);
        wrapper_fpga_dma_burst_write(xdma_write_fd, src, addr.op3_argument, addr.op2_argument);
        printf("finished transfering\n"); fflush(stdout);
        break;
      }
#endif
    }
    // un-lock client to read response
    pthread_mutex_unlock(&addr.data_cmd_recieve_resp_lock);
    // re-lock self to stall
    pthread_mutex_lock(&addr.server_mut);
  }

  delete allocator;
  return nullptr;
}

void data_server::start() {
  pthread_t thread;
  pthread_create(&thread, nullptr, data_server_f, nullptr);
}

void *address_translator::translate(uint64_t fp_addr) {
  auto it = mappings.begin();
  while (it != mappings.end()) {
    if (it->fpga_addr <= fp_addr and it->fpga_addr + it->mapping_length > fp_addr) {
      break;
    }
    it++;
  }
  if (it == mappings.end()) {
    fprintf(stderr, "BAD ADDRESS IN TRANSLATION FROM FPGA -> CPU: %llx\n", fp_addr);
#ifdef SIM
    kill_sig = true;
    return nullptr;
#else
    exit(1);
#endif
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

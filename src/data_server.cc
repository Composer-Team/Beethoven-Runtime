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
#include "verilator.h"
#include "verilated_vcd_c.h"

#endif

#ifdef FPGA
#include "fpga_utils.h"
#endif

using namespace composer;

static composer::data_server_file *cf;
address_translator at;

uint64_t f1_hack_addr(uint64_t addr) {
  // 1 2 0 3
  // 0 1 2 3
  uint64_t dimm = addr >> 34;
  switch (dimm) {
    case 0:
      return 0x800000000 | (addr & 0x3ffffffff);
    case 1:
      return addr & 0x3ffffffff;
    case 2:
      return 0x100000000 | (addr & 0x3ffffffff);
    default:
      return addr;
  }
}

[[noreturn]] static void *data_server_f(void *server) {
  auto *ds = (data_server *) server;

  int fd_composer = shm_open(data_server_file_name.c_str(), O_CREAT | O_RDWR, file_access_flags);
  ftruncate(fd_composer, sizeof(data_server_file));
  auto &addr = *(data_server_file *) mmap(nullptr, sizeof(data_server_file), file_access_prots,
                                          MAP_SHARED, fd_composer, 0);
  cf = &addr;

  fprintf(stderr, "Constructing allocator\n");
  auto allocator = new composer_allocator();
  fprintf(stderr, "Constructed allocator\n");
  data_server_file::init(addr);

  int req_num = 0;
  pthread_mutex_lock(&addr.data_cmd_recieve_resp_lock);
  pthread_mutex_lock(&addr.server_mut);
  pthread_mutex_lock(&addr.server_mut);
  while (true) {
//    printf("data server got cmd\n"); fflush(stdout);
    // get file name, descriptor, expand the file, and map it to address space
    switch (addr.operation) {
      case data_server_op::ALLOC: {
        auto fname = "/composer_file_" + std::to_string(req_num);
        req_num++;
        int nfd = shm_open(fname.c_str(), O_CREAT | O_RDWR, file_access_flags);
        if (nfd < 0) {
          printf("Failed to open shared memory segment: %s\n", strerror(errno));
          throw std::exception();
        }
	/*
        int rc = ftruncate(nfd, (off_t) addr.op_argument);
        if (rc) {
          printf("Failed to truncate! - %d, %d, %llu\t %s\n", rc, nfd, (off_t)addr.op_argument, strerror(errno));
          throw std::exception();
        }
	*/
        void *naddr = mmap(nullptr, addr.op_argument, file_access_prots, MAP_SHARED, nfd, 0);

        if (naddr == nullptr) {
          printf("Failed to mmap address! - %s\n", strerror(errno));
        }
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
#if defined(SIM)
        case data_server_op::MOVE_TO_FPGA:
        case data_server_op::MOVE_FROM_FPGA:
          // noop
          break;
#elif defined (FPGA)
      case data_server_op::MOVE_FROM_FPGA: {
        auto *dst = (uint8_t *) addr.op_argument;
        wrapper_fpga_dma_burst_read(xdma_read_fd, dst, addr.op3_argument, addr.op2_argument);
	for (int i = 0; i < addr.op3_argument; ++i) printf("%d", dst[i]);
        break;
      }
      case data_server_op::MOVE_TO_FPGA: {
        auto *src = (uint8_t *) addr.op2_argument;
//        printf("trying to transfer\n"); fflush(stdout);
        wrapper_fpga_dma_burst_write(xdma_write_fd, src, addr.op3_argument, addr.op2_argument);
//        printf("finished transfering\n"); fflush(stdout);
        break;
      }
#else
#error("Doesn't appear that we're covering all cases inside data server")
#endif
    }
    // un-lock client to read response
    pthread_mutex_unlock(&addr.data_cmd_recieve_resp_lock);
    // re-lock self to stall
    pthread_mutex_lock(&addr.server_mut);
  }
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
    tfp->close();
#endif
    throw std::exception();
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

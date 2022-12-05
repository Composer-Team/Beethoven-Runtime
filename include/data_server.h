//
// Created by Chris Kjellqvist on 9/28/22.
//

#ifndef COMPOSER_VERILATOR_DATA_SERVER_H
#define COMPOSER_VERILATOR_DATA_SERVER_H

#include <queue>
#include <set>
#include <composer_allocator_declaration.h>

#if defined(SIM) && defined(COMPOSER_HAS_DMA)
#include <pthread.h>
extern pthread_mutex_t dma_lock;
extern pthread_mutex_t dma_wait_lock;
extern bool dma_valid;
extern char *dma_ptr;
extern uint64_t dma_fpga_addr;
extern size_t dma_len;
extern bool dma_write;
extern bool dma_in_progress;



#endif
struct address_translator {
  struct addr_pair {
    uint64_t fpga_addr;
    uint64_t mapping_length;
    void* cpu_addr;

    explicit addr_pair(uint64_t fpgaAddr, void *cpuAddr, uint64_t map_length):
    fpga_addr(fpgaAddr), cpu_addr(cpuAddr), mapping_length(map_length) {}

    bool operator<(const addr_pair &other) const {
      return fpga_addr < other.fpga_addr;
    }
  };
  std::set<addr_pair> mappings;

  void* translate(uint64_t fp_addr);
  std::pair<void*, uint64_t> get_mapping(uint64_t fpga_addr);
  void add_mapping(uint64_t fpga_addr, uint64_t mapping_length, void * cpu_addr);
  void remove_mapping(uint64_t fpga_addr);
};

extern address_translator at;

struct data_server {
  static void start();
};


struct memory_transaction {
  char *addr;
  int size;
  int len;
  int progress;
  int id;
  bool fixed;
  uint64_t fpga_addr;
  int dram_tx_len = 0;
  int dram_tx_progress = 0;

  memory_transaction(char *addr,
                     int size,
                     int len,
                     int progress,
                     bool fixed,
                     int id,
                     uint64_t fpga_addr) :
          addr(addr),
          size(size),
          len(len),
          progress(progress),
          fixed(fixed),
          id(id),
          fpga_addr(fpga_addr){}

  memory_transaction() = delete;
};

#endif //COMPOSER_VERILATOR_DATA_SERVER_H

//
// Created by Chris Kjellqvist on 9/28/22.
//

#ifndef COMPOSER_VERILATOR_DATA_SERVER_H
#define COMPOSER_VERILATOR_DATA_SERVER_H

#include <queue>
#include <set>

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
  void add_mapping(uint64_t fpga_addr, uint64_t mapping_length, void * cpu_addr);
  void remove_mapping(uint64_t fpga_addr);

};

class data_server {
  pthread_t thread;
  bool stop_cond = false;
public:
  address_translator at;
  void start();
  void stop();

  [[nodiscard]] bool isStopCond() const;
  data_server() {
    at.mappings.clear();
  }
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

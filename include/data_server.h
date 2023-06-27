//
// Created by Chris Kjellqvist on 9/28/22.
//

#ifndef COMPOSER_VERILATOR_DATA_SERVER_H
#define COMPOSER_VERILATOR_DATA_SERVER_H

#include <composer_allocator_declaration.h>
#include <queue>
#include <set>

#if defined(SIM)
#if defined(COMPOSER_HAS_DMA)
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

extern int DDR_BUS_WIDTH_BITS;
extern int DDR_BUS_WIDTH_BYTES;
extern int axi_ddr_bus_multiplicity;
extern int DDR_BUS_BURST_LENGTH;
// = (DATA_BUS_WIDTH / 8) / DDR_BUS_WIDTH_BYTES
struct memory_transaction {
  char *addr;
  int size;
  int len;
  int axi_bus_beats_progress;
  int id;
  bool fixed;
  uint64_t fpga_addr;
  int dram_tx_len_bus_beats;
  int dram_tx_axi_enqueue_progress = 0;
  int dram_tx_load_progress = 0;
  bool can_be_last = true;

  std::vector<bool> ddr_bus_beats_retrieved;

  memory_transaction(char *addr,
                     int size,
                     int len,
                     int progress,
                     bool fixed,
                     int id,
                     uint64_t fpga_addr) : addr(addr),
                                           size(size),
                                           len(len),
                                           axi_bus_beats_progress(progress),
                                           fixed(fixed),
                                           id(id),
                                           fpga_addr(fpga_addr) {
    dram_tx_len_bus_beats = (len * size) / DDR_BUS_WIDTH_BYTES;
    if (dram_tx_len_bus_beats == 0) dram_tx_len_bus_beats = 1;
    for (int i = 0; i < dram_tx_len_bus_beats; ++i) {
      ddr_bus_beats_retrieved.emplace_back(false);
    }
  }

  memory_transaction() = delete;

  int dramsim_hasBeatReady() {
    assert(DDR_BUS_WIDTH_BYTES <= (DATA_BUS_WIDTH / 8));
    if (axi_bus_beats_progress == axi_bus_beats_length()) return false;
    for (int i = 0; i < axi_ddr_bus_multiplicity; ++i) {
      if (!ddr_bus_beats_retrieved[axi_bus_beats_progress * axi_ddr_bus_multiplicity + i]) return false;
    }
    return ddr_bus_beats_retrieved[axi_bus_beats_progress * axi_ddr_bus_multiplicity];
  }

  [[nodiscard]] int bankId() const {
    return int(fpga_addr >> 12);
  }

  [[nodiscard]] bool dramsim_tx_finished() const {
    return dram_tx_axi_enqueue_progress * DDR_BUS_BURST_LENGTH >= dram_tx_len_bus_beats;
  }

  [[nodiscard]] int axi_bus_beats_length() const {
    return len * size / (DATA_BUS_WIDTH / 8);
  }
};

#endif

struct data_server {
  static void start();
};

struct address_translator {
  struct addr_pair {
    uint64_t fpga_addr;
    uint64_t mapping_length;
    void *cpu_addr;

    explicit addr_pair(uint64_t fpgaAddr, void *cpuAddr, uint64_t map_length) : fpga_addr(fpgaAddr), cpu_addr(cpuAddr), mapping_length(map_length) {}

    bool operator<(const addr_pair &other) const {
      return fpga_addr < other.fpga_addr;
    }
  };
  std::set<addr_pair> mappings;

  [[nodiscard]] void *translate(uint64_t fp_addr) const;
  [[nodiscard]] std::pair<void *, uint64_t> get_mapping(uint64_t fpga_addr) const;
  void add_mapping(uint64_t fpga_addr, uint64_t mapping_length, void *cpu_addr);
  void remove_mapping(uint64_t fpga_addr);
};

extern address_translator at;


#endif//COMPOSER_VERILATOR_DATA_SERVER_H

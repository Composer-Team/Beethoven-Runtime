//
// Created by Chris Kjellqvist on 9/28/22.
//

#ifndef COMPOSER_VERILATOR_DATA_SERVER_H
#define COMPOSER_VERILATOR_DATA_SERVER_H

#include <verilated.h>
#include <queue>

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
template<typename addr_ty>
struct v_address_channel {
  CData *ready = nullptr;
  CData *valid = nullptr;
  CData *id = nullptr;
  CData *size = nullptr;
  CData *burst = nullptr;
  addr_ty *addr = nullptr;
  CData *len = nullptr;

  explicit v_address_channel(CData *ready,
                             CData *valid,
                             CData *id,
                             CData *size ,
                             CData *burst,
                             addr_ty *addr,
                             CData *len) :
          ready(ready),
          valid(valid),
          id(id),
          size(size),
          burst(burst),
          addr(addr),
          len(len) {

  }
};

struct data_channel {
  CData *ready;
  CData *valid;
  VlWide<16> *data;
  CData *id;
  QData *strobe;
  CData *last;

  explicit data_channel(CData *ready,
                        CData *valid,
                        VlWide<16> *data,
                        QData *strobe,
                        CData *last,
                        CData *id) :
          ready(ready),
          valid(valid),
          data(data),
          strobe(strobe),
          last(last),
          id(id) {}
};

struct response_channel {
  CData *ready;
  CData *valid;
  CData *id;

  std::queue<int> send_ids;
  int to_enqueue = -1;
  explicit response_channel(CData *ready = nullptr,
                            CData *valid = nullptr,
                            CData *id = nullptr) :
          ready(ready),
          valid(valid),
          id(id) {}
};

template<typename addr_ty>
struct mem_interface {
  v_address_channel<addr_ty> *aw = nullptr;
  v_address_channel<addr_ty> *ar = nullptr;
  data_channel *w = nullptr;
  data_channel *r = nullptr;
  response_channel *b = nullptr;
  std::queue<memory_transaction*> write_transactions;
  std::queue<memory_transaction*> read_transactions;
  int current_read_channel_contents = -1;
};

#endif //COMPOSER_VERILATOR_DATA_SERVER_H

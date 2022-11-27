//
// Created by Chris Kjellqvist on 10/29/22.
//

#ifndef COMPOSER_VERILATOR_VERILATOR_H
#define COMPOSER_VERILATOR_VERILATOR_H
#include <verilated.h>
#ifdef USE_DRAMSIM
#include "dram_system.h"
#endif
extern VerilatedVcdC *tfp;

void run_verilator();
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
  CData * ready;
   CData *valid;
   VlWide<16> *data;
   CData * const id;
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
  int id;
#ifdef USE_DRAMSIM
  std::map<uint64_t, std::queue<memory_transaction *> *> in_flight_reads;
  std::map<uint64_t, std::queue<memory_transaction *> *> in_flight_writes;
  dramsim3::JedecDRAMSystem *mem_sys;

  memory_transaction* to_enqueue_read = nullptr;
  memory_transaction* to_enqueue_write = nullptr;
  int r_progress = 0;
  int w_progress = 0;
#endif

  int current_read_channel_contents = -1;
};

#endif //COMPOSER_VERILATOR_VERILATOR_H

//
// Created by Chris Kjellqvist on 9/28/22.
//

#ifndef COMPOSER_VERILATOR_DATA_SERVER_H
#define COMPOSER_VERILATOR_DATA_SERVER_H

#include <verilated.h>
#include <queue>


class data_server {
  pthread_t thread;
  bool stop_cond;
public:
  void start();
  void stop();
};


struct memory_transaction {
  char *addr;
  int size;
  int len;
  int progress;
  int id;
  bool fixed;

  memory_transaction(char *addr,
                     int size,
                     int len,
                     int progress,
                     bool fixed,
                     int id) :
          addr(addr),
          size(size),
          len(len),
          progress(progress),
          fixed(fixed),
          id(id) {}

  memory_transaction() = delete;
};
template<typename addr_ty>
struct address_channel {
  CData *ready = nullptr;
  CData *valid = nullptr;
  CData *id = nullptr;
  CData *size = nullptr;
  CData *burst = nullptr;
  addr_ty *addr = nullptr;
  CData *len = nullptr;

  explicit address_channel(CData *ready = nullptr,
                           CData *valid = nullptr,
                           CData *id = nullptr,
                           CData *size = nullptr,
                           CData *burst = nullptr,
                           addr_ty *addr = nullptr,
                           CData *len = nullptr) :
          ready(ready),
          valid(valid),
          id(id),
          size(size),
          burst(burst),
          addr(addr),
          len(len) {}
};

struct data_channel {
  CData *ready;
  CData *valid;
  VlWide<16> *data;
  CData *id;
  QData *strobe;
  CData *last;

  explicit data_channel(CData *ready = nullptr,
                        CData *valid = nullptr,
                        VlWide<16> *data = nullptr,
                        QData *strobe = nullptr,
                        CData *last = nullptr,
                        CData *id = nullptr) :
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
  explicit response_channel(CData *ready = nullptr,
                            CData *valid = nullptr,
                            CData *id = nullptr) :
          ready(ready),
          valid(valid),
          id(id) {}
};

template<typename addr_ty>
struct mem_interface {
  address_channel<addr_ty> *aw = nullptr;
  address_channel<addr_ty> *ar = nullptr;
  data_channel *w = nullptr;
  data_channel *r = nullptr;
  response_channel *b = nullptr;
  std::queue<memory_transaction*> write_transactions;
  std::queue<memory_transaction*> read_transactions;
  int current_read_channel_contents = -1;
};

#endif //COMPOSER_VERILATOR_DATA_SERVER_H

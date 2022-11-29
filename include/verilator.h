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
class v_address_channel {
  CData *ready = nullptr;
  CData *valid = nullptr;
  CData *id = nullptr;
  CData *size = nullptr;
  CData *burst = nullptr;
  addr_ty *addr = nullptr;
  CData *len = nullptr;
public:

  explicit v_address_channel(CData &ready,
                             CData &valid,
                             CData &id,
                             CData &size ,
                             CData &burst,
                             addr_ty &addr,
                             CData &len) :
          ready(&ready),
          valid(&valid),
          id(&id),
          size(&size),
          burst(&burst),
          addr(&addr),
          len(&len) {}

  [[nodiscard]] CData getReady() const {
    return *ready;
  }

  void setReady(CData ready) {
    *v_address_channel::ready = ready;
  }

  CData getValid() const {
    return *valid;
  }

  void setValid(CData valid) {
    *v_address_channel::valid = valid;
  }

  CData getId() const {
    return *id;
  }

  void setId(CData id) {
    *v_address_channel::id = id;
  }

  CData getSize() const {
    return *size;
  }

  void setSize(CData size) {
    *v_address_channel::size = size;
  }

  CData getBurst() const {
    return *burst;
  }

  void setBurst(CData burst) {
    *v_address_channel::burst = burst;
  }

  addr_ty getAddr() const {
    return *addr;
  }

  void setAddr(addr_ty addr) {
    *v_address_channel::addr = addr;
  }

  CData getLen() const {
    return *len;
  }

  void setLen(CData len) {
    *v_address_channel::len = len;
  }
};

class data_channel {
  CData * ready;
   CData *valid;
   VlWide<16> *data;
   CData * id;
   QData *strobe;
   CData *last;
public:
  explicit data_channel(CData &ready,
                        CData &valid,
                        VlWide<16> &data,
                        QData *strobe,
                        CData &last,
                        CData *id) :
          ready(&ready),
          valid(&valid),
          data(&data),
          strobe(strobe),
          last(&last),
          id(id) {}

  CData getReady() const {
    return *ready;
  }

  void setReady(CData ready) {
    *data_channel::ready = ready;
  }

  CData getValid() const {
    return *valid;
  }

  void setValid(CData valid) {
    *data_channel::valid = valid;
  }

  VlWide<16>& getData() const {
    return *data;
  }

  CData getId() const {
    return *id;
  }

  void setId(CData id) {
    *data_channel::id = id;
  }

  QData getStrobe() const {
    return *strobe;
  }

  void setStrobe(QData strobe) {
    *data_channel::strobe = strobe;
  }

  CData getLast() const {
    return *last;
  }

  void setLast(CData last) {
    *data_channel::last = last;
  }
};

class response_channel {
  CData *ready;
  CData *valid;
  CData *id;
public:

  std::queue<int> send_ids;
  int to_enqueue = -1;
  explicit response_channel(CData &ready,
                            CData &valid,
                            CData &id) :
          ready(&ready),
          valid(&valid),
          id(&id) {}

  CData getReady() const {
    return *ready;
  }

  void setReady(CData ready) {
    *response_channel::ready = ready;
  }

  CData getValid() const {
    return *valid;
  }

  void setValid(CData valid) {
    *response_channel::valid = valid;
  }

  CData getId() const {
    return *id;
  }

  void setId(CData id) {
    *response_channel::id = id;
  }

  const std::queue<int> &getSendIds() const {
    return send_ids;
  }

  void setSendIds(const std::queue<int> &sendIds) {
    send_ids = sendIds;
  }

  int getToEnqueue() const {
    return to_enqueue;
  }

  void setToEnqueue(int toEnqueue) {
    to_enqueue = toEnqueue;
  }
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

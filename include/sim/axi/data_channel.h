//
// Created by Christopher Kjellqvist on 8/5/24.
//

#ifndef BEETHOVENRUNTIME_DATA_CHANNEL_H
#define BEETHOVENRUNTIME_DATA_CHANNEL_H

#include <cinttypes>

template<typename id_t, typename strb_t, typename byte_t, typename data_t>
struct data_channel {
  byte_t ready;
  byte_t valid;
  data_t data;
  strb_t strb;
  id_t id;
  byte_t last;

public:

  ~data_channel() = default;

  data_channel() = default;

  void init(byte_t ready,
            byte_t valid,
            byte_t last,
            id_t id,
            strb_t strb,
            data_t data) {
    data_channel::ready = ready;
    data_channel::valid = valid;
    data_channel::last = last;
    data_channel::id = id;
    data_channel::strb = strb;
  }

  void setData(uint8_t *data) {
    data_channel::data.set(data);
  }

  void setData(uint8_t payload, uint32_t idx) {
    data.set(payload, idx);
  }

  uint8_t getReady() const {
    return ready.get(0);
  }

  void setReady(uint8_t ready) {
    this->ready.set(ready);
  }

  uint8_t getValid() const {
    return valid.get(0);
  }

  void setValid(uint8_t valid) {
    this->valid.set(valid);
  }

  std::unique_ptr<uint8_t[]> getData() const {
    return data.get();
  }

  uint64_t getId() const {
    return id.get();
  }

  void setId(uint64_t id) {
    data_channel::id.set(id);
  }

  uint8_t getLast() const {
    return last.get();
  }

  void setLast(uint8_t last) {
    data_channel::last.set(last);
  }

  bool fire() {
    return ready.get() && valid.get();
  }

  bool getStrb(int i) const {
    int chunk = i / 8;
    return strb.get(chunk) >> (i % 8);
  }
};

#endif //BEETHOVENRUNTIME_DATA_CHANNEL_H

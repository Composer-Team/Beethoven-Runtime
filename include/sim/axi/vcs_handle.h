//
// Created by Christopher Kjellqvist on 8/5/24.
//

#ifndef BEETHOVENRUNTIME_VCS_HANDLE_H
#define BEETHOVENRUNTIME_VCS_HANDLE_H

#include "vpi_user.h"

class VCSShortHandle {
  vpiHandle handle;
public:
  VCSShortHandle(vpiHandle handle) {
    this->handle = handle;
    if ((vpi_get(vpiSize, handle) >> 5) > 1) {
      std::cerr << "value is too big, quitting";
      exit(1);
    }
  }

  VCSShortHandle() = default;

  [[nodiscard]] uint32_t get() const {
    s_vpi_value value;
    value.format = vpiIntVal;
    vpi_get_value(handle, &value);
    return value.value.integer;
  }

  [[nodiscard]] uint32_t get(int chunk) const {
    s_vpi_value value;
    value.format = vpiVectorVal;
    vpi_get_value(handle, &value);
    return value.value.vector[chunk].aval;
  }

  void set(int32_t val) const {
    s_vpi_value value;
    value.format = vpiIntVal;
    value.value.integer = val;
    vpi_put_value(handle, &value, nullptr, vpiNoDelay);
  }

  void set(const uint8_t &payload, uint32_t idx) const {
    // first, get the payload
    s_vpi_value value;
    value.format = vpiIntVal;
    vpi_get_value(handle, &value);
    // then, set the payload
    value.value.integer = (value.value.integer & ~(0xFF << (idx * 8))) | ((uint32_t)(payload) << (idx * 8));
    vpi_put_value(handle, &value, nullptr, vpiNoDelay);
  }

};

class VCSLongHandle {
  vpiHandle handle;
  int nchunks;
public:
  VCSLongHandle(vpiHandle handle) {
    this->handle = handle;
    nchunks = vpi_get(vpiSize, handle) >> 5; // number of 32-bit chunks
  }

  VCSLongHandle() = default;

  [[nodiscard]] uint32_t get(int idx) const {
    if (nchunks == 1) {
      s_vpi_value value;
      value.format = vpiIntVal;
      vpi_get_value(handle, &value);
      return value.value.integer;
    } else {
      // get the full payload and only return the requested chunk
      s_vpi_value value;
      value.format = vpiVectorVal;
      vpi_get_value(handle, &value);
      return value.value.vector[idx].aval;
    }
  }

  [[nodiscard]] std::unique_ptr<uint8_t[]> get() const {
    s_vpi_value value;
    value.format = vpiVectorVal;
    vpi_get_value(handle, &value);
    std::unique_ptr<uint8_t[]> ret(new uint8_t[nchunks]);
    for (int i = 0; i < nchunks; i++) {
      uint32_t payload = value.value.vector[i].aval;
      for (int j = 0; j < 4; ++j) {
        ret.get()[i * 4 + j] = (payload >> (j * 8)) & 0xFF;
      }
    }
    return std::move(ret);

  }

  void set(int32_t val) const {
    s_vpi_value value;
    value.format = vpiIntVal;
    value.value.integer = val;
    vpi_put_value(handle, &value, nullptr, vpiNoDelay);
  }

  void set(const uint32_t *val) const {
    s_vpi_value value;
    value.format = vpiVectorVal;
    printf("trying to write %d chunks\n", nchunks);
    auto vec = new s_vpi_vecval[nchunks];
    for (int i = 0; i < nchunks; i++) {
      vec[i].aval = val[i];
      vec[i].bval = 0;
    }
    value.value.vector = vec;
    vpi_put_value(handle, &value, nullptr, vpiNoDelay);
    delete[] vec;
  }

  void set(const uint32_t &payload, uint32_t chunk) const {
    //printf("called set with (%d) <- %x\n", chunk, payload); fflush(stdout);
    // first, get the payload
    s_vpi_value value;
    value.format = vpiVectorVal;
    vpi_get_value(handle, &value);
    // then, set the payload inside the correct chunk
    int chunkVal = value.value.vector[chunk].aval;
    value.value.vector[chunk].aval = payload; 
    value.value.vector[chunk].bval = 0;
    // now write back
    vpi_put_value(handle, &value, nullptr, vpiNoDelay);
  }
};

#endif //BEETHOVENRUNTIME_VCS_HANDLE_H

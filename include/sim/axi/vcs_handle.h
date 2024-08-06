//
// Created by Christopher Kjellqvist on 8/5/24.
//

#ifndef BEETHOVENRUNTIME_VCS_HANDLE_H
#define BEETHOVENRUNTIME_VCS_HANDLE_H

#include "vpi_user.h"

class VCSHandle {
  vpiHandle handle;
  int nchunks;
public:
  explicit VCSHandle(vpiHandle handle) {
    this->handle = handle;
    nchunks = vpi_get(vpiSize, handle) >> 5; // number of 32-bit chunks
  }

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
  [[nodiscard]] std::unique_ptr<uint32_t> get() const {
    s_vpi_value value;
    value.format = vpiVectorVal;
    vpi_get_value(handle, &value);
    std::unique_ptr<uint32_t> ret(new uint32_t[nchunks]);
    for (int i = 0; i < nchunks; i++) {
      ret.get()[i] = value.value.vector[i].aval;
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
    auto vec = new s_vpi_vecval[nchunks];
    for (int i = 0; i < nchunks; i++) {
      vec[i].aval = val[i];
      vec[i].bval = 0;
    }
    value.value.vector = vec;
    vpi_put_value(handle, &value, nullptr, vpiNoDelay);
    delete [] vec;
  }
};

#endif //BEETHOVENRUNTIME_VCS_HANDLE_H

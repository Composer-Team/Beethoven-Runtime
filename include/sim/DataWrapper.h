#ifndef DATAWRAPPER_H
#define DATAWRAPPER_H

template <typename T>
struct GetSetWrapper {
  T* ptr = nullptr;
  size_t l = sizeof(T);

  explicit GetSetWrapper(T &v) {
    ptr = &v;
  }

  GetSetWrapper() = default;

  T get(int idx) const {
    return ptr[idx];
  }

  T get() const {
    return this->get(0);
  }

  void set(uint32_t value) {
    if (sizeof(T) > 4) {
      memcpy(ptr, &value, 8);
    } else {
      *ptr = value;
    }
  }
};

template <typename T, int l>
struct GetSetDataWrapper {
  T* ptr = nullptr;

  explicit GetSetDataWrapper(void *v) {
    ptr = (T*)v;
  }
  GetSetDataWrapper() = default;

  T get(int idx) const {
    return ptr[idx];
  }

  std::unique_ptr<uint32_t[]> get() const {
    std::unique_ptr<uint32_t[]> alloc(new uint32_t[l]);
    memcpy(alloc.get(), ptr, l);
    return std::move(alloc);
  }

  void set(uint32_t payload, uint32_t idx) {
    uint32_t *dst = (uint32_t*)ptr;
    dst[idx] = payload;
  }

  void set(int32_t *value) {
    memcpy(ptr, value, l*4);
  }
};

#endif

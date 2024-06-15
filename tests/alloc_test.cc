//
// Created by Christopher Kjellqvist on 5/25/24.
//

#include <iostream>
#include "data_server.h"
#include <beethoven/alloc.h>
#include <random>

device_allocator device_allocator;

int main() {
  std::vector<unsigned long> sizes;
// for 4K up to 512MB by power of two, make allocations
  for (int i = 12; i <= 29; i++) {
    sizes.push_back(1 << i);
  }
  // shuffle sizes
  std::random_device rd;
  std::mt19937 g(rd());
  std::shuffle(sizes.begin(), sizes.end(), g);

  std::vector<uint64_t> ptrs;

  for (auto size : sizes) {
    auto ptr = device_allocator.malloc(size);
    ptrs.push_back(ptr);
  }

  // reshuffle ptrs
  std::shuffle(ptrs.begin(), ptrs.end(), g);
  // free them
  for (auto ptr : ptrs) {
    device_allocator.free(ptr);
  }


  return 0;
}
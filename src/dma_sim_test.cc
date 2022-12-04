//
// Created by Chris Kjellqvist on 12/3/22.
//
#include <Vcomposer.h>
#include <composer_allocator_declaration.h>
#include <verilated.h>
#include <verilated_vcd_c.h>
#include <ctime>

int main() {
  Verilated::traceEverOn(true);
  Vcomposer top;
  VerilatedVcdC tracer;
  unsigned long long time = 0;

  top.ocl_0_ar_valid = 0;
  top.ocl_0_aw_valid = 0;
  top.ocl_0_w_valid = 0;
  top.ocl_0_r_ready = 0;
  top.ocl_0_b_ready = 0;

  top.dma_ar_valid = 0;
  top.dma_aw_valid = 0;
  top.dma_w_valid = 0;
  top.dma_r_ready = 0;
  top.dma_b_ready = 0;

  top.mem_0_ar_ready = 0;
  top.mem_0_aw_ready = 0;
  top.mem_0_w_ready = 0;
  top.mem_0_r_valid = 0;
  top.mem_0_b_valid = 0;

  top.trace(&tracer, -1);
  tracer.open("trace_dramsim.vcd");
  // reset is active high for whatever reason
  top.reset = 1;
  for (int i = 0; i < 50; ++i) {
    top.clock = 1;
    top.eval();
    tracer.dump(time++);
    top.clock = 0;
    top.eval();
    tracer.dump(time++);
  }
  top.reset = 0;
  for (int i = 0; i < 50; ++i) {
    top.clock = 1;
    top.eval();
    tracer.dump(time++);
    top.clock = 0;
    top.eval();
    tracer.dump(time++);
  }


  // generate a random transaction and align it
  srand(17929);
  uint64_t addr = rand() & 0xFFFFFF00;
  top.dma_ar_valid = 1;
  top.dma_ar_bits_addr = addr;
  top.dma_ar_bits_qos = 0;
  top.dma_ar_bits_size = 6; // 2^6 bytes == 64 * 8 bits = 512 which is the bus width
  top.dma_ar_bits_len = 15;
  top.dma_ar_bits_id = 4;
  top.dma_ar_bits_lock = 0;
  top.dma_ar_bits_burst = 0;
  top.dma_ar_bits_prot = 0;

  top.clock = 1;
  top.eval();
  tracer.dump(time++);
  top.clock = 0;
  top.eval();
  tracer.dump(time++);

  while (!top.dma_ar_ready ) {
    top.clock = 1;
    top.eval();
    tracer.dump(time++);
    top.clock = 0;
    top.eval();
    tracer.dump(time++);

  }

  top.dma_ar_valid = 0;

  while (!top.mem_0_ar_valid) {
    top.clock = 1;
    top.eval();
    tracer.dump(time++);
    top.clock = 0;
    top.eval();
    tracer.dump(time++);
  }
  tracer.close();

}
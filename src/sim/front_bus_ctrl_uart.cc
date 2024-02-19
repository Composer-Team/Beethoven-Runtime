//
// Created by Chris Kjellqvist on 8/9/23.
//

#include "composer_allocator_declaration.h"
#include "sim/front_bus_ctrl_axi.h"
#include "sim/mem_ctrl.h"
#include <verilated_fst_c.h>

extern mem_ctrl::mem_interface<ComposerMemIDDtype> axi4_mems[NUM_DDR_CHANNELS];
#ifdef USE_VCD
extern VerilatedVcdC *tfp;
#else
extern VerilatedFstC *tfp;
#endif

static void sig_handle(int sig) {
  for (auto q: axi4_mems) {
    q.mem_sys->PrintEpochStats();
  }
  tfp->close();
  fprintf(stderr, "FST written!\n");
  fflush(stderr);
  exit(sig);
}

enum uart_state_t {
  IDLE, START_BAUD, START, BITS, BITS_BAUD, STOP
};

uart_state_t in_state = IDLE;
uart_state_t out_state = IDLE;
int baud_count_in = 0;
int baud_count_out = 0;

static int in_byte_progress;
static int out_byte_progress;
static int stop_progress;
static unsigned char out_byte = 0;

static bool test(unsigned char q, int idx) {
  return q & (1 << idx);
}

static void set(unsigned char &target, unsigned char src, int idx) {
  target = (src & (1 << idx)) >> idx;
}

void queue_uart(std::queue<unsigned char> &in_stream,
                std::queue<unsigned char> &out_stream,
                unsigned char &rxd,
                unsigned char &txd,
                int baud_div,
                char in_enable = true,
                char out_enable = true) {
  switch (in_state) {
    case IDLE:
      if (in_stream.size() && in_enable) {
        rxd = 0; // START
        in_byte_progress = 0;
        if (baud_div > 1)
          in_state = START;
        else
          in_state = BITS;
        baud_count_in++;
      }
      break;
    case START:
      if ((++baud_count_in) == baud_div) {
        set(rxd, in_stream.front(), in_byte_progress);
        in_byte_progress++;
        baud_count_in = 1;
        in_state = BITS_BAUD;
      }
      break;
    case BITS:
      set(rxd, in_stream.front(), in_byte_progress);
      in_byte_progress++;
      baud_count_in = 1;
      if (baud_div > 1)
        in_state = BITS_BAUD;
      else {
        if (in_byte_progress == 8) {
          in_state = STOP;
          in_stream.pop();
          baud_count_in = 0;
        } else {
          in_state = BITS;
        }
      }
      break;
    case BITS_BAUD:
      if (baud_count_in == baud_div-1) {
        baud_count_in = 0;
        in_state = BITS;
        if (in_byte_progress == 8) {
          in_state = STOP;
          in_stream.pop();
          in_byte_progress = 0;
          rxd = 1;
        }
      } else {
        baud_count_in++;
      }
      break;
    case STOP:
      if(++baud_count_in == 2 * baud_div) {
        in_state = IDLE;
        baud_count_in = 0;
      }
      break;
  }
  switch (out_state) {
    case IDLE:
      if (txd == 0 && out_enable) {
        baud_count_out = 1;
        out_state = START_BAUD;
      }
      break;
    case START_BAUD:
      if (++baud_count_out == baud_div) {
        out_state = BITS;
        out_byte_progress = 0;
        baud_count_out = 0;
      }
      break;
    case BITS:
      out_byte = (out_byte << 1) | txd;
      out_byte_progress++;
      baud_count_out = 1;
      out_state = BITS_BAUD;
      break;
    case BITS_BAUD:
      if (++baud_count_out == baud_div) {
        if (out_byte_progress == 8) {
          out_state = STOP;
        } else {
          out_state = BITS;
        }
      }
      baud_count_out = 0;
      break;
    case STOP:
      if (++baud_count_out == 2 * baud_div) {
        out_state = IDLE;
        out_stream.push(out_byte);
        out_byte = 0;
      }
      break;

  }
}

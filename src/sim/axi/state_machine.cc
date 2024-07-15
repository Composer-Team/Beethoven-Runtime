//
// Created by Christopher Kjellqvist on 7/8/24.
//

#include "sim/axi/state_machine.h"
#include "sim/verilator.h"
#include "trace/trace_read.h"

Trace *trace = nullptr;

enum TraceMachineState {
  IdleState,
  ReadAddressState,
  ReadState,
  WriteAddressState,
  WriteState,
  WriteResponse
};

TraceMachineState state = IdleState;

int time_in_idle = 0;

void trace_rising_edge_pre(BeethovenTop &top) {
  if (top.reset == active_reset) return;
  switch (state) {
    case IdleState:
      time_in_idle++;
      if (time_in_idle == 100) {
        if (trace->empty()) {
          sig_handle(0);
        } else {
          if (trace->front().ty == ReadConditionType)
            state = ReadAddressState;
          else
            state = WriteAddressState;
        }
      }
      break;
    case ReadAddressState:
      if (top.S00_AXI_arready && top.S00_AXI_arvalid) {
        state = ReadState;
      }
      break;
    case ReadState:
      if (top.S00_AXI_rready && top.S00_AXI_rvalid) {
        state = IdleState;
        time_in_idle = 0;
        if (trace->front().payload == top.S00_AXI_rdata) {
          trace->pop();
        }
      }
      break;
    case WriteAddressState:
      if (top.S00_AXI_awready && top.S00_AXI_awvalid) {
        state = WriteState;
      }
      break;
    case WriteState:
      if (top.S00_AXI_wready && top.S00_AXI_wvalid) {
        state = WriteResponse;
      }
      break;
    case WriteResponse:
      if (top.S00_AXI_bvalid && top.S00_AXI_bready) {
        trace->pop();
        state = IdleState;
        time_in_idle = 0;
      }
      break;
  }
}

void trace_rising_edge_post(BeethovenTop &top) {
  if (top.reset == active_reset) return;
  top.S00_AXI_bready = false;
  top.S00_AXI_awvalid = false;
  top.S00_AXI_arvalid = false;
  top.S00_AXI_wvalid = false;
  top.S00_AXI_rready = false;
  switch (state) {
    case IdleState:
      break;
    case WriteAddressState:
      top.S00_AXI_awvalid = true;
      top.S00_AXI_awaddr = trace->front().address;
      break;
    case WriteState:
      top.S00_AXI_wdata = trace->front().payload;
      top.S00_AXI_wstrb = 0xF;
      top.S00_AXI_wvalid = true;
      break;
    case ReadAddressState:
      top.S00_AXI_arvalid = true;
      top.S00_AXI_araddr = trace->front().address;
      break;
    case ReadState:
      top.S00_AXI_rready = true;
      break;
    case WriteResponse:
      top.S00_AXI_bready = true;
      break;
  }
}


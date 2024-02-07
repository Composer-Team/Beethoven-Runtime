//
// Created by Christopher Kjellqvist on 2/2/24.
//

#include "trace/trace_read.h"
#include "sim/verilator.h"

Trace *trace = nullptr;

void init_trace(const std::string &fname) {
  FILE *f = fopen(fname.c_str(), "r");
  // read f line by line
  if (f == nullptr) throw std::runtime_error("Could not open file");

  trace = new Trace {};

  char * line = nullptr;
  size_t huh;
  int nchar = getline(&line, &huh, f);
  // valid operations
  // read <address> <comparison_value>
  //    read should return true if you want the trace to continue
  // write <address> <value>
  uint32_t payloads[4];
  int payload_id;
  std::optional<TraceType> mode;
  while (nchar != -1) {
    line = strtok(line, " ");
    payload_id = 0;
    mode = {};
    bool comment = false;
    while (line && !comment) {
      if (strcmp(line, "read") == 0) {
        mode = ReadConditionType;
      } else if (strcmp(line, "write") == 0) {
        mode = WriteType;
      } else if (strcmp(line, "#") == 0) {
        comment = true;
      } else {
        payloads[payload_id] = std::strtol(line, nullptr, 0);
        payload_id++;
      }
      line = strtok(nullptr, " ");
    }
    if (mode.has_value()) {
      assert(payload_id == 2);
      trace->push(TraceUnit(*mode, payloads[0], payloads[1]));
    }
    nchar = getline(&line, &huh, f);
  }
  return;
}

enum TraceMachineState {
  IdleState,
  ReadAddressState,
  ReadState,
  WriteAddressState,
  WriteState,
  WriteResponse
};

static TraceMachineState state = IdleState;

int time_in_idle = 0;

void trace_rising_edge_pre(VComposerTop &top) {
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

void trace_rising_edge_post(VComposerTop &top) {
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
TraceUnit::TraceUnit(TraceType ty, uint64_t address, uint32_t payload) : ty(ty), address(address), payload(payload) {}

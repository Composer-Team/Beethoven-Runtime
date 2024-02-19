//
// Created by Christopher Kjellqvist on 2/2/24.
//

#ifndef COMPOSERRUNTIME_TRACE_READ_H
#define COMPOSERRUNTIME_TRACE_READ_H

#include "sim/front_bus_ctrl_axi.h"
#include <cinttypes>
#include <functional>
#include <queue>
#include <string>

enum TraceType {
  ReadConditionType,
  WriteType,
  Comment
};


struct TraceUnit {
  TraceType ty;
  uint64_t address;
  uint32_t payload;
  TraceUnit(TraceType ty, uint64_t address, uint32_t payload);
};


typedef std::queue<TraceUnit> Trace;

void init_trace(const std::string &fname);

void trace_rising_edge_pre(VComposerTop &top);
void trace_rising_edge_post(VComposerTop &top);



#endif//COMPOSERRUNTIME_TRACE_READ_H

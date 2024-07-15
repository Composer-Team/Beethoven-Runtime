//
// Created by Christopher Kjellqvist on 7/8/24.
//

#ifndef BEETHOVENRUNTIME_STATE_MACHINE_H
#define BEETHOVENRUNTIME_STATE_MACHINE_H
#include "BeethovenTop.h"

enum cmd_transfer_state {
  CMD_INACTIVE,
  CMD_BITS_WRITE_ADDR,
  CMD_BITS_WRITE_DAT,
  CMD_BITS_WRITE_B,
  CMD_VALID_ADDR,
  CMD_VALID_DAT,
  CMD_VALID_WRITE_B,
  CMD_RECHECK_READY_ADDR,
  CMD_RECHECK_READY_DAT,

};

enum resp_transfer_state {
  RESPT_INACTIVE,
  RESPT_BITS_ADDR,
  RESPT_BITS_READ,
  RESPT_READY_ADDR,
  RESPT_READY_WRITE,
  RESPT_READY_WRITE_B,
  RESPT_RECHECK_VALID_ADDR,
  RESPT_RECHECK_VALID_READ,
};

enum update_state {
  UPDATE_IDLE_RESP,
  UPDATE_IDLE_CMD,
  UPDATE_CMD_ADDR,
  UPDATE_CMD_WAIT,
  UPDATE_RESP_ADDR,
  UPDATE_RESP_WAIT
};


struct command_transaction {
  uint32_t cmdbuf[5];
  int8_t progress = 0;
  int id = 0;
  cmd_transfer_state state = CMD_INACTIVE;
  bool ready_for_command = false;

  static const int payload_length = 5;
};

struct response_transaction {
  static const int payload_length = 3;
  uint32_t resbuf[payload_length]{};
  uint8_t progress = 0;

  resp_transfer_state state = RESPT_INACTIVE;
};

void update_command_state(BeethovenTop &top);

void update_resp_state(BeethovenTop &top);

void update_update_state(BeethovenTop &top);

void trace_rising_edge_pre(BeethovenTop &top);
void trace_rising_edge_post(BeethovenTop &top);

#endif //BEETHOVENRUNTIME_STATE_MACHINE_H

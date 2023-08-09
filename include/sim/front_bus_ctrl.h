//
// Created by Chris Kjellqvist on 8/9/23.
//

#ifndef COMPOSERRUNTIME_FRONT_BUS_CTRL_H
#define COMPOSERRUNTIME_FRONT_BUS_CTRL_H

#include <cinttypes>
#include <Vcomposer.h>

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
  uint32_t *cmdbuf = nullptr;
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

void update_command_state(command_transaction &ongoing_cmd,
                          response_transaction &ongoing_rsp,
                          update_state &ongoing_update,
                          Vcomposer &top);

void update_resp_state(command_transaction &ongoing_cmd,
                       response_transaction &ongoing_rsp,
                       update_state &ongoing_update,
                       Vcomposer &top);

void update_update_state(command_transaction &ongoing_cmd,
                         response_transaction &ongoing_rsp,
                         update_state &ongoing_update,
                         Vcomposer &top);


#endif//COMPOSERRUNTIME_FRONT_BUS_CTRL_H

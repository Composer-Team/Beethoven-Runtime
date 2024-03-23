//
// Created by Chris Kjellqvist on 8/9/23.
//

#ifndef COMPOSERRUNTIME_FRONT_BUS_CTRL_AXI_H
#define COMPOSERRUNTIME_FRONT_BUS_CTRL_AXI_H

#include <queue>

void queue_uart(std::queue<unsigned char> &in_stream,
                std::queue<unsigned char> &out_stream,
                unsigned char &rxd,
                unsigned char &txd,
                char in_enable = true,
                char out_enable = true);


#endif//COMPOSERRUNTIME_FRONT_BUS_CTRL_AXI_H

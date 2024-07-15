//
// Created by Chris Kjellqvist on 8/9/23.
//

#ifndef BEETHOVENRUNTIME_MEM_CTRL_H
#define BEETHOVENRUNTIME_MEM_CTRL_H

#include "beethoven_allocator_declaration.h"
#include "data_server.h"
#include "ddr_macros.h"
#include "dram_system.h"
#include "BeethovenTop.h"
#include <dramsim3.h>
#include <memory>
#include <queue>

#ifdef USE_VCD
#include <verilated_vcd_c.h>
extern VerilatedVcdC *tfp;
#else

#include <verilated_fst_c.h>

extern VerilatedFstC *tfp;
#endif
extern uint64_t main_time;


#define RLOCK pthread_mutex_lock(&axi4_mem.read_queue_lock);
#define WLOCK pthread_mutex_lock(&axi4_mem.write_queue_lock);
#define RUNLOCK pthread_mutex_unlock(&axi4_mem.read_queue_lock);
#define WUNLOCK pthread_mutex_unlock(&axi4_mem.write_queue_lock);

//extern int DDR_BUS_WIDTH_BITS;
//extern int DDR_BUS_WIDTH_BYTES;
extern int axi_ddr_bus_multiplicity;
extern int DDR_ENQUEUE_SIZE_BYTES;
extern int TOTAL_BURST;
//extern int DDR_BUS_BURST_LENGTH;
extern address_translator at;
extern dramsim3::Config *dramsim3config;

namespace mem_ctrl {
  uint64_t get_dimm_address(uint64_t addr);

  void init(const std::string &dram_ini_file);

  // = (DATA_BUS_WIDTH / 8) / DDR_BUS_WIDTH_BYTES
  struct memory_transaction {
    uintptr_t addr;
    int size;
    int len;
    int axi_bus_beats_progress;
    int id;
    bool fixed;
    uint64_t fpga_addr;
    int dram_tx_n_enqueues;
    int dram_tx_axi_enqueue_progress = 0;
    int dram_tx_load_progress = 0;
    bool can_be_last = true;

    bool is_intermediate;

    std::vector<bool> ddr_bus_beats_retrieved;

    memory_transaction(uintptr_t addr,
                       int size,
                       int len,
                       int progress,
                       bool fixed,
                       int id,
                       uint64_t fpga_addr,
                       bool is_intermediate) : addr(addr),
                                               size(size),
                                               len(len),
                                               axi_bus_beats_progress(progress),
                                               fixed(fixed),
                                               id(id),
                                               fpga_addr(fpga_addr),
                                               is_intermediate(is_intermediate) {
      dram_tx_n_enqueues = (len * size) / DDR_ENQUEUE_SIZE_BYTES;
      if (!is_intermediate) {
//        printf("Transaction of size %d, len %d, n_enqueues %d\n\n", size, len, dram_tx_n_enqueues);
      }
      if (dram_tx_n_enqueues == 32) {
        printf("");
      }
//      fflush(stdout);
      if (dram_tx_n_enqueues == 0) dram_tx_n_enqueues = 1;
      for (int i = 0; i < dram_tx_n_enqueues * TOTAL_BURST; ++i) {
        ddr_bus_beats_retrieved.emplace_back(false);
      }
    }

    memory_transaction() = delete;

    int dramsim_hasBeatReady() {
      if (axi_bus_beats_progress == axi_bus_beats_length()) return false;
      for (int i = 0; i < axi_ddr_bus_multiplicity; ++i) {
        if (!ddr_bus_beats_retrieved[axi_bus_beats_progress * axi_ddr_bus_multiplicity + i]) return false;
      }
      return ddr_bus_beats_retrieved[axi_bus_beats_progress * axi_ddr_bus_multiplicity];
    }

    [[nodiscard]] int bankId() const {
      return int(fpga_addr >> 12);
    }

    [[nodiscard]] bool dramsim_tx_finished() const {
      return dram_tx_axi_enqueue_progress >= dram_tx_n_enqueues;
    }

    [[nodiscard]] int axi_bus_beats_length() const {
      return len * size / (DATA_BUS_WIDTH / 8);
    }
  };

  template<typename id_t, typename axisize_t, typename burst_t, typename addr_t, typename len_t>
  class v_address_channel {
    uint8_t *ready_field = nullptr;
    uint8_t *valid_field = nullptr;
    id_t *id_field = nullptr;
    axisize_t *size_field = nullptr;
    burst_t *burst_field = nullptr;
    addr_t *addr_field = nullptr;
    len_t *len_field = nullptr;
  public:

    ~v_address_channel() = default;

    v_address_channel() = default;

    void init(uint8_t &ready,
              uint8_t &valid,
              id_t &id,
              axisize_t &size,
              burst_t &burst,
              addr_t &addr,
              len_t &len) {
      ready_field = &ready;
      valid_field = &valid;
      id_field = &id;
      size_field = &size;
      burst_field = &burst;
      addr_field = &addr;
      len_field = &len;
    }

    [[nodiscard]] bool getReady() const {
      return bool(*ready_field);
    }

    void setReady(bool ready) {
      *v_address_channel::ready_field = (uint8_t) ready;
    }

    bool getValid() const {
      return *valid_field;
    }

    void setValid(bool valid) {
      *v_address_channel::valid_field = valid;
    }

    id_t getId() const {
      return *id_field;
    }

    void setId(uint64_t id) {
      *v_address_channel::id_field = id;
    }

    CData getSize() const {
      return *size_field;
    }

    void setSize(CData size) {
      *v_address_channel::size_field = size;
    }

    CData getBurst() const {
      return *burst_field;
    }

    void setBurst(CData burst) {
      *v_address_channel::burst_field = burst;
    }

    BeethovenMemAddressSimDtype getAddr() const {
      return *addr_field;
    }

    void setAddr(BeethovenMemAddressSimDtype addr) {
      *v_address_channel::addr_field = addr;
    }

    CData getLen() const {
      return *len_field;
    }

    void setLen(CData len) {
      *v_address_channel::len_field = len;
    }

    bool fire() {
      return *ready_field && *valid_field;
    }
  };

  template<typename id_t, typename strb_t>
  struct data_channel {
    uint8_t *ready;
    uint8_t *valid;
    uint8_t *data;
    strb_t *strb;
    id_t *id;
    uint8_t *last;

  public:

    ~data_channel() = default;

    data_channel() = default;

    void init(uint8_t &ready,
              uint8_t &valid,
              uint8_t &last,
              id_t *id,
              strb_t *strb) {
      data_channel::ready = &ready;
      data_channel::valid = &valid;
      data_channel::last = &last;
      data_channel::id = id;
      data_channel::strb = strb;
    }

    void setData(uint8_t *data) {
      data_channel::data = data;
    }

    uint8_t getReady() const {
      return *ready;
    }

    void setReady(uint8_t ready) {
      *data_channel::ready = ready;
    }

    uint8_t getValid() const {
      return *valid;
    }

    void setValid(uint8_t valid) {
      *data_channel::valid = valid;
    }

    uint8_t *getData() const {
      return data;
    }

    id_t getId() const {
      return *id;
    }

    void setId(id_t id) {
      *data_channel::id = id;
    }

    uint8_t getLast() const {
      return *last;
    }

    void setLast(uint8_t last) {
      *data_channel::last = last;
    }

    bool fire() {
      return *ready && *valid;
    }

    bool getStrb(int i) const {
      return *strb & (1 << i);
    }
  };

  template<typename id_t>
  class response_channel {
    uint8_t *ready_field;
    uint8_t *valid_field;
    id_t *id_field;

  public:
    std::queue<id_t> send_ids;
    std::queue<id_t> to_enqueue;

    ~response_channel() = default;

    response_channel() = default;

    void init(uint8_t &ready,
              uint8_t &valid,
              id_t *id) {
      ready_field = &ready;
      valid_field = &valid;
      id_field = id;
    }

    uint8_t getReady() const {
      return *ready_field;
    }

    void setReady(uint8_t ready) {
      *response_channel::ready_field = ready;
    }

    uint8_t getValid() const {
      return *valid_field;
    }

    void setValid(uint8_t valid) {
      *response_channel::valid_field = valid;
    }

    id_t getId() const {
      return *id_field;
    }

    void setId(id_t id) {
      *response_channel::id_field = id;
    }

    const std::queue<id_t> &getSendIds() const {
      return send_ids;
    }

    void setSendIds(const std::queue<id_t> &sendIds) {
      send_ids = sendIds;
    }

    bool fire() {
      return *ready_field && *valid_field;
    }
  };

  struct with_dramsim3_support {

    virtual void enqueue_read(std::shared_ptr<mem_ctrl::memory_transaction> &tx) = 0;

    std::map<uint64_t, std::queue<std::shared_ptr<memory_transaction>> *> in_flight_reads;
    std::map<uint64_t, std::queue<std::shared_ptr<memory_transaction>> *> in_flight_writes;
    dramsim3::JedecDRAMSystem *mem_sys;
    pthread_mutex_t read_queue_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t write_queue_lock = PTHREAD_MUTEX_INITIALIZER;

    const static int max_q_length = 40;
    std::vector<std::shared_ptr<memory_transaction>> ddr_read_q;
    std::vector<std::shared_ptr<memory_transaction>> ddr_write_q;

    //  std::set<int> bank2tx;
    bool can_accept_write() {
      return ddr_write_q.size() < max_q_length;
    }

    bool can_accept_read() {
      return ddr_read_q.size() < max_q_length;
    }

    void init_dramsim3();

    virtual void enqueue_response(int id) = 0;
  };

  template<typename id_t, typename axisize_t, typename burst_t, typename addr_t, typename len_t, typename strb_t>
  struct mem_interface : with_dramsim3_support {
    v_address_channel<id_t, axisize_t, burst_t, addr_t, len_t> aw;
    v_address_channel<id_t, axisize_t, burst_t, addr_t, len_t> ar;
    data_channel<id_t, strb_t> w;
    data_channel<id_t, strb_t> r;
    response_channel<id_t> b;
    std::queue<std::shared_ptr<memory_transaction>> write_transactions;
    std::queue<std::shared_ptr<memory_transaction>> read_transactions;

    ~mem_interface() = default;

    int num_in_flight_writes = 0;
    static const int max_in_flight_writes = 32;
    int id;

    void enqueue_read(std::shared_ptr<mem_ctrl::memory_transaction> &tx) override {
      read_transactions.push(tx);
    }

    void enqueue_response(int id) {
      b.to_enqueue.push(id);
    }
  };

}




#define prep(x) std::decay_t<decltype(x)>

typedef mem_ctrl::mem_interface<prep(BeethovenTop::M00_AXI_arid), prep(BeethovenTop::M00_AXI_arsize), prep(BeethovenTop::M00_AXI_arburst), prep(BeethovenTop::M00_AXI_araddr), prep(BeethovenTop::M00_AXI_arlen), prep(BeethovenTop::M00_AXI_wstrb)> mem_intf_t;
#ifdef BEETHOVEN_HAS_DMA
typedef mem_ctrl::mem_interface<prep(BeethovenTop::dma_arid), prep(BeethovenTop::dma_arsize), prep(BeethovenTop::dma_arburst), prep(BeethovenTop::dma_araddr), prep(BeethovenTop::dma_arlen), prep(BeethovenTop::dma_wstrb)> dma_intf_t;
#endif

void try_to_enqueue_ddr(mem_intf_t &);

#endif//BEETHOVENRUNTIME_MEM_CTRL_H

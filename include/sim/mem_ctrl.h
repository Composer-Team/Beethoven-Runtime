//
// Created by Chris Kjellqvist on 8/9/23.
//

#ifndef BEETHOVENRUNTIME_MEM_CTRL_H
#define BEETHOVENRUNTIME_MEM_CTRL_H
#include "beethoven_allocator_declaration.h"
#include "data_server.h"
#include "ddr_macros.h"
#include "dram_system.h"
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

  template<typename idtype >
  class v_address_channel {
    CData *ready = nullptr;
    CData *valid = nullptr;
    idtype *id = nullptr;
    CData *size = nullptr;
    CData *burst = nullptr;
    BeethovenMemAddressSimDtype *addr = nullptr;
    CData *len = nullptr;

  public:
    explicit v_address_channel(CData &ready,
                               CData &valid,
                               idtype &id,
                               CData &size,
                               CData &burst,
                               BeethovenMemAddressSimDtype &addr,
                               CData &len) : ready(&ready),
                                             valid(&valid),
                                             id(&id),
                                             size(&size),
                                             burst(&burst),
                                             addr(&addr),
                                             len(&len) {}

    [[nodiscard]] CData getReady() const {
      return *ready;
    }

    void setReady(CData ready) {
      *v_address_channel::ready = ready;
    }

    CData getValid() const {
      return *valid;
    }

    void setValid(CData valid) {
      *v_address_channel::valid = valid;
    }

    idtype getId() const {
      return *id;
    }

    void setId(idtype id) {
      *v_address_channel::id = id;
    }

    CData getSize() const {
      return *size;
    }

    void setSize(CData size) {
      *v_address_channel::size = size;
    }

    CData getBurst() const {
      return *burst;
    }

    void setBurst(CData burst) {
      *v_address_channel::burst = burst;
    }

    BeethovenMemAddressSimDtype getAddr() const {
      return *addr;
    }

    void setAddr(BeethovenMemAddressSimDtype addr) {
      *v_address_channel::addr = addr;
    }

    CData getLen() const {
      return *len;
    }

    void setLen(CData len) {
      *v_address_channel::len = len;
    }

    bool fire() {
      return *ready && *valid;
    }
  };

  template<typename idtype>
  class data_channel {
    CData *ready;
    CData *valid;
    char *data = nullptr;
    idtype *id;
    CData *last;

  public:
    explicit data_channel(CData &ready,
                          CData &valid,
                          CData &last,
                          idtype *id) : ready(&ready),
                                        valid(&valid),
                                        last(&last),
                                        id(id) {}

    void setData(char *data) {
      data_channel::data = data;
    }

    CData getReady() const {
      return *ready;
    }

    void setReady(CData ready) {
      *data_channel::ready = ready;
    }

    CData getValid() const {
      return *valid;
    }

    void setValid(CData valid) {
      *data_channel::valid = valid;
    }

    char *getData() const {
      return data;
    }

    idtype getId() const {
      return *id;
    }

    void setId(idtype id) {
      *data_channel::id = id;
      //    std::cerr << "Got " << id << ", set as " << *data_channel::id << std::endl;
    }

    CData getLast() const {
      return *last;
    }

    void setLast(CData last) {
      *data_channel::last = last;
    }

    bool fire() {
      return *ready && *valid;
    }
  };

  template<typename idtype>
  class response_channel {
    CData *ready;
    CData *valid;
    idtype *id;

  public:
    std::queue<int> send_ids;
    std::queue<int> to_enqueue;
    explicit response_channel(CData &ready,
                              CData &valid,
                              idtype &id) : ready(&ready),
                                            valid(&valid),
                                            id(&id) {}

    CData getReady() const {
      return *ready;
    }

    void setReady(CData ready) {
      *response_channel::ready = ready;
    }

    CData getValid() const {
      return *valid;
    }

    void setValid(CData valid) {
      *response_channel::valid = valid;
    }

    idtype getId() const {
      return *id;
    }

    void setId(idtype id) {
      *response_channel::id = id;
    }

    const std::queue<int> &getSendIds() const {
      return send_ids;
    }

    void setSendIds(const std::queue<int> &sendIds) {
      send_ids = sendIds;
    }

    bool fire() {
      return *ready && *valid;
    }
  };

  struct with_dramsim3_support {

    virtual void enqueue_read(std::shared_ptr<mem_ctrl::memory_transaction> &tx) = 0;

    virtual void enqueue_response(int id) = 0;

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

    static int tx2bank(const std::shared_ptr<memory_transaction> mt) {
      auto dimm_base = get_dimm_address(mt->fpga_addr);
      auto dimm_addr = dimm_base + 8 * mt->dram_tx_axi_enqueue_progress;

      return (int) ((dimm_addr & ~(0xFFFL)) >> 12);
    }

    void init_dramsim3();
  };

  template<typename IDType>
  struct mem_interface: with_dramsim3_support
  {
    v_address_channel<IDType> *aw = nullptr;
    v_address_channel<IDType> *ar = nullptr;
    data_channel<IDType> *w = nullptr;
    data_channel<IDType> *r = nullptr;
    response_channel<IDType> *b = nullptr;
    std::queue<std::shared_ptr<memory_transaction>> write_transactions;
    std::queue<std::shared_ptr<memory_transaction>> read_transactions;

    int num_in_flight_writes = 0;
    static const int max_in_flight_writes = 32;
    int id;
    void enqueue_read(std::shared_ptr<mem_ctrl::memory_transaction> &tx) override {
      read_transactions.push(tx);
    }

    void enqueue_response(int id) override {
      b->to_enqueue.push(static_cast<IDType>(id));
    }

    int current_read_channel_contents = -1;
  };

  void enqueue_transaction(v_address_channel<BeethovenMemIDDtype> &chan, std::queue<std::shared_ptr<memory_transaction>> &lst);
}

void try_to_enqueue_ddr(mem_ctrl::mem_interface<BeethovenMemIDDtype> &axi4_mem);

#endif//BEETHOVENRUNTIME_MEM_CTRL_H

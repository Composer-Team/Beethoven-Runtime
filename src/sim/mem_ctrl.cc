//
// Created by Chris Kjellqvist on 8/9/23.
//

#include "sim/mem_ctrl.h"
#include "verilated.h"
#include <verilated_fst_c.h>


int DDR_BUS_WIDTH_BITS = 64;
int DDR_BUS_WIDTH_BYTES = 8;
int axi_ddr_bus_multiplicity;
int DDR_BUS_BURST_LENGTH;
dramsim3::Config *dramsim3config = nullptr;

extern uint64_t main_time;
using namespace mem_ctrl;

uint64_t mem_ctrl::get_dimm_address(uint64_t addr) {
  uint64_t acc = 0;
  uint64_t cursor = 1;
  int real = 0;
  for (int i = 0; i < 64; ++i) {
    if (addrMask & cursor) {
      if (addr & cursor) {
        acc |= 1 << real;
      }
      ++real;
    }
    cursor <<= 1;
  }
  return acc;
}

void with_dramsim3_support::init_dramsim3() {
  mem_sys = new dramsim3::JedecDRAMSystem(
      *dramsim3config, "",
      [this](uint64_t addr) {
        pthread_mutex_lock(&this->read_queue_lock);
        auto tx = in_flight_reads[addr]->front();
        tx->dram_tx_load_progress++;
        for (int i = 0; i < dramsim3config->BL; ++i) {
          tx->ddr_bus_beats_retrieved[int(addr - tx->fpga_addr) / DDR_BUS_WIDTH_BYTES + i] = true;
        }
        int bytes_loaded = tx->dram_tx_load_progress * DDR_BUS_WIDTH_BYTES * dramsim3config->BL;
        int total_tx_size = tx->size * tx->len;

        while (tx->dramsim_hasBeatReady()) {
          bool done = (tx->axi_bus_beats_progress == tx->axi_bus_beats_length() - 1);
          auto intermediate_tx = std::make_shared<mem_ctrl::memory_transaction>(tx->addr, tx->size, 1, 0, false, tx->id, 0);
          tx->addr += tx->size;
          intermediate_tx->fpga_addr = tx->fpga_addr;
          intermediate_tx->can_be_last = done;
          tx->axi_bus_beats_progress++;
          enqueue_read(intermediate_tx);
        }
        in_flight_reads[addr]->pop();
        pthread_mutex_unlock(&this->read_queue_lock);
      },
      [this](uint64_t addr) {
        pthread_mutex_lock(&write_queue_lock);
        auto tx = in_flight_writes[addr]->front();
        tx->axi_bus_beats_progress--;
        if (tx->axi_bus_beats_progress == 0) {
          enqueue_response(tx->id);
//#ifdef VERBOSE
//          fprintf(stderr, "Enqueing id %d\n", tx->id); fflush(stderr);
//#endif
        }
        in_flight_writes[addr]->pop();
        pthread_mutex_unlock(&write_queue_lock);
      });

}

void try_to_enqueue_ddr(mem_interface<ComposerMemIDDtype> &axi4_mem) {
  RLOCK
  std::shared_ptr<mem_ctrl::memory_transaction> to_enqueue_read = nullptr;
  // find next read we should send to DRAM. Prioritize older txs
  for (auto it = axi4_mem.ddr_read_q.begin(); it != axi4_mem.ddr_read_q.end(); ++it) {
    auto &mt = *it;
    if (axi4_mem.mem_sys->WillAcceptTransaction(mt->fpga_addr, false)) {
      // AXI stipulates that for multiple transactions on the same ID, the returned packets need to be serialized
      // Since this list is ordered old -> new, we need to search from the beginning of the list to the current iterator
      //  to see if there is a transaction with the same ID. If so, we need to skip and issue later.
      bool foundHigherPriorityInID = false;
      for (auto it2 = axi4_mem.ddr_read_q.begin(); it2 != it; ++it2) {
        if (mt->id == (*it2)->id) {
          foundHigherPriorityInID = true;
        }
      }
      if (foundHigherPriorityInID) continue;

      to_enqueue_read = mt;
      auto dimm_base = mem_ctrl::get_dimm_address(to_enqueue_read->fpga_addr);
      auto dimm_addr = dimm_base + DDR_BUS_WIDTH_BYTES * dramsim3config->BL * to_enqueue_read->dram_tx_axi_enqueue_progress;
      axi4_mem.mem_sys->AddTransaction(dimm_addr, false);

      // remember it as being in flight. Make a queue if necessary and store it there
      if (axi4_mem.in_flight_reads.find(dimm_addr) == axi4_mem.in_flight_reads.end())
        axi4_mem.in_flight_reads[dimm_addr] = new std::queue<std::shared_ptr<mem_ctrl::memory_transaction>>();

      auto &q = *axi4_mem.in_flight_reads[dimm_addr];
      q.push(to_enqueue_read);
      to_enqueue_read->dram_tx_axi_enqueue_progress++;

      if (to_enqueue_read->dramsim_tx_finished()) {
        axi4_mem.ddr_read_q.erase(std::find(axi4_mem.ddr_read_q.begin(), axi4_mem.ddr_read_q.end(), to_enqueue_read));
      }

      break;
    }
  }
  RUNLOCK

  WLOCK
  std::shared_ptr<mem_ctrl::memory_transaction> to_enqueue_write = nullptr;
  for (const auto &mt: axi4_mem.ddr_write_q) {
    int bank_id = mt->bankId();
    if (//axi4_mem.bank2tx.find(bank_id) == axi4_mem.bank2tx.end() &&
        axi4_mem.mem_sys->WillAcceptTransaction(mt->fpga_addr, true)) {
      to_enqueue_write = mt;
      //            axi4_mem.bank2tx.insert(bank_id);
      break;
    }
  }

  if (to_enqueue_write != nullptr) {
    auto dimm_addr = mem_ctrl::get_dimm_address(to_enqueue_write->fpga_addr) +
                     to_enqueue_write->dram_tx_axi_enqueue_progress * DDR_BUS_WIDTH_BYTES * axi_ddr_bus_multiplicity * dramsim3config->BL;
    to_enqueue_write->dram_tx_axi_enqueue_progress++;
    axi4_mem.mem_sys->AddTransaction(dimm_addr, true);
    if (axi4_mem.in_flight_writes.find(dimm_addr) == axi4_mem.in_flight_writes.end())
      axi4_mem.in_flight_writes[dimm_addr] = new std::queue<std::shared_ptr<mem_ctrl::memory_transaction>>;
    axi4_mem.in_flight_writes[dimm_addr]->push(to_enqueue_write);
    if (to_enqueue_write->dramsim_tx_finished()) {
      axi4_mem.ddr_write_q.erase(std::find(axi4_mem.ddr_write_q.begin(), axi4_mem.ddr_write_q.end(), to_enqueue_write));
    }
//    fprintf(stderr, "Starting write tx %d\n", to_enqueue_write->id);
  }
  WUNLOCK
}

void mem_ctrl::init(const std::string &dram_ini_file) {
  dramsim3config = new dramsim3::Config("../custom_dram_configs/hyperram.ini", "./");
  // KRIA has much slower memory!
  // Config dramsim3config("../DRAMsim3/configs/Kria.ini", "./");
  DDR_BUS_WIDTH_BITS = dramsim3config->bus_width;
  DDR_BUS_WIDTH_BYTES = DDR_BUS_WIDTH_BITS / 8;
  axi_ddr_bus_multiplicity = (DATA_BUS_WIDTH / 8) / DDR_BUS_WIDTH_BYTES;
  DDR_BUS_BURST_LENGTH = dramsim3config->BL;
}

void mem_ctrl::enqueue_transaction(v_address_channel<ComposerMemIDDtype> &chan, std::queue<std::shared_ptr<memory_transaction>> &lst) {
  if (chan.getValid() && chan.getReady()) {
    try {
      char *addr = (char *) at.translate(chan.getAddr());
      int sz = 1 << chan.getSize();
      int len = 1 + chan.getLen();// per axi
      bool is_fixed = chan.getBurst() == 0;
      int id = chan.getId();
//      fprintf(stderr, "Enqueueing on awid channel %d\n", id);
      uint64_t fpga_addr = chan.getAddr();
      auto tx = std::make_shared<memory_transaction>(addr, sz, len, 0, is_fixed, id, fpga_addr);
      lst.push(tx);
    } catch (std::exception &e) {
      tfp->dump(main_time);
      tfp->close();
      throw e;
    }
  }
}


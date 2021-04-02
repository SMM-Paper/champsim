#ifndef MEMORY_CLASS_H
#define MEMORY_CLASS_H

#include "champsim.h"
#include "block.h"

#include <limits>

// CACHE ACCESS TYPE
#define LOAD      0
#define RFO       1
#define PREFETCH  2
#define WRITEBACK 3
#define TRANSLATION 4
#define NUM_TYPES 5

// CACHE BLOCK
class BLOCK {
  public:
    uint8_t valid = 0,
            prefetch = 0,
            dirty = 0,
            used = 0;

    int delta = 0,
        depth = 0,
        signature = 0,
        confidence = 0;

    uint64_t address = 0,
             full_addr = 0,
             v_address,
             full_v_addr,
             tag = 0,
             data = 0,
             ip,
             cpu = 0,
             instr_id = 0;

    // replacement state
    uint32_t lru = std::numeric_limits<uint32_t>::max();

    BLOCK() {}

    BLOCK(const PACKET &packet) :
        valid(1),
        prefetch(packet.type == PREFETCH),
        dirty(0),
        used(0),
        delta(packet.delta),
        depth(packet.depth),
        signature(packet.signature),
        confidence(packet.confidence),
        address(packet.address),
        full_addr(packet.full_addr),
        v_address(packet.v_address),
        full_v_addr(packet.full_v_addr),
        tag(packet.address),
        data(packet.data),
        ip(packet.ip),
        cpu(packet.cpu),
        instr_id(packet.instr_id)
    {}
};

class MemoryRequestConsumer
{
    public:
        virtual int  add_rq(PACKET *packet) = 0;
        virtual int  add_wq(PACKET *packet) = 0;
        virtual int  add_pq(PACKET *packet) = 0;
        virtual uint32_t get_occupancy(uint8_t queue_type, uint64_t address) = 0;
        virtual uint32_t get_size(uint8_t queue_type, uint64_t address) = 0;
};

class MemoryRequestProducer
{
    public:
        MemoryRequestConsumer *lower_level;
        virtual void return_data(PACKET *packet) = 0;
    protected:
        MemoryRequestProducer() {}
        explicit MemoryRequestProducer(MemoryRequestConsumer *ll) : lower_level(ll) {}
};

#endif


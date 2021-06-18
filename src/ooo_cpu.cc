#include <algorithm>
#include <vector>

#include "ooo_cpu.h"
#include "instruction.h"

#define DEADLOCK_CYCLE 1000000

extern uint8_t warmup_complete[NUM_CPUS];
extern uint8_t knob_cloudsuite;

void O3_CPU::operate()
{
    operated = true;
    instrs_to_read_this_cycle = std::min((std::size_t)FETCH_WIDTH, IFETCH_BUFFER.size() - IFETCH_BUFFER.occupancy());

    retire_rob(); // retire
    complete_inflight_instruction(); // finalize execution
    execute_instruction(); // execute instructions
    schedule_instruction(); // schedule instructions
    handle_memory_return(); // finalize memory transactions
    operate_lsq(); // execute memory transactions
    schedule_memory_instruction(); // schedule memory transactions
    dispatch_instruction(); // dispatch
    decode_instruction(); // decode
    promote_to_decode();
    fetch_instruction(); // fetch
    translate_fetch();
    check_dib();

    // check for deadlock
    if (ROB.front().ip && (ROB.front().event_cycle + DEADLOCK_CYCLE) <= current_cycle)
        print_deadlock(cpu);
}

void O3_CPU::initialize_core()
{
    // BRANCH PREDICTOR & BTB
    impl_branch_predictor_initialize();
    impl_btb_initialize();
}

void O3_CPU::init_instruction(ooo_model_instr arch_instr)
{
    // actual processors do not work like this but for easier implementation,
    // we read instruction traces and virtually add them in the ROB
    // note that these traces are not yet translated and fetched
    instrs_to_read_this_cycle--;

    // first, read PIN trace

    arch_instr.instr_id = instr_unique_id;

    bool writes_sp   = (std::find(std::begin(arch_instr.destination_registers), std::end(arch_instr.destination_registers), REG_STACK_POINTER) != std::end(arch_instr.destination_registers));
    bool writes_ip   = (std::find(std::begin(arch_instr.destination_registers), std::end(arch_instr.destination_registers), REG_INSTRUCTION_POINTER) != std::end(arch_instr.destination_registers));
    bool reads_sp    = (std::find(std::begin(arch_instr.source_registers), std::end(arch_instr.source_registers), REG_STACK_POINTER) != std::end(arch_instr.source_registers));
    bool reads_flags = (std::find(std::begin(arch_instr.source_registers), std::end(arch_instr.source_registers), REG_FLAGS) != std::end(arch_instr.source_registers));
    bool reads_ip    = (std::find(std::begin(arch_instr.source_registers), std::end(arch_instr.source_registers), REG_INSTRUCTION_POINTER) != std::end(arch_instr.source_registers));
    bool reads_other = std::any_of(std::begin(arch_instr.source_registers), std::end(arch_instr.source_registers), [](uint8_t sreg){ return sreg != REG_STACK_POINTER && sreg != REG_FLAGS && sreg != REG_INSTRUCTION_POINTER; });

    std::fill_n(std::back_inserter(STA), std::size(arch_instr.destination_memory), instr_unique_id);
    assert(std::size(STA) <= std::size(ROB)*NUM_INSTR_DESTINATIONS_SPARC);

    arch_instr.num_reg_ops = std::size(arch_instr.source_registers) + std::size(arch_instr.destination_registers);
    arch_instr.num_mem_ops = std::size(arch_instr.source_memory) + std::size(arch_instr.destination_memory);

    if (arch_instr.num_mem_ops > 0)
        arch_instr.is_memory = 1;

    // determine what kind of branch this is, if any
    if(!reads_sp && !reads_flags && writes_ip && !reads_other)
    {
        // direct jump
        arch_instr.is_branch = 1;
        arch_instr.branch_taken = 1;
        arch_instr.branch_type = BRANCH_DIRECT_JUMP;
    }
    else if(!reads_sp && !reads_flags && writes_ip && reads_other)
    {
        // indirect branch
        arch_instr.is_branch = 1;
        arch_instr.branch_taken = 1;
        arch_instr.branch_type = BRANCH_INDIRECT;
    }
    else if(!reads_sp && reads_ip && !writes_sp && writes_ip && reads_flags && !reads_other)
    {
        // conditional branch
        arch_instr.is_branch = 1;
        arch_instr.branch_taken = arch_instr.branch_taken; // don't change this
        arch_instr.branch_type = BRANCH_CONDITIONAL;
    }
    else if(reads_sp && reads_ip && writes_sp && writes_ip && !reads_flags && !reads_other)
    {
        // direct call
        arch_instr.is_branch = 1;
        arch_instr.branch_taken = 1;
        arch_instr.branch_type = BRANCH_DIRECT_CALL;
    }
    else if(reads_sp && reads_ip && writes_sp && writes_ip && !reads_flags && reads_other)
    {
        // indirect call
        arch_instr.is_branch = 1;
        arch_instr.branch_taken = 1;
        arch_instr.branch_type = BRANCH_INDIRECT_CALL;
    }
    else if(reads_sp && !reads_ip && writes_sp && writes_ip)
    {
        // return
        arch_instr.is_branch = 1;
        arch_instr.branch_taken = 1;
        arch_instr.branch_type = BRANCH_RETURN;
    }
    else if(writes_ip)
    {
        // some other branch type that doesn't fit the above categories
        arch_instr.is_branch = 1;
        arch_instr.branch_taken = arch_instr.branch_taken; // don't change this
        arch_instr.branch_type = BRANCH_OTHER;
    }

    total_branch_types[arch_instr.branch_type]++;

    if((arch_instr.is_branch != 1) || (arch_instr.branch_taken != 1))
      {
	// clear the branch target for this instruction
	arch_instr.branch_target = 0;
      }

    // Stack Pointer Folding
    // The exact, true value of the stack pointer for any given instruction can
    // usually be determined immediately after the instruction is decoded without
    // waiting for the stack pointer's dependency chain to be resolved.
    // We're doing it here because we already have writes_sp and reads_other handy,
    // and in ChampSim it doesn't matter where before execution you do it.
    if(writes_sp)
      {
       // Avoid creating register dependencies on the stack pointer for calls, returns, pushes,
       // and pops, but not for variable-sized changes in the stack pointer position.
       // reads_other indicates that the stack pointer is being changed by a variable amount,
       // which can't be determined before execution.
       if((arch_instr.is_branch != 0) || (arch_instr.num_mem_ops > 0) || (!reads_other))
       {
           auto sp = std::find(std::begin(arch_instr.destination_registers), std::end(arch_instr.destination_registers), REG_STACK_POINTER);
           if (sp != std::end(arch_instr.destination_registers))
           {
               arch_instr.destination_registers.erase(sp);
               arch_instr.num_reg_ops--;
           }
       }
      }

    // add this instruction to the IFETCH_BUFFER

    // handle branch prediction
    if (arch_instr.is_branch) {

        DP( if (warmup_complete[cpu]) {
                cout << "[BRANCH] instr_id: " << instr_unique_id << " ip: " << hex << arch_instr.ip << dec << " taken: " << +arch_instr.branch_taken << endl; });

        num_branch++;

        auto [predicted_branch_target, always_taken] = impl_btb_prediction(arch_instr.ip, arch_instr.branch_type);
        uint8_t branch_prediction = impl_predict_branch(arch_instr.ip, predicted_branch_target, always_taken, arch_instr.branch_type);
        if ((branch_prediction == 0) && (always_taken == 0))
        {
            predicted_branch_target = 0;
        }

        // call code prefetcher every time the branch predictor is used
        impl_prefetcher_branch_operate(arch_instr.ip, arch_instr.branch_type, predicted_branch_target);

        if(predicted_branch_target != arch_instr.branch_target)
        {
            branch_mispredictions++;
            total_rob_occupancy_at_branch_mispredict += ROB.occupancy();
	    branch_type_misses[arch_instr.branch_type]++;
            if(warmup_complete[cpu])
            {
                fetch_stall = 1;
                instrs_to_read_this_cycle = 0;
                arch_instr.branch_mispredicted = 1;
            }
        }
        else
        {
            // if correctly predicted taken, then we can't fetch anymore instructions this cycle
            if(arch_instr.branch_taken == 1)
            {
                instrs_to_read_this_cycle = 0;
            }
        }

        impl_update_btb(arch_instr.ip, arch_instr.branch_target, arch_instr.branch_taken, arch_instr.branch_type);
        impl_last_branch_result(arch_instr.ip, arch_instr.branch_target, arch_instr.branch_taken, arch_instr.branch_type);
    }

    arch_instr.event_cycle = current_cycle;

    // fast warmup eliminates register dependencies between instructions
    // branch predictor, cache contents, and prefetchers are still warmed up
    if(!warmup_complete[cpu])
    {
        arch_instr.source_registers.clear();
        arch_instr.destination_registers.clear();
        arch_instr.num_reg_ops = 0;
    }

    // Add to IFETCH_BUFFER
    IFETCH_BUFFER.push_back(arch_instr);

    instr_unique_id++;
}

void O3_CPU::check_dib()
{
    // scan through IFETCH_BUFFER to find instructions that hit in the decoded instruction buffer
    auto end = std::min(IFETCH_BUFFER.end(), std::next(IFETCH_BUFFER.begin(), FETCH_WIDTH));
    for (auto it = IFETCH_BUFFER.begin(); it != end; ++it)
        do_check_dib(*it);
}

void O3_CPU::do_check_dib(ooo_model_instr &instr)
{
    // Check DIB to see if we recently fetched this line
    auto dib_set_begin = std::next(DIB.begin(), ((instr.ip >> lg2(dib_window)) % dib_set) * dib_way);
    auto dib_set_end   = std::next(dib_set_begin, dib_way);
    auto way = std::find_if(dib_set_begin, dib_set_end, eq_addr<dib_t::value_type>(instr.ip, lg2(dib_window)));

    if (way != dib_set_end)
    {
        // The cache line is in the L0, so we can mark this as complete
        instr.translated = COMPLETED;
        instr.fetched = COMPLETED;

        // Also mark it as decoded
        instr.decoded = COMPLETED;

        // It can be acted on immediately
        instr.event_cycle = current_cycle;

        // Update LRU
        std::for_each(dib_set_begin, dib_set_end, lru_updater<dib_entry_t>(way));
    }
}

void O3_CPU::translate_fetch()
{
    if (IFETCH_BUFFER.empty())
        return;

    // scan through IFETCH_BUFFER to find instructions that need to be translated
    auto itlb_req_begin = std::find_if(IFETCH_BUFFER.begin(), IFETCH_BUFFER.end(), [](const ooo_model_instr &x){ return !x.translated; });
    uint64_t find_addr = itlb_req_begin->ip;
    auto itlb_req_end   = std::find_if(itlb_req_begin, IFETCH_BUFFER.end(), [find_addr](const ooo_model_instr &x){ return (find_addr >> LOG2_PAGE_SIZE) != (x.ip >> LOG2_PAGE_SIZE);});
    if (itlb_req_end != IFETCH_BUFFER.end() || itlb_req_begin == IFETCH_BUFFER.begin())
    {
        do_translate_fetch(itlb_req_begin, itlb_req_end);
    }
}

void O3_CPU::do_translate_fetch(champsim::circular_buffer<ooo_model_instr>::iterator begin, champsim::circular_buffer<ooo_model_instr>::iterator end)
{
    // begin process of fetching this instruction by sending it to the ITLB
    // add it to the ITLB's read queue
    PACKET trace_packet;
    trace_packet.fill_level = ITLB_bus.lower_level->fill_level;
    trace_packet.cpu = cpu;
    trace_packet.address = begin->ip;
    trace_packet.v_address = begin->ip;
    trace_packet.instr_id = begin->instr_id;
    trace_packet.ip = begin->ip;
    trace_packet.type = LOAD;
    trace_packet.asid[0] = 0;
    trace_packet.asid[1] = 0;
    trace_packet.to_return = {&ITLB_bus};
    for (; begin != end; ++begin)
        trace_packet.instr_depend_on_me.push_back(begin);

    int rq_index = ITLB_bus.lower_level->add_rq(&trace_packet);

    if(rq_index != -2)
    {
        // successfully sent to the ITLB, so mark all instructions in the IFETCH_BUFFER that match this ip as translated INFLIGHT
        for (auto dep_it : trace_packet.instr_depend_on_me)
        {
            dep_it->translated = INFLIGHT;
        }
    }
}

void O3_CPU::fetch_instruction()
{
  // if we had a branch mispredict, turn fetching back on after the branch mispredict penalty
  if((fetch_stall == 1) && (current_cycle >= fetch_resume_cycle) && (fetch_resume_cycle != 0))
    {
      fetch_stall = 0;
      fetch_resume_cycle = 0;
    }

  if (IFETCH_BUFFER.empty())
      return;

      // fetch cache lines that were part of a translated page but not the cache line that initiated the translation
    auto l1i_req_begin = std::find_if(IFETCH_BUFFER.begin(), IFETCH_BUFFER.end(),
            [](const ooo_model_instr &x){ return x.translated == COMPLETED && !x.fetched; });
    uint64_t find_addr = l1i_req_begin->instruction_pa;
    auto l1i_req_end   = std::find_if(l1i_req_begin, IFETCH_BUFFER.end(),
            [find_addr](const ooo_model_instr &x){ return (find_addr >> LOG2_BLOCK_SIZE) != (x.instruction_pa >> LOG2_BLOCK_SIZE);});
    if (l1i_req_end != IFETCH_BUFFER.end() || l1i_req_begin == IFETCH_BUFFER.begin())
    {

        do_fetch_instruction(l1i_req_begin, l1i_req_end);
    }
}

void O3_CPU::do_fetch_instruction(champsim::circular_buffer<ooo_model_instr>::iterator begin, champsim::circular_buffer<ooo_model_instr>::iterator end)
{
    // add it to the L1-I's read queue
    PACKET fetch_packet;
    fetch_packet.fill_level = L1I_bus.lower_level->fill_level;
    fetch_packet.cpu = cpu;
    fetch_packet.address = begin->instruction_pa;
    fetch_packet.data = begin->instruction_pa;
    fetch_packet.v_address = begin->ip;
    fetch_packet.instr_id = begin->instr_id;
    fetch_packet.ip = begin->ip;
    fetch_packet.type = LOAD; 
    fetch_packet.asid[0] = 0;
    fetch_packet.asid[1] = 0;
    fetch_packet.to_return = {&L1I_bus};
    for (; begin != end; ++begin)
        fetch_packet.instr_depend_on_me.push_back(begin);

    int rq_index = L1I_bus.lower_level->add_rq(&fetch_packet);

    if(rq_index != -2)
    {
        // mark all instructions from this cache line as having been fetched
        for (auto dep_it : fetch_packet.instr_depend_on_me)
        {
            dep_it->fetched = INFLIGHT;
        }
    }
}

void O3_CPU::promote_to_decode()
{
    unsigned available_fetch_bandwidth = FETCH_WIDTH;
    while (available_fetch_bandwidth > 0 && !IFETCH_BUFFER.empty() && !DECODE_BUFFER.full() &&
            IFETCH_BUFFER.front().translated == COMPLETED && IFETCH_BUFFER.front().fetched == COMPLETED)
    {
        if (!warmup_complete[cpu] || IFETCH_BUFFER.front().decoded)
            DECODE_BUFFER.push_back_ready(IFETCH_BUFFER.front());
        else
            DECODE_BUFFER.push_back(IFETCH_BUFFER.front());

        IFETCH_BUFFER.pop_front();

	available_fetch_bandwidth--;
    }
}

void O3_CPU::decode_instruction()
{
    std::size_t available_decode_bandwidth = DECODE_WIDTH;

    // Send decoded instructions to dispatch
    while (available_decode_bandwidth > 0 && DECODE_BUFFER.has_ready() && !DISPATCH_BUFFER.full())
    {
        ooo_model_instr &db_entry = DECODE_BUFFER.front();
        do_dib_update(db_entry);

	// Resume fetch 
	if (db_entry.branch_mispredicted)
	  {
	    // These branches detect the misprediction at decode
	    if ((db_entry.branch_type == BRANCH_DIRECT_JUMP) || (db_entry.branch_type == BRANCH_DIRECT_CALL))
	      {
		// clear the branch_mispredicted bit so we don't attempt to resume fetch again at execute
		db_entry.branch_mispredicted = 0;
		// pay misprediction penalty
		fetch_resume_cycle = current_cycle + BRANCH_MISPREDICT_PENALTY;
	      }
	  }
	
        // Add to dispatch
        if (warmup_complete[cpu])
            DISPATCH_BUFFER.push_back(db_entry);
        else
            DISPATCH_BUFFER.push_back_ready(db_entry);
        DECODE_BUFFER.pop_front();

	available_decode_bandwidth--;
    }

    DECODE_BUFFER.operate();
}

void O3_CPU::do_dib_update(const ooo_model_instr &instr)
{
    // Search DIB to see if we need to add this instruction
    auto dib_set_begin = std::next(DIB.begin(), ((instr.ip >> lg2(dib_window)) % dib_set) * dib_way);
    auto dib_set_end   = std::next(dib_set_begin, dib_way);
    auto way = std::find_if(dib_set_begin, dib_set_end, eq_addr<dib_t::value_type>(instr.ip, lg2(dib_window)));

    // If we did not find the entry in the DIB, find a victim
    if (way == dib_set_end)
    {
        way = std::max_element(dib_set_begin, dib_set_end, lru_comparator<dib_entry_t>());
        assert(way != dib_set_end);

        // update way
        way->valid = true;
        way->address = instr.ip;
    }

    std::for_each(dib_set_begin, dib_set_end, lru_updater<dib_entry_t>(way));
}

void O3_CPU::dispatch_instruction()
{
    if (DISPATCH_BUFFER.empty())
        return;

    std::size_t available_dispatch_bandwidth = DISPATCH_WIDTH;

    // dispatch DISPATCH_WIDTH instructions into the ROB
    while (available_dispatch_bandwidth > 0 && DISPATCH_BUFFER.has_ready() && !ROB.full())
    {
        // Add to ROB
        ROB.push_back(DISPATCH_BUFFER.front());
        DISPATCH_BUFFER.pop_front();
	available_dispatch_bandwidth--;
    }

    DISPATCH_BUFFER.operate();
}

int O3_CPU::prefetch_code_line(uint64_t pf_v_addr)
{
    return static_cast<CACHE*>(L1I_bus.lower_level)->prefetch_line(0, pf_v_addr, pf_v_addr, FILL_L1, 0);
}

void O3_CPU::schedule_instruction()
{
    std::size_t search_bw = SCHEDULER_SIZE;
    for (auto rob_it = std::begin(ROB); rob_it != std::end(ROB) && search_bw > 0; ++rob_it)
    {
        if (rob_it->scheduled == 0)
        {
            do_scheduling(rob_it);

            if (rob_it->scheduled == COMPLETED && rob_it->num_reg_dependent == 0) {

                // remember this rob_index in the Ready-To-Execute array 1
                assert(ready_to_execute.size() < ROB.size());
                ready_to_execute.push(rob_it);

                DP ( if (warmup_complete[cpu]) {
                        std::cout << "[ready_to_execute] " << __func__ << " instr_id: " << rob_it->instr_id << " is added to ready_to_execute" << std::endl; });
            }
        }

        if (rob_it->executed == 0)
            --search_bw;
    }
}

struct instr_reg_will_produce
{
    const uint8_t match_reg;
    explicit instr_reg_will_produce(uint8_t reg) : match_reg(reg) {}
    bool operator() (const ooo_model_instr &test) const
    {
        auto dreg_begin = std::begin(test.destination_registers);
        auto dreg_end   = std::end(test.destination_registers);
        return test.executed != COMPLETED && std::find(dreg_begin, dreg_end, match_reg) != dreg_end;
    }
};

void O3_CPU::do_scheduling(champsim::circular_buffer<ooo_model_instr>::iterator rob_it)
{
    // Mark register dependencies
    for (auto src_reg : rob_it->source_registers) {
        auto prior = std::find_if(std::reverse_iterator{rob_it}, ROB.rend(), instr_reg_will_produce(src_reg));
        if (prior != ROB.rend() && (prior->registers_instrs_depend_on_me.empty() || prior->registers_instrs_depend_on_me.back() != rob_it))
        {
            prior->registers_instrs_depend_on_me.push_back(rob_it);
            rob_it->num_reg_dependent++;
        }
    }

    if (rob_it->is_memory)
        rob_it->scheduled = INFLIGHT;
    else {
        rob_it->scheduled = COMPLETED;

        // ADD LATENCY
        rob_it->event_cycle = current_cycle + (warmup_complete[cpu] ? SCHEDULING_LATENCY : 0);
    }
}

void O3_CPU::execute_instruction()
{
    // out-of-order execution for non-memory instructions
    // memory instructions are handled by memory_instruction()
    uint32_t exec_issued = 0;
    while (exec_issued < EXEC_WIDTH && !ready_to_execute.empty())
    {
        do_execution(ready_to_execute.front());
        ready_to_execute.pop();
        exec_issued++;
    }
}

void O3_CPU::do_execution(champsim::circular_buffer<ooo_model_instr>::iterator rob_it)
{
    rob_it->executed = INFLIGHT;

    // ADD LATENCY
    rob_it->event_cycle = current_cycle + (warmup_complete[cpu] ? EXEC_LATENCY : 0);

    DP (if (warmup_complete[cpu]) {
            std::cout << "[ROB] " << __func__ << " non-memory instr_id: " << rob_it->instr_id << " event_cycle: " << rob_it->event_cycle << std::endl;});
}

void O3_CPU::schedule_memory_instruction()
{
    // execution is out-of-order but we have an in-order scheduling algorithm to detect all RAW dependencies
    unsigned search_bw = SCHEDULER_SIZE;
    for (auto rob_it = std::begin(ROB); rob_it != std::end(ROB) && search_bw > 0; ++rob_it)
    {
        if (rob_it->is_memory && rob_it->num_reg_dependent == 0 && (rob_it->scheduled == INFLIGHT))
            do_memory_scheduling(rob_it);

        if (rob_it->executed == 0)
            --search_bw;
    }
}

struct instr_mem_will_produce
{
    const uint64_t match_mem;
    explicit instr_mem_will_produce(uint64_t mem) : match_mem(mem) {}
    bool operator() (const ooo_model_instr &test) const
    {
        auto dmem_begin = std::begin(test.destination_memory);
        auto dmem_end   = std::end(test.destination_memory);
        return std::find_if(dmem_begin, dmem_end, eq_addr<ooo_model_instr::lsq_info>(match_mem)) != dmem_end;
    }
};

struct sq_will_forward
{
    //const uint64_t match_id;
    const uint64_t match_addr;
    sq_will_forward(/*uint64_t id,*/ uint64_t addr) : /*match_id(id),*/ match_addr(addr) {}
    bool operator() (const LSQ_ENTRY &sq_test) const
    {
        return sq_test.fetched == COMPLETED /*&& sq_test.instr_id == match_id */&& sq_test.virtual_address == match_addr;
    }
};

void O3_CPU::do_memory_scheduling(champsim::circular_buffer<ooo_model_instr>::iterator rob_it)
{
    // load
    for (auto &smem_info : rob_it->source_memory)
    {
        if (!smem_info.added && !smem_info.will_forward)
        {
            // Is this already in the SQ?
            auto sq_it = std::find_if(std::begin(SQ), std::end(SQ), sq_will_forward(/*prior_it->instr_id, */smem_info.address));
            if (sq_it != std::end(SQ))
            {
                DP(if(warmup_complete[cpu]) {
                        std::cout << "[LQ] " << __func__ << " instr_id: " << rob_it->instr_id << std::hex;
                        std::cout << " full_addr: " << smem_info.address << std::dec << " is forwarded by store instr_id: ";
                        std::cout << sq_it->instr_id << " remain_num_ops: " << rob_it->num_mem_ops << " cycle: " << current_cycle << std::endl; });

                rob_it->num_mem_ops--;
                rob_it->event_cycle = current_cycle;
                assert(rob_it->num_mem_ops >= 0);

                smem_info.added = true;
            }
            else
            {
                // Mark RAW in the ROB since the producer might not be added in the store queue yet
                auto prior_it = std::find_if(std::reverse_iterator{rob_it}, std::rend(ROB), instr_mem_will_produce(smem_info.address));
                if (prior_it != std::rend(ROB))
                {
                    // this load cannot be executed until the prior store gets executed
                    prior_it->memory_instrs_depend_on_me.push_back(rob_it);
                    smem_info.will_forward = true;
                }
                else
                {
                    auto lq_it = std::find_if_not(std::begin(LQ), std::end(LQ), is_valid<LSQ_ENTRY>());
                    if (lq_it != std::end(LQ))
                    {
                        *lq_it = {rob_it->instr_id, smem_info.address, rob_it->ip, rob_it->asid[0], rob_it->asid[1], rob_it, current_cycle+SCHEDULING_LATENCY};
                        smem_info.q_it = lq_it;
                        smem_info.added = true;

                        // If this entry is not waiting on RAW
                        RTL0.push(lq_it);
                    }
                    else
                    {
                        DP(if(warmup_complete[cpu]) {
                                cout << "[LQ] " << __func__ << " instr_id: " << rob_it->instr_id;
                                cout << " cannot be added in the load queue occupancy: " << std::count_if(std::begin(LQ), std::end(LQ), is_valid<LSQ_ENTRY>()) << " cycle: " << current_cycle << endl; });
                    }
                }
            }
        }
    }

    // store
    for (uint32_t i=0; i<std::size(rob_it->destination_memory); i++)
    {
        if (!rob_it->destination_memory[i].added)
        {
            auto sq_it = std::find_if_not(std::begin(SQ), std::end(SQ), is_valid<LSQ_ENTRY>());
            if (sq_it != std::end(SQ))
            {
                if (STA.front() == rob_it->instr_id)
                    add_store_queue(rob_it, i);
            }
            else
            {
                DP(if(warmup_complete[cpu]) {
                cout << "[SQ] " << __func__ << " instr_id: " << rob_it->instr_id;
                cout << " cannot be added in the store queue occupancy: " << std::count_if(std::begin(SQ), std::end(SQ), is_valid<LSQ_ENTRY>()) << " cycle: " << current_cycle << endl; });
            }
        }
    }

    auto all_smem = std::all_of(std::begin(rob_it->source_memory), std::end(rob_it->source_memory), [](ooo_model_instr::lsq_info x){ return x.added; });
    auto all_dmem = std::all_of(std::begin(rob_it->destination_memory), std::end(rob_it->destination_memory), [](ooo_model_instr::lsq_info x){ return x.added; });

    if (all_smem && all_dmem) {
        rob_it->scheduled = COMPLETED;
        if (rob_it->executed == 0) // it could be already set to COMPLETED due to store-to-load forwarding
            rob_it->executed  = INFLIGHT;

        DP (if (warmup_complete[cpu]) {
        cout << "[ROB] " << __func__ << " instr_id: " << rob_it->instr_id;
        cout << " scheduled all num_mem_ops: " << rob_it->num_mem_ops << endl; });
    }
}

void O3_CPU::add_store_queue(champsim::circular_buffer<ooo_model_instr>::iterator rob_it, uint32_t data_index)
{
    auto sq_it = std::find_if_not(std::begin(SQ), std::end(SQ), is_valid<LSQ_ENTRY>());
    assert(sq_it->virtual_address == 0);

    // add it to the store queue
    rob_it->destination_memory[data_index].q_it = sq_it;
    sq_it->instr_id = rob_it->instr_id;
    sq_it->virtual_address = rob_it->destination_memory[data_index].address;
    sq_it->ip = rob_it->ip;
    sq_it->rob_index = rob_it;
    sq_it->asid[0] = rob_it->asid[0];
    sq_it->asid[1] = rob_it->asid[1];
    sq_it->event_cycle = current_cycle + SCHEDULING_LATENCY;

    // succesfully added to the store queue
    rob_it->destination_memory[data_index].added = true;

    STA.pop_front();

    RTS0.push(sq_it);

    DP(if(warmup_complete[cpu]) {
            std::cout << "[SQ] " << __func__ << " instr_id: " << sq_it->instr_id;
            std::cout << " is added in the SQ translated: " << +sq_it->translated << " fetched: " << +sq_it->fetched;
            std::cout << " cycle: " << current_cycle << std::endl; });
}

void O3_CPU::operate_lsq()
{
    // handle store

    uint32_t store_issued = 0;

    while (store_issued < SQ_WIDTH && !RTS0.empty())
    {
        // add it to DTLB
        int rq_index = do_translate_store(RTS0.front());

        if (rq_index == -2)
            break;

        RTS0.pop();
        store_issued++;
    }

    while (store_issued < SQ_WIDTH && !RTS1.empty())
    {
        execute_store(RTS1.front());

        RTS1.pop();
        store_issued++;
    }

    unsigned load_issued = 0;

    while (load_issued < LQ_WIDTH && !RTL0.empty())
    {
        // add it to DTLB
        int rq_index = do_translate_load(RTL0.front());

        if (rq_index == -2)
            break;

        RTL0.pop();
        load_issued++;
    }

    while (load_issued < LQ_WIDTH && !RTL1.empty())
    {
        int rq_index = execute_load(RTL1.front());

        if (rq_index == -2)
            break;

        RTL1.pop();
        load_issued++;
    }
}

int O3_CPU::do_translate_store(std::vector<LSQ_ENTRY>::iterator sq_it)
{
    PACKET data_packet;

    data_packet.fill_level = DTLB_bus.lower_level->fill_level;
    data_packet.cpu = cpu;
    data_packet.address = sq_it->virtual_address;
    data_packet.v_address = sq_it->virtual_address;
    data_packet.instr_id = sq_it->instr_id;
    data_packet.ip = sq_it->ip;
    data_packet.type = RFO;
    data_packet.asid[0] = sq_it->asid[0];
    data_packet.asid[1] = sq_it->asid[1];
    data_packet.to_return = {&DTLB_bus};
    data_packet.sq_index_depend_on_me = {sq_it};

    DP (if (warmup_complete[cpu]) {
            std::cout << "[RTS0] " << __func__ << " instr_id: " << sq_it->instr_id << " rob_index: " << sq_it->rob_index << " is popped from to RTS0" << std::endl; })

    int rq_index = DTLB_bus.lower_level->add_rq(&data_packet);

    if (rq_index != -2)
        sq_it->translated = INFLIGHT;

    return rq_index;
}

void O3_CPU::execute_store(std::vector<LSQ_ENTRY>::iterator sq_it)
{
    sq_it->fetched = COMPLETED;
    sq_it->event_cycle = current_cycle;

    sq_it->rob_index->num_mem_ops--;
    sq_it->rob_index->event_cycle = current_cycle;
    assert(sq_it->rob_index->num_mem_ops >= 0);

    DP (if (warmup_complete[cpu]) {
            std::cout << "[SQ1] " << __func__ << " instr_id: " << sq_it->instr_id << std::hex;
            std::cout << " full_address: " << sq_it->physical_address << std::dec << " remain_mem_ops: " << sq_it->rob_index->num_mem_ops;
            std::cout << " event_cycle: " << sq_it->event_cycle << std::endl; });

    // resolve RAW dependency after DTLB access
    // check if this store has dependent loads
    for (auto dependent : sq_it->rob_index->memory_instrs_depend_on_me) {
        // check if dependent loads are already added in the load queue
        auto dsm_info = std::find_if(std::begin(dependent->source_memory), std::end(dependent->source_memory), eq_addr<ooo_model_instr::lsq_info>(sq_it->virtual_address));

        if (dsm_info != std::end(dependent->source_memory))
        {
            DP(if(warmup_complete[cpu]) {
                std::cout << "[LQ] " << __func__ << " instr_id: " << dependent->instr_id << std::hex;
                std::cout << " full_addr: " << dsm_info->address << std::dec << " is forwarded by store instr_id: ";
                std::cout << sq_it->instr_id << " remain_num_ops: " << dependent->num_mem_ops << " cycle: " << current_cycle << std::endl; });

            dependent->num_mem_ops--;
            dependent->event_cycle = current_cycle;
            assert(dependent->num_mem_ops >= 0);

            dsm_info->added = true;
        }
    }
}

int O3_CPU::do_translate_load(std::vector<LSQ_ENTRY>::iterator lq_it)
{
    PACKET data_packet;
    data_packet.fill_level = DTLB_bus.lower_level->fill_level;
    data_packet.cpu = cpu;
    data_packet.address = lq_it->virtual_address;
    data_packet.v_address = lq_it->virtual_address;
    data_packet.instr_id = lq_it->instr_id;
    data_packet.ip = lq_it->ip;
    data_packet.type = LOAD;
    data_packet.asid[0] = lq_it->asid[0];
    data_packet.asid[1] = lq_it->asid[1];
    data_packet.to_return = {&DTLB_bus};
    data_packet.lq_index_depend_on_me = {lq_it};

    DP (if (warmup_complete[cpu]) {
            std::cout << "[RTL0] " << __func__ << " instr_id: " << lq_it->instr_id << " rob_index: " << lq_it->rob_index << " is popped to RTL0" << std::endl; })

    int rq_index = DTLB_bus.lower_level->add_rq(&data_packet);

    if (rq_index != -2)
        lq_it->translated = INFLIGHT;

    return rq_index;
}

int O3_CPU::execute_load(std::vector<LSQ_ENTRY>::iterator lq_it)
{
    // add it to L1D
    PACKET data_packet;
    data_packet.fill_level = L1D_bus.lower_level->fill_level;
    data_packet.cpu = cpu;
    data_packet.address = lq_it->physical_address;
    data_packet.v_address = lq_it->virtual_address;
    data_packet.instr_id = lq_it->instr_id;
    data_packet.ip = lq_it->ip;
    data_packet.type = LOAD;
    data_packet.asid[0] = lq_it->asid[0];
    data_packet.asid[1] = lq_it->asid[1];
    data_packet.to_return = {&L1D_bus};
    data_packet.lq_index_depend_on_me = {lq_it};

    int rq_index = L1D_bus.lower_level->add_rq(&data_packet);

    if (rq_index != -2)
        lq_it->fetched = INFLIGHT;

    return rq_index;
}

void O3_CPU::do_complete_execution(champsim::circular_buffer<ooo_model_instr>::iterator rob_it)
{
    rob_it->executed = COMPLETED;

    for (auto dependent : rob_it->registers_instrs_depend_on_me)
    {
        dependent->num_reg_dependent--;
        assert(dependent->num_reg_dependent >= 0);

        if (dependent->num_reg_dependent == 0) {
            if (dependent->is_memory)
                dependent->scheduled = INFLIGHT;
            else {
                dependent->scheduled = COMPLETED;
            }
        }
    }

    if (rob_it->branch_mispredicted)
        fetch_resume_cycle = current_cycle + BRANCH_MISPREDICT_PENALTY;
}

void O3_CPU::complete_inflight_instruction()
{
    // update ROB entries with completed executions
    std::size_t complete_bw = EXEC_WIDTH;
    for (auto rob_it = std::begin(ROB); rob_it != std::end(ROB) && complete_bw > 0; ++rob_it)
    {
        if ((rob_it->executed == INFLIGHT) && (rob_it->event_cycle <= current_cycle) && rob_it->num_mem_ops == 0)
        {
            do_complete_execution(rob_it);
            --complete_bw;

            for (auto dependent : rob_it->registers_instrs_depend_on_me)
            {
                if (dependent->scheduled == COMPLETED && dependent->num_reg_dependent == 0)
                {
                    assert(ready_to_execute.size() < ROB.size());
                    ready_to_execute.push(dependent);

                    DP ( if (warmup_complete[cpu]) {
                            std::cout << "[ready_to_execute] " << __func__ << " instr_id: " << dependent->instr_id << " is added to ready_to_execute" << std::endl; })
                }
            }

        }
    }
}

void O3_CPU::handle_memory_return()
{
  // Instruction Memory

  std::size_t available_fetch_bandwidth = FETCH_WIDTH;
  std::size_t to_read = static_cast<CACHE*>(ITLB_bus.lower_level)->MAX_READ;

  while (available_fetch_bandwidth > 0 && to_read > 0 && !ITLB_bus.PROCESSED.empty())
    {
        PACKET &itlb_entry = ITLB_bus.PROCESSED.front();

      // mark the appropriate instructions in the IFETCH_BUFFER as translated and ready to fetch
      while (available_fetch_bandwidth > 0 && !itlb_entry.instr_depend_on_me.empty())
      {
          auto it = itlb_entry.instr_depend_on_me.front();
          if ((it->ip >> LOG2_PAGE_SIZE) == (itlb_entry.address >> LOG2_PAGE_SIZE) && it->translated != 0)
          {
              //if ((it->ip >> LOG2_PAGE_SIZE) == (itlb_entry.address >> LOG2_PAGE_SIZE) && it->translated != 0)
              {
                  it->translated = COMPLETED;
                  // recalculate a physical address for this cache line based on the translated physical page address
                  it->instruction_pa = splice_bits(itlb_entry.data << LOG2_PAGE_SIZE, it->ip, LOG2_PAGE_SIZE);
              }

              available_fetch_bandwidth--;
          }

          itlb_entry.instr_depend_on_me.pop_front();
      }


      // remove this entry if we have serviced all of its instructions
      if (itlb_entry.instr_depend_on_me.empty())
      {
          ITLB_bus.PROCESSED.pop_front();
      }
      --to_read;
  }

  available_fetch_bandwidth = FETCH_WIDTH;
  to_read = static_cast<CACHE*>(L1I_bus.lower_level)->MAX_READ;

  while (available_fetch_bandwidth > 0 && to_read > 0 && !L1I_bus.PROCESSED.empty())
  {
      PACKET &l1i_entry = L1I_bus.PROCESSED.front();

      // this is the L1I cache, so instructions are now fully fetched, so mark them as such
      while (available_fetch_bandwidth > 0 && !l1i_entry.instr_depend_on_me.empty())
      {
          auto it = l1i_entry.instr_depend_on_me.front();
          if ((it->instruction_pa >> LOG2_BLOCK_SIZE) == (l1i_entry.address >> LOG2_BLOCK_SIZE) && it->fetched != 0 && it->translated == COMPLETED)
          {
              it->fetched = COMPLETED;
              available_fetch_bandwidth--;
          }

          l1i_entry.instr_depend_on_me.pop_front();
      }

      // remove this entry if we have serviced all of its instructions
      if (l1i_entry.instr_depend_on_me.empty())
          L1I_bus.PROCESSED.pop_front();
      --to_read;
  }

  // Data Memory
  to_read = static_cast<CACHE*>(DTLB_bus.lower_level)->MAX_READ;

  while (to_read > 0 && !DTLB_bus.PROCESSED.empty())
	{ // DTLB
	  PACKET &dtlb_entry = DTLB_bus.PROCESSED.front();

	  for (auto sq_merged : dtlb_entry.sq_index_depend_on_me)
	    {
	      sq_merged->physical_address = splice_bits(dtlb_entry.data << LOG2_PAGE_SIZE, sq_merged->virtual_address, LOG2_PAGE_SIZE); // translated address
	      sq_merged->translated = COMPLETED;
	      sq_merged->event_cycle = current_cycle;

          RTS1.push(sq_merged);
	    }

	  for (auto lq_merged : dtlb_entry.lq_index_depend_on_me)
	    {
	      lq_merged->physical_address = splice_bits(dtlb_entry.data << LOG2_PAGE_SIZE, lq_merged->virtual_address, LOG2_PAGE_SIZE); // translated address
	      lq_merged->translated = COMPLETED;
	      lq_merged->event_cycle = current_cycle;

          RTL1.push(lq_merged);
	    }

	  // remove this entry
	  DTLB_bus.PROCESSED.pop_front();
      --to_read;
    }

  to_read = static_cast<CACHE*>(L1D_bus.lower_level)->MAX_READ;
  while (to_read > 0 && !L1D_bus.PROCESSED.empty())
	{ // L1D
	  PACKET &l1d_entry = L1D_bus.PROCESSED.front();

	  for (auto merged : l1d_entry.lq_index_depend_on_me)
	    {
            merged->fetched = COMPLETED;
            merged->event_cycle = current_cycle;
            merged->rob_index->num_mem_ops--;
            merged->rob_index->event_cycle = current_cycle;

            *merged = {};
	    }

	  // remove this entry
	  L1D_bus.PROCESSED.pop_front();
      --to_read;;
    }
}

void O3_CPU::retire_rob()
{
    unsigned retire_bandwidth = RETIRE_WIDTH;

    while (retire_bandwidth > 0 && !ROB.empty() && (ROB.front().executed == COMPLETED))
    {
        for (auto &dmem : ROB.front().destination_memory) {
            if (dmem.address) {

                PACKET data_packet;
                auto sq_it = dmem.q_it;

                // sq_index and rob_index are no longer available after retirement
                // but we pass this information to avoid segmentation fault
                data_packet.fill_level = FILL_L1;
                data_packet.cpu = cpu;
                data_packet.address = sq_it->physical_address >> LOG2_BLOCK_SIZE;
                data_packet.v_address = sq_it->virtual_address >> LOG2_BLOCK_SIZE;
                data_packet.instr_id = sq_it->instr_id;
                data_packet.ip = sq_it->ip;
                data_packet.type = RFO;
                data_packet.asid[0] = sq_it->asid[0];
                data_packet.asid[1] = sq_it->asid[1];

                auto result = L1D_bus.lower_level->add_wq(&data_packet);
                if (result != -2)
                {
                    dmem.address = 0;
                    *sq_it = {};
                }
                else
                {
                    return;
                }
            }
        }

        // release ROB entry
        DP ( if (warmup_complete[cpu]) {
                cout << "[ROB] " << __func__ << " instr_id: " << ROB.front().instr_id << " is retired" << endl; });

        ROB.pop_front();
        num_retired++;
        retire_bandwidth--;
    }
}

void CacheBus::return_data(PACKET *packet)
{
    if (packet->type != PREFETCH)
    {
        PROCESSED.push_back(*packet);
    }
}


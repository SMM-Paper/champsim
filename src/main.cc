#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <getopt.h>
#include <fstream>
#include <functional>
#include <iomanip>
#include <numeric>
#include <signal.h>
#include <string.h>
#include <vector>

#include "champsim_constants.h"
#include "dram_controller.h"
#include "ooo_cpu.h"
#include "operable.h"
#include "vmem.h"
#include "tracereader.h"

std::bitset<NUM_CPUS> warmup_complete = {};
std::size_t MAX_INSTR_DESTINATIONS = NUM_INSTR_DESTINATIONS;
bool knob_cloudsuite = false,
     knob_heartbeat = true;

uint64_t warmup_instructions     = 1000000,
         simulation_instructions = 10000000,
         champsim_seed;

const auto start_time = std::chrono::steady_clock::now();

extern MEMORY_CONTROLLER DRAM;
extern VirtualMemory vmem;
extern std::array<O3_CPU*, NUM_CPUS> ooo_cpu;
extern std::array<CACHE*, NUM_CACHES> caches;
extern std::array<champsim::operable*, NUM_OPERABLES> operables;

std::vector<tracereader*> traces;

void record_roi_stats(uint32_t cpu, CACHE *cache)
{
    std::copy(std::begin(cache->sim_hit[cpu]), std::end(cache->sim_hit[cpu]), std::begin(cache->roi_hit[cpu]));
    std::copy(std::begin(cache->sim_miss[cpu]), std::end(cache->sim_miss[cpu]), std::begin(cache->roi_miss[cpu]));

    cache->roi_pf_requested   = cache->pf_requested;
    cache->roi_pf_issued      = cache->pf_issued;
    cache->roi_pf_fill        = cache->pf_fill;
    cache->roi_pf_useful      = cache->pf_useful;
    cache->roi_pf_useless     = cache->pf_useless;
    cache->roi_pf_polluting   = cache->pf_polluting;
}

void print_roi_stats(CACHE *cache)
{
    std::bitset<NUM_CPUS> active_cpus;
    uint64_t TOTAL_MISS = 0;

    for (std::size_t cpu = 0; cpu < NUM_CPUS; ++cpu)
    {
        uint64_t PER_CPU_HIT  = std::accumulate(std::begin(cache->roi_hit[cpu]), std::end(cache->roi_hit[cpu]), 0ll);
        uint64_t PER_CPU_MISS = std::accumulate(std::begin(cache->roi_miss[cpu]), std::end(cache->roi_miss[cpu]), 0ll);

        if (PER_CPU_HIT > 0 || PER_CPU_MISS > 0)
        {
            std::cout << "CPU" << cpu << " " << cache->NAME << " ROI TOTAL      ";
            std::cout << "  ACCESS: " << std::setw(10) << PER_CPU_HIT + PER_CPU_MISS;
            std::cout << "  HIT: "    << std::setw(10) << PER_CPU_HIT;
            std::cout << "  MISS: "   << std::setw(10) << PER_CPU_MISS;
            std::cout << std::endl;

            std::cout << "CPU" << cpu << " " << cache->NAME << " ROI LOAD       ";
            std::cout << "  ACCESS: " << std::setw(10) << cache->roi_hit[cpu][LOAD] + cache->roi_miss[cpu][LOAD];
            std::cout << "  HIT: "    << std::setw(10) << cache->roi_hit[cpu][LOAD];
            std::cout << "  MISS: "   << std::setw(10) << cache->roi_miss[cpu][LOAD];
            std::cout << std::endl;

            std::cout << "CPU" << cpu << " " << cache->NAME << " ROI RFO        ";
            std::cout << "  ACCESS: " << std::setw(10) << cache->roi_hit[cpu][RFO] + cache->roi_miss[cpu][RFO];
            std::cout << "  HIT: "    << std::setw(10) << cache->roi_hit[cpu][RFO];
            std::cout << "  MISS: "   << std::setw(10) << cache->roi_miss[cpu][RFO];
            std::cout << std::endl;

            std::cout << "CPU" << cpu << " " << cache->NAME << " ROI PREFETCH   ";
            std::cout << "  ACCESS: " << std::setw(10) << cache->roi_hit[cpu][PREFETCH] + cache->roi_miss[cpu][PREFETCH];
            std::cout << "  HIT: "    << std::setw(10) << cache->roi_hit[cpu][PREFETCH];
            std::cout << "  MISS: "   << std::setw(10) << cache->roi_miss[cpu][PREFETCH];
            std::cout << std::endl;

            std::cout << "CPU" << cpu << " " << cache->NAME << " ROI WRITEBACK  ";
            std::cout << "  ACCESS: " << std::setw(10) << cache->roi_hit[cpu][WRITEBACK] + cache->roi_miss[cpu][WRITEBACK];
            std::cout << "  HIT: "    << std::setw(10) << cache->roi_hit[cpu][WRITEBACK];
            std::cout << "  MISS: "   << std::setw(10) << cache->roi_miss[cpu][WRITEBACK];
            std::cout << std::endl;

            std::cout << "CPU" << cpu << " " << cache->NAME << " ROI TRANSLATION";
            std::cout << "  ACCESS: " << std::setw(10) << cache->roi_hit[cpu][TRANSLATION] + cache->roi_miss[cpu][TRANSLATION];
            std::cout << "  HIT: "    << std::setw(10) << cache->roi_hit[cpu][TRANSLATION];
            std::cout << "  MISS: "   << std::setw(10) << cache->roi_miss[cpu][TRANSLATION];
            std::cout << std::endl;

            active_cpus.set(cpu);
        }

        TOTAL_MISS += PER_CPU_MISS;
    }

    std::size_t cpu = 0;
    while (cpu < NUM_CPUS && !active_cpus.test(cpu)) ++cpu;

    if (active_cpus.count() == 1)
        std::cout << "CPU" << cpu << " ";
    std::cout << cache->NAME << " ROI PREFETCH ";
    std::cout << "  REQUESTED: "   << std::setw(10) << cache->roi_pf_requested;
    std::cout << "  ISSUED: "      << std::setw(10) << cache->roi_pf_issued;
    std::cout << "  FILLED: "      << std::setw(10) << cache->roi_pf_fill;
    std::cout << "  USEFUL: "      << std::setw(10) << cache->roi_pf_useful;
    std::cout << "  USELESS: "     << std::setw(10) << cache->roi_pf_useless;
    std::cout << "  POLLUTING: "   << std::setw(10) << cache->roi_pf_polluting;
    std::cout << std::endl;

    if (active_cpus.count() == 1)
        std::cout << "CPU" << cpu << " ";
    std::cout << cache->NAME;
    std::cout << " AVERAGE MISS LATENCY: " << (1.0*(cache->total_miss_latency))/TOTAL_MISS << " cycles";
    std::cout << std::endl;
}

void print_sim_stats(CACHE *cache)
{
    std::bitset<NUM_CPUS> active_cpus;
    uint64_t TOTAL_MISS = 0;

    for (std::size_t cpu = 0; cpu < NUM_CPUS; ++cpu)
    {
        uint64_t PER_CPU_HIT  = std::accumulate(std::begin(cache->sim_hit[cpu]), std::end(cache->sim_hit[cpu]), 0ll);
        uint64_t PER_CPU_MISS = std::accumulate(std::begin(cache->sim_miss[cpu]), std::end(cache->sim_miss[cpu]), 0ll);

        if (PER_CPU_HIT > 0 || PER_CPU_MISS > 0)
        {
            std::cout << "CPU" << cpu << " " << cache->NAME << " SIM TOTAL    ";
            std::cout << "  ACCESS: " << std::setw(10) << PER_CPU_HIT + PER_CPU_MISS;
            std::cout << "  HIT: "    << std::setw(10) << PER_CPU_HIT;
            std::cout << "  MISS: "   << std::setw(10) << PER_CPU_MISS;
            std::cout << std::endl;

            std::cout << "CPU" << cpu << " " << cache->NAME << " SIM LOAD     ";
            std::cout << "  ACCESS: " << std::setw(10) << cache->sim_hit[cpu][LOAD] + cache->sim_miss[cpu][LOAD];
            std::cout << "  HIT: "    << std::setw(10) << cache->sim_hit[cpu][LOAD];
            std::cout << "  MISS: "   << std::setw(10) << cache->sim_miss[cpu][LOAD];
            std::cout << std::endl;

            std::cout << "CPU" << cpu << " " << cache->NAME << " SIM RFO      ";
            std::cout << "  ACCESS: " << std::setw(10) << cache->sim_hit[cpu][RFO] + cache->sim_miss[cpu][RFO];
            std::cout << "  HIT: "    << std::setw(10) << cache->sim_hit[cpu][RFO];
            std::cout << "  MISS: "   << std::setw(10) << cache->sim_miss[cpu][RFO];
            std::cout << std::endl;

            std::cout << "CPU" << cpu << " " << cache->NAME << " SIM PREFETCH ";
            std::cout << "  ACCESS: " << std::setw(10) << cache->sim_hit[cpu][PREFETCH] + cache->sim_miss[cpu][PREFETCH];
            std::cout << "  HIT: "    << std::setw(10) << cache->sim_hit[cpu][PREFETCH];
            std::cout << "  MISS: "   << std::setw(10) << cache->sim_miss[cpu][PREFETCH];
            std::cout << std::endl;

            std::cout << "CPU" << cpu << " " << cache->NAME << " SIM WRITEBACK";
            std::cout << "  ACCESS: " << std::setw(10) << cache->sim_hit[cpu][WRITEBACK] + cache->sim_miss[cpu][WRITEBACK];
            std::cout << "  HIT: "    << std::setw(10) << cache->sim_hit[cpu][WRITEBACK];
            std::cout << "  MISS: "   << std::setw(10) << cache->sim_miss[cpu][WRITEBACK];
            std::cout << std::endl;

            active_cpus.set(cpu);
        }

        TOTAL_MISS += PER_CPU_MISS;
    }

    std::size_t cpu = 0;
    while (cpu < NUM_CPUS && !active_cpus.test(cpu)) ++cpu;

    if (active_cpus.count() == 1)
        std::cout << "CPU" << cpu << " ";
    std::cout << cache->NAME << " SIM PREFETCH ";
    std::cout << "  REQUESTED: "   << std::setw(10) << cache->pf_requested;
    std::cout << "  ISSUED: "      << std::setw(10) << cache->pf_issued;
    std::cout << "  FILLED: "      << std::setw(10) << cache->pf_fill;
    std::cout << "  USEFUL: "      << std::setw(10) << cache->pf_useful;
    std::cout << "  USELESS: "     << std::setw(10) << cache->pf_useless;
    std::cout << "  POLLUTING: "   << std::setw(10) << cache->pf_polluting;
    std::cout << std::endl;

    if (active_cpus.count() == 1)
        std::cout << "CPU" << cpu << " ";
    std::cout << cache->NAME;
    std::cout << " AVERAGE MISS LATENCY: " << (1.0*(cache->total_miss_latency))/TOTAL_MISS << " cycles";
    std::cout << std::endl;
}

void print_branch_stats()
{
    for (uint32_t i=0; i<NUM_CPUS; i++) {
        cout << endl << "CPU " << i << " Branch Prediction Accuracy: ";
        cout << (100.0*(ooo_cpu[i]->num_branch - ooo_cpu[i]->branch_mispredictions)) / ooo_cpu[i]->num_branch;
        cout << "% MPKI: " << (1000.0*ooo_cpu[i]->branch_mispredictions)/(ooo_cpu[i]->num_retired - warmup_instructions);
	cout << " Average ROB Occupancy at Mispredict: " << (1.0*ooo_cpu[i]->total_rob_occupancy_at_branch_mispredict)/ooo_cpu[i]->branch_mispredictions << endl;

	/*
	cout << "Branch types" << endl;
	cout << "NOT_BRANCH: " << ooo_cpu[i]->total_branch_types[0] << " " << (100.0*ooo_cpu[i]->total_branch_types[0])/(ooo_cpu[i]->num_retired - ooo_cpu[i]->begin_phase_instr) << "%" << endl;
	cout << "BRANCH_DIRECT_JUMP: " << ooo_cpu[i]->total_branch_types[1] << " " << (100.0*ooo_cpu[i]->total_branch_types[1])/(ooo_cpu[i]->num_retired - ooo_cpu[i]->begin_phase_instr) << "%" << endl;
	cout << "BRANCH_INDIRECT: " << ooo_cpu[i]->total_branch_types[2] << " " << (100.0*ooo_cpu[i]->total_branch_types[2])/(ooo_cpu[i]->num_retired - ooo_cpu[i]->begin_phase_instr) << "%" << endl;
	cout << "BRANCH_CONDITIONAL: " << ooo_cpu[i]->total_branch_types[3] << " " << (100.0*ooo_cpu[i]->total_branch_types[3])/(ooo_cpu[i]->num_retired - ooo_cpu[i]->begin_phase_instr) << "%" << endl;
	cout << "BRANCH_DIRECT_CALL: " << ooo_cpu[i]->total_branch_types[4] << " " << (100.0*ooo_cpu[i]->total_branch_types[4])/(ooo_cpu[i]->num_retired - ooo_cpu[i]->begin_phase_instr) << "%" << endl;
	cout << "BRANCH_INDIRECT_CALL: " << ooo_cpu[i]->total_branch_types[5] << " " << (100.0*ooo_cpu[i]->total_branch_types[5])/(ooo_cpu[i]->num_retired - ooo_cpu[i]->begin_phase_instr) << "%" << endl;
	cout << "BRANCH_RETURN: " << ooo_cpu[i]->total_branch_types[6] << " " << (100.0*ooo_cpu[i]->total_branch_types[6])/(ooo_cpu[i]->num_retired - ooo_cpu[i]->begin_phase_instr) << "%" << endl;
	cout << "BRANCH_OTHER: " << ooo_cpu[i]->total_branch_types[7] << " " << (100.0*ooo_cpu[i]->total_branch_types[7])/(ooo_cpu[i]->num_retired - ooo_cpu[i]->begin_phase_instr) << "%" << endl << endl;
	*/

	cout << "Branch type MPKI" << endl;
	cout << "BRANCH_DIRECT_JUMP: " << (1000.0*ooo_cpu[i]->branch_type_misses[1]/(ooo_cpu[i]->num_retired - ooo_cpu[i]->begin_phase_instr)) << endl;
	cout << "BRANCH_INDIRECT: " << (1000.0*ooo_cpu[i]->branch_type_misses[2]/(ooo_cpu[i]->num_retired - ooo_cpu[i]->begin_phase_instr)) << endl;
	cout << "BRANCH_CONDITIONAL: " << (1000.0*ooo_cpu[i]->branch_type_misses[3]/(ooo_cpu[i]->num_retired - ooo_cpu[i]->begin_phase_instr)) << endl;
	cout << "BRANCH_DIRECT_CALL: " << (1000.0*ooo_cpu[i]->branch_type_misses[4]/(ooo_cpu[i]->num_retired - ooo_cpu[i]->begin_phase_instr)) << endl;
	cout << "BRANCH_INDIRECT_CALL: " << (1000.0*ooo_cpu[i]->branch_type_misses[5]/(ooo_cpu[i]->num_retired - ooo_cpu[i]->begin_phase_instr)) << endl;
	cout << "BRANCH_RETURN: " << (1000.0*ooo_cpu[i]->branch_type_misses[6]/(ooo_cpu[i]->num_retired - ooo_cpu[i]->begin_phase_instr)) << endl << endl;
    }
}

void print_dram_stats()
{
    uint64_t total_congested_cycle = 0;
    uint64_t total_congested_count = 0;
    for (uint32_t i=0; i<DRAM_CHANNELS; i++)
    {
        total_congested_cycle += DRAM.channels[i].dbus_cycle_congested;
        total_congested_count += DRAM.channels[i].dbus_count_congested;
    }

    std::cout << std::endl;
    std::cout << "DRAM Statistics" << std::endl;
    for (uint32_t i=0; i<DRAM_CHANNELS; i++) {
        std::cout << " CHANNEL " << i << std::endl;
        std::cout << " RQ ROW_BUFFER_HIT: " << std::setw(10) << DRAM.channels[i].RQ_ROW_BUFFER_HIT << "  ROW_BUFFER_MISS: " << std::setw(10) << DRAM.channels[i].RQ_ROW_BUFFER_MISS << std::endl;
        std::cout << " DBUS_CONGESTED: " << std::setw(10) << total_congested_count << std::endl;
        std::cout << " WQ ROW_BUFFER_HIT: " << std::setw(10) << DRAM.channels[i].WQ_ROW_BUFFER_HIT << "  ROW_BUFFER_MISS: " << std::setw(10) << DRAM.channels[i].WQ_ROW_BUFFER_MISS;
        std::cout << "  FULL: " << setw(10) << DRAM.channels[i].WQ_FULL << std::endl;
        std::cout << std::endl;
    }

    if (total_congested_count)
        cout << " AVG_CONGESTED_CYCLE: " << ((double)total_congested_cycle / total_congested_count) << endl;
    else
        cout << " AVG_CONGESTED_CYCLE: -" << endl;
}

std::tuple<uint64_t, uint64_t, uint64_t> elapsed_time()
{
    auto duration = std::chrono::steady_clock::now() - start_time;

    auto hours = std::chrono::floor<std::chrono::hours>(duration);
    auto minutes = std::chrono::floor<std::chrono::minutes>(duration) - hours;
    auto seconds = std::chrono::floor<std::chrono::seconds>(duration) - hours - minutes;

    return { hours.count(), minutes.count(), seconds.count() };
}

void print_deadlock(uint32_t i)
{
    cout << "DEADLOCK! CPU " << i << " instr_id: " << ooo_cpu[i]->ROB.front().instr_id;
    cout << " translated: " << +ooo_cpu[i]->ROB.front().translated;
    cout << " fetched: " << +ooo_cpu[i]->ROB.front().fetched;
    cout << " scheduled: " << +ooo_cpu[i]->ROB.front().scheduled;
    cout << " executed: " << +ooo_cpu[i]->ROB.front().executed;
    cout << " is_memory: " << +ooo_cpu[i]->ROB.front().is_memory;
    cout << " num_reg_dependent: " << +ooo_cpu[i]->ROB.front().num_reg_dependent;
    cout << " event: " << ooo_cpu[i]->ROB.front().event_cycle;
    cout << " current: " << ooo_cpu[i]->current_cycle << endl;

    // print LQ entry
    cout << endl << "Load Queue Entry" << endl;
    for (uint32_t j=0; j<ooo_cpu[i]->LQ.size(); j++) {
        cout << "[LQ] entry: " << j << " instr_id: " << ooo_cpu[i]->LQ[j].instr_id << " address: " << hex << ooo_cpu[i]->LQ[j].physical_address << dec << " translated: " << +ooo_cpu[i]->LQ[j].translated << " fetched: " << +ooo_cpu[i]->LQ[i].fetched << endl;
    }

    // print SQ entry
    cout << endl << "Store Queue Entry" << endl;
    for (uint32_t j=0; j<ooo_cpu[i]->SQ.size(); j++) {
        cout << "[SQ] entry: " << j << " instr_id: " << ooo_cpu[i]->SQ[j].instr_id << " address: " << hex << ooo_cpu[i]->SQ[j].physical_address << dec << " translated: " << +ooo_cpu[i]->SQ[j].translated << " fetched: " << +ooo_cpu[i]->SQ[i].fetched << endl;
    }

    // print L1D MSHR entry
    std::cout << std::endl << "L1D MSHR Entry" << std::endl;
    std::size_t j = 0;
    for (PACKET &entry : static_cast<CACHE*>(ooo_cpu[i]->L1D_bus.lower_level)->MSHR) {
        std::cout << "[L1D MSHR] entry: " << j << " instr_id: " << entry.instr_id;
        std::cout << " address: " << std::hex << (entry.address >> LOG2_BLOCK_SIZE) << " full_addr: " << entry.address << std::dec << " type: " << +entry.type;
        std::cout << " fill_level: " << entry.fill_level << " event_cycle: " << entry.event_cycle << std::endl;
        ++j;
    }

    assert(0);
}

void signal_handler(int signal) 
{
	cout << "Caught signal: " << signal << endl;
	exit(1);
}

int main(int argc, char** argv)
{
	// interrupt signal hanlder
	struct sigaction sigIntHandler;
	sigIntHandler.sa_handler = signal_handler;
	sigemptyset(&sigIntHandler.sa_mask);
	sigIntHandler.sa_flags = 0;
	sigaction(SIGINT, &sigIntHandler, NULL);

    cout << endl << "*** ChampSim Multicore Out-of-Order Simulator ***" << endl << endl;

    // initialize knobs
    uint32_t seed_number = 0;

    // check to see if knobs changed using getopt_long()
    int c;
    while (1) {
        static struct option long_options[] =
        {
            {"warmup_instructions", required_argument, 0, 'w'},
            {"simulation_instructions", required_argument, 0, 'i'},
            {"hide_heartbeat", no_argument, 0, 'h'},
            {"cloudsuite", no_argument, 0, 'c'},
            {"traces",  no_argument, 0, 't'},
            {0, 0, 0, 0}      
        };

        int option_index = 0;

        c = getopt_long_only(argc, argv, "wihsb", long_options, &option_index);

        // no more option characters
        if (c == -1)
            break;

        bool traces_encountered = false;

        switch(c) {
            case 'w':
                warmup_instructions = atol(optarg);
                break;
            case 'i':
                simulation_instructions = atol(optarg);
                break;
            case 'h':
                knob_heartbeat = false;
                break;
            case 'c':
                knob_cloudsuite = true;
                MAX_INSTR_DESTINATIONS = NUM_INSTR_DESTINATIONS_SPARC;
                break;
            case 't':
                traces_encountered = true;
                break;
            default:
                abort();
        }

        if (traces_encountered == true)
            break;
    }

    // consequences of knobs
    cout << "Warmup Instructions: " << warmup_instructions << endl;
    cout << "Simulation Instructions: " << simulation_instructions << endl;
    //cout << "Scramble Loads: " << (knob_scramble_loads ? "ture" : "false") << endl;
    cout << "Number of CPUs: " << NUM_CPUS << endl;
    //cout << "LLC sets: " << LLC.NUM_SET << endl;
    //cout << "LLC ways: " << LLC.NUM_WAY << endl;

    long long int dram_size = DRAM_CHANNELS * DRAM_RANKS * DRAM_BANKS * DRAM_ROWS * DRAM_COLUMNS * DRAM_LINES_PER_COLUMN * BLOCK_SIZE / 1024 / 1024; // in MiB
    std::cout << "Off-chip DRAM Size: ";
    if (dram_size > 1024)
        std::cout << dram_size/1024 << " GiB";
    else
        std::cout << dram_size << " MiB";
    std::cout << " Channels: " << DRAM_CHANNELS << " Width: " << 8*DRAM_CHANNEL_WIDTH << "-bit Data Rate: " << DRAM_IO_FREQ << " MT/s" << std::endl;

    // end consequence of knobs

    // search through the argv for "-traces"
    int found_traces = 0;
    std::cout << std::endl;
    for (int i=0; i<argc; i++) {
        if (found_traces)
        {
            std::cout << "CPU " << traces.size() << " runs " << argv[i] << std::endl;

            traces.push_back(get_tracereader(argv[i], i, knob_cloudsuite));

            char *pch[100];
            int count_str = 0;
            pch[0] = strtok (argv[i], " /,.-");
            while (pch[count_str] != NULL) {
                //printf ("%s %d\n", pch[count_str], count_str);
                count_str++;
                pch[count_str] = strtok (NULL, " /,.-");
            }

            //printf("max count_str: %d\n", count_str);
            //printf("application: %s\n", pch[count_str-3]);

            int j = 0;
            while (pch[count_str-3][j] != '\0') {
                seed_number += pch[count_str-3][j];
                //printf("%c %d %d\n", pch[count_str-3][j], j, seed_number);
                j++;
            }

            if (traces.size() > NUM_CPUS) {
                printf("\n*** Too many traces for the configured number of cores ***\n\n");
                assert(0);
            }
        }
        else if(strcmp(argv[i],"-traces") == 0) {
            found_traces = 1;
        }
    }

    if (traces.size() != NUM_CPUS) {
        printf("\n*** Not enough traces for the configured number of cores ***\n\n");
        assert(0);
    }
    // end trace file setup

    srand(seed_number);
    champsim_seed = seed_number;

    // SHARED CACHE
    for (O3_CPU* cpu : ooo_cpu)
    {
        cpu->initialize_core();
    }

    for (auto it = caches.rbegin(); it != caches.rend(); ++it)
    {
        (*it)->impl_prefetcher_initialize();
        (*it)->impl_replacement_initialize();
    }

    // simulation entry point
    for (auto phase_duration : {warmup_instructions, simulation_instructions})
    {
        std::bitset<NUM_CPUS> phase_complete = {};

        /////
        // PRE-PHASE
        /////

        // reset stats
        for (auto op : operables)
            op->reset_stats();

        // mark cycle begin
        for (auto cpu : ooo_cpu)
        {
            cpu->begin_phase_instr = cpu->num_retired;
            cpu->begin_phase_cycle = cpu->current_cycle;
        }

        /////
        // PHASE
        /////
        while (!phase_complete.all())
        {
            // Operate all elements
            for (auto op : operables)
                op->_operate();
            std::sort(std::begin(operables), std::end(operables), champsim::by_next_operate());

            // Send instructions to CPUs
            for (auto cpu : ooo_cpu)
            {
                while (cpu->instrs_to_read_this_cycle > 0)
                    cpu->init_instruction(traces[cpu->cpu]->get());
            }

            // check for warmup complete
            for (auto cpu : ooo_cpu)
                warmup_complete.set(cpu->cpu, (cpu->num_retired > warmup_instructions));

            // check for phase complete
            for (auto cpu : ooo_cpu)
            {
                if (!phase_complete[cpu->cpu] && (cpu->num_retired >= (cpu->begin_phase_instr + phase_duration)))
                {
                    auto [elapsed_hour, elapsed_minute, elapsed_second] = elapsed_time();
                    phase_complete[cpu->cpu] = true;
                    cpu->finish_phase_instr = cpu->num_retired;
                    cpu->finish_phase_cycle = cpu->current_cycle;

                    std::cout << "Phase finished CPU " << cpu->cpu << " instructions: " << cpu->num_retired << " cycles: " << cpu->current_cycle;
                    std::cout << " cumulative IPC: " << 1.0 * (cpu->finish_phase_instr - cpu->begin_phase_instr) / (cpu->finish_phase_cycle - cpu->begin_phase_cycle);
                    std::cout << " (Simulation time: " << elapsed_hour << " hr " << elapsed_minute << " min " << elapsed_second << " sec) " << std::endl;

                    for (auto cache : caches)
                        record_roi_stats(cpu->cpu, cache);
                }
            }
        }

        /////
        // POST-PHASE
        /////

        std::cout << std::endl;
        for (auto cpu : ooo_cpu)
        {
            auto [elapsed_hour, elapsed_minute, elapsed_second] = elapsed_time();
            std::cout << "Phase complete CPU " << cpu->cpu << " instructions: " << cpu->num_retired << " cycles: " << cpu->current_cycle;
            std::cout << " (Simulation time: " << elapsed_hour << " hr " << elapsed_minute << " min " << elapsed_second << " sec) " << std::endl;
        }
        std::cout << std::endl;
    }

    std::cout << std::endl;
    std::cout << "ChampSim completed all CPUs" << std::endl << std::endl;;

    // In a multicore simulation, print full-time statistics
    // In a single-core environment, the full simulation and the ROI are the same
    if (NUM_CPUS > 1) {
        std::cout << "Total Simulation Statistics (not including warmup)" << std::endl << std::endl;

        for (auto cpu : ooo_cpu)
        {
            std::cout << "CPU" << cpu->cpu << " SIM cumulative IPC: " << 1.0 * (cpu->num_retired - cpu->begin_phase_instr) / (cpu->current_cycle - cpu->begin_phase_cycle);
            std::cout << " instructions: " << cpu->num_retired - cpu->begin_phase_instr;
            std::cout << " cycles: " << cpu->current_cycle - cpu->begin_phase_cycle << std::endl;
        }

        for (auto it = caches.rbegin(); it != caches.rend(); ++it)
            print_sim_stats(*it);
    }

    std::cout << std::endl;
    std::cout << "Region of Interest Statistics" << std::endl << std::endl;
    for (auto cpu : ooo_cpu)
    {
        std::cout << "CPU" << cpu->cpu << " ROI cumulative IPC: " << 1.0 * (cpu->finish_phase_instr - cpu->begin_phase_instr) / (cpu->finish_phase_cycle - cpu->begin_phase_cycle);
        std::cout << " instructions: " << cpu->finish_phase_instr - cpu->begin_phase_instr;
        std::cout << " cycles: " << cpu->finish_phase_cycle - cpu->begin_phase_cycle << std::endl;
    }

    for (auto it = caches.rbegin(); it != caches.rend(); ++it)
        print_roi_stats(*it);

    for (auto it = caches.rbegin(); it != caches.rend(); ++it)
        (*it)->impl_prefetcher_final_stats();

    for (auto it = caches.rbegin(); it != caches.rend(); ++it)
        (*it)->impl_replacement_final_stats();

#ifndef CRC2_COMPILE
    print_dram_stats();
    print_branch_stats();
#endif

    return 0;
}

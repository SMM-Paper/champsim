#ifndef SPP_H
#define SPP_H

#include <iostream>
#include <fstream>

// Signature table parameters
#define ST_SET 1
#define ST_WAY 256
#define ST_TAG_BIT 16
#define ST_TAG_MASK ((1 << ST_TAG_BIT) - 1)
#define SIG_SHIFT 3
#define SIG_BIT 12
#define SIG_MASK ((1 << SIG_BIT) - 1)
#define SIG_DELTA_BIT 7

// Pattern table parameters
#define PT_SET 512
#define PT_WAY 4
#define C_SIG_BIT 4
#define C_DELTA_BIT 4
#define C_SIG_MAX ((1 << C_SIG_BIT) - 1)
#define C_DELTA_MAX ((1 << C_DELTA_BIT) - 1)

// Prefetch filter parameters
#define QUOTIENT_BIT  10
#define REMAINDER_BIT 6
#define HASH_BIT (QUOTIENT_BIT + REMAINDER_BIT + 1)
#define FILTER_SET (1 << QUOTIENT_BIT)

// To enable / disable negative training using reject filter
#ifdef PPF_TRAIN_NEG
#define QUOTIENT_BIT_REJ  10
#define REMAINDER_BIT_REJ 8
#define HASH_BIT_REJ (QUOTIENT_BIT_REJ + REMAINDER_BIT_REJ + 1)
#define FILTER_SET_REJ (1 << QUOTIENT_BIT_REJ)
#endif

#define FILL_THRESHOLD 80
#define PF_THRESHOLD 1

// Global register parameters
#define GLOBAL_COUNTER_BIT 10
#define GLOBAL_COUNTER_MAX ((1 << GLOBAL_COUNTER_BIT) - 1)
#define MAX_GHR_ENTRY 8

// Perceptron paramaters
#define PERC_ENTRIES (1 << 12) //Upto 12-bit addressing in hashed perceptron
#define PERC_FEATURES_IN  9
#define PERC_FEATURES_OUT 5 //Keep increasing based on new features
#define PERC_COUNTER_BITS 5
#define PERC_COUNTER_MIN (-1*(1 << (PERC_COUNTER_BITS - 1)))
#define PERC_COUNTER_MAX ((1 << (PERC_COUNTER_BITS - 1)) - 1)
#define PERC_THRESHOLD_HI  -5
#define PERC_THRESHOLD_LO  -15
#define POS_UPDT_THRESHOLD  90
#define NEG_UPDT_THRESHOLD -80

// Perceptron element widths (ifndef guards to let me redefine at compile time)
#ifndef PERC_ELEM0_WIDTH
#define PERC_ELEM0_WIDTH 12
#endif

#ifndef PERC_ELEM1_WIDTH
#define PERC_ELEM1_WIDTH 12
#endif

#ifndef PERC_ELEM2_WIDTH
#define PERC_ELEM2_WIDTH 12
#endif

#ifndef PERC_ELEM3_WIDTH
#define PERC_ELEM3_WIDTH 12
#endif

#ifndef PERC_ELEM4_WIDTH
#define PERC_ELEM4_WIDTH 12
#endif

enum FILTER_REQUEST {SPP_L2C_PREFETCH, SPP_LLC_PREFETCH, L2C_DEMAND, L2C_EVICT, SPP_PERC_REJECT}; // Request type for prefetch filter

uint64_t get_hash(uint64_t key);

class SIGNATURE_TABLE {
    public:
        bool     valid[ST_SET][ST_WAY];
        uint32_t tag[ST_SET][ST_WAY],
        last_offset[ST_SET][ST_WAY],
        sig[ST_SET][ST_WAY],
        lru[ST_SET][ST_WAY];

        SIGNATURE_TABLE() {
            std::cout << "Initialize SIGNATURE TABLE" << std::endl;
            std::cout << "ST_SET: " << ST_SET << std::endl;
            std::cout << "ST_WAY: " << ST_WAY << std::endl;
            std::cout << "ST_TAG_BIT: " << ST_TAG_BIT << std::endl;
            std::cout << "ST_TAG_MASK: " << hex << ST_TAG_MASK << dec << std::endl;

            for (uint32_t set = 0; set < ST_SET; set++)
                for (uint32_t way = 0; way < ST_WAY; way++) {
                    valid[set][way] = 0;
                    tag[set][way] = 0;
                    last_offset[set][way] = 0;
                    sig[set][way] = 0;
                    lru[set][way] = way;
                }
        };

        void read_and_update_sig(uint64_t page, uint32_t page_offset, uint32_t &last_sig, uint32_t &curr_sig, int32_t &delta);
};

class PATTERN_TABLE {
    public:
        int      delta[PT_SET][PT_WAY];
        uint32_t c_delta[PT_SET][PT_WAY],
        c_sig[PT_SET];

        PATTERN_TABLE() {
            std::cout << std::endl << "Initialize PATTERN TABLE" << std::endl;
            std::cout << "PT_SET: " << PT_SET << std::endl;
            std::cout << "PT_WAY: " << PT_WAY << std::endl;
            std::cout << "SIG_DELTA_BIT: " << SIG_DELTA_BIT << std::endl;
            std::cout << "C_SIG_BIT: " << C_SIG_BIT << std::endl;
            std::cout << "C_DELTA_BIT: " << C_DELTA_BIT << std::endl;

            for (uint32_t set = 0; set < PT_SET; set++) {
                for (uint32_t way = 0; way < PT_WAY; way++) {
                    delta[set][way] = 0;
                    c_delta[set][way] = 0;
                }
                c_sig[set] = 0;
            }
        }

        void update_pattern(uint32_t last_sig, int curr_delta),
             read_pattern(uint32_t curr_sig, int *prefetch_delta, uint32_t *confidence_q, int32_t *perc_sum_q, uint32_t &lookahead_way, uint32_t &lookahead_conf, uint32_t &pf_q_tail, uint32_t &depth, uint64_t addr, uint64_t base_addr, uint64_t train_addr, uint64_t curr_ip, int32_t train_delta, uint32_t last_sig);
};

class PREFETCH_FILTER {
    public:
        uint64_t remainder_tag[FILTER_SET],
        pc[FILTER_SET],
        pc_1[FILTER_SET],
        pc_2[FILTER_SET],
        pc_3[FILTER_SET],
        address[FILTER_SET];
        bool     valid[FILTER_SET],  // Consider this as "prefetched"
                 useful[FILTER_SET]; // Consider this as "used"
        int32_t	 delta[FILTER_SET],
        perc_sum[FILTER_SET];
        uint32_t last_signature[FILTER_SET],
        confidence[FILTER_SET],
        cur_signature[FILTER_SET],
        la_depth[FILTER_SET];

#ifdef PPF_TRAIN_NEG
        uint64_t remainder_tag_reject[FILTER_SET_REJ],
        pc_reject[FILTER_SET_REJ],
        pc_1_reject[FILTER_SET_REJ],
        pc_2_reject[FILTER_SET_REJ],
        pc_3_reject[FILTER_SET_REJ],
        address_reject[FILTER_SET_REJ];
        bool 	 valid_reject[FILTER_SET_REJ]; // Entries which the perceptron rejected
        int32_t	 delta_reject[FILTER_SET_REJ],
        perc_sum_reject[FILTER_SET_REJ];
        uint32_t last_signature_reject[FILTER_SET_REJ],
        confidence_reject[FILTER_SET_REJ],
        cur_signature_reject[FILTER_SET_REJ],
        la_depth_reject[FILTER_SET_REJ];
#endif

        // Tried the set-dueling idea which din't work out
        uint32_t PSEL_1;
        uint32_t PSEL_2;

        float hist_hits[55];
        float hist_tots[55];

        PREFETCH_FILTER() {
            std::cout << std::endl << "Initialize PREFETCH FILTER" << std::endl;
            std::cout << "FILTER_SET: " << FILTER_SET << std::endl;

            for (int i = 0; i < 55; i++) {
                hist_hits[i] = 0;
                hist_tots[i] = 0;
            }
            for (uint32_t set = 0; set < FILTER_SET; set++) {
                remainder_tag[set] = 0;
                valid[set] = 0;
                useful[set] = 0;
            }
#ifdef PPF_TRAIN_NEG
            for (uint32_t set = 0; set < FILTER_SET_REJ; set++) {
                valid_reject[set] = 0;
                remainder_tag_reject[set] = 0;
            }
#endif
        }

        bool     check(uint64_t pf_addr, uint64_t base_addr, uint64_t ip, FILTER_REQUEST filter_request, int32_t cur_delta, uint32_t last_sign, uint32_t cur_sign, uint32_t confidence, int32_t sum, uint32_t depth);
};

typedef struct {
    unsigned int base_addr: 24;
    unsigned int ip: 12;
    unsigned int ip_1: 12;
    unsigned int ip_2: 12;
    unsigned int ip_3: 12;
      signed int cur_delta: 7;
      //signed int last_delta: 7;
    unsigned int last_sig: 10;
    unsigned int curr_sig: 10;
    unsigned int confidence: 7;
    unsigned int depth: 4;
} PERC_DATA;

class PERCEPTRON {
    public:
        // Crossbar indices
        int32_t crossbar_idx[PERC_FEATURES_OUT];

        // Perc Weights
        int32_t perc_weights[PERC_ENTRIES][PERC_FEATURES_OUT];

#ifdef SPP_PERC_WGHT
        // Only for dumping csv
        bool    perc_touched[PERC_ENTRIES][PERC_FEATURES_OUT];
#endif

        // CONST depths for different features
        static const int32_t PERC_DEPTH[PERC_FEATURES_OUT];

        PERCEPTRON() {
            std::cout << "\nInitialize PERCEPTRON" << std::endl;
            std::cout << "PERC_ENTRIES: " << PERC_ENTRIES << std::endl;
            std::cout << "PERC_FEATURES_IN: " << PERC_FEATURES_IN << std::endl;
            std::cout << "PERC_FEATURES_OUT: " << PERC_FEATURES_OUT << std::endl;

            for (int i = 0; i < PERC_ENTRIES; i++) {
                for (int j = 0;j < PERC_FEATURES_OUT; j++) {
                    perc_weights[i][j] = 0;
#ifdef SPP_PERC_WGHT
                    perc_touched[i][j] = 0;
#endif
                }
            }
        }

        void    perc_update(PERC_DATA data, bool direction, int32_t perc_sum);
        int32_t perc_predict(PERC_DATA data);
};

const int32_t PERCEPTRON::PERC_DEPTH[PERC_FEATURES_OUT] = { 1 << PERC_ELEM0_WIDTH, 1 << PERC_ELEM1_WIDTH, 1 << PERC_ELEM2_WIDTH, 1 << PERC_ELEM3_WIDTH, 1 << PERC_ELEM4_WIDTH};

class GLOBAL_REGISTER {
    public:
        // Global counters to calculate global prefetching accuracy
        uint64_t pf_useful,
                 pf_issued,
                 global_accuracy; // Alpha value in Section III. Equation 3

        // Global History Register (GHR) entries
        uint8_t  valid[MAX_GHR_ENTRY];
        uint32_t sig[MAX_GHR_ENTRY],
        confidence[MAX_GHR_ENTRY],
        offset[MAX_GHR_ENTRY];
        int      delta[MAX_GHR_ENTRY];

        uint64_t ip_0,
                 ip_1,
                 ip_2,
                 ip_3;

        // Stats Collection
        double 	  depth_val,
                  depth_sum,
                  depth_num;
        double 	  pf_total,
                  pf_l2c,
                  pf_llc,
                  pf_l2c_good;
        long 	  perc_pass,
                  perc_reject,
                  reject_update;
        // Stats

        GLOBAL_REGISTER() {
            pf_useful = 0;
            pf_issued = 0;
            global_accuracy = 0;
            ip_0 = 0;
            ip_1 = 0;
            ip_2 = 0;
            ip_3 = 0;

            // These are just for stats printing
            depth_val = 0;
            depth_sum = 0;
            depth_num = 0;
            pf_total = 0;
            pf_l2c = 0;
            pf_llc = 0;
            pf_l2c_good = 0;
            perc_pass = 0;
            perc_reject = 0;
            reject_update = 0;

            for (uint32_t i = 0; i < MAX_GHR_ENTRY; i++) {
                valid[i] = 0;
                sig[i] = 0;
                confidence[i] = 0;
                offset[i] = 0;
                delta[i] = 0;
            }
        }

        void update_entry(uint32_t pf_sig, uint32_t pf_confidence, uint32_t pf_offset, int pf_delta);
        uint32_t check_entry(uint32_t page_offset);
};

#endif

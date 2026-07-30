#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <iomanip>

const uint64_t MULTIPLIER = 0x5DEECE66DULL;
const uint64_t ADDEND = 0xBULL;
const uint64_t MASK = (1ULL << 48) - 1;
const uint64_t INV_MULTIPLIER = 0xDFE05BCB1365ULL;

const uint64_t RANGE = 23; 

struct Constraint {
    int64_t offset;
    int out1;
    int out2_min;
    int out2_max;
};

struct L0_Data {
    uint64_t mod_val;
    uint64_t L0;
    bool operator<(const L0_Data& other) const {
        return mod_val < other.mod_val;
    }
};

std::atomic<bool> seed_found(false);
std::atomic<uint64_t> progress_counter(0);

bool verify(uint64_t base, bool use_xor, const std::vector<Constraint>& cons) {
    for (size_t i = 1; i < cons.size(); i++) { 
        uint64_t state = (base + cons[i].offset) & MASK;
        if (use_xor) state ^= 0x5DEECE66DULL;
        
        state = (state * MULTIPLIER + ADDEND) & MASK;
        if ((state >> 17) % RANGE != cons[i].out1) return false;
        
        state = (state * MULTIPLIER + ADDEND) & MASK;
        int val2 = (state >> 17) % RANGE;
        if (val2 < cons[i].out2_min || val2 > cons[i].out2_max) return false;
    }
    return true;
}

int main() {
    // Range-based constraints
    std::vector<Constraint> constraints = {
        {30084232LL, 7, 0, 2},
        {-132867903309LL, 20, 20, 22},
        {341903212944LL, 6, 0, 2},
        {209005225403LL, 19, 20, 22},
        {683776341656LL, 5, 0, 2}
    };

    uint64_t MOD_WINDOW = RANGE * 131072ULL; 

    std::vector<L0_Data> l0_table(131072);
    for (uint64_t l0 = 0; l0 < 131072; l0++) {
        l0_table[l0] = {(l0 * MULTIPLIER) % MOD_WINDOW, l0};
    }
    std::sort(l0_table.begin(), l0_table.end());

    std::cout << "Cracking across all CPU cores for Range Bounds..." << std::endl;
    
    const uint64_t TOTAL_K0 = 93368854; 
    auto start_time = std::chrono::steady_clock::now();

    #pragma omp parallel for schedule(dynamic, 4096)
    for (uint64_t K0 = 0; K0 <= TOTAL_K0; K0++) {
        if (seed_found.load(std::memory_order_relaxed)) continue;
        
        uint64_t current_prog = progress_counter.fetch_add(1, std::memory_order_relaxed);
        if (current_prog > 0 && current_prog % 10000 == 0) {
            auto now = std::chrono::steady_clock::now();
            std::chrono::duration<double> elapsed = now - start_time;
            double speed = current_prog / elapsed.count();
            
            #pragma omp critical
            {
                std::cout << "Progress: " << std::fixed << std::setprecision(2) 
                          << (current_prog * 100.0 / TOTAL_K0) << "% (" 
                          << current_prog << " / " << TOTAL_K0 << ") "
                          << "| Speed: " << (speed / 1000000.0) << " M/s\n";
            }
        }

        uint64_t U1 = RANGE * K0 + constraints[0].out1; 
        uint64_t BaseVal = ((U1 << 17) * MULTIPLIER + ADDEND) & MASK;

        // Iterate over allowed out2 bounds for constraint 0
        for (int target_out2 = constraints[0].out2_min; target_out2 <= constraints[0].out2_max; target_out2++) {
            for (uint64_t W = 0; W <= 13; W++) {
                
                int64_t target_base = (int64_t)target_out2 * 131072LL;
                
                int64_t T = target_base + (int64_t)(W * (1ULL << 48)) - (int64_t)BaseVal;
                int64_t T_mod = T % (int64_t)MOD_WINDOW;
                if (T_mod < 0) T_mod += MOD_WINDOW;
                
                uint64_t req_start = T_mod;
                uint64_t req_end = (T_mod + 131071) % MOD_WINDOW;

                auto check_range = [&](uint64_t start, uint64_t end) {
                    L0_Data dummy = {start, 0};
                    auto it = std::lower_bound(l0_table.begin(), l0_table.end(), dummy);
                    
                    while (it != l0_table.end() && it->mod_val <= end) {
                        uint64_t L0 = it->L0;
                        uint64_t S2 = (BaseVal + L0 * MULTIPLIER) & MASK;
                        
                        int val2 = (S2 >> 17) % RANGE;
                        if (val2 >= constraints[0].out2_min && val2 <= constraints[0].out2_max) { 
                            
                            uint64_t S0_1 = (U1 << 17) | L0;
                            uint64_t S0_0 = ((S0_1 - ADDEND) * INV_MULTIPLIER) & MASK;
                            
                            uint64_t base_xor = (S0_0 ^ 0x5DEECE66DULL) - constraints[0].offset;
                            uint64_t base_no_xor = S0_0 - constraints[0].offset;
                            
                            if (verify(base_xor, true, constraints)) {
                                bool expected = false;
                                if (seed_found.compare_exchange_strong(expected, true)) {
                                    std::cout << "\n===============================\n";
                                    std::cout << " MATCH FOUND (WITH XOR)! \n";
                                    std::cout << " Base Seed: " << (base_xor & MASK) << "\n";
                                    std::cout << "===============================\n";
                                }
                            }
                            if (verify(base_no_xor, false, constraints)) {
                                bool expected = false;
                                if (seed_found.compare_exchange_strong(expected, true)) {
                                    std::cout << "\n===============================\n";
                                    std::cout << " MATCH FOUND (NO XOR)! \n";
                                    std::cout << " Base Seed: " << (base_no_xor & MASK) << "\n";
                                    std::cout << "===============================\n";
                                }
                            }
                        }
                        it++;
                    }
                };

                if (req_start <= req_end) {
                    check_range(req_start, req_end);
                } else {
                    check_range(req_start, MOD_WINDOW - 1);
                    check_range(0, req_end);
                }
            }
        }
    }

    if (!seed_found) {
        std::cout << "Scan finished. No seed found." << std::endl;
    }
    return 0;
}
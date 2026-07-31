#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <pthread.h>
#include <csignal>
#include <unistd.h>

using namespace std;

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

atomic<bool> stop_search(false);
atomic<uint64_t> global_K0(0);
atomic<uint64_t> progress_counter(0);

vector<Constraint> constraints;
vector<L0_Data> l0_table;
uint64_t MOD_WINDOW;
const uint64_t TOTAL_K0 = 93368854; 
chrono::time_point<chrono::steady_clock> start_time;

vector<uint64_t> found_seeds;
pthread_mutex_t result_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;

void handle_sigint(int sig) {
    stop_search = true;
}

bool verify(uint64_t base, const vector<Constraint>& cons) {
    for (size_t i = 1; i < cons.size(); i++) { 
        uint64_t state = (base + cons[i].offset) & MASK;
        state ^= 0x5DEECE66DULL; // Hardcoded XOR application
        
        state = (state * MULTIPLIER + ADDEND) & MASK;
        if ((state >> 17) % RANGE != cons[i].out1) return false;
        
        state = (state * MULTIPLIER + ADDEND) & MASK;
        int val2 = (state >> 17) % RANGE;
        if (val2 < cons[i].out2_min || val2 > cons[i].out2_max) return false;
    }
    return true;
}

void* worker_thread(void* arg) {
    while (!stop_search) {
        uint64_t chunk_start = global_K0.fetch_add(4096);
        if (chunk_start > TOTAL_K0) break;
        uint64_t chunk_end = min(chunk_start + 4096 - 1, TOTAL_K0);

        for (uint64_t K0 = chunk_start; K0 <= chunk_end; K0++) {
            if (stop_search) break;

            uint64_t current_prog = progress_counter.fetch_add(1);
            if (current_prog > 0 && current_prog % 10000 == 0) {
                auto now = chrono::steady_clock::now();
                chrono::duration<double> elapsed = now - start_time;
                double speed = current_prog / elapsed.count();
                
                pthread_mutex_lock(&print_mutex);
                cout << "Progress: " << fixed << setprecision(2) 
                     << (current_prog * 100.0 / TOTAL_K0) << "% (" 
                     << current_prog << " / " << TOTAL_K0 << ") "
                     << "| Speed: " << (speed / 1000000.0) << " M/s\n";
                pthread_mutex_unlock(&print_mutex);
            }

            uint64_t U1 = RANGE * K0 + constraints[0].out1; 
            uint64_t BaseVal = ((U1 << 17) * MULTIPLIER + ADDEND) & MASK;

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
                        auto it = lower_bound(l0_table.begin(), l0_table.end(), dummy);
                        
                        while (it != l0_table.end() && it->mod_val <= end) {
                            uint64_t L0 = it->L0;
                            uint64_t S2 = (BaseVal + L0 * MULTIPLIER) & MASK;
                            
                            int val2 = (S2 >> 17) % RANGE;
                            if (val2 >= constraints[0].out2_min && val2 <= constraints[0].out2_max) { 
                                
                                uint64_t S0_1 = (U1 << 17) | L0;
                                uint64_t S0_0 = ((S0_1 - ADDEND) * INV_MULTIPLIER) & MASK;
                                
                                uint64_t base_seed = (S0_0 ^ 0x5DEECE66DULL) - constraints[0].offset;
                                
                                if (verify(base_seed, constraints)) {
                                    pthread_mutex_lock(&result_mutex);
                                    found_seeds.push_back(base_seed & MASK);
                                    pthread_mutex_unlock(&result_mutex);
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
    }
    return nullptr;
}

int main() {
    signal(SIGINT, handle_sigint); //ctrl-c

    constraints = {
        {30084232LL, 7, 0, 1},
        {-132867903309LL, 20, 21, 22},
        {341903212944LL, 6, 0, 1},
        {209005225403LL, 19, 21, 22},
        {683776341656LL, 5, 0, 1}
    };

    MOD_WINDOW = RANGE * 131072ULL; 

    l0_table.resize(131072);
    for (uint64_t l0 = 0; l0 < 131072; l0++) {
        l0_table[l0] = {(l0 * MULTIPLIER) % MOD_WINDOW, l0};
    }
    sort(l0_table.begin(), l0_table.end());    

    cout << "Searching... Press Ctrl-C to stop early and dump seeds." << endl;
    start_time = chrono::steady_clock::now();

    int num_threads = 12;
    vector<pthread_t> threads(num_threads);

    for (int i = 0; i < num_threads; i++) {
        pthread_create(&threads[i], nullptr, worker_thread, nullptr);
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], nullptr);
    }

    // Sort and deduplicate findings
    sort(found_seeds.begin(), found_seeds.end()); 
    auto it = unique(found_seeds.begin(), found_seeds.end()); 
    found_seeds.erase(it, found_seeds.end()); 

    if (found_seeds.empty()) {
        cout << "Scan finished. No seed found." << endl;
    } else {
        cout << "Found " << found_seeds.size() << " unique seeds:" << endl;
        for (uint64_t seed : found_seeds) {
            cout << seed << endl;
        }
    }

    return 0;
}
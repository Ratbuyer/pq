#define DEBUG_PRINT 0
#define STATS 0

#include <algorithm>
#include <assert.h>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <pthread.h>
#include <random>
#include <vector>

extern "C" {
#include "gc/gc.h"
#include "prioq.h"
#include "common.h"
}

/* ---------------- Config (match style of your driver) ---------------- */

// #ifndef NUM_THREADS
// #define NUM_THREADS 1
// #endif

#ifndef TOTAL_OPS
#define TOTAL_OPS 100000000ULL
#endif

/* If you want to cap key range (more duplicates / more contention):
 * #define KEY_MAX 1000000000ULL
 */
#ifndef KEY_MAX
#define KEY_MAX (std::numeric_limits<uint64_t>::max())
#endif

/* If your PQ collapses duplicates and you want to avoid empty deletemin crashes,
 * keep this enabled: it makes keys unique but still randomized.
 */
#ifndef UNIQUE_KEYS
#define UNIQUE_KEYS 1
#endif

/* ---------------- pthread parallel_for identical vibe ---------------- */

struct ThreadArgs {
  std::function<void(int, int)> func;
  int start;
  int end;
};

static void *threadFunction(void *arg) {
  ThreadArgs *args = static_cast<ThreadArgs *>(arg);
  args->func(args->start, args->end);
  return nullptr;
}

template <typename F>
inline void parallel_for(int numThreads, size_t start, size_t end, F f) {
  pthread_t threads[numThreads];
  ThreadArgs threadArgs[numThreads];
  size_t per_thread = (end - start) / (size_t)numThreads;

  for (int i = 0; i < numThreads; i++) {
    threadArgs[i].func = [&f](int a, int b) {
      for (int k = a; k < b; k++) f((size_t)k);
    };

    size_t s = start + (size_t)i * per_thread;
    size_t e = (i == numThreads - 1) ? end : (start + (size_t)(i + 1) * per_thread);

    threadArgs[i].start = (int)s;
    threadArgs[i].end = (int)e;

    int result = pthread_create(&threads[i], nullptr, threadFunction, &threadArgs[i]);
    if (result != 0) {
      std::cerr << "Failed to create thread " << i << std::endl;
      std::exit(-1);
    }
  }

  for (int i = 0; i < numThreads; i++) pthread_join(threads[i], nullptr);
}

/* ---------------- Helpers ---------------- */

static inline uint64_t get_usecs() {
  using namespace std::chrono;
  return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

static inline uint64_t mix64(uint64_t x) {
  // simple reversible-ish mix for uniqueness/randomness
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}

/* Generate random data in parallel, matching your driver style */
template <class T>
std::vector<T> create_random_data_in_parallel(int numThreads, size_t n, uint64_t max_val) {
  std::vector<T> v(n);

  // base seed like your code
  std::random_device rd;
  uint64_t base_seed =
      (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count() ^ (uint64_t)rd();

  // Split work among NUM_THREADS (to mirror your driver closely)
  parallel_for(numThreads, 0, n, [&](size_t idx) {
    // Thread-local RNG seeded by base + coarse worker id derived from idx chunk
    // (Keeps it simple while still deterministic per chunk)
    static thread_local std::mt19937_64 eng;
    static thread_local bool inited = false;
    if (!inited) {
      // Derive a per-thread seed using pthread_self() and base_seed
      uint64_t tid = (uint64_t)(uintptr_t)pthread_self();
      eng.seed(base_seed ^ mix64(tid));
      inited = true;
    }
    std::uniform_int_distribution<uint64_t> dist(1, max_val);
    v[idx] = (T)dist(eng);
  });

  return v;
}

/* ---------------- The PQ benchmark (insert then deletemin) ---------------- */

static pq_t *pq = nullptr;

static void setup_pq(int max_offset) {
  _init_gc_subsystem();
  pq = pq_init(max_offset);
  assert(pq);
}

static void teardown_pq() {
  pq_destroy(pq);
  pq = nullptr;
  _destroy_gc_subsystem();
}

void test(int t) {
  const uint64_t total = (uint64_t)TOTAL_OPS;

  std::cout << "Benchmark: TOTAL_OPS=" << total << ", NUM_THREADS=" << t << "\n";

  setup_pq(/*max_offset=*/10);

  // Pre-generate keys (NOT timed), like you wanted
using K = uint64_t;
std::vector<K> keys =
    create_random_data_in_parallel<K>(
        t, total, (std::numeric_limits<K>::max)());

#if UNIQUE_KEYS
  // Make them unique but still "random-looking" to avoid PQs that collapse duplicates
  // This preserves your “random keys” spirit while guaranteeing total elements.
  parallel_for(t, 0, total, [&](size_t i) {
    keys[i] = (mix64(keys[i]) ^ (uint64_t)i) + 1;
  });
#endif

  // ---------------- Insert timing ----------------
  uint64_t start_us = get_usecs();

  parallel_for(t, 0, total, [&](size_t i) {
    uint64_t k = keys[i];
    insert(pq, (unsigned long)k, (pval_t)k);
  });

  uint64_t end_us = get_usecs();
  uint64_t ins_dur = end_us - start_us;
  std::cout << "\tInsert took " << ins_dur << " us, throughput = "
            << ((double)total) / (double)ins_dur << " ops/us\n";

  // ---------------- DeleteMin timing ----------------
  start_us = get_usecs();

  // std::sort(keys.begin(), keys.end());

  parallel_for(t, 0, total, [&](size_t /*i*/) {
    (void)deletemin(pq);
  });

//   for (int i = 0; i < total; i++) {
//     unsigned long ret = (unsigned long)deletemin(pq);
//     if (ret != keys[i]) {
//         printf("wrong results! %d\n", i);
//         printf("ret = %ul, keys[i] = %ul\n", ret, keys[i]);
//     }
//   }

  end_us = get_usecs();
  uint64_t del_dur = end_us - start_us;
  std::cout << "\tDeleteMin took " << del_dur << " us, throughput = "
            << ((double)total) / (double)del_dur << " ops/us\n";

  // teardown_pq();
}

int main(int argc, char **argv) {
    const int num_thread = atoi(argv[1]);
    test(num_thread);
    std::cout << "success\n";
    return 0;
}

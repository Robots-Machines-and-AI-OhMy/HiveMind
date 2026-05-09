// compute_check.cpp
// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: three sets of matrix multiplication (256², 512², 1024²).
//
// Scoring rules (from specification):
//   - Seeded RNG: srand(42) then rand() for all matrix values.
//   - Each size is run 7 times; first 2 runs discarded (cache warm-up).
//   - If the 3rd run (first recorded) completes in < 200 ms → skip
//     remaining runs for that size.
//   - If a run exceeds 500 ms mid-computation → score 0 for that size,
//     stop further tests.
//   - Recorded runs: average of whichever were completed (3–5 runs).
//   - Score = offset / avg_ns_of_largest_nonzero_size
//       offset: 1,000,000 if large (1024²), 1,000 if medium (512²),
//               1 if small (256²).
//   - If all sizes score 0 → return 0.0.
// ─────────────────────────────────────────────────────────────────────────────

#include "compute_check.hpp"

#include <cstdlib>
#include <cstring>
#include <chrono>
#include <vector>
#include <cstdio>

using Clock = std::chrono::high_resolution_clock;
using ns    = std::chrono::nanoseconds;

// ── Matrix helpers ────────────────────────────────────────────────────────────

static std::vector<double> make_matrix(int n)
{
    return std::vector<double>(static_cast<size_t>(n) * n);
}

// Fill with seeded rand() — called once before each size to ensure
// all nodes use the same values regardless of run ordering.
static void seed_matrix(std::vector<double>& m, int n, unsigned seed)
{
    srand(seed);
    for (int i = 0; i < n * n; ++i)
        m[i] = static_cast<double>(rand());
}

// Naive O(n³) multiply — intentionally not optimised so the benchmark
// reflects raw CPU throughput rather than SIMD or cache tricks.
// Returns false if the 500 ms wall-clock limit was exceeded mid-run.
static bool multiply(const std::vector<double>& A,
                     const std::vector<double>& B,
                     std::vector<double>&       C,
                     int n,
                     std::chrono::time_point<Clock> start_time)
{
    const double* a = A.data();
    const double* b = B.data();
    double*       c = C.data();

    memset(c, 0, sizeof(double) * static_cast<size_t>(n) * n);

    constexpr auto LIMIT = std::chrono::milliseconds(500);

    for (int i = 0; i < n; ++i) {
        // Check wall-clock only once per row to avoid syscall overhead.
        if (Clock::now() - start_time > LIMIT)
            return false;
        for (int k = 0; k < n; ++k) {
            double aik = a[i * n + k];
            for (int j = 0; j < n; ++j)
                c[i * n + j] += aik * b[k * n + j];
        }
    }
    return true;
}

// ── Per-size benchmark ────────────────────────────────────────────────────────
// Returns average nanoseconds of recorded runs, or -1 if all timed out.
static double benchmark_size(int n, unsigned seed)
{
    constexpr int TOTAL_RUNS    = 7;
    constexpr int WARMUP_RUNS   = 2;  // discarded
    constexpr int FAST_SKIP_MS  = 200;

    auto A = make_matrix(n);
    auto B = make_matrix(n);
    auto C = make_matrix(n);

    seed_matrix(A, n, seed);
    seed_matrix(B, n, seed + 1);

    long long total_ns   = 0;
    int       recorded   = 0;
    bool      timed_out  = false;

    for (int run = 0; run < TOTAL_RUNS; ++run) {
        auto t0 = Clock::now();
        bool ok = multiply(A, B, C, n, t0);
        auto t1 = Clock::now();
        long long elapsed_ns = std::chrono::duration_cast<ns>(t1 - t0).count();
        long long elapsed_ms = elapsed_ns / 1'000'000LL;

        if (run < WARMUP_RUNS)
            continue;  // discard warm-up

        if (!ok) {
            // Exceeded 500 ms mid-computation → score 0, stop all sizes.
            timed_out = true;
            break;
        }

        total_ns += elapsed_ns;
        recorded++;

        // First recorded run (run == WARMUP_RUNS): fast-skip check.
        if (run == WARMUP_RUNS && elapsed_ms < FAST_SKIP_MS)
            break;
    }

    if (timed_out || recorded == 0)
        return -1.0;  // signals 0 score for this size

    return static_cast<double>(total_ns) / recorded;
}

// ── Public API ────────────────────────────────────────────────────────────────

double metric_calculation()
{
    // Sizes from small to large.
    constexpr int    sizes[]   = { 256, 512, 1024 };
    constexpr double offsets[] = { 1.0, 1'000.0, 1'000'000.0 };
    constexpr int    N_SIZES   = 3;

    double avg_ns[N_SIZES]  = { 0.0, 0.0, 0.0 };
    bool   valid[N_SIZES]   = { false, false, false };
    bool   stop_all         = false;

    // Seed chosen for reproducibility across all nodes.
    unsigned seeds[] = { 42u, 4242u, 424242u };

    for (int s = 0; s < N_SIZES && !stop_all; ++s) {
        double result = benchmark_size(sizes[s], seeds[s]);
        if (result < 0.0) {
            // Timed out — zero for this size and all larger.
            stop_all = true;
        } else {
            avg_ns[s] = result;
            valid[s]  = true;
        }
    }

    // Find largest non-zero size.
    int    best_idx    = -1;
    double best_avg_ns = 0.0;
    for (int s = N_SIZES - 1; s >= 0; --s) {
        if (valid[s] && avg_ns[s] > 0.0) {
            best_idx    = s;
            best_avg_ns = avg_ns[s];
            break;
        }
    }

    if (best_idx < 0 || best_avg_ns <= 0.0)
        return 0.0;

    return offsets[best_idx] / best_avg_ns;
}

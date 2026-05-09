// Calculate_Performance.cpp
// Implementation of getSystemHealth() using Windows PDH API.
// Returns CPU and RAM health metrics on [0, 1].

#include "Calculate_Performance.hpp"

#include <cmath>
#include <algorithm>
#include <mutex>

// PDH is the Windows Performance Data Helper API.
// Link: pdh.lib (add to CMakeLists target_link_libraries)
#include <pdh.h>
#include <psapi.h>
#pragma comment(lib, "pdh.lib")

// ─────────────────────────────────────────────────────────────────────────────
// CPU temperature metric
// tempMetric = 1 - sqrt((log(T) - log(50)) / (log(100) - log(50)))
// Clamped: T <= 50 -> 1.0,  T >= 100 -> 0.0
// ─────────────────────────────────────────────────────────────────────────────
static double temp_metric(double cpu_temp_c)
{
    if (cpu_temp_c <= 50.0) return 1.0;
    if (cpu_temp_c >= 100.0) return 0.0;
    static const double log50  = std::log(50.0);
    static const double log100 = std::log(100.0);
    double ratio = (std::log(cpu_temp_c) - log50) / (log100 - log50);
    return 1.0 - std::sqrt(ratio);
}

// ─────────────────────────────────────────────────────────────────────────────
// PDH singleton — lazily initialised, thread-safe
// ─────────────────────────────────────────────────────────────────────────────
namespace {

struct PdhSampler {
    PDH_HQUERY  query       = nullptr;
    PDH_HCOUNTER cpu_counter = nullptr;
    bool        ready       = false;
    std::mutex  mtx;

    PdhSampler() {
        if (PdhOpenQuery(nullptr, 0, &query) != ERROR_SUCCESS) return;
        // "% Processor Time" across all cores (_Total instance)
        if (PdhAddEnglishCounterW(query,
                L"\\Processor(_Total)\\% Processor Time",
                0, &cpu_counter) != ERROR_SUCCESS) {
            PdhCloseQuery(query);
            query = nullptr;
            return;
        }
        // First collect call seeds the counter; the next call gives a real value.
        PdhCollectQueryData(query);
        ready = true;
    }

    ~PdhSampler() {
        if (query) PdhCloseQuery(query);
    }

    // Returns CPU utilisation [0, 100] or -1 on failure.
    double cpu_util() {
        std::lock_guard<std::mutex> lk(mtx);
        if (!ready) return -1.0;
        if (PdhCollectQueryData(query) != ERROR_SUCCESS) return -1.0;
        PDH_FMT_COUNTERVALUE val;
        if (PdhGetFormattedCounterValue(cpu_counter,
                PDH_FMT_DOUBLE, nullptr, &val) != ERROR_SUCCESS) return -1.0;
        return std::max(0.0, std::min(100.0, val.doubleValue));
    }
};

PdhSampler& sampler() {
    static PdhSampler s;
    return s;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// RAM utilisation via GlobalMemoryStatusEx
// ─────────────────────────────────────────────────────────────────────────────
static double ram_util_pct()
{
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return 50.0;  // fallback
    return static_cast<double>(ms.dwMemoryLoad);  // 0–100
}

// ─────────────────────────────────────────────────────────────────────────────
// CPU temperature
// Windows doesn't expose CPU temperature via a standard API.
// WMI or OHM are the typical routes; both require COM or a separate process.
// For now, assume a moderate temperature (65°C) which gives a tempMetric of ~0.7.
// Replace with a real WMI query if temperature monitoring is required.
// ─────────────────────────────────────────────────────────────────────────────
static double cpu_temp_estimate()
{
    return 65.0;  // assumed moderate load temperature
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────
SystemHealth getSystemHealth()
{
    double cpu_pct  = sampler().cpu_util();
    if (cpu_pct < 0.0) cpu_pct = 50.0;  // fallback if PDH unavailable

    double cpu_util_metric = 1.0 - (cpu_pct / 100.0);
    double t_metric        = temp_metric(cpu_temp_estimate());

    SystemHealth h;
    h.cpuHealth = std::min(t_metric, cpu_util_metric);
    h.ramHealth = 1.0 - (ram_util_pct() / 100.0);
    return h;
}
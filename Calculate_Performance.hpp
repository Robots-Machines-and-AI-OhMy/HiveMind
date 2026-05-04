#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// system_health.hpp
//
// Provides a single free function:
//
//   SystemHealth getSystemHealth();
//
// Returns two metrics on [0, 1] where 1.0 = best, 0.0 = worst:
//
//   cpuHealth  = min( tempMetric(cpuTempC), 1 - (cpu% / 100) )
//                  tempMetric = 1 - sqrt((log(T)-log(50)) / (log(100)-log(50)))
//                  clamped: T <= 50 -> 1.0,  T >= 100 -> 0.0
//
//   ramHealth  = 1 - (ram% / 100)
//
// The internal PDH sampler is a singleton initialized on first call.
// Thread safety: getSystemHealth() is safe to call from multiple threads.
// ─────────────────────────────────────────────────────────────────────────────

struct SystemHealth {
    double cpuHealth;  // min(tempMetric, cpuUtilMetric) in [0, 1]
    double ramHealth;  // 1 - ramUtil                    in [0, 1]
};

SystemHealth getSystemHealth();
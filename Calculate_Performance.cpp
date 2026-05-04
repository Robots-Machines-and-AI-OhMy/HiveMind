#include "Calculate_Performance.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <pdh.h>
#include <comdef.h>
#include <wbemidl.h>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <stdexcept>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

// ─────────────────────────────────────────────────────────────────────────────
// PDH singleton — owns the persistent CPU counter state required for the
// two-sample delta. Initialized once on first call to getSystemHealth().
// ─────────────────────────────────────────────────────────────────────────────
namespace {

    class PdhCpuSampler {
    public:
        static PdhCpuSampler& instance() {
            static PdhCpuSampler inst; // initialized once, lives for program lifetime
            return inst;
        }

        // Returns CPU utilization in [0, 100]
        double cpuPercent() {
            std::lock_guard<std::mutex> lock(mutex_);
            PDH_FMT_COUNTERVALUE val{};
            PdhCollectQueryData(query_);
            PdhGetFormattedCounterValue(counter_, PDH_FMT_DOUBLE, nullptr, &val);
            return std::clamp(val.doubleValue, 0.0, 100.0);
        }

    private:
        PDH_HQUERY   query_{};
        PDH_HCOUNTER counter_{};
        std::mutex   mutex_;

        PdhCpuSampler() {
            if (PdhOpenQuery(nullptr, 0, &query_) != ERROR_SUCCESS)
                throw std::runtime_error("getSystemHealth: PdhOpenQuery failed");

            if (PdhAddEnglishCounter(query_,
                    L"\\Processor(_Total)\\% Processor Time",
                    0, &counter_) != ERROR_SUCCESS)
                throw std::runtime_error("getSystemHealth: PdhAddEnglishCounter failed");

            PdhCollectQueryData(query_); // prime the counter
            Sleep(200);                  // establish baseline before first read
        }

        ~PdhCpuSampler() { PdhCloseQuery(query_); }

        // Non-copyable, non-movable
        PdhCpuSampler(const PdhCpuSampler&)            = delete;
        PdhCpuSampler& operator=(const PdhCpuSampler&) = delete;
    };

// ─────────────────────────────────────────────────────────────────────────────
// WMI helpers
// ─────────────────────────────────────────────────────────────────────────────

    struct WmiSession {
        IWbemLocator*  locator  = nullptr;
        IWbemServices* services = nullptr;
        bool           comInit  = false;

        bool connect(const wchar_t* ns) {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            comInit = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;

            CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                RPC_C_AUTHN_LEVEL_DEFAULT,
                RPC_C_IMP_LEVEL_IMPERSONATE,
                nullptr, EOAC_NONE, nullptr);

            hr = CoCreateInstance(CLSID_WbemLocator, nullptr,
                CLSCTX_INPROC_SERVER, IID_IWbemLocator, (void**)&locator);
            if (FAILED(hr)) return false;

            hr = locator->ConnectServer(_bstr_t(ns),
                nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
            if (FAILED(hr)) return false;

            CoSetProxyBlanket(services,
                RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                RPC_C_AUTHN_LEVEL_CALL,
                RPC_C_IMP_LEVEL_IMPERSONATE,
                nullptr, EOAC_NONE);

            return true;
        }

        ~WmiSession() {
            if (services) services->Release();
            if (locator)  locator->Release();
            if (comInit)  CoUninitialize();
        }
    };

    double wmiQueryDouble(const wchar_t* ns,
                          const wchar_t* query,
                          const wchar_t* prop)
    {
        WmiSession s;
        if (!s.connect(ns)) return -1.0;

        IEnumWbemClassObject* enumerator = nullptr;
        HRESULT hr = s.services->ExecQuery(
            _bstr_t(L"WQL"), _bstr_t(query),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr, &enumerator);
        if (FAILED(hr)) return -1.0;

        double result = -1.0;
        IWbemClassObject* obj = nullptr;
        ULONG returned = 0;

        if (enumerator->Next(WBEM_INFINITE, 1, &obj, &returned) == S_OK) {
            VARIANT vt;
            VariantInit(&vt);
            if (SUCCEEDED(obj->Get(prop, 0, &vt, nullptr, nullptr))) {
                switch (vt.vt) {
                    case VT_I4:  result = vt.intVal;  break;
                    case VT_R8:  result = vt.dblVal;  break;
                    case VT_R4:  result = vt.fltVal;  break;
                    case VT_UI4: result = vt.uintVal; break;
                    default:                           break;
                }
            }
            VariantClear(&vt);
            obj->Release();
        }
        enumerator->Release();
        return result;
    }

// ─────────────────────────────────────────────────────────────────────────────
// Raw data readers
// ─────────────────────────────────────────────────────────────────────────────

    double readCpuPercent() {
        return PdhCpuSampler::instance().cpuPercent();
    }

    double readRamPercent() {
        MEMORYSTATUSEX m{};
        m.dwLength = sizeof(m);
        GlobalMemoryStatusEx(&m);
        return static_cast<double>(m.dwMemoryLoad); // OS-computed %, matches Task Manager
    }

    // OpenHardwareMonitor / LibreHardwareMonitor WMI — most accurate
    // Requires OHM/LHM running as admin: https://github.com/LibreHardwareMonitor/LibreHardwareMonitor
    double readTempOHM() {
        return wmiQueryDouble(
            L"root\\OpenHardwareMonitor",
            L"SELECT Value FROM Sensor "
            L"WHERE SensorType='Temperature' AND Name='CPU Package'",
            L"Value"
        );
    }

    // ACPI thermal zone fallback — always available, less precise
    double readTempACPI() {
        double raw = wmiQueryDouble(
            L"root\\WMI",
            L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature",
            L"CurrentTemperature"
        );
        if (raw < 0.0) return -1.0;
        return (raw / 10.0) - 273.15; // tenths of Kelvin -> Celsius
    }

    double readTempC() {
        double t = readTempOHM();
        return (t >= 0.0) ? t : readTempACPI();
    }

// ─────────────────────────────────────────────────────────────────────────────
// Metric formulas
// ─────────────────────────────────────────────────────────────────────────────

    double cpuUtilMetric(double cpuPct) {
        return 1.0 - std::clamp(cpuPct / 100.0, 0.0, 1.0);
    }

    double ramUtilMetric(double ramPct) {
        return 1.0 - std::clamp(ramPct / 100.0, 0.0, 1.0);
    }

    // 1 - sqrt((log(T) - log(50)) / (log(100) - log(50)))
    // Returns -1.0 if temp is unavailable
    double tempMetric(double tempC) {
        if (tempC < 0.0)    return -1.0; // unavailable sentinel
        if (tempC <= 50.0)  return  1.0;
        if (tempC >= 100.0) return  0.0;

        static const double logDenom = std::log(100.0) - std::log(50.0); // log(2), constant
        return 1.0 - std::sqrt((std::log(tempC) - std::log(50.0)) / logDenom);
    }

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public interface
// ─────────────────────────────────────────────────────────────────────────────

SystemHealth getSystemHealth() {
    double cpuUtil = cpuUtilMetric(readCpuPercent());
    double temp    = tempMetric(readTempC());
    double ram     = ramUtilMetric(readRamPercent());

    // If temp is unavailable, fall back to util alone rather than returning -1
    double cpuHealth = (temp >= 0.0) ? std::min(cpuUtil, temp) : cpuUtil;

    return { cpuHealth, ram };
}
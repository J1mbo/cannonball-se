// cputier.hpp
//
// Lightweight ARM CPU tier probe used to resolve the AUDIO CALLBACK INTERVAL
// AUTO setting (see src/main/sdl2/audio.hpp/.cpp). Classifies the running
// CPU as "Slow" (ARM1176 / Cortex-A7 / Cortex-A53 class - roughly Raspberry
// Pi 1/2/3) or "Fast" (anything else, including all non-ARM/non-Linux
// builds) by reading the "CPU implementer" / "CPU part" fields of
// /proc/cpuinfo. Header-only, alongside singlecorepi.hpp.
#pragma once

#include <cstdlib>
#include <fstream>
#include <string>

enum class CpuTier { Unknown, Slow, Fast };

namespace cputier_detail {

struct CpuInfoResult {
    CpuTier       tier      = CpuTier::Unknown;
    unsigned long first_part = 0;
    bool          has_part   = false;
};

// Trim trailing whitespace/tab characters (the /proc/cpuinfo key field is
// separated from its value by one or more tabs before the ':').
inline void rtrim(std::string& s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.pop_back();
}

// Parses cpuinfo_path (real /proc/cpuinfo, or a synthetic fixture for
// tests) and returns the CPU tier plus the first "CPU part" value seen.
// Never throws: an unreadable file, or one with no "CPU part" lines,
// simply yields Unknown.
inline CpuInfoResult parse_cpuinfo(const char* cpuinfo_path)
{
    CpuInfoResult result;

#ifdef __linux__
    std::ifstream f(cpuinfo_path);
    if (!f.is_open())
        return result; // Unknown

    bool any_bad_implementer = false;
    bool any_bad_part        = false;
    bool any_part            = false;

    std::string line;
    while (std::getline(f, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;

        std::string key = line.substr(0, colon);
        rtrim(key);
        const std::string value = line.substr(colon + 1);

        if (key == "CPU implementer") {
            const unsigned long v = std::strtoul(value.c_str(), nullptr, 16);
            if (v != 0x41)
                any_bad_implementer = true;
        } else if (key == "CPU part") {
            const unsigned long v = std::strtoul(value.c_str(), nullptr, 16);
            any_part = true;
            if (!result.has_part) {
                result.has_part   = true;
                result.first_part = v;
            }
            if (v != 0xb76 && v != 0xc07 && v != 0xd03)
                any_bad_part = true;
        }
    }

    if (!any_part)
        result.tier = CpuTier::Unknown;
    else if (any_bad_implementer || any_bad_part)
        result.tier = CpuTier::Fast;
    else
        result.tier = CpuTier::Slow;
#else
    (void)cpuinfo_path;
    result.tier = CpuTier::Fast;
#endif

    return result;
}

} // namespace cputier_detail

// Slow = ARM1176 (0xb76), Cortex-A7 (0xc07) or Cortex-A53 (0xd03), all with
// implementer 0x41 (ARM). Any other part (e.g. Cortex-A72/A76, or a
// heterogeneous mix with at least one fast core) -> Fast. No "CPU part"
// lines at all (x86, unreadable file) -> Unknown. Non-Linux builds never
// touch the filesystem and report Fast.
//
// With no argument the real /proc/cpuinfo is parsed once per process and the
// result cached (the SOUND menu asks every frame). Pass a path to parse a
// specific file, e.g. a synthetic fixture in tests; that is never cached.
inline CpuTier detect_cpu_tier(const char* cpuinfo_path = nullptr)
{
    if (cpuinfo_path == nullptr) {
        static const CpuTier cached = cputier_detail::parse_cpuinfo("/proc/cpuinfo").tier;
        return cached;
    }
    return cputier_detail::parse_cpuinfo(cpuinfo_path).tier;
}

// Short name for the running CPU's first "CPU part" entry, for the
// -verbose AUDIO CALLBACK diagnostic line. Reads the real /proc/cpuinfo at
// most once per process (cached in a function-local static).
inline const char* cpu_tier_description()
{
    static const std::string description = [] {
        cputier_detail::CpuInfoResult result = cputier_detail::parse_cpuinfo("/proc/cpuinfo");
        if (!result.has_part)
            return std::string("unknown");
        switch (result.first_part) {
            case 0xb76: return std::string("ARM1176");
            case 0xc07: return std::string("Cortex-A7");
            case 0xd03: return std::string("Cortex-A53");
            case 0xd08: return std::string("Cortex-A72");
            case 0xd0b: return std::string("Cortex-A76");
            default:    return std::string("other");
        }
    }();
    return description.c_str();
}

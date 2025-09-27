// singleCorePi.hpp
#pragma once
#include <omp.h>
#include <fstream>
#include <string>
#include <algorithm>

/// @brief Returns true if the program is running on a single‑core Raspberry Pi.
/// @details
///   1. `omp_get_max_threads()` must be **1** – the OpenMP runtime reports only one
///      thread, which is a strong hint that the CPU is single‑core.
///   2. The device tree must contain a model string that starts with `"Raspberry Pi"`.
///      The function looks for either
///        /sys/firmware/devicetree/base/model
///      or, if that fails,
///        /proc/device-tree/model
///   If **both** checks pass, the function returns `true`.
///
///   The function does *not* throw exceptions – any I/O or OpenMP error
///   simply causes a `false` result.
///
/// @return true if the conditions are satisfied; otherwise false.
inline bool singleCorePi()
{
    /* -------------------------------------------------
       1️⃣  OpenMP thread count
       ------------------------------------------------- */
    const int ompThreads = omp_get_max_threads();
    if (ompThreads != 1) return false;           // not a single‑core OMP machine

    /* -------------------------------------------------
       2️⃣  Check device‑tree model string
       ------------------------------------------------- */
    const std::string paths[] = {
        "/sys/firmware/devicetree/base/model",
        "/proc/device-tree/model"
    };

    std::string model;
    for (const auto &path : paths)
    {
        std::ifstream ifs(path, std::ios::in | std::ios::binary);
        if (!ifs.is_open()) continue;

        std::getline(ifs, model);                // read first line
        if (!model.empty())
            break;                               // we found something
    }

    if (model.empty()) return false;             // no model string found

    // Remove possible trailing NUL bytes (some device‑tree entries are null‑terminated)
    model.erase(std::find_if(model.rbegin(), model.rend(),
                             [](unsigned char ch){ return ch != '\0'; }).base(),
                model.end());

    // Case‑insensitive prefix comparison
    const std::string prefix = "Raspberry Pi";
    if (model.size() < prefix.size()) return false;
    const auto cmp = std::mismatch(prefix.begin(), prefix.end(),
                                   model.begin(),
                                   [](char a, char b){ return std::tolower(a) == std::tolower(b); });
    return cmp.first == prefix.end();            // true iff the prefix matches
}


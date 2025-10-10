/***************************************************************************
    Fixed-width integer types — boost-free replacement.
    Copyright (c) 2025, James Pearce

    This header replaces any Boost <cstdint> usage with the standard C++
    <cstdint> header and provides a few convenience aliases that some parts
    of the codebase may expect.
***************************************************************************/
#pragma once

// Use the C++ standard fixed-width integer types
#include <cstdint>
#include <cstddef>

// Bring the standard names into the current namespace (uint8_t, int16_t, ...)
using std::int8_t;   using std::int16_t;   using std::int32_t;   using std::int64_t;
using std::uint8_t;  using std::uint16_t;  using std::uint32_t;  using std::uint64_t;
using std::intptr_t; using std::uintptr_t;

// Optional short aliases used by parts of the project (non-standard)
using s8  = int8_t;   using s16 = int16_t;   using s32 = int32_t;   using s64 = int64_t;
using u8  = uint8_t;  using u16 = uint16_t;  using u32 = uint32_t;  using u64 = uint64_t;

// Size type convenience
using usize = std::size_t;


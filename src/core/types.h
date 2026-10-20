/*
 * blook-drv-lite - portable kernel/user shared core.
 *
 * Everything under src/core is free-standing C++17: no WDK headers, no CRT,
 * no exceptions, no RTTI, no STL containers. The same translation units are
 * compiled into the kernel driver and into the host unit-test binary, which
 * is how the page-table walker, instruction decoder and resolvers are tested
 * against synthetic inputs on the build machine.
 *
 * MIT licensed; authorized security research only.
 */
#ifndef BLOOK_DRVLITE_CORE_TYPES_H
#define BLOOK_DRVLITE_CORE_TYPES_H

namespace drvlite::core {

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;
using i8 = signed char;
using i32 = int;
using i64 = long long;
using usize = unsigned long long;
using uptr = unsigned long long;

constexpr u32 kPageSize = 0x1000u;
constexpr u64 kPageMask = 0xFFFULL;
constexpr u64 kPageRoundDown = ~kPageMask;
constexpr u64 kOneGiB = 1ULL << 30;
constexpr u64 kTwoMiB = 1ULL << 21;
constexpr u64 kFourKiB = 1ULL << 12;

/* x64 canonical address ranges. */
constexpr u64 kUserMin = 0x10000ULL;
constexpr u64 kUserMax = 0x0000800000000000ULL;
constexpr u64 kKernelMin = 0xFFFF800000000000ULL;

constexpr bool is_canonical(u64 address) {
    const u64 high = address >> 47;
    return high == 0 || high == 0x1FFFFULL;
}

constexpr bool is_canonical_user(u64 address) {
    return is_canonical(address) && address >= kUserMin && address < kUserMax;
}

constexpr bool is_canonical_kernel(u64 address) {
    return is_canonical(address) && address >= kKernelMin;
}

constexpr u64 align_down(u64 value, u64 alignment) {
    return value & ~(alignment - 1);
}

constexpr u64 align_up(u64 value, u64 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

/* Page-table entry bits. */
constexpr u64 kPtePresent = 0x001ULL;
constexpr u64 kPteLarge = 0x080ULL;
constexpr u64 kPteAddressMask = 0x000FFFFFFFFFF000ULL;
constexpr u64 kPteLarge2MiBMask = 0x000FFFFFFFE00000ULL;
constexpr u64 kPteLarge1GiBMask = 0x000FFFFFC0000000ULL;

}  // namespace drvlite::core

#endif /* BLOOK_DRVLITE_CORE_TYPES_H */

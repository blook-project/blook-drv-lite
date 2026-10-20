/*
 * blook-drv-lite - kernel mode common header.
 *
 * Include order is fixed: <ntddk.h>, then the kchannel kernel API (it also
 * defines the fixed width types used by the protocol), then our protocol.
 */
#ifndef BLOOK_DRVLITE_DRIVER_NT_H
#define BLOOK_DRVLITE_DRIVER_NT_H

#include <ntifs.h>

#include <intrin.h>

#include <blook/kchannel_km.h>

#pragma warning(push)
#pragma warning(disable : 4201) /* nameless struct/union (NT headers, us) */
#include <blook/drvlite/protocol.h>
#pragma warning(pop)

/* Portable, host-tested core. Everything below is free-standing C++17. */
#include "core/pagewalk.h"
#include "core/ranges.h"
#include "core/handle_table.h"
#include "core/detour.h"
#include "core/protocol_codec.h"
#include "core/pe.h"
#include "core/dse_resolver.h"

/*
 * Declarations for ntoskrnl routines that the WDK headers do not declare
 * (they are exported and resolvable by every manual mapper). Everything else
 * (Mm*, Ke*, Ob* used below) is already declared by <ntddk.h>.
 */
extern "C" {
PVOID NTAPI PsGetProcessPeb(PEPROCESS process);
PVOID NTAPI PsGetProcessSectionBaseAddress(PEPROCESS process);
PCHAR NTAPI PsGetProcessImageFileName(PEPROCESS process);
extern POBJECT_TYPE* PsProcessType;
extern POBJECT_TYPE* PsThreadType;
extern PLIST_ENTRY PsLoadedModuleList;
/* Kernel debug-register offsets used by the probe are documented in state.h. */
}

namespace drvlite {

/* Shorthand for the portable fixed width types. */
using core::i32;
using core::i64;
using core::kPageMask;
using core::kPageSize;
using core::u16;
using core::u32;
using core::u64;
using core::u8;
using core::usize;

constexpr ULONG kMmCopyMemoryPhysical = 0x1;
constexpr ULONG kMmCopyMemoryVirtual = 0x2;

/* EPROCESS / KPROCESS offsets that are stable across supported builds. */
constexpr ULONG kEprocessDirectoryTableBase = 0x28;

/* KTHREAD.SystemCallNumber (stored by the syscall entry before dispatch). */
constexpr ULONG kKthreadSystemCallNumber = 0x80;

/* KTRAP_FRAME offsets (x64 layout, used only as validation heuristics). */
constexpr ULONG kTrapFrameRip = 0xA0;
constexpr ULONG kTrapFrameSegCs = 0xA8;
constexpr ULONG kTrapFrameSegSs = 0xC0;

/* Kernel service entry: the dispatch is `mov rax, r10; call rax` in the
 * supported Windows 10/11 x64 KiSystemCall64. */
constexpr u32 kSyscallDispatchPatternLength = 5;

constexpr u32 kMaxProcessors = 128;

static_assert(sizeof(void*) == 8, "x64 only");

inline u64 read_cr3() {
    return __readcr3() & ~0xFFFULL;
}

inline u32 current_processor() {
    return static_cast<u32>(KeGetCurrentProcessorNumber());
}

inline u32 nt_build_number() {
    return static_cast<u32>(SharedUserData->NtBuildNumber);
}

inline PEPROCESS current_process() {
    return PsGetCurrentProcess();
}

inline PETHREAD current_thread() {
    return PsGetCurrentThread();
}

inline bool is_kernel_pointer(u64 address) {
    return (address >> 47) == 0x1FFFF;
}

inline bool is_user_pointer(u64 address) {
    return address >= 0x10000 && (address >> 47) == 0;
}

template <typename T>
inline T* as_pointer(u64 address) {
    return reinterpret_cast<T*>(static_cast<ULONG_PTR>(address));
}

inline u64 as_u64(const void* pointer) {
    return static_cast<u64>(reinterpret_cast<ULONG_PTR>(pointer));
}

inline u64 read_u64_unaligned(const void* pointer) {
    u64 value = 0;
    RtlCopyMemory(&value, pointer, sizeof(value));
    return value;
}

inline u32 read_u32_unaligned(const void* pointer) {
    u32 value = 0;
    RtlCopyMemory(&value, pointer, sizeof(value));
    return value;
}

}  // namespace drvlite

#endif /* BLOOK_DRVLITE_DRIVER_NT_H */

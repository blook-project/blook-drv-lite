/*
 * blook-drv-lite - portable x64 page-table walker.
 *
 * The walker never attaches to a process and never dereferences a virtual
 * address: it reads physical memory through an injected reader. That keeps
 * the same code usable for kernel virtual addresses and for arbitrary
 * processes whose CR3 was obtained out of band, and it makes the logic
 * testable on the build machine with synthetic page tables.
 */
#ifndef BLOOK_DRVLITE_CORE_PAGEWALK_H
#define BLOOK_DRVLITE_CORE_PAGEWALK_H

#include "types.h"

namespace drvlite::core {

/* Reads `size` bytes of physical memory into `buffer`. Returns false when the
 * range cannot be read (MMIO hole, invalid physical address, ...). */
using phys_read_fn = bool (*)(void* ctx, u64 physical, void* buffer,
                              usize size);

struct phys_reader {
    void* ctx;
    phys_read_fn read;

    bool valid() const { return read != nullptr; }
    bool read_at(u64 physical, void* buffer, usize size) const {
        return read != nullptr && read(ctx, physical, buffer, size);
    }
};

enum class walk_code : u32 {
    ok = 0,
    no_reader,
    non_canonical,
    not_present,
    bad_entry,
    read_failed,
};

const char* walk_code_str(walk_code code);

struct translation {
    u64 physical = 0;
    u32 span = 0; /* bytes covered by the resolved page: 4 KiB, 2 MiB, 1 GiB */
};

/*
 * Translates one virtual address. `cr3` is the physical address of the PML4
 * (page aligned; the low 12 bits are ignored).
 */
walk_code translate(const phys_reader& reader, u64 cr3, u64 virtual_address,
                    translation* out);

struct read_result {
    u64 copied = 0;      /* bytes actually read from physical memory */
    u64 zero_filled = 0; /* bytes zero-filled because the range is not mapped */
    u64 failed_at = 0;   /* virtual address of the first failure */
    walk_code error = walk_code::ok;
};

/*
 * Reads a possibly large virtual range page by page, respecting 4 KiB/2 MiB/
 * 1 GiB pages and chunking physical reads at page boundaries.
 *
 * tolerant == false: stop at the first unmapped page and report the error.
 * tolerant == true : zero-fill unmapped/unreadable pages and keep going
 *                    (used for module dumps, which always have holes).
 */
read_result read_virtual(const phys_reader& reader, u64 cr3,
                         u64 virtual_address, void* buffer, usize size,
                         bool tolerant);

/*
 * Validates that `cr3` really is the address space of the process owning
 * `peb`: the PEB must be mapped, PEB.ImageBaseAddress and PEB.Ldr must be
 * canonical user pointers, and when `expected_image` is non-zero it must
 * equal PEB.ImageBaseAddress. This is the check that rejects a decoy CR3.
 */
bool validate_cr3(const phys_reader& reader, u64 cr3, u64 peb,
                  u64 expected_image);

/* Convenience: translate and read a single small object (e.g. a UINT64). */
bool read_u64(const phys_reader& reader, u64 cr3, u64 virtual_address,
              u64* value);

}  // namespace drvlite::core

#endif /* BLOOK_DRVLITE_CORE_PAGEWALK_H */

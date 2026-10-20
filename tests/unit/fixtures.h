/*
 * Host-side test fixtures: a synthetic physical memory, a page-table builder,
 * a minimal PE image builder and a ci.dll-like image builder. None of these
 * depend on Windows or the WDK.
 */
#ifndef BLOOK_DRVLITE_TESTS_FIXTURES_H
#define BLOOK_DRVLITE_TESTS_FIXTURES_H

#include <cstring>
#include <vector>

#include "core/types.h"

namespace drvlite::test {

/* Physical memory starting at `base`; every byte is readable through the
 * core::phys_reader interface. */
class mock_phys {
   public:
    static constexpr drvlite::core::u64 kBase = 0x1000;
    static constexpr drvlite::core::u64 page_size = drvlite::core::kPageSize;

    mock_phys() : bytes_(0x400000, 0) {}

    drvlite::core::u64 allocate_page() {
        const drvlite::core::u64 address = kBase + next_;
        next_ += page_size;
        return address;
    }

    drvlite::core::u64 size() const { return bytes_.size(); }

    void write(drvlite::core::u64 address, const void* data,
               drvlite::core::usize size) {
        const drvlite::core::u64 offset = address - kBase;
        std::memcpy(bytes_.data() + offset, data, size);
    }

    void write_u64(drvlite::core::u64 address, drvlite::core::u64 value) {
        write(address, &value, sizeof(value));
    }

    void write_u32(drvlite::core::u64 address, drvlite::core::u32 value) {
        write(address, &value, sizeof(value));
    }

    void write_u8(drvlite::core::u64 address, drvlite::core::u8 value) {
        write(address, &value, sizeof(value));
    }

    drvlite::core::u64 read_u64(drvlite::core::u64 address) const {
        drvlite::core::u64 value = 0;
        std::memcpy(&value, bytes_.data() + (address - kBase), sizeof(value));
        return value;
    }

    drvlite::core::u8 read_u8(drvlite::core::u64 address) const {
        return bytes_[static_cast<drvlite::core::usize>(address - kBase)];
    }

    static bool read_callback(void* ctx, drvlite::core::u64 address,
                              void* buffer, drvlite::core::usize size) {
        auto* self = static_cast<mock_phys*>(ctx);
        if (address < kBase || address + size > kBase + self->bytes_.size()) {
            return false;
        }
        std::memcpy(buffer, self->bytes_.data() + (address - kBase), size);
        return true;
    }

    drvlite::core::phys_reader reader() {
        return drvlite::core::phys_reader{this, &mock_phys::read_callback};
    }

   private:
    std::vector<drvlite::core::u8> bytes_;
    drvlite::core::u64 next_ = 0;
};

/* Builds page tables in a mock_phys. Supports 4 KiB, 2 MiB and 1 GiB leaves,
 * enough to exercise every walker branch. */
class page_table_builder {
   public:
    explicit page_table_builder(mock_phys& memory)
        : memory_(memory), pml4_(memory.allocate_page()) {}

    drvlite::core::u64 cr3() const { return pml4_; }

    void map_4k(drvlite::core::u64 virtual_address,
                drvlite::core::u64 physical_address) {
        const drvlite::core::u64 pt = table_page(virtual_address, 3);
        set_entry(pt, index(virtual_address, 12), make_entry(physical_address));
    }

    void map_2m(drvlite::core::u64 virtual_address,
                drvlite::core::u64 physical_address) {
        const drvlite::core::u64 pd = table_page(virtual_address, 2);
        set_entry(pd, index(virtual_address, 21),
                  make_entry(physical_address) | drvlite::core::kPteLarge);
    }

    void map_1g(drvlite::core::u64 virtual_address,
                drvlite::core::u64 physical_address) {
        const drvlite::core::u64 pdpt = table_page(virtual_address, 1);
        set_entry(pdpt, index(virtual_address, 30),
                  make_entry(physical_address) | drvlite::core::kPteLarge);
    }

    void unmap(drvlite::core::u64 virtual_address) {
        const drvlite::core::u64 pt = table_page(virtual_address, 3);
        set_entry(pt, index(virtual_address, 12), 0);
    }

   private:
    static drvlite::core::u64 index(drvlite::core::u64 virtual_address,
                                    drvlite::core::u32 shift) {
        return (virtual_address >> shift) & 0x1FF;
    }

    static drvlite::core::u64 make_entry(drvlite::core::u64 address) {
        return (address & drvlite::core::kPteAddressMask) | 0x3;
    }

    drvlite::core::u64 table_page(drvlite::core::u64 virtual_address,
                                  drvlite::core::u32 depth) {
        drvlite::core::u64 table = pml4_;
        const drvlite::core::u32 shifts[] = {39, 30, 21};
        for (drvlite::core::u32 level = 0; level < depth; ++level) {
            const drvlite::core::u64 slot =
                index(virtual_address, shifts[level]);
            drvlite::core::u64 next = memory_.read_u64(table + slot * 8);
            if ((next & drvlite::core::kPtePresent) == 0) {
                next = memory_.allocate_page();
                set_entry(table, slot, make_entry(next));
            }
            table = next & drvlite::core::kPteAddressMask;
        }
        return table;
    }

    void set_entry(drvlite::core::u64 table, drvlite::core::u64 slot,
                   drvlite::core::u64 value) {
        memory_.write_u64(table + slot * 8, value);
    }

    mock_phys& memory_;
    drvlite::core::u64 pml4_;
};

}  // namespace drvlite::test

#endif /* BLOOK_DRVLITE_TESTS_FIXTURES_H */

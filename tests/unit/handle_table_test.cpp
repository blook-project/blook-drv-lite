#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "core/handle_table.h"

namespace {

using namespace drvlite::core;

constexpr u64 kKernelBase = 0xFFFF800000000000ULL;
constexpr u32 kMemorySize = 0x10000;

class kernel_memory {
   public:
    kernel_memory() : bytes_(kMemorySize, 0) {}

    u8* at(u64 address) {
        return bytes_.data() + static_cast<usize>(address - kKernelBase);
    }

    void write_u64(u64 address, u64 value) {
        std::memcpy(at(address), &value, sizeof(value));
    }

    void write_u8(u64 address, u8 value) { *at(address) = value; }

    u64 read_u64(u64 address) const {
        u64 value = 0;
        std::memcpy(&value,
                    bytes_.data() + static_cast<usize>(address - kKernelBase),
                    sizeof(value));
        return value;
    }

    static bool read_callback(void* ctx, u64 address, void* buffer,
                              usize size) {
        auto* self = static_cast<kernel_memory*>(ctx);
        if (address < kKernelBase ||
            address + size > kKernelBase + kMemorySize) {
            return false;
        }
        std::memcpy(
            buffer,
            self->bytes_.data() + static_cast<usize>(address - kKernelBase),
            size);
        return true;
    }

    table_reader reader() {
        return table_reader{this, &kernel_memory::read_callback};
    }

   private:
    std::vector<u8> bytes_;
};

TEST(HandleTable, LocatesEntryInSingleLevelTable) {
    kernel_memory memory;
    const u64 entries = kKernelBase + 0x1000;
    const u32 handle = 0x40; /* index 0x10 */
    u64 entry = 0;
    ASSERT_EQ(table_code::ok,
              locate_entry(memory.reader(), entries, handle, 1024, &entry));
    EXPECT_EQ(entries + 0x10 * kHandleEntrySize, entry);
}

TEST(HandleTable, LocatesEntryAcrossLevels) {
    kernel_memory memory;
    const u64 root = kKernelBase + 0x1000;
    const u64 level1 = kKernelBase + 0x2000;
    const u64 level2 = kKernelBase + 0x3000;
    memory.write_u64(root, level1);
    memory.write_u64(root + 0x91 * 8, level1);
    memory.write_u64(level1 + 0x91 * 8, level2);

    /* handle index = 0x12345 => level1 slot 0x91, level2 slot 0x145. */
    const u32 handle = 0x12345 * 4;
    u64 entry = 0;
    ASSERT_EQ(table_code::ok,
              locate_entry(memory.reader(), root | 2, handle, 1 << 22, &entry));
    EXPECT_EQ(level2 + 0x145 * kHandleEntrySize, entry);

    /* Two-level table: TableCode low bits 1, level 1 pointer in the root. */
    ASSERT_EQ(table_code::ok,
              locate_entry(memory.reader(), root | 1, handle, 1 << 22, &entry));
    EXPECT_EQ(level1 + 0x145 * kHandleEntrySize, entry);
}

TEST(HandleTable, RejectsBadTableCode) {
    kernel_memory memory;
    u64 entry = 0;
    EXPECT_EQ(table_code::bad_table,
              locate_entry(memory.reader(), 0, 0x40, 1024, &entry));
    EXPECT_EQ(table_code::bad_table,
              locate_entry(memory.reader(), kKernelBase + 0x1000, 0x42, 1024,
                           &entry));
    EXPECT_EQ(table_code::out_of_range,
              locate_entry(memory.reader(), 0, 0x10000, 512, &entry));
}

TEST(HandleTable, ReadFailuresAreReported) {
    kernel_memory memory;
    /* Table code points outside the mocked kernel memory range. */
    u64 entry = 0;
    const u64 far = kKernelBase + 0x200000;
    EXPECT_EQ(table_code::read_failed,
              locate_entry(memory.reader(), far | 1, 0x40, 1024, &entry));
}

TEST(HandleTable, ReadsAndWritesWideBitfields) {
    u8 data[16] = {};
    set_bits(data, sizeof(data), 44, 25, 0x1400);
    EXPECT_EQ(0x1400u, get_bits(data, sizeof(data), 44, 25));
    set_bits(data, sizeof(data), 40, 25, 0x1FFFFF);
    EXPECT_EQ(0x1FFFFF, get_bits(data, sizeof(data), 40, 25));
    /* A byte buffer is bounds checked. */
    EXPECT_EQ(0u, get_bits(data, sizeof(data), 128, 25));
    set_bits(data, sizeof(data), 200, 25, 0xFFFFFFFFu);
}

TEST(HandleTable, FindsGrantedAccessField) {
    u8 entry[16] = {};
    /* Real layout (Win10 1909+): ObjectPointerBits:44, GrantedAccessBits:25 */
    set_bits(entry, sizeof(entry), 44, 25, 0x1400);
    u32 offset = 0;
    ASSERT_TRUE(find_access_field(entry, sizeof(entry), 25, 0x1400, &offset));
    EXPECT_EQ(44u, offset);

    set_bits(entry, sizeof(entry), offset, 25, 0x1FFFFF);
    EXPECT_EQ(0x1FFFFF, get_bits(entry, sizeof(entry), 44, 25));
    EXPECT_FALSE(find_access_field(entry, sizeof(entry), 25, 0x1400, &offset));
}

TEST(HandleTable, AmbiguousAccessFieldIsRejected) {
    u8 entry[16] = {};
    set_bits(entry, sizeof(entry), 0, 25, 0x1400);
    set_bits(entry, sizeof(entry), 30, 25, 0x1400);
    u32 offset = 0;
    EXPECT_FALSE(find_access_field(entry, sizeof(entry), 25, 0x1400, &offset));
}

}  // namespace

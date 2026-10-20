#include <gtest/gtest.h>

#include "core/pagewalk.h"
#include "fixtures.h"

namespace {

namespace core = drvlite::core;
using drvlite::test::mock_phys;

constexpr core::u64 kUserVa = 0x00007FF612000000ULL;   /* 2 MiB aligned */
constexpr core::u64 kKernelVa = 0xFFFFF80000000000ULL; /* 1 GiB aligned */

TEST(PageWalk, Translates4KiBPage) {
    mock_phys memory;
    drvlite::test::page_table_builder tables(memory);
    const core::u64 page = memory.allocate_page();
    tables.map_4k(kUserVa, page);
    memory.write_u64(page + 0x20, 0x1122334455667788ULL);

    core::translation mapped;
    ASSERT_EQ(core::walk_code::ok,
              core::translate(memory.reader(), tables.cr3(), kUserVa + 0x20,
                              &mapped));
    EXPECT_EQ(page + 0x20, mapped.physical);
    EXPECT_EQ(0x1000u, mapped.span);

    core::u64 value = 0;
    ASSERT_TRUE(
        core::read_u64(memory.reader(), tables.cr3(), kUserVa + 0x20, &value));
    EXPECT_EQ(0x1122334455667788ULL, value);
}

TEST(PageWalk, Translates2MiBPage) {
    mock_phys memory;
    drvlite::test::page_table_builder tables(memory);
    const core::u64 page = 0x200000; /* 2 MiB aligned physical page */
    tables.map_2m(kUserVa, page);
    memory.write_u8(page + 0x1234, 0x5A);

    core::translation mapped;
    ASSERT_EQ(core::walk_code::ok,
              core::translate(memory.reader(), tables.cr3(), kUserVa + 0x1234,
                              &mapped));
    EXPECT_EQ(page + 0x1234, mapped.physical);
    EXPECT_EQ(0x200000u, mapped.span);
    EXPECT_EQ(0x5A, memory.read_u8(mapped.physical));
}

TEST(PageWalk, Translates1GiBPage) {
    mock_phys memory;
    drvlite::test::page_table_builder tables(memory);
    const core::u64 page = 0x40000000; /* 1 GiB aligned physical page */
    tables.map_1g(kKernelVa, page);

    core::translation mapped;
    ASSERT_EQ(core::walk_code::ok,
              core::translate(memory.reader(), tables.cr3(), kKernelVa + 0xABC,
                              &mapped));
    EXPECT_EQ(page + 0xABC, mapped.physical);
    EXPECT_EQ(0x40000000u, mapped.span);
}

TEST(PageWalk, ReportsNotPresent) {
    mock_phys memory;
    drvlite::test::page_table_builder tables(memory);
    const core::u64 page = memory.allocate_page();
    tables.map_4k(kUserVa, page);

    core::translation mapped;
    EXPECT_EQ(core::walk_code::not_present,
              core::translate(memory.reader(), tables.cr3(), kUserVa + 0x2000,
                              &mapped));
}

TEST(PageWalk, RejectsNonCanonicalAddress) {
    mock_phys memory;
    drvlite::test::page_table_builder tables(memory);
    core::translation mapped;
    EXPECT_EQ(core::walk_code::non_canonical,
              core::translate(memory.reader(), tables.cr3(),
                              0x0000800000000000ULL, &mapped));
}

TEST(PageWalk, ReadSpansPages) {
    mock_phys memory;
    drvlite::test::page_table_builder tables(memory);
    const core::u64 first = memory.allocate_page();
    const core::u64 second = memory.allocate_page();
    tables.map_4k(kUserVa, first);
    tables.map_4k(kUserVa + 0x1000, second);
    for (core::u32 index = 0; index < 0x1000; ++index) {
        memory.write_u8(first + index, static_cast<core::u8>(index));
        memory.write_u8(second + index, static_cast<core::u8>(index ^ 0xA5));
    }

    core::u8 buffer[0x200] = {};
    const core::read_result result =
        core::read_virtual(memory.reader(), tables.cr3(), kUserVa + 0xF00,
                           buffer, sizeof(buffer), false);
    EXPECT_EQ(sizeof(buffer), result.copied);
    EXPECT_EQ(core::walk_code::ok, result.error);
    for (core::u32 index = 0; index < sizeof(buffer); ++index) {
        const core::u8 expected =
            index < 0x100 ? static_cast<core::u8>(0xF00 + index)
                          : static_cast<core::u8>((index - 0x100) ^ 0xA5);
        EXPECT_EQ(expected, buffer[index]) << "index " << index;
    }
}

TEST(PageWalk, TolerantReadZeroFillsHoles) {
    mock_phys memory;
    drvlite::test::page_table_builder tables(memory);
    const core::u64 first = memory.allocate_page();
    const core::u64 third = memory.allocate_page();
    tables.map_4k(kUserVa, first);
    tables.map_4k(kUserVa + 0x2000, third);
    memory.write_u8(first, 0x11);
    memory.write_u8(third, 0x33);

    core::u8 buffer[0x3000] = {};
    const core::read_result result = core::read_virtual(
        memory.reader(), tables.cr3(), kUserVa, buffer, sizeof(buffer), true);
    EXPECT_EQ(core::walk_code::not_present, result.error);
    EXPECT_EQ(0x2000ULL, result.copied);
    EXPECT_EQ(0x1000ULL, result.zero_filled);
    EXPECT_EQ(0x11, buffer[0]);
    EXPECT_EQ(0x00, buffer[0x1000]);
    EXPECT_EQ(0x33, buffer[0x2000]);
}

TEST(PageWalk, StrictReadFailsOnHole) {
    mock_phys memory;
    drvlite::test::page_table_builder tables(memory);
    const core::u64 first = memory.allocate_page();
    tables.map_4k(kUserVa, first);
    core::u8 buffer[0x2000] = {};
    const core::read_result result = core::read_virtual(
        memory.reader(), tables.cr3(), kUserVa, buffer, sizeof(buffer), false);
    EXPECT_EQ(core::walk_code::not_present, result.error);
    EXPECT_EQ(kUserVa + 0x1000, result.failed_at);
}

TEST(PageWalk, ValidatesCr3ThroughPeb) {
    mock_phys memory;
    drvlite::test::page_table_builder tables(memory);
    constexpr core::u64 peb = 0x00007FF6A0000000ULL;
    constexpr core::u64 image = 0x00007FF612340000ULL;
    const core::u64 peb_page = memory.allocate_page();
    tables.map_4k(peb, peb_page);
    memory.write_u64(peb_page + 0x10, image);
    memory.write_u64(peb_page + 0x18, peb + 0x1000);

    EXPECT_TRUE(core::validate_cr3(memory.reader(), tables.cr3(), peb, image));
    EXPECT_TRUE(core::validate_cr3(memory.reader(), tables.cr3(), peb, 0));
    EXPECT_FALSE(
        core::validate_cr3(memory.reader(), tables.cr3(), peb, image + 0x1000));
    /* A decoy CR3 whose PEB resolves elsewhere fails the image-base check. */
    mock_phys other;
    drvlite::test::page_table_builder decoy(other);
    const core::u64 decoy_page = other.allocate_page();
    decoy.map_4k(peb, decoy_page);
    other.write_u64(decoy_page + 0x10, 0x00007FF6B0000000ULL);
    other.write_u64(decoy_page + 0x18, peb + 0x1000);
    EXPECT_FALSE(core::validate_cr3(other.reader(), decoy.cr3(), peb, image));
}

TEST(PageWalk, RejectsBadCr3) {
    mock_phys memory;
    EXPECT_FALSE(core::validate_cr3(memory.reader(), 0, kUserVa, 0));
    EXPECT_FALSE(core::validate_cr3(memory.reader(), 0x1234, kUserVa, 0));
}

}  // namespace

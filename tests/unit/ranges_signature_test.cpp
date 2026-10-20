#include <gtest/gtest.h>

#include "core/ranges.h"
#include "core/signature.h"

namespace {

using namespace drvlite::core;

TEST(Ranges, PageStepStaysInsideOnePage) {
    EXPECT_EQ(0x1000u, page_step(0x1000, 0x2000));
    EXPECT_EQ(0x100u, page_step(0x1000, 0x100));
    EXPECT_EQ(0x1u, page_step(0xFFF, 0x100));
    EXPECT_EQ(0x800u, page_step(0x1800, 0x1000));
    EXPECT_EQ(0x0u, page_step(0x1000, 0));
}

TEST(Ranges, MaxChunkCapsStep) {
    EXPECT_EQ(0x100u, page_step(0x1000, 0x2000, 0x100));
    EXPECT_EQ(0x40u, page_step(0xFC0, 0x1000, 0x40));
}

TEST(Ranges, ChunkerWalksWholeRange) {
    range_chunker chunker(0x1F00, 0x1800, 0);
    u64 offset = 0;
    usize length = 0;
    EXPECT_TRUE(chunker.next(&offset, &length));
    EXPECT_EQ(0u, offset);
    EXPECT_EQ(0x100u, length);
    EXPECT_TRUE(chunker.next(&offset, &length));
    EXPECT_EQ(0x100u, offset);
    EXPECT_EQ(0x1000u, length);
    EXPECT_TRUE(chunker.next(&offset, &length));
    EXPECT_EQ(0x1100u, offset);
    EXPECT_EQ(0x700u, length);
    EXPECT_FALSE(chunker.next(&offset, &length));
    EXPECT_EQ(0x1800u, chunker.consumed());
}

TEST(Ranges, CheckedAddDetectsOverflow) {
    u64 result = 0;
    EXPECT_TRUE(checked_add(1, 2, &result));
    EXPECT_EQ(3u, result);
    EXPECT_FALSE(checked_add(~0ULL, 1, &result));
}

TEST(Signature, FindsLiteralPattern) {
    const u8 data[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    const u8 needle[] = {0x22, 0x33};
    const u8 mask[] = {0xFF, 0xFF};
    EXPECT_EQ(2u, find_pattern(data, sizeof(data), pattern{needle, mask, 2}));
}

TEST(Signature, WildcardsMatchAnyByte) {
    const u8 data[] = {0x8B, 0x05, 0xAA, 0xBB, 0xCC, 0xDD, 0xC3};
    const u8 needle[] = {0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0xC3};
    const u8 mask[] = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF};
    EXPECT_EQ(0u, find_pattern(data, sizeof(data), pattern{needle, mask, 7}));
}

TEST(Signature, ParsesTextPattern) {
    u8 bytes[8] = {};
    u8 mask[8] = {};
    usize length = 0;
    ASSERT_TRUE(parse_pattern("48 8B 05 ?? ?? ?? ?? C3", bytes, mask,
                              sizeof(bytes), &length));
    EXPECT_EQ(8u, length);
    EXPECT_EQ(0x48, bytes[0]);
    EXPECT_EQ(0xFF, mask[2]);
    EXPECT_EQ(0x05, bytes[2]);
    EXPECT_EQ(0x00, mask[3]);
    EXPECT_FALSE(parse_pattern("48 8G", bytes, mask, sizeof(bytes), &length));
}

TEST(Signature, RejectsShortBuffers) {
    const u8 data[] = {0xAA};
    const u8 needle[] = {0xAA, 0xBB};
    const u8 mask[] = {0xFF, 0xFF};
    EXPECT_EQ(kNotFound,
              find_pattern(data, sizeof(data), pattern{needle, mask, 2}));
    EXPECT_FALSE(matches_at(data, sizeof(data), 0, pattern{needle, mask, 2}));
}

}  // namespace

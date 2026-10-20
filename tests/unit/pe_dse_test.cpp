#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "core/dse_resolver.h"
#include "core/pe.h"

namespace {

using namespace drvlite::core;

void put_u16(std::vector<u8>& image, usize offset, u16 value) {
    image[offset] = static_cast<u8>(value & 0xFF);
    image[offset + 1] = static_cast<u8>((value >> 8) & 0xFF);
}

void put_u32(std::vector<u8>& image, usize offset, u32 value) {
    for (u32 index = 0; index < 4; ++index) {
        image[offset + index] = static_cast<u8>((value >> (8 * index)) & 0xFF);
    }
}

void put_i32(std::vector<u8>& image, usize offset, i32 value) {
    put_u32(image, offset, static_cast<u32>(value));
}

void put_section(std::vector<u8>& image, usize offset, const char* name,
                 u32 virtual_address, u32 virtual_size, u32 raw_offset,
                 u32 raw_size) {
    std::memset(image.data() + offset, 0, 8);
    std::memcpy(image.data() + offset, name, std::strlen(name));
    put_u32(image, offset + 8, virtual_size);
    put_u32(image, offset + 12, virtual_address);
    put_u32(image, offset + 16, raw_size);
    put_u32(image, offset + 20, raw_offset);
}

/*
 * Builds a ci.dll lookalike:
 *   .text 0x1000  CiInitialize  E8 -> CipInitialize at 0x1100
 *   .text 0x1100  CipInitialize 89 0D -> g_CiOptions at 0x3000
 *   .rdata 0x2000 export directory (one export: CiInitialize)
 *   .data 0x3000  g_CiOptions = 6
 */
std::vector<u8> build_ci_image(u64 image_base) {
    (void)image_base;
    std::vector<u8> image(0x4000, 0);
    put_u16(image, 0x00, 0x5A4D);
    put_u32(image, 0x3C, 0x80);
    put_u32(image, 0x80, 0x00004550);
    put_u16(image, 0x84, 0x8664);
    put_u16(image, 0x86, 3);
    put_u16(image, 0x94, 0xF0);
    put_u16(image, 0x98, 0x20B);
    put_u32(image, 0x98 + 56, 0x4000);
    put_u32(image, 0x98 + 60, 0x400);
    put_u32(image, 0x98 + 108, 16);
    put_u32(image, 0x98 + 112, 0x2000);
    put_u32(image, 0x98 + 116, 0x100);
    put_section(image, 0x188, ".text", 0x1000, 0x400, 0x1000, 0x400);
    put_section(image, 0x1B0, ".rdata", 0x2000, 0x200, 0x2000, 0x200);
    put_section(image, 0x1D8, ".data", 0x3000, 0x100, 0x3000, 0x100);

    image[0x1000] = 0xE8;
    put_i32(image, 0x1001, static_cast<i32>(0x1100 - (0x1000 + 5)));
    image[0x1100] = 0x89;
    image[0x1101] = 0x0D;
    put_i32(image, 0x1102, static_cast<i32>(0x3000 - (0x1100 + 6)));
    put_u32(image, 0x3000, 6);

    put_u32(image, 0x2000 + 12, 0x2100);
    put_u32(image, 0x2000 + 16, 0);
    put_u32(image, 0x2000 + 20, 1);
    put_u32(image, 0x2000 + 24, 1);
    put_u32(image, 0x2000 + 28, 0x20C0);
    put_u32(image, 0x2000 + 32, 0x2080);
    put_u32(image, 0x2000 + 36, 0x20A0);
    put_u32(image, 0x20C0, 0x1000);
    put_u32(image, 0x2080, 0x2100);
    put_u16(image, 0x20A0, 0);
    const char name[] = "CiInitialize";
    std::memcpy(image.data() + 0x2100, name, sizeof(name));
    return image;
}

constexpr u64 kImageBase = 0xFFFFF80010000000ULL;

TEST(PeImage, ParsesHeadersAndExports) {
    const std::vector<u8> image = build_ci_image(kImageBase);
    pe_image parsed;
    ASSERT_EQ(pe_code::ok,
              pe_parse(image.data(), image.size(), kImageBase, &parsed));
    EXPECT_EQ(kImageBase, parsed.image_base);
    EXPECT_EQ(3u, parsed.number_of_sections);
    EXPECT_EQ(0x1000u, parsed.sections[0].virtual_address);
    EXPECT_STREQ(".text", parsed.sections[0].name);
    EXPECT_EQ(0x1000u, pe_find_export_rva(parsed, "CiInitialize"));
    EXPECT_EQ(0u, pe_find_export_rva(parsed, "CipInitialize"));
    EXPECT_EQ(0u, pe_find_export_rva(parsed, "g_CiOptions"));

    usize offset = 0;
    usize available = 0;
    ASSERT_TRUE(pe_rva_to_offset(parsed, 0x1234, &offset, &available));
    EXPECT_EQ(0x1234u, offset);
    EXPECT_EQ(0x4000u - 0x1234u, available);
    EXPECT_FALSE(pe_rva_to_offset(parsed, 0x9000, &offset, &available));
}

TEST(PeImage, RejectsNonPeBuffers) {
    const u8 garbage[0x100] = {};
    pe_image parsed;
    EXPECT_NE(pe_code::ok,
              pe_parse(garbage, sizeof(garbage), kImageBase, &parsed));
    EXPECT_EQ(pe_code::too_small, pe_parse(nullptr, 0, 0, &parsed));
}

TEST(DseResolver, ResolvesCiOptionsFromCipInitialize) {
    const std::vector<u8> image = build_ci_image(kImageBase);
    const dse_resolution result =
        dse_resolve_ci_options(image.data(), image.size(), kImageBase);
    ASSERT_EQ(dse_code::ok, result.code);
    EXPECT_EQ(0x3000u, result.options_rva);
    EXPECT_EQ(kImageBase + 0x3000, result.options_va);
    EXPECT_EQ(0x1100u, result.reference_rva);
    EXPECT_EQ(1u, result.via);
}

TEST(DseResolver, ReportsMissingExport) {
    std::vector<u8> image = build_ci_image(kImageBase);
    /* Corrupt the export name so CiInitialize disappears. */
    image[0x2100] = 'X';
    const dse_resolution result =
        dse_resolve_ci_options(image.data(), image.size(), kImageBase);
    EXPECT_EQ(dse_code::no_ci_initialize, result.code);
}

TEST(DseResolver, ResolvesLegacyCiEnabledPattern) {
    std::vector<u8> image(0x2000, 0);
    image[0x100] = 0xEB;
    image[0x101] = 0x06;
    image[0x102] = 0x1D;
    image[0x103] = 0x88;
    put_i32(image, 0x104, static_cast<i32>(0x1500 - 0x108));
    const dse_resolution result = dse_resolve_legacy_ci_enabled(
        image.data(), image.size(), 0xFFFFF80020000000ULL);
    ASSERT_EQ(dse_code::ok, result.code);
    EXPECT_EQ(0x1500u, result.options_rva);
    EXPECT_EQ(0xFFFFF80020001500ULL, result.options_va);
}

TEST(DseResolver, LegacyPatternAbsentFailsCleanly) {
    const std::vector<u8> image(0x1000, 0x90);
    const dse_resolution result =
        dse_resolve_legacy_ci_enabled(image.data(), image.size(), kImageBase);
    EXPECT_EQ(dse_code::not_found, result.code);
}

}  // namespace

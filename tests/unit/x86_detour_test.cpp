#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "core/detour.h"
#include "core/signature.h"
#include "core/x86_decode.h"

namespace {

using namespace drvlite::core;

struct decode_case {
    std::vector<u8> bytes;
    u32 length;
    u32 rel_offset;
    u32 rel_size;
    u32 disp_offset;
};

u32 decode_length(const std::vector<u8>& bytes, instruction* out = nullptr) {
    instruction insn;
    if (!decode_instruction(bytes.data(), bytes.size(), &insn)) {
        return 0;
    }
    if (out != nullptr) {
        *out = insn;
    }
    return insn.length;
}

TEST(X86Decode, CommonPrologueEncodings) {
    const decode_case cases[] = {
        {{0x48, 0x89, 0x5C, 0x24, 0x08}, 5, 0, 0, 0},
        {{0x57}, 1, 0, 0, 0},
        {{0x48, 0x83, 0xEC, 0x20}, 4, 0, 0, 0},
        {{0xE8, 0x0A, 0x00, 0x00, 0x00}, 5, 1, 4, 0},
        {{0xEB, 0xFE}, 2, 1, 1, 0},
        {{0x0F, 0x84, 0x00, 0x01, 0x00, 0x00}, 6, 2, 4, 0},
        {{0xFF, 0x25, 0x00, 0x00, 0x00, 0x00}, 6, 0, 0, 2},
        {{0x48, 0x8D, 0x05, 0x34, 0x12, 0x00, 0x00}, 7, 0, 0, 3},
        {{0xC7, 0x05, 0x78, 0x56, 0x34, 0x12, 0x01, 0x00, 0x00, 0x00},
         10,
         0,
         0,
         2},
        {{0xF3, 0x0F, 0x1E, 0xFA}, 4, 0, 0, 0},
        {{0x65, 0x48, 0x8B, 0x24, 0x25, 0xA8, 0x01, 0x00, 0x00}, 9, 0, 0, 0},
        {{0x6A, 0x2B}, 2, 0, 0, 0},
        {{0x41, 0x53}, 2, 0, 0, 0},
        {{0x48, 0x81, 0xEC, 0x58, 0x01, 0x00, 0x00}, 7, 0, 0, 0},
        {{0x4C, 0x8D, 0x15, 0xE5, 0x46, 0xB7, 0x00}, 7, 0, 0, 3},
        {{0x0F, 0x01, 0xF8}, 3, 0, 0, 0},
        {{0xF3, 0x48, 0x0F, 0x1E, 0xCA}, 5, 0, 0, 0},
        {{0x48, 0x8B, 0x05, 0x34, 0x12, 0x00, 0x00}, 7, 0, 0, 3},
        {{0xF6, 0xC3, 0x01}, 3, 0, 0, 0},
        {{0xF7, 0xC3, 0x01, 0x00, 0x00, 0x00}, 6, 0, 0, 0},
        {{0xD1, 0xE0}, 2, 0, 0, 0},
        {{0x0F, 0xAE, 0x5D, 0xAC}, 4, 0, 0, 0},
        {{0x66, 0x65, 0x8B, 0x0C, 0x25, 0x62, 0x08, 0x00, 0x00}, 9, 0, 0, 0},
        {{0x49, 0x8B, 0xCA}, 3, 0, 0, 0},
        {{0xF3, 0x0F, 0x01, 0x29}, 4, 0, 0, 0},
        {{0x0F, 0x30}, 2, 0, 0, 0},
        {{0x0F, 0x05}, 2, 0, 0, 0},
        {{0xB8, 0x78, 0x56, 0x34, 0x12}, 5, 0, 0, 0},
        {{0x48, 0xB8, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08},
         10,
         0,
         0,
         0},
    };
    for (const decode_case& item : cases) {
        instruction insn;
        const u32 length = decode_length(item.bytes, &insn);
        EXPECT_EQ(item.length, length)
            << "bytes start " << static_cast<int>(item.bytes[0]);
        EXPECT_EQ(item.rel_offset, insn.rel_offset);
        EXPECT_EQ(item.rel_size, insn.rel_size);
        EXPECT_EQ(item.disp_offset, insn.disp_offset);
    }
}

TEST(X86Decode, FailsClosedOnUnknownEncodings) {
    /* VEX encoded instructions are not needed for hook sites. */
    const u8 vex[] = {0xC5, 0xF8, 0x77};
    instruction insn;
    EXPECT_FALSE(decode_instruction(vex, sizeof(vex), &insn));
    const u8 truncated[] = {0xE8, 0x00, 0x00};
    EXPECT_FALSE(decode_instruction(truncated, sizeof(truncated), &insn));
    const u8 empty[1] = {0};
    EXPECT_FALSE(decode_instruction(empty, 0, &insn));
}

TEST(X86Decode, CopyRelocatedFixesRelativeBranch) {
    /* Keep the relocation within the +-2 GiB reach of a rel32 operand. */
    const u64 source_address = 0xFFFFF80000001000ULL;
    const u64 destination_address = 0xFFFFF80000103000ULL;
    const u64 target = 0xFFFFF80000002000ULL;
    std::vector<u8> source(5, 0);
    source[0] = 0xE8;
    const i32 relative = static_cast<i32>(target - (source_address + 5));
    for (u32 index = 0; index < 4; ++index) {
        source[1 + index] = static_cast<u8>((relative >> (8 * index)) & 0xFF);
    }
    std::vector<u8> destination(8, 0xCC);
    ASSERT_EQ(5u,
              copy_relocated(source.data(), source_address, destination.data(),
                             destination_address, 5, destination.size()));
    const i32 adjusted = load_i32(destination.data() + 1);
    EXPECT_EQ(target, destination_address + 5 +
                          static_cast<u64>(static_cast<i64>(adjusted)));
}

TEST(Detour, PlansPatchAndTrampoline) {
    const u64 target = 0xFFFFF80000100000ULL;
    const u64 detour = 0xFFFFF80000300000ULL;
    const u64 trampoline = 0xFFFFF80000200000ULL;
    /* mov [rsp+8], rbx; push rdi; sub rsp, 0x20; mov rax, [rip+0x1234] */
    const std::vector<u8> prologue = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57,
                                      0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
                                      0x05, 0x34, 0x12, 0x00, 0x00};
    detour_plan plan;
    ASSERT_TRUE(plan_detour(prologue.data(), prologue.size(), target, detour,
                            trampoline, &plan));
    EXPECT_EQ(17u, plan.patch_length);
    EXPECT_EQ(plan.patch_length + kAbsoluteJumpLength, plan.trampoline_length);
    /* Patch begins with `jmp qword ptr [rip+0]` to the detour. */
    EXPECT_EQ(0xFF, plan.patch[0]);
    EXPECT_EQ(0x25, plan.patch[1]);
    u64 patched_target = 0;
    std::memcpy(&patched_target, plan.patch + 6, sizeof(patched_target));
    EXPECT_EQ(detour, patched_target);
    std::memcmp(plan.original, prologue.data(), plan.patch_length);

    /* Trampoline keeps the copied prologue; only the RIP-relative operand is
     * re-based, so compare the bytes before it. */
    EXPECT_EQ(0, std::memcmp(plan.trampoline, prologue.data(), 10));
    u64 jump_back = 0;
    std::memcpy(&jump_back, plan.trampoline + plan.patch_length + 6,
                sizeof(jump_back));
    EXPECT_EQ(target + plan.patch_length, jump_back);

    /* The RIP-relative `mov rax, [rip+0x1234]` at prologue offset 10 must be
     * re-based from target+10 to trampoline+10. */
    const i32 copied_disp = load_i32(plan.trampoline + 10 + 3);
    const u64 copied_target =
        trampoline + 10 + 7 + static_cast<u64>(static_cast<i64>(copied_disp));
    EXPECT_EQ(target + 10 + 7 + 0x1234, copied_target);
}

TEST(Detour, FailsClosedOnBadPrologue) {
    const u8 vex[] = {0xC5, 0xF8, 0x77, 0x48, 0x89, 0x5C, 0x24, 0x08};
    detour_plan plan;
    EXPECT_FALSE(plan_detour(vex, sizeof(vex), 0xFFFFF80000100000ULL,
                             0xFFFFF88000100000ULL, 0xFFFFF88000200000ULL,
                             &plan));
    const u8 truncated[] = {0x48, 0x89, 0x5C, 0x24};
    EXPECT_FALSE(plan_detour(truncated, sizeof(truncated),
                             0xFFFFF80000100000ULL, 0xFFFFF88000100000ULL,
                             0xFFFFF88000200000ULL, &plan));
}

TEST(X86Decode, CopyRelocatedFailsWhenRelativeTargetIsUnreachable) {
    /* A rel32 cannot reach a destination more than 2 GiB away: the copy must
     * fail instead of silently truncating the new displacement. */
    const u64 source_address = 0xFFFFF80000001000ULL;
    const u64 target = 0xFFFFF80000002000ULL;
    const u64 destination_address = 0xFFFFF88000100000ULL;
    std::vector<u8> source(5, 0);
    source[0] = 0xE8;
    const i32 relative = static_cast<i32>(target - (source_address + 5));
    for (u32 index = 0; index < 4; ++index) {
        source[1 + index] = static_cast<u8>((relative >> (8 * index)) & 0xFF);
    }
    std::vector<u8> destination(8, 0xCC);
    EXPECT_EQ(0u,
              copy_relocated(source.data(), source_address, destination.data(),
                             destination_address, 5, destination.size()));
}

TEST(Detour, FailsOnNonCanonicalAddresses) {
    const std::vector<u8> prologue = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57,
                                      0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
                                      0x05, 0x34, 0x12, 0x00, 0x00};
    detour_plan plan;
    EXPECT_FALSE(plan_detour(prologue.data(), prologue.size(),
                             0xFFFFF80000100000ULL, 0x0000800000000001ULL,
                             0xFFFFF80000200000ULL, &plan));
    EXPECT_FALSE(plan_detour(prologue.data(), prologue.size(), 0,
                             0xFFFFF80000300000ULL, 0xFFFFF80000200000ULL,
                             &plan));
}

}  // namespace

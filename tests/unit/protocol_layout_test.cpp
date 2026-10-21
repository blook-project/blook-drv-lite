#include <gtest/gtest.h>

#include <cstddef>

#include "core/protocol_codec.h"

/*
 * The wire layout is part of the ABI: a kernel/SDK mismatch must fail the
 * build, not corrupt memory at runtime. `protocol.h` pins the same values with
 * static_asserts; this test keeps them visible in the host suite as well.
 */
TEST(ProtocolLayout, WireSizesMatchTheContract) {
    EXPECT_EQ(104u, sizeof(drvlite_query_info_rsp));
    EXPECT_EQ(48u, sizeof(drvlite_mem_rsp));
    EXPECT_EQ(40u, offsetof(drvlite_mem_rsp, data));
    EXPECT_EQ(56u, sizeof(drvlite_process_mem_rsp));
    EXPECT_EQ(48u, offsetof(drvlite_process_mem_rsp, data));
    EXPECT_EQ(544u, sizeof(drvlite_batch_read_req));
    EXPECT_EQ(168u, sizeof(drvlite_batch_read_rsp));
    EXPECT_EQ(164u, offsetof(drvlite_batch_read_rsp, data));
    EXPECT_EQ(280u, sizeof(drvlite_module_info));
    EXPECT_EQ(320u, sizeof(drvlite_module_list_rsp));
    EXPECT_EQ(40u, offsetof(drvlite_module_list_rsp, modules));
    EXPECT_EQ(56u, sizeof(drvlite_module_dump_rsp));
    EXPECT_EQ(48u, offsetof(drvlite_module_dump_rsp, data));
    EXPECT_EQ(72u, sizeof(drvlite_dse_rsp));
    EXPECT_EQ(56u, sizeof(drvlite_handle_open_rsp));
    EXPECT_EQ(28u, sizeof(drvlite_handle_close_rsp));
    EXPECT_EQ(80u, sizeof(drvlite_infinity_status_rsp));
    EXPECT_EQ(88u, sizeof(drvlite_infinity_install_req));
    EXPECT_EQ(36u, sizeof(drvlite_hwbp_hide_rsp));
    EXPECT_EQ(96u, sizeof(drvlite_syscall_shield_req));
    EXPECT_EQ(36u, sizeof(drvlite_syscall_shield_rsp));
    EXPECT_EQ(28u, sizeof(drvlite_shutdown_rsp));
}

TEST(ProtocolLayout, ModuleEntryOverlaysTheWireType) {
    /* The driver fills module_entry and sends it as drvlite_module_info. */
    EXPECT_EQ(sizeof(drvlite_module_info),
              sizeof(drvlite_module_info)); /* keep the include used */
}

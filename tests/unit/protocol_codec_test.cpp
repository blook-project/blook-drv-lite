#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "core/protocol_codec.h"

namespace {

using namespace drvlite::core;

template <typename T>
std::vector<u8> encode(const T& value) {
    std::vector<u8> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

drvlite_query_info_req make_info_request() {
    drvlite_query_info_req request{};
    request.hdr.magic = DRVLITE_REQ_MAGIC;
    request.hdr.abi_major = DRVLITE_ABI_MAJOR;
    request.hdr.abi_minor = DRVLITE_ABI_MINOR;
    request.hdr.op = DRVLITE_OP_QUERY_INFO;
    request.hdr.size = sizeof(request);
    return request;
}

TEST(ProtocolCodec, AcceptsWellFormedRequest) {
    const drvlite_query_info_req request = make_info_request();
    const std::vector<u8> bytes = encode(request);
    const header_check check =
        check_request(bytes.data(), static_cast<u32>(bytes.size()),
                      DRVLITE_OP_QUERY_INFO, sizeof(drvlite_query_info_req));
    EXPECT_EQ(codec_code::ok, check.code);
    EXPECT_EQ(sizeof(drvlite_query_info_req), check.declared_size);
}

TEST(ProtocolCodec, RejectsMalformedRequests) {
    std::vector<u8> bytes = encode(make_info_request());
    bytes[0] = 0;
    EXPECT_EQ(
        codec_code::bad_magic,
        check_request(bytes.data(), static_cast<u32>(bytes.size()),
                      DRVLITE_OP_QUERY_INFO, sizeof(drvlite_query_info_req))
            .code);

    bytes = encode(make_info_request());
    bytes[4] = 2; /* abi_major */
    EXPECT_EQ(
        codec_code::bad_version,
        check_request(bytes.data(), static_cast<u32>(bytes.size()),
                      DRVLITE_OP_QUERY_INFO, sizeof(drvlite_query_info_req))
            .code);

    bytes = encode(make_info_request());
    bytes[8] = 0x22; /* op */
    EXPECT_EQ(
        codec_code::bad_op,
        check_request(bytes.data(), static_cast<u32>(bytes.size()),
                      DRVLITE_OP_QUERY_INFO, sizeof(drvlite_query_info_req))
            .code);

    drvlite_query_info_req request = make_info_request();
    request.hdr.size = sizeof(request) - 8;
    bytes = encode(request);
    EXPECT_EQ(
        codec_code::bad_size,
        check_request(bytes.data(), static_cast<u32>(bytes.size()),
                      DRVLITE_OP_QUERY_INFO, sizeof(drvlite_query_info_req))
            .code);

    EXPECT_EQ(codec_code::too_small,
              check_request(bytes.data(), 4, DRVLITE_OP_QUERY_INFO,
                            sizeof(drvlite_query_info_req))
                  .code);
    EXPECT_EQ(codec_code::too_small,
              check_request(nullptr, 16, DRVLITE_OP_QUERY_INFO,
                            sizeof(drvlite_query_info_req))
                  .code);
}

TEST(ProtocolCodec, ValidatesReplies) {
    drvlite_query_info_rsp response{};
    response.hdr.magic = DRVLITE_RSP_MAGIC;
    response.hdr.abi_major = DRVLITE_ABI_MAJOR;
    response.hdr.abi_minor = DRVLITE_ABI_MINOR;
    response.hdr.op = DRVLITE_OP_QUERY_INFO;
    response.hdr.status = DRVLITE_OK;
    response.hdr.size = sizeof(response);
    std::vector<u8> bytes = encode(response);
    EXPECT_EQ(codec_code::ok,
              check_reply(bytes.data(), static_cast<u32>(bytes.size()),
                          DRVLITE_OP_QUERY_INFO, sizeof(response))
                  .code);
    EXPECT_EQ(codec_code::bad_op,
              check_reply(bytes.data(), static_cast<u32>(bytes.size()),
                          DRVLITE_OP_DSE_QUERY, sizeof(response))
                  .code);
    bytes[0] = 0;
    EXPECT_EQ(codec_code::bad_magic,
              check_reply(bytes.data(), static_cast<u32>(bytes.size()),
                          DRVLITE_OP_QUERY_INFO, sizeof(response))
                  .code);
}

TEST(ProtocolCodec, BatchTotalsAreBounded) {
    drvlite_range ranges[DRVLITE_MAX_BATCH_RANGES] = {};
    ranges[0].size = 0x100;
    ranges[1].size = 0x200;
    u64 total = 0;
    EXPECT_TRUE(batch_total(ranges, 2, &total));
    EXPECT_EQ(0x300u, total);

    ranges[1].size = 0;
    EXPECT_FALSE(batch_total(ranges, 2, &total));
    ranges[1].size = 0x200;
    EXPECT_FALSE(batch_total(ranges, 0, &total));
    EXPECT_FALSE(batch_total(ranges, DRVLITE_MAX_BATCH_RANGES + 1, &total));
    EXPECT_FALSE(batch_total(nullptr, 2, &total));

    ranges[0].size = DRVLITE_MAX_TRANSFER + 1;
    EXPECT_FALSE(batch_total(ranges, 1, &total));
}

TEST(ProtocolCodec, PayloadPointersAreBoundsChecked) {
    const u8 buffer[32] = {};
    EXPECT_EQ(buffer + 16, payload_after(buffer, 32, 16));
    EXPECT_EQ(nullptr, payload_after(buffer, 8, 16));
    EXPECT_EQ(nullptr, payload_after(nullptr, 32, 16));
}

TEST(ProtocolCodec, RejectsMessagesAboveTheTransferLimit) {
    std::vector<u8> oversized(DRVLITE_MAX_TRANSFER + 8, 0);
    auto* header = reinterpret_cast<drvlite_query_info_req*>(oversized.data());
    header->hdr.magic = DRVLITE_REQ_MAGIC;
    header->hdr.abi_major = DRVLITE_ABI_MAJOR;
    header->hdr.abi_minor = DRVLITE_ABI_MINOR;
    header->hdr.op = DRVLITE_OP_QUERY_INFO;
    header->hdr.size = sizeof(drvlite_query_info_req);
    const header_check check =
        check_request(oversized.data(), static_cast<u32>(oversized.size()),
                      DRVLITE_OP_QUERY_INFO, sizeof(drvlite_query_info_req));
    EXPECT_EQ(codec_code::too_large, check.code);
}

}  // namespace

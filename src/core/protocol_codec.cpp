#include "protocol_codec.h"

#include "ranges.h"

namespace drvlite::core {

const char* codec_code_str(codec_code code) {
    switch (code) {
        case codec_code::ok:
            return "ok";
        case codec_code::too_small:
            return "too-small";
        case codec_code::bad_magic:
            return "bad-magic";
        case codec_code::bad_version:
            return "bad-version";
        case codec_code::bad_op:
            return "bad-op";
        case codec_code::bad_size:
            return "bad-size";
        case codec_code::too_large:
            return "too-large";
    }
    return "unknown";
}

namespace {
/* The reference decoder below is intentionally unused; the explicit per-header
 * readers keep short-buffer handling obvious. */
}  // namespace

header_check check_request(const void* data, u32 length, u16 expected_op,
                           u32 min_size) {
    header_check result;
    result.available = length;
    if (data == nullptr) {
        result.code = codec_code::too_small;
        return result;
    }
    if (length < sizeof(drvlite_req_header)) {
        result.code = codec_code::too_small;
        return result;
    }
    if (length > DRVLITE_MAX_TRANSFER) {
        result.code = codec_code::too_large;
        return result;
    }
    drvlite_req_header header{};
    const u8* bytes = static_cast<const u8*>(data);
    for (usize index = 0; index < sizeof(header); ++index) {
        reinterpret_cast<u8*>(&header)[index] = bytes[index];
    }
    result.declared_size = header.size;
    if (header.magic != DRVLITE_REQ_MAGIC) {
        result.code = codec_code::bad_magic;
    } else if (header.abi_major != DRVLITE_ABI_MAJOR ||
               header.abi_minor != DRVLITE_ABI_MINOR) {
        result.code = codec_code::bad_version;
    } else if (header.op != expected_op) {
        result.code = codec_code::bad_op;
    } else if (header.size != min_size) {
        result.code = codec_code::bad_size;
    } else if (length != min_size && header.size > length) {
        result.code = codec_code::bad_size;
    }
    return result;
}

header_check check_reply(const void* data, u32 length, u16 expected_op,
                         u32 min_size) {
    header_check result;
    result.available = length;
    if (data == nullptr) {
        result.code = codec_code::too_small;
        return result;
    }
    if (length < sizeof(drvlite_rsp_header)) {
        result.code = codec_code::too_small;
        return result;
    }
    drvlite_rsp_header header{};
    const u8* bytes = static_cast<const u8*>(data);
    for (usize index = 0; index < sizeof(header); ++index) {
        reinterpret_cast<u8*>(&header)[index] = bytes[index];
    }
    result.declared_size = header.size;
    if (header.magic != DRVLITE_RSP_MAGIC) {
        result.code = codec_code::bad_magic;
    } else if (header.abi_major != DRVLITE_ABI_MAJOR ||
               header.abi_minor != DRVLITE_ABI_MINOR) {
        result.code = codec_code::bad_version;
    } else if (header.op != expected_op) {
        result.code = codec_code::bad_op;
    } else if (header.size < min_size || header.size > length) {
        result.code = codec_code::bad_size;
    }
    return result;
}

bool batch_total(const drvlite_range* ranges, u32 count, u64* total) {
    if (total == nullptr || count == 0 || count > DRVLITE_MAX_BATCH_RANGES ||
        ranges == nullptr) {
        return false;
    }
    u64 sum = 0;
    for (u32 index = 0; index < count; ++index) {
        if (ranges[index].size == 0) {
            return false;
        }
        if (!checked_add(sum, ranges[index].size, &sum)) {
            return false;
        }
    }
    if (sum > DRVLITE_MAX_TRANSFER) {
        return false;
    }
    *total = sum;
    return true;
}

const u8* payload_after(const void* data, u32 length, u32 header_size) {
    if (data == nullptr || header_size > length) {
        return nullptr;
    }
    return static_cast<const u8*>(data) + header_size;
}

}  // namespace drvlite::core

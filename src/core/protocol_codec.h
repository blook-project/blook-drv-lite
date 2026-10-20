/*
 * blook-drv-lite - application protocol validation shared by the driver and
 * the SDK. Keeping the checks here (instead of in the handlers) means the
 * host tests cover exactly the code that decides whether a peer payload is
 * safe to interpret.
 */
#ifndef BLOOK_DRVLITE_CORE_PROTOCOL_CODEC_H
#define BLOOK_DRVLITE_CORE_PROTOCOL_CODEC_H

#include <blook/drvlite/protocol.h>

#include "types.h"

namespace drvlite::core {

enum class codec_code : u32 {
    ok = 0,
    too_small,
    bad_magic,
    bad_version,
    bad_op,
    bad_size,  /* declared size is not the actual usable size */
    too_large, /* request exceeds the transport transfer limit */
};

const char* codec_code_str(codec_code code);

struct header_check {
    codec_code code = codec_code::ok;
    u32 declared_size = 0;
    u32 available = 0;
};

/* Validates a request header. `length` is the number of bytes the peer sent,
 * `min_size` the minimum structure length for the opcode (usually
 * sizeof(request struct)). On success declared_size == sizeof(request). */
header_check check_request(const void* data, u32 length, u16 expected_op,
                           u32 min_size);

/* Validates a reply header. */
header_check check_reply(const void* data, u32 length, u16 expected_op,
                         u32 min_size);

/* Sums the lengths of `count` ranges. Rejects count > DRVLITE_MAX_BATCH_RANGES,
 * zero lengths and u64 overflow. */
bool batch_total(const drvlite_range* ranges, u32 count, u64* total);

/* Returns a pointer to the byte just after `header_size` bytes, or nullptr
 * when that would point past `length`. */
const u8* payload_after(const void* data, u32 length, u32 header_size);

}  // namespace drvlite::core

#endif /* BLOOK_DRVLITE_CORE_PROTOCOL_CODEC_H */

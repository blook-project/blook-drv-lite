/*
 * blook-drv-lite - range chunking helpers.
 *
 * Large transfers are always split at 4 KiB boundaries so a physical read or
 * a non-cached physical mapping never straddles a page belonging to a
 * different allocation (and therefore a different cache attribute).
 */
#ifndef BLOOK_DRVLITE_CORE_RANGES_H
#define BLOOK_DRVLITE_CORE_RANGES_H

#include "types.h"

namespace drvlite::core {

/*
 * Returns how many bytes can be transferred from `address` in one step:
 * bounded by the end of the current 4 KiB page and by `max_chunk`
 * (0 means "no additional bound"). `remaining` is how much the caller still
 * wants; the result never exceeds it.
 */
usize page_step(u64 address, usize remaining, usize max_chunk = 0);

/* Iterates a range in page-bounded chunks of at most `max_chunk` bytes. */
class range_chunker {
   public:
    range_chunker(u64 address, u64 size, usize max_chunk = 0)
        : address_(address), remaining_(size), max_chunk_(max_chunk) {}

    bool next(u64* offset, usize* length) {
        if (remaining_ == 0) {
            return false;
        }
        const usize step =
            page_step(address_ + consumed_, remaining_, max_chunk_);
        *offset = consumed_;
        *length = step;
        consumed_ += step;
        remaining_ -= step;
        return true;
    }

    u64 size() const { return remaining_ + consumed_; }
    u64 consumed() const { return consumed_; }
    u64 remaining() const { return remaining_; }

   private:
    u64 address_;
    u64 remaining_;
    usize max_chunk_;
    u64 consumed_ = 0;
};

/* Saturating add used when validating user supplied lengths. */
inline bool checked_add(u64 lhs, u64 rhs, u64* out) {
    if (lhs > ~0ULL - rhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

}  // namespace drvlite::core

#endif /* BLOOK_DRVLITE_CORE_RANGES_H */

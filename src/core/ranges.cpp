#include "ranges.h"

namespace drvlite::core {

usize page_step(u64 address, usize remaining, usize max_chunk) {
    if (remaining == 0) {
        return 0;
    }
    const usize in_page = static_cast<usize>(kPageSize - (address & kPageMask));
    usize step = in_page < remaining ? in_page : remaining;
    if (max_chunk != 0 && step > max_chunk) {
        step = max_chunk;
    }
    return step;
}

}  // namespace drvlite::core

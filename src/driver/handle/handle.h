/*
 * blook-drv-lite - ObReference bypass: full-permission handles.
 *
 * A minimal handle is opened in the client process with the access check
 * bypassed (kernel mode open while attached), then the handle table entry's
 * GrantedAccess bitfield is rewritten to the requested access. The bitfield
 * position is discovered by matching the access value, so the code does not
 * depend on the EPROCESS.ObjectTable offset or on the entry layout.
 */
#ifndef BLOOK_DRVLITE_DRIVER_HANDLE_H
#define BLOOK_DRVLITE_DRIVER_HANDLE_H

#include "../nt.h"

namespace drvlite {

struct handle_result {
    u64 handle = 0;
    u64 object = 0;
    u32 granted = 0;
    u32 requested = 0;
};

/* type: DRVLITE_HANDLE_TYPE_PROCESS / _THREAD. */
bool handle_open(u32 pid, u32 type, u32 tid, u32 access, handle_result* out);

/* Closes a handle previously returned by handle_open (in the client process).
 */
bool handle_close(u32 pid, u64 handle);

}  // namespace drvlite

#endif /* BLOOK_DRVLITE_DRIVER_HANDLE_H */

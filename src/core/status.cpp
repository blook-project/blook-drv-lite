/*
 * blook-drv-lite - status strings shared by the driver, the SDK and the CLI.
 */
#include <blook/drvlite/protocol.h>

extern "C" const char* drvlite_status_str(int status) {
    switch (status) {
        case DRVLITE_OK:
            return "ok";
        case DRVLITE_ERR_PARAM:
            return "invalid-parameter";
        case DRVLITE_ERR_NOMEM:
            return "out-of-memory";
        case DRVLITE_ERR_MAGIC:
            return "bad-magic";
        case DRVLITE_ERR_VERSION:
            return "abi-mismatch";
        case DRVLITE_ERR_STATE:
            return "bad-state";
        case DRVLITE_ERR_AUTH:
            return "auth-failed";
        case DRVLITE_ERR_TIMEOUT:
            return "timeout";
        case DRVLITE_ERR_BUSY:
            return "busy";
        case DRVLITE_ERR_IO:
            return "io-error";
        case DRVLITE_ERR_MAP:
            return "map-error";
        case DRVLITE_ERR_NOT_FOUND:
            return "not-found";
        case DRVLITE_ERR_ALREADY:
            return "already";
        case DRVLITE_ERR_INTERNAL:
            return "internal-error";
        case DRVLITE_ERR_PEER_GONE:
            return "peer-gone";
        case DRVLITE_ERR_HANDLER:
            return "handler-error";
        case DRVLITE_ERR_UNSUPPORTED:
            return "unsupported";
        case DRVLITE_ERR_RANGE:
            return "range-error";
        case DRVLITE_ERR_ACCESS:
            return "access-denied";
        case DRVLITE_ERR_VERIFY:
            return "verification-failed";
        case DRVLITE_ERR_NOT_LOADED:
            return "driver-not-loaded";
        default:
            return "unknown";
    }
}

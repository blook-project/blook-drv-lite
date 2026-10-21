#include <gtest/gtest.h>

#include "core/protocol_codec.h"

/*
 * The status strings are part of the user-facing contract (CLI output, SDK
 * diagnostics). Every enum value must map to a distinct, non-"unknown" string
 * and arbitrary integers must fail safe.
 */
TEST(Status, EveryCodeHasAUniqueString) {
    const char* seen[32] = {};
    for (int value = DRVLITE_OK; value <= DRVLITE_ERR_NOT_LOADED; ++value) {
        const char* text = drvlite_status_str(value);
        ASSERT_NE(nullptr, text);
        EXPECT_STRNE("unknown", text) << "value " << value;
        for (int other = DRVLITE_OK; other < value; ++other) {
            if (seen[other] != nullptr) {
                EXPECT_STRNE(seen[other], text) << "value " << value;
            }
        }
        seen[value] = text;
    }
}

TEST(Status, UnknownValuesFailSafe) {
    EXPECT_STREQ("unknown", drvlite_status_str(-1));
    EXPECT_STREQ("unknown", drvlite_status_str(9999));
}

#include <gtest/gtest.h>

/* The test target deliberately keeps its own main so the suite stays a plain
 * console binary (xmake run drvlite-tests). */
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

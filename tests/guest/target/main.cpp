/*
 * drvlite-target - disposable test process used by the e2e suite.
 *
 * It allocates a page-aligned buffer, fills it with a known pattern and
 * publishes a JSON descriptor with its pid, the buffer address and the
 * current value, then keeps running until killed. The e2e driver reads and
 * writes the buffer through the kernel CR3 walk.
 *
 * Usage: drvlite-target.exe --out <json-path> [--seconds N]
 */
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr size_t kBufferSize = 0x1000;
constexpr unsigned char kPattern[] = {'D', 'R', 'V', 'L', 'I', 'T', 'E', '!'};

void write_descriptor(const char* path, uint64_t buffer, uint32_t pid,
                      uint32_t seconds) {
    FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") != 0 || file == nullptr) {
        return;
    }
    std::fprintf(file,
                 "{\n"
                 "  \"pid\": %u,\n"
                 "  \"buffer\": \"0x%llx\",\n"
                 "  \"size\": %zu,\n"
                 "  \"seconds\": %u,\n"
                 "  \"pattern\": \"DRVLITE!\"\n"
                 "}\n",
                 pid, static_cast<unsigned long long>(buffer), kBufferSize,
                 seconds);
    std::fclose(file);
}

}  // namespace

int main(int argc, char** argv) {
    const char* output_path = "drvlite-target.json";
    uint32_t seconds = 60;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--out") == 0 && index + 1 < argc) {
            output_path = argv[++index];
        } else if (std::strcmp(argv[index], "--seconds") == 0 &&
                   index + 1 < argc) {
            seconds =
                static_cast<uint32_t>(std::strtoul(argv[++index], nullptr, 0));
        }
    }

    void* buffer = VirtualAlloc(nullptr, kBufferSize, MEM_COMMIT | MEM_RESERVE,
                                PAGE_READWRITE);
    if (buffer == nullptr) {
        std::fprintf(stderr, "VirtualAlloc failed: %lu\n", GetLastError());
        return 1;
    }
    std::memset(buffer, 0x41, kBufferSize);
    std::memcpy(buffer, kPattern, sizeof(kPattern));

    const uint32_t pid = GetCurrentProcessId();
    write_descriptor(output_path, reinterpret_cast<uint64_t>(buffer), pid,
                     seconds);
    std::printf(
        "drvlite-target pid=%u buffer=0x%llx\n", pid,
        static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(buffer)));

    /* Keep the address space stable while the e2e suite works on it. */
    for (uint32_t elapsed = 0; elapsed < seconds * 10; ++elapsed) {
        Sleep(100);
    }
    return 0;
}

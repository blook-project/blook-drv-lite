/*
 * drvlite-target - disposable test process used by the e2e suite.
 *
 * - allocates a known buffer and publishes its pid/address in a JSON file,
 * - spawns a worker thread that installs hardware breakpoint DR0 on itself
 *   and then blocks in Sleep, so the debug-register cloak can be exercised,
 * - watches the buffer and records the first externally written value.
 */
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr size_t kBufferSize = 0x4000;

volatile LONG g_stop = 0;

DWORD WINAPI debug_thread(LPVOID) {
    CONTEXT context{};
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(GetCurrentThread(), &context)) {
        context.Dr0 = 0x1122334455667788ULL;
        context.Dr7 = 0x1; /* local breakpoint 0, execute, 1 byte */
        SetThreadContext(GetCurrentThread(), &context);
    }
    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
        Sleep(100);
    }
    return 0;
}

void write_descriptor(const char* path, uint64_t buffer, uint32_t pid,
                      uint32_t tid, uint32_t seconds) {
    FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") != 0 || file == nullptr) {
        return;
    }
    std::fprintf(file,
                 "{\n"
                 "  \"pid\": %u,\n"
                 "  \"tid\": %u,\n"
                 "  \"buffer\": \"0x%llx\",\n"
                 "  \"size\": %zu,\n"
                 "  \"seconds\": %u,\n"
                 "  \"pattern\": \"DRVLITE!\"\n"
                 "}\n",
                 pid, tid, static_cast<unsigned long long>(buffer), kBufferSize,
                 seconds);
    std::fclose(file);
}

}  // namespace

int main(int argc, char** argv) {
    const char* output_path = "drvlite-target.json";
    const char* changed_path = "drvlite-target-changed.txt";
    uint32_t seconds = 120;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--out") == 0 && index + 1 < argc) {
            output_path = argv[++index];
        } else if (std::strcmp(argv[index], "--changed") == 0 &&
                   index + 1 < argc) {
            changed_path = argv[++index];
        } else if (std::strcmp(argv[index], "--seconds") == 0 &&
                   index + 1 < argc) {
            seconds =
                static_cast<uint32_t>(std::strtoul(argv[++index], nullptr, 0));
        }
    }
    DeleteFileA(changed_path);

    void* buffer = VirtualAlloc(nullptr, kBufferSize, MEM_COMMIT | MEM_RESERVE,
                                PAGE_READWRITE);
    if (buffer == nullptr) {
        std::fprintf(stderr, "VirtualAlloc failed: %lu\n", GetLastError());
        return 1;
    }
    std::memset(buffer, 0x41, kBufferSize);
    std::memcpy(buffer, "DRVLITE!", 8);

    HANDLE thread =
        CreateThread(nullptr, 0, &debug_thread, nullptr, 0, nullptr);
    if (thread == nullptr) {
        std::fprintf(stderr, "CreateThread failed: %lu\n", GetLastError());
        return 1;
    }
    const uint32_t tid = GetThreadId(thread);

    const uint32_t pid = GetCurrentProcessId();
    write_descriptor(output_path, reinterpret_cast<uint64_t>(buffer), pid, tid,
                     seconds);
    std::printf(
        "drvlite-target pid=%u tid=%u buffer=0x%llx\n", pid, tid,
        static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(buffer)));

    const uint32_t initial = *static_cast<volatile uint32_t*>(buffer);
    bool change_recorded = false;
    for (uint32_t elapsed = 0; elapsed < seconds * 10; ++elapsed) {
        const uint32_t current = *static_cast<volatile uint32_t*>(buffer);
        if (!change_recorded && current != initial) {
            FILE* file = nullptr;
            if (fopen_s(&file, changed_path, "wb") == 0 && file != nullptr) {
                std::fprintf(file, "0x%x\n", current);
                std::fclose(file);
            }
            change_recorded = true;
            std::printf("drvlite-target buffer changed to 0x%x\n", current);
        }
        Sleep(100);
    }
    InterlockedExchange(&g_stop, 1);
    WaitForSingleObject(thread, 2000);
    CloseHandle(thread);
    return 0;
}

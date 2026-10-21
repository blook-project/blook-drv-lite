/*
 * drvlite-cli - command line front end for blook-drv-lite.
 *
 * The driver is manual-map-only, so every command that talks to the kernel
 * needs the two-phase load: `run` creates the kchannel session, hands the
 * bootstrap descriptor to an external mapper command, finishes the handshake
 * and then executes one subcommand in the same process.
 *
 *   drvlite-cli run --image drvlite.sys --map-cmd "kdu map --driver {image} \
 *       --bootstrap {bootstrap}" -- info
 */
#include <blook/drvlite/sdk.h>

#include <windows.h>

#include "mapper_abi.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void print_status(const char* what, int status) {
    if (status == DRVLITE_OK) {
        std::printf("%s: ok\n", what);
    } else {
        std::printf("%s: %s (%d)\n", what, drvlite::status_string(status),
                    status);
        ++g_failures;
    }
}

bool parse_u64(const char* text, uint64_t* out) {
    if (text == nullptr || out == nullptr) {
        return false;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 0);
    if (end == text) {
        return false;
    }
    *out = value;
    return true;
}

bool parse_u32(const char* text, uint32_t* out) {
    uint64_t value = 0;
    if (!parse_u64(text, &value) || value > 0xFFFFFFFFull) {
        return false;
    }
    *out = static_cast<uint32_t>(value);
    return true;
}

std::string hex_u64(uint64_t value) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%llx",
                  static_cast<unsigned long long>(value));
    return buffer;
}

bool read_whole_file(const char* path, std::vector<uint8_t>* bytes) {
    FILE* file = nullptr;
    if (fopen_s(&file, path, "rb") != 0 || file == nullptr) {
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    const long size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    bytes->resize(static_cast<size_t>(size));
    const size_t read = fread(bytes->data(), 1, bytes->size(), file);
    fclose(file);
    return read == bytes->size();
}

bool mapper_map_image(const std::vector<uint8_t>& image,
                      const kc_bootstrap& bootstrap, uint64_t* mapped_base,
                      uint32_t* entry_status) {
    HANDLE device =
        CreateFileA(DRVLITE_MAPPER_WIN32_NAME, GENERIC_READ | GENERIC_WRITE, 0,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (device == INVALID_HANDLE_VALUE) {
        std::printf("cannot open mapper device (error %lu)\n", GetLastError());
        return false;
    }
    const size_t request_size =
        sizeof(drvlite_map_request) + image.size() + sizeof(bootstrap);
    std::vector<uint8_t> request(request_size, 0);
    auto* header = reinterpret_cast<drvlite_map_request*>(request.data());
    header->image_size = static_cast<unsigned int>(image.size());
    header->bootstrap_size = sizeof(kc_bootstrap);
    header->flags = 1;
    header->image_offset = sizeof(drvlite_map_request);
    header->bootstrap_offset = sizeof(drvlite_map_request) + image.size();
    std::memcpy(request.data() + header->image_offset, image.data(),
                image.size());
    std::memcpy(request.data() + header->bootstrap_offset, &bootstrap,
                sizeof(bootstrap));
    drvlite_map_response response{};
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(
        device, static_cast<DWORD>(IOCTL_DRVLITE_MAPPER_MAP), request.data(),
        static_cast<DWORD>(request.size()), &response,
        static_cast<DWORD>(sizeof(response)), &returned, nullptr);
    CloseHandle(device);
    if (!ok) {
        std::printf("map ioctl failed (error %lu)\n", GetLastError());
        return false;
    }
    *mapped_base = response.mapped_base;
    if (entry_status != nullptr) {
        *entry_status = response.status;
    }
    return response.status == 0 && response.mapped_base != 0;
}

bool mapper_unmap_image(uint64_t base) {
    HANDLE device =
        CreateFileA(DRVLITE_MAPPER_WIN32_NAME, GENERIC_READ | GENERIC_WRITE, 0,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (device == INVALID_HANDLE_VALUE) {
        return false;
    }
    drvlite_unmap_request request{};
    request.mapped_base = base;
    drvlite_unmap_response response{};
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(
        device, static_cast<DWORD>(IOCTL_DRVLITE_MAPPER_UNMAP), &request,
        static_cast<DWORD>(sizeof(request)), &response,
        static_cast<DWORD>(sizeof(response)), &returned, nullptr);
    CloseHandle(device);
    return ok && response.status == 0;
}

/* Fast map -> handshake -> shutdown -> unmap loop used by the 1000x
 * load/unload e2e case. Avoids process spawn overhead per cycle. */
int run_cycle_test(const char* image_path, uint32_t cycles) {
    std::vector<uint8_t> image;
    if (!read_whole_file(image_path, &image)) {
        std::printf("cannot read %s\n", image_path);
        return 1;
    }
    uint32_t failures = 0;
    for (uint32_t index = 0; index < cycles; ++index) {
        drvlite::session session;
        if (session.attach() != DRVLITE_OK) {
            ++failures;
            continue;
        }
        uint64_t mapped = 0;
        if (!mapper_map_image(image, session.bootstrap(), &mapped, nullptr) ||
            session.finish(5000) != DRVLITE_OK) {
            ++failures;
            session.detach();
            continue;
        }
        drvlite::device_info info;
        if (session.query_info(&info) != DRVLITE_OK || info.driver_base == 0) {
            ++failures;
        }
        if (session.shutdown() != DRVLITE_OK) {
            ++failures;
        }
        session.detach();
        if (mapped != 0 && !mapper_unmap_image(mapped)) {
            ++failures;
        }
    }
    std::printf("cycle-test: %u cycles, %u failures\n", cycles, failures);
    return failures == 0 ? 0 : 1;
}

bool try_load_driver_service(const char* service_name, const char* path,
                             DWORD* error) {
    SC_HANDLE manager = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (manager == nullptr) {
        *error = GetLastError();
        return false;
    }
    SC_HANDLE service = CreateServiceA(
        manager, service_name, service_name, SC_MANAGER_ALL_ACCESS,
        SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, path,
        nullptr, nullptr, nullptr, nullptr, nullptr);
    if (service == nullptr) {
        *error = GetLastError();
        CloseServiceHandle(manager);
        return false;
    }
    const BOOL started = StartServiceA(service, 0, nullptr);
    *error = started ? ERROR_SUCCESS : GetLastError();
    if (started) {
        SERVICE_STATUS status{};
        ControlService(service, SERVICE_CONTROL_STOP, &status);
    }
    DeleteService(service);
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return started != FALSE;
}

/* DSE patch with a concurrent loader: a worker thread hammers
 * CreateService/StartService while the main thread keeps reading kernel
 * memory through the driver, then DSE is restored. */
int run_dse_load_test(drvlite::session& session, const char* driver_path,
                      uint32_t iterations) {
    DWORD baseline_error = ERROR_SUCCESS;
    const bool baseline = try_load_driver_service("DrvliteConcurrentBaseline",
                                                  driver_path, &baseline_error);
    drvlite::dse_status dse;
    if (session.dse_query(&dse) != DRVLITE_OK || !dse.supported) {
        std::printf("dse-load-test: DSE resolver unsupported\n");
        return 1;
    }
    if (session.dse_set(0) != DRVLITE_OK) {
        std::printf("dse-load-test: dse_set failed\n");
        return 1;
    }
    volatile LONG loader_failures = 0;
    volatile LONG loader_successes = 0;
    std::thread loader([&] {
        for (uint32_t index = 0; index < iterations; ++index) {
            char name[64] = {};
            std::snprintf(name, sizeof(name), "DrvliteConcurrent%u", index % 8);
            DWORD error = ERROR_SUCCESS;
            if (try_load_driver_service(name, driver_path, &error)) {
                InterlockedIncrement(&loader_successes);
            } else {
                InterlockedIncrement(&loader_failures);
            }
        }
    });
    uint32_t memory_failures = 0;
    drvlite::device_info info;
    session.query_info(&info);
    while (loader_failures + loader_successes < static_cast<LONG>(iterations)) {
        uint8_t bytes[16] = {};
        if (info.kernel_base == 0 ||
            session.read_kernel(info.kernel_base, bytes, sizeof(bytes)) !=
                DRVLITE_OK) {
            ++memory_failures;
        }
    }
    loader.join();
    const int restore = session.dse_restore();
    std::printf(
        "dse-load-test: baseline=%s after-patch=%ld ok/%ld failed, "
        "memory_failures=%u restore=%s\n",
        baseline ? "loaded(unexpected)" : "blocked", loader_successes,
        loader_failures, memory_failures,
        restore == DRVLITE_OK ? "ok" : "failed");
    return (baseline == false &&
            loader_successes == static_cast<LONG>(iterations) &&
            memory_failures == 0 && restore == DRVLITE_OK)
               ? 0
               : 1;
}

int usage() {
    std::printf(
        "drvlite-cli %s - blook-drv-lite command line\n\n"
        "  run --image <sys> --map-cmd <template> [--region N] -- <command>\n"
        "      attach, invoke the mapper (placeholders {image} {bootstrap}),\n"
        "      finish the handshake and run one command in this process\n\n"
        "  commands:\n"
        "    info\n"
        "    read-phys  <addr> <size>\n"
        "    write-phys <addr> <hex-bytes>\n"
        "    read-kernel  <addr> <size>\n"
        "    write-kernel <addr> <hex-bytes>\n"
        "    read-proc  <pid> <addr> <size>\n"
        "    write-proc <pid> <addr> <hex-bytes>\n"
        "    process <pid>\n"
        "    modules [pid]\n"
        "    dump <pid> <addr> <size> <file>\n"
        "    dse [--disable|--restore]\n"
        "    handle --pid <pid> [--full]\n"
        "    hook install|status|remove|hide --pid <pid>|shield --pid <pid> "
        "--ssn <n>\n"
        "    memtest <pid> <buffer>\n"
        "    hook-test [ssn...] | hook-shutdown-test\n"
        "    dse-load-test <unsigned.sys> [iterations]\n"
        "    shutdown\n"
        "    version\n\n"
        "  Every command has a --timeout-ms option (default 5000).\n",
        drvlite::version_string());
    return 1;
}

std::vector<uint8_t> parse_hex(const char* text) {
    std::vector<uint8_t> bytes;
    if (text == nullptr) {
        return bytes;
    }
    int high = -1;
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        const char value = *cursor;
        if (value == ' ' || value == '-' || value == ':') {
            continue;
        }
        int digit = -1;
        if (value >= '0' && value <= '9') {
            digit = value - '0';
        } else if (value >= 'a' && value <= 'f') {
            digit = value - 'a' + 10;
        } else if (value >= 'A' && value <= 'F') {
            digit = value - 'A' + 10;
        } else {
            return {};
        }
        if (high < 0) {
            high = digit;
        } else {
            bytes.push_back(static_cast<uint8_t>((high << 4) | digit));
            high = -1;
        }
    }
    if (high >= 0) {
        bytes.clear();
    }
    return bytes;
}

/* Accepts the byte list either as one argument ("43 52 33") or as several
 * shell-split tokens ("43" "52" "33"), which is how batch files pass it. */
std::vector<uint8_t> parse_hex_args(int argc, char** argv, int start) {
    std::string joined;
    for (int index = start; index < argc; ++index) {
        if (!joined.empty()) {
            joined.push_back(' ');
        }
        joined += argv[index];
    }
    return parse_hex(joined.c_str());
}

void dump_bytes(const uint8_t* data, uint32_t size) {
    for (uint32_t index = 0; index < size; ++index) {
        if (index % 16 == 0) {
            std::printf("\n%08x  ", index);
        }
        std::printf("%02x ", data[index]);
    }
    std::printf("\n");
}

int run_command(drvlite::session& session, int argc, char** argv,
                uint32_t timeout_ms) {
    if (argc == 0) {
        return usage();
    }
    const std::string command = argv[0];
    bool json = false;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--json") == 0) {
            json = true;
        }
    }
    if (command == "info") {
        drvlite::device_info info;
        const int status = session.query_info(&info);
        if (status != DRVLITE_OK) {
            print_status("query_info", status);
            return 1;
        }
        if (json) {
            std::printf(
                "{\"nt_build\":%u,\"abi\":\"%u.%u\",\"features\":%u,"
                "\"driver_base\":\"%s\",\"driver_size\":%u,"
                "\"kernel_base\":\"%s\",\"kernel_size\":%u,"
                "\"client_pid\":%u,\"client_eprocess\":\"%s\"}\n",
                info.nt_build, info.abi >> 16, info.abi & 0xFFFF, info.features,
                hex_u64(info.driver_base).c_str(), info.driver_size,
                hex_u64(info.kernel_base).c_str(), info.kernel_size,
                info.client_pid, hex_u64(info.client_eprocess).c_str());
            return 0;
        }
        std::printf("nt build       : %u\n", info.nt_build);
        std::printf("abi            : %u.%u\n", info.abi >> 16,
                    info.abi & 0xFFFF);
        std::printf("features       : 0x%08x\n", info.features);
        std::printf("driver base    : %s (%u bytes)\n",
                    hex_u64(info.driver_base).c_str(), info.driver_size);
        std::printf("kernel base    : %s (%u bytes)\n",
                    hex_u64(info.kernel_base).c_str(), info.kernel_size);
        std::printf("client pid     : %u\n", info.client_pid);
        std::printf("client eprocess: %s\n",
                    hex_u64(info.client_eprocess).c_str());
        return 0;
    }
    if (command == "read-phys" || command == "read-kernel") {
        if (argc != 3) {
            return usage();
        }
        uint64_t address = 0;
        uint32_t size = 0;
        if (!parse_u64(argv[1], &address) || !parse_u32(argv[2], &size) ||
            size == 0) {
            std::printf("invalid address/size\n");
            return 1;
        }
        std::vector<uint8_t> buffer(size);
        const int status =
            command == "read-phys"
                ? session.read_physical(address, buffer.data(), size)
                : session.read_kernel(address, buffer.data(), size);
        if (status != DRVLITE_OK) {
            print_status("read", status);
            return 1;
        }
        dump_bytes(buffer.data(), size);
        return 0;
    }
    if (command == "write-phys" || command == "write-kernel") {
        if (argc < 3) {
            return usage();
        }
        uint64_t address = 0;
        if (!parse_u64(argv[1], &address)) {
            std::printf("invalid address\n");
            return 1;
        }
        const std::vector<uint8_t> bytes = parse_hex_args(argc, argv, 2);
        if (bytes.empty()) {
            std::printf("invalid or empty byte list\n");
            return 1;
        }
        const int status =
            command == "write-phys"
                ? session.write_physical(address, bytes.data(),
                                         static_cast<uint32_t>(bytes.size()))
                : session.write_kernel(address, bytes.data(),
                                       static_cast<uint32_t>(bytes.size()));
        print_status("write", status);
        return status == DRVLITE_OK ? 0 : 1;
    }
    if (command == "read-proc") {
        if (argc != 4) {
            return usage();
        }
        uint32_t pid = 0;
        uint64_t address = 0;
        uint32_t size = 0;
        if (!parse_u32(argv[1], &pid) || !parse_u64(argv[2], &address) ||
            !parse_u32(argv[3], &size) || size == 0) {
            std::printf("invalid arguments\n");
            return 1;
        }
        std::vector<uint8_t> buffer(size);
        const int status =
            session.read_process(pid, address, buffer.data(), size);
        print_status("read_process", status);
        if (status == DRVLITE_OK) {
            dump_bytes(buffer.data(), size);
        }
        return status == DRVLITE_OK ? 0 : 1;
    }
    if (command == "write-proc") {
        if (argc < 4) {
            return usage();
        }
        uint32_t pid = 0;
        uint64_t address = 0;
        if (!parse_u32(argv[1], &pid) || !parse_u64(argv[2], &address)) {
            std::printf("invalid arguments\n");
            return 1;
        }
        const std::vector<uint8_t> bytes = parse_hex_args(argc, argv, 3);
        if (bytes.empty()) {
            std::printf("invalid or empty byte list\n");
            return 1;
        }
        const int status = session.write_process(
            pid, address, bytes.data(), static_cast<uint32_t>(bytes.size()));
        print_status("write_process", status);
        return status == DRVLITE_OK ? 0 : 1;
    }
    if (command == "process") {
        if (argc != 2) {
            return usage();
        }
        uint32_t pid = 0;
        if (!parse_u32(argv[1], &pid)) {
            return 1;
        }
        drvlite::process_details info;
        const int status = session.process_info(pid, &info);
        if (status != DRVLITE_OK) {
            print_status("process_info", status);
            return 1;
        }
        std::printf("pid       : %u\n", info.pid);
        std::printf("eprocess  : %s\n", hex_u64(info.eprocess).c_str());
        std::printf("cr3       : %s\n", hex_u64(info.cr3).c_str());
        std::printf("peb       : %s\n", hex_u64(info.peb).c_str());
        std::printf("image base: %s\n", hex_u64(info.image_base).c_str());
        std::printf("image size: %u\n", info.image_size);
        std::printf("validated : %s\n", info.cr3_validated ? "yes" : "no");
        return info.cr3_validated ? 0 : 2;
    }
    if (command == "modules") {
        const uint32_t pid =
            argc >= 2 ? static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 0))
                      : 0;
        std::vector<drvlite_module_info> modules;
        const int status = session.modules(pid, &modules);
        if (status != DRVLITE_OK) {
            print_status("modules", status);
            return 1;
        }
        if (json) {
            std::printf("[");
            for (size_t index = 0; index < modules.size(); ++index) {
                const drvlite_module_info& module = modules[index];
                std::printf(
                    "%s{\"base\":\"%s\",\"size\":%llu,"
                    "\"name\":\"%s\"}",
                    index == 0 ? "" : ",", hex_u64(module.base).c_str(),
                    static_cast<unsigned long long>(module.size), module.name);
            }
            std::printf("]\n");
            return 0;
        }
        for (const drvlite_module_info& module : modules) {
            std::printf("%s  %-12s %8llu  %s\n", hex_u64(module.base).c_str(),
                        module.name,
                        static_cast<unsigned long long>(module.size),
                        module.name);
        }
        std::printf("total: %zu\n", modules.size());
        return 0;
    }
    if (command == "dump") {
        if (argc != 5) {
            return usage();
        }
        uint32_t pid = 0;
        uint64_t address = 0;
        uint32_t size = 0;
        if (!parse_u32(argv[1], &pid) || !parse_u64(argv[2], &address) ||
            !parse_u32(argv[3], &size)) {
            return 1;
        }
        std::vector<uint8_t> data;
        uint32_t copied = 0;
        const int status =
            session.module_dump(pid, address, size, &data, &copied);
        if (status != DRVLITE_OK) {
            print_status("module_dump", status);
            return 1;
        }
        FILE* file = nullptr;
        if (fopen_s(&file, argv[4], "wb") != 0 || file == nullptr) {
            std::printf("cannot open %s\n", argv[4]);
            return 1;
        }
        fwrite(data.data(), 1, copied, file);
        fclose(file);
        std::printf("wrote %u bytes to %s\n", copied, argv[4]);
        return 0;
    }
    if (command == "dse") {
        if (argc >= 2 && std::strcmp(argv[1], "--disable") == 0) {
            const int status = session.dse_set(0);
            print_status("dse_disable", status);
            return status == DRVLITE_OK ? 0 : 1;
        }
        if (argc >= 2 && std::strcmp(argv[1], "--restore") == 0) {
            const int status = session.dse_restore();
            print_status("dse_restore", status);
            return status == DRVLITE_OK ? 0 : 1;
        }
        drvlite::dse_status dse;
        const int status = session.dse_query(&dse);
        if (status != DRVLITE_OK) {
            print_status("dse_query", status);
            return 1;
        }
        if (json) {
            std::printf(
                "{\"supported\":%s,\"patched\":%s,\"address\":\"%s\","
                "\"original\":%u,\"current\":%u,\"ci_base\":\"%s\","
                "\"ci_size\":%u,\"via\":%u,\"error\":%u}\n",
                dse.supported ? "true" : "false",
                dse.patched ? "true" : "false", hex_u64(dse.address).c_str(),
                dse.original, dse.current, hex_u64(dse.ci_base).c_str(),
                dse.ci_size, dse.via, dse.error);
            return dse.supported ? 0 : 2;
        }
        std::printf("supported : %s\n", dse.supported ? "yes" : "no");
        std::printf("patched   : %s\n", dse.patched ? "yes" : "no");
        std::printf("address   : %s\n", hex_u64(dse.address).c_str());
        std::printf("original  : 0x%x\n", dse.original);
        std::printf("current   : 0x%x\n", dse.current);
        std::printf("ci.dll    : %s (%u bytes, ref rva 0x%x)\n",
                    hex_u64(dse.ci_base).c_str(), dse.ci_size,
                    dse.via != 0 ? dse.via : 0);
        return dse.supported ? 0 : 2;
    }
    if (command == "handle") {
        uint32_t pid = 0;
        uint32_t access = DRVLITE_ACCESS_MINIMAL;
        bool prove_with_terminate = false;
        for (int index = 1; index < argc; ++index) {
            if (std::strcmp(argv[index], "--pid") == 0 && index + 1 < argc) {
                if (!parse_u32(argv[++index], &pid)) {
                    return 1;
                }
            } else if (std::strcmp(argv[index], "--full") == 0) {
                access = DRVLITE_ACCESS_FULL;
            } else if (std::strcmp(argv[index], "--kill") == 0) {
                prove_with_terminate = true;
            }
        }
        if (pid == 0) {
            return usage();
        }
        HANDLE handle = nullptr;
        uint32_t granted = 0;
        const int status =
            session.open_process_handle(pid, access, &handle, &granted);
        if (status != DRVLITE_OK) {
            print_status("handle", status);
            return 1;
        }
        std::printf("handle   : 0x%" PRIx64 "\n",
                    static_cast<uint64_t>(reinterpret_cast<ULONG_PTR>(handle)));
        std::printf("granted  : 0x%x\n", granted);
        /* Prove the access rewrite with an operation that requires full
         * access. Destructive, so only when explicitly requested. */
        if (prove_with_terminate) {
            const BOOL terminated = TerminateProcess(handle, 0);
            std::printf("terminate: %s (error %lu)\n",
                        terminated ? "yes" : "no", GetLastError());
        }
        return 0;
    }
    if (command == "hook") {
        if (argc < 2) {
            return usage();
        }
        const std::string action = argv[1];
        if (action == "install") {
            std::vector<uint32_t> filter;
            for (int index = 2; index < argc; ++index) {
                uint32_t ssn = 0;
                if (parse_u32(argv[index], &ssn)) {
                    filter.push_back(ssn);
                }
            }
            print_status("hook_install", session.infinity_install(filter));
            return g_failures == 0 ? 0 : 1;
        }
        if (action == "remove") {
            print_status("hook_remove", session.infinity_remove());
            return g_failures == 0 ? 0 : 1;
        }
        if (action == "hide") {
            uint32_t pid = 0;
            for (int index = 2; index < argc; ++index) {
                if (std::strcmp(argv[index], "--pid") == 0 &&
                    index + 1 < argc) {
                    parse_u32(argv[++index], &pid);
                }
            }
            print_status("hide", session.hide_debug_registers(true, pid));
            return g_failures == 0 ? 0 : 1;
        }
        if (action == "shield") {
            uint32_t pid = 0;
            std::vector<uint32_t> ssns;
            for (int index = 2; index < argc; ++index) {
                if (std::strcmp(argv[index], "--pid") == 0 &&
                    index + 1 < argc) {
                    parse_u32(argv[++index], &pid);
                } else if (std::strcmp(argv[index], "--ssn") == 0 &&
                           index + 1 < argc) {
                    uint32_t ssn = 0;
                    if (parse_u32(argv[++index], &ssn)) {
                        ssns.push_back(ssn);
                    }
                }
            }
            print_status("shield", session.set_syscall_shield(true, pid, ssns));
            return g_failures == 0 ? 0 : 1;
        }
        drvlite::hook_status status{};
        const int result = session.infinity_status(&status);
        if (result != DRVLITE_OK) {
            print_status("infinity_status", result);
            return 1;
        }
        std::printf("installed   : %s\n", status.installed ? "yes" : "no");
        std::printf("supported   : %s\n", status.supported ? "yes" : "no");
        std::printf("entry       : %s\n", hex_u64(status.entry).c_str());
        std::printf("dispatch    : %s\n", hex_u64(status.dispatch).c_str());
        std::printf("syscalls    : %" PRIu64 "\n", status.total_syscalls);
        std::printf("filter hits : %" PRIu64 "\n", status.filtered_hits);
        std::printf("handled     : %" PRIu64 "\n", status.handled_calls);
        std::printf("spliced     : %u bytes\n", status.spliced_bytes);
        std::printf("error       : 0x%x\n", status.flags);
        return status.installed ? 0 : 0;
    }
    if (command == "phys") {
        drvlite::device_info info;
        const int info_status = session.query_info(&info);
        if (info_status != DRVLITE_OK || info.test_page_pa == 0) {
            print_status("phys", info_status != DRVLITE_OK
                                     ? info_status
                                     : DRVLITE_ERR_NOT_FOUND);
            return 1;
        }
        uint8_t header[16] = {};
        int status =
            session.read_physical(info.test_page_pa, header, sizeof(header));
        if (status != DRVLITE_OK ||
            std::memcmp(header, "DRVLITE-PHYS", 12) != 0) {
            print_status("phys_read", status);
            return 1;
        }
        std::printf("diagnostic page: va=%s pa=%s magic=%.12s\n",
                    hex_u64(info.test_page_va).c_str(),
                    hex_u64(info.test_page_pa).c_str(),
                    reinterpret_cast<const char*>(header));
        const uint64_t probe_pa = info.test_page_pa + 0x400;
        uint32_t original = 0;
        status = session.read_physical(probe_pa, &original, sizeof(original));
        if (status != DRVLITE_OK) {
            print_status("phys_read_probe", status);
            return 1;
        }
        const uint32_t canary = 0xDEADBEEFu;
        status = session.write_physical(probe_pa, &canary, sizeof(canary));
        if (status != DRVLITE_OK) {
            print_status("phys_write", status);
            return 1;
        }
        uint32_t observed = 0;
        status = session.read_physical(probe_pa, &observed, sizeof(observed));
        if (status != DRVLITE_OK || observed != canary) {
            print_status("phys_verify",
                         status != DRVLITE_OK ? status : DRVLITE_ERR_VERIFY);
            return 1;
        }
        session.write_physical(probe_pa, &original, sizeof(original));
        std::printf(
            "physical read/write roundtrip: ok (0x%x -> 0x%x -> 0x%x)\n",
            original, canary, observed);
        return 0;
    }
    if (command == "ssn") {
        if (argc != 2) {
            return usage();
        }
        const uint32_t ssn = drvlite::ntdll_syscall_number(argv[1]);
        if (ssn == 0) {
            std::printf("syscall %s not found\n", argv[1]);
            return 1;
        }
        std::printf("%s ssn=%u\n", argv[1], ssn);
        return 0;
    }
    if (command == "thread-dr") {
        if (argc != 3) {
            return usage();
        }
        uint32_t pid = 0;
        uint32_t tid = 0;
        if (!parse_u32(argv[1], &pid) || !parse_u32(argv[2], &tid)) {
            return 1;
        }
        HANDLE thread = nullptr;
        uint32_t granted = 0;
        const int status = session.open_thread_handle(
            pid, tid, DRVLITE_ACCESS_FULL, &thread, &granted);
        if (status != DRVLITE_OK) {
            print_status("open_thread", status);
            return 1;
        }
        CONTEXT context{};
        context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (!GetThreadContext(thread, &context)) {
            std::printf("GetThreadContext failed: %lu\n", GetLastError());
            session.close_handle(thread, pid);
            return 1;
        }
        std::printf(
            "granted=0x%x dr0=0x%llx dr1=0x%llx dr2=0x%llx "
            "dr3=0x%llx dr6=0x%llx dr7=0x%llx\n",
            granted, static_cast<unsigned long long>(context.Dr0),
            static_cast<unsigned long long>(context.Dr1),
            static_cast<unsigned long long>(context.Dr2),
            static_cast<unsigned long long>(context.Dr3),
            static_cast<unsigned long long>(context.Dr6),
            static_cast<unsigned long long>(context.Dr7));
        session.close_handle(thread, pid);
        return 0;
    }
    if (command == "stress") {
        if (argc != 4) {
            return usage();
        }
        uint32_t pid = 0;
        uint64_t address = 0;
        uint32_t iterations = 0;
        if (!parse_u32(argv[1], &pid) || !parse_u64(argv[2], &address) ||
            !parse_u32(argv[3], &iterations)) {
            return 1;
        }
        uint32_t failures = 0;
        drvlite::device_info info;
        session.query_info(&info);
        for (uint32_t index = 0; index < iterations; ++index) {
            uint32_t value = index;
            if (session.write_process(pid, address, &value, sizeof(value)) !=
                DRVLITE_OK) {
                ++failures;
                continue;
            }
            uint32_t observed = 0;
            if (session.read_process(pid, address, &observed,
                                     sizeof(observed)) != DRVLITE_OK ||
                observed != value) {
                ++failures;
            }
            uint8_t header[4] = {};
            if (info.kernel_base != 0 &&
                session.read_kernel(info.kernel_base, header, sizeof(header)) !=
                    DRVLITE_OK) {
                ++failures;
            }
            if (info.test_page_pa != 0) {
                uint32_t canary = 0;
                if (session.read_physical(info.test_page_pa, &canary,
                                          sizeof(canary)) != DRVLITE_OK) {
                    ++failures;
                }
            }
        }
        std::printf("stress: %u iterations, %u failures\n", iterations,
                    failures);
        return failures == 0 ? 0 : 1;
    }
    if (command == "hook-test") {
        std::vector<uint32_t> filter;
        for (int index = 1; index < argc; ++index) {
            uint32_t ssn = 0;
            if (parse_u32(argv[index], &ssn)) {
                filter.push_back(ssn);
            }
        }
        const int install_status = session.infinity_install(filter);
        if (install_status != DRVLITE_OK) {
            print_status("hook_install", install_status);
            return 1;
        }
        drvlite::hook_status before;
        session.infinity_status(&before);
        for (int iteration = 0; iteration < 2000; ++iteration) {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                         FALSE, GetCurrentProcessId());
            if (process != nullptr) {
                DWORD exit_code = 0;
                GetExitCodeProcess(process, &exit_code);
                CloseHandle(process);
            }
        }
        drvlite::hook_status after;
        session.infinity_status(&after);
        const int remove_status = session.infinity_remove();
        std::printf(
            "syscalls before=%llu after=%llu filter_hits=%llu "
            "installed=%d removed=%d\n",
            static_cast<unsigned long long>(before.total_syscalls),
            static_cast<unsigned long long>(after.total_syscalls),
            static_cast<unsigned long long>(after.filtered_hits),
            after.installed ? 1 : 0, remove_status);
        if (after.total_syscalls <= before.total_syscalls) {
            std::printf("hook-test: no syscalls observed\n");
            return 1;
        }
        return remove_status == DRVLITE_OK ? 0 : 1;
    }
    if (command == "hwbp-test") {
        if (argc != 3) {
            return usage();
        }
        uint32_t pid = 0;
        uint32_t tid = 0;
        if (!parse_u32(argv[1], &pid) || !parse_u32(argv[2], &tid)) {
            return 1;
        }
        auto print_context = [&](const char* label) -> bool {
            HANDLE thread = nullptr;
            uint32_t granted = 0;
            if (session.open_thread_handle(pid, tid, DRVLITE_ACCESS_FULL,
                                           &thread, &granted) != DRVLITE_OK) {
                std::printf("%s: open_thread failed\n", label);
                return false;
            }
            CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            /* Suspend first: Windows then reports the saved context,
             * including the debug registers, deterministically. */
            const DWORD suspended = SuspendThread(thread);
            const BOOL ok = GetThreadContext(thread, &context);
            if (suspended != static_cast<DWORD>(-1)) {
                ResumeThread(thread);
            }
            std::printf("%s: dr0=0x%llx dr7=0x%llx ok=%d\n", label,
                        static_cast<unsigned long long>(context.Dr0),
                        static_cast<unsigned long long>(context.Dr7), ok);
            session.close_handle(thread, pid);
            return ok != 0;
        };
        const bool before = print_context("before");
        const int hide_status = session.hide_debug_registers(true, pid);
        const bool hidden = print_context("hidden");
        session.hide_debug_registers(false, pid);
        const bool restored = print_context("restored");
        std::printf("hwbp-test: hide=%d before=%d hidden=%d restored=%d\n",
                    hide_status, before, hidden, restored);
        return hide_status == DRVLITE_OK && before && hidden && restored ? 0
                                                                         : 1;
    }
    if (command == "bench") {
        uint32_t iterations = 500;
        uint32_t pid = GetCurrentProcessId();
        uint64_t address = 0;
        for (int index = 1; index < argc; ++index) {
            if (std::strcmp(argv[index], "--iterations") == 0 &&
                index + 1 < argc) {
                parse_u32(argv[++index], &iterations);
            } else if (std::strcmp(argv[index], "--pid") == 0 &&
                       index + 1 < argc) {
                parse_u32(argv[++index], &pid);
            } else if (std::strcmp(argv[index], "--addr") == 0 &&
                       index + 1 < argc) {
                parse_u64(argv[++index], &address);
            }
        }
        if (iterations == 0) {
            iterations = 500;
        }
        drvlite::device_info info;
        if (session.query_info(&info) != DRVLITE_OK) {
            print_status("bench", DRVLITE_ERR_STATE);
            return 1;
        }
        void* local = VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_READWRITE);
        if (local == nullptr) {
            std::printf("VirtualAlloc failed\n");
            return 1;
        }
        /* Touch the whole range so the CR3 walk sees present pages. */
        std::memset(local, 0x41, 0x2000);
        if (address == 0) {
            address = reinterpret_cast<uint64_t>(local);
        }
        std::vector<uint8_t> buffer(0x1000, 0x41);
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        auto measure = [&](const char* name, auto&& operation) {
            std::vector<double> samples;
            samples.reserve(iterations);
            uint32_t failures = 0;
            for (uint32_t index = 0; index < iterations; ++index) {
                LARGE_INTEGER start{};
                LARGE_INTEGER end{};
                QueryPerformanceCounter(&start);
                const int status = operation();
                QueryPerformanceCounter(&end);
                if (status != DRVLITE_OK) {
                    ++failures;
                }
                const double micros =
                    static_cast<double>(end.QuadPart - start.QuadPart) * 1e6 /
                    static_cast<double>(frequency.QuadPart);
                samples.push_back(micros);
            }
            std::sort(samples.begin(), samples.end());
            const double median = samples[samples.size() / 2];
            const double p99 = samples[static_cast<size_t>(
                static_cast<double>(samples.size()) * 0.99)];
            const double megabytes_per_second =
                median > 0.0 ? (4096.0 / (median / 1e6)) / (1024.0 * 1024.0)
                             : 0.0;
            std::printf(
                "%-24s median=%8.2f us  p99=%8.2f us  %8.1f MiB/s  "
                "failures=%u\n",
                name, median, p99, megabytes_per_second, failures);
            return failures;
        };
        uint32_t failures = 0;
        if (info.test_page_pa != 0) {
            failures += measure("phys-read-4k", [&] {
                return session.read_physical(info.test_page_pa, buffer.data(),
                                             4096);
            });
        }
        failures += measure("kernel-read-4k", [&] {
            return session.read_kernel(info.kernel_base, buffer.data(), 4096);
        });
        failures += measure("process-read-4k", [&] {
            return session.read_process(pid, address, buffer.data(), 4096);
        });
        failures += measure("process-write-4k", [&] {
            return session.write_process(pid, address, buffer.data(), 4096);
        });
        if (info.test_page_pa != 0) {
            failures += measure("phys-write-4k", [&] {
                return session.write_physical(info.test_page_pa, buffer.data(),
                                              4096);
            });
            /* Restore the diagnostic magic after the write benchmark. */
            session.write_physical(info.test_page_pa, "DRVLITE-PHYS", 12);
        }
        /* Syscall-entry interception overhead: same user loop with the
         * trampoline removed and installed. */
        auto measure_syscalls = [&]() -> double {
            constexpr int kCalls = 20000;
            LARGE_INTEGER start{};
            LARGE_INTEGER end{};
            QueryPerformanceCounter(&start);
            volatile DWORD pid_value = 0;
            for (int index = 0; index < kCalls; ++index) {
                pid_value = GetCurrentProcessId();
            }
            QueryPerformanceCounter(&end);
            (void)pid_value;
            return static_cast<double>(end.QuadPart - start.QuadPart) * 1e9 /
                   static_cast<double>(frequency.QuadPart) / kCalls;
        };
        const double baseline_syscall_ns = measure_syscalls();
        if (session.infinity_install({}) == DRVLITE_OK) {
            const double hooked_syscall_ns = measure_syscalls();
            session.infinity_remove();
            std::printf(
                "syscall-overhead        baseline=%8.1f ns  hooked=%8.1f ns  "
                "delta=%8.1f ns/call\n",
                baseline_syscall_ns, hooked_syscall_ns,
                hooked_syscall_ns - baseline_syscall_ns);
        } else {
            std::printf("syscall-overhead        unsupported on this build\n");
        }
        VirtualFree(local, 0, MEM_RELEASE);
        std::printf("bench: %u iterations, %u failures\n", iterations,
                    failures);
        return failures == 0 ? 0 : 1;
    }
    if (command == "fuzz") {
        auto make_header = [](uint16_t opcode, uint32_t size) {
            drvlite_req_header header{};
            header.magic = DRVLITE_REQ_MAGIC;
            header.abi_major = DRVLITE_ABI_MAJOR;
            header.abi_minor = DRVLITE_ABI_MINOR;
            header.op = opcode;
            header.size = size;
            return header;
        };
        auto expect_error = [&](const char* name, uint16_t opcode,
                                const void* input, uint32_t length) {
            uint8_t output[64] = {};
            uint32_t output_length = 0;
            const int status =
                session.call(opcode, input, length, output, sizeof(output),
                             &output_length, 2000);
            const bool rejected = status != DRVLITE_OK;
            std::printf("%-30s %-24s %s\n", name,
                        drvlite::status_string(status),
                        rejected ? "rejected" : "ACCEPTED");
            return rejected;
        };
        bool ok = true;
        uint8_t bad_magic[64] = {};
        ok &= expect_error("unknown-opcode", 0x8FFF, bad_magic,
                           sizeof(bad_magic));
        ok &= expect_error("bad-request-magic", DRVLITE_OP_QUERY_INFO,
                           bad_magic, sizeof(drvlite_query_info_req));
        drvlite_query_info_req wrong_size{};
        wrong_size.hdr =
            make_header(DRVLITE_OP_QUERY_INFO, sizeof(wrong_size) - 4);
        ok &= expect_error("query-info-wrong-size", DRVLITE_OP_QUERY_INFO,
                           &wrong_size, sizeof(wrong_size));
        drvlite_mem_req zero_read{};
        zero_read.hdr =
            make_header(DRVLITE_OP_MEM_KERNEL_READ, sizeof(zero_read));
        zero_read.address = 0xFFFFF80000000000ULL;
        zero_read.size = 0;
        ok &= expect_error("kernel-read-zero-size", DRVLITE_OP_MEM_KERNEL_READ,
                           &zero_read, sizeof(zero_read));
        drvlite_mem_req huge_read{};
        huge_read.hdr =
            make_header(DRVLITE_OP_MEM_KERNEL_READ, sizeof(huge_read));
        huge_read.address = 0xFFFFF80000000000ULL;
        huge_read.size = DRVLITE_MAX_TRANSFER + 1;
        ok &= expect_error("kernel-read-oversize", DRVLITE_OP_MEM_KERNEL_READ,
                           &huge_read, sizeof(huge_read));
        drvlite_process_mem_req bad_process{};
        bad_process.hdr =
            make_header(DRVLITE_OP_MEM_PROCESS_READ, sizeof(bad_process));
        bad_process.pid = 0xFFFFFFFF;
        bad_process.address = 0x7FF000000000ULL;
        bad_process.size = 16;
        ok &= expect_error("process-read-bad-pid", DRVLITE_OP_MEM_PROCESS_READ,
                           &bad_process, sizeof(bad_process));
        bad_process.pid = GetCurrentProcessId();
        bad_process.address = 0x0000800000000000ULL; /* non-canonical */
        ok &= expect_error("process-read-noncanonical",
                           DRVLITE_OP_MEM_PROCESS_READ, &bad_process,
                           sizeof(bad_process));
        drvlite_batch_read_req empty_batch{};
        empty_batch.hdr =
            make_header(DRVLITE_OP_MEM_BATCH_READ, sizeof(empty_batch));
        empty_batch.pid = GetCurrentProcessId();
        empty_batch.count = 0;
        ok &= expect_error("batch-read-empty", DRVLITE_OP_MEM_BATCH_READ,
                           &empty_batch, sizeof(empty_batch));
        empty_batch.count = DRVLITE_MAX_BATCH_RANGES + 1;
        ok &= expect_error("batch-read-too-many", DRVLITE_OP_MEM_BATCH_READ,
                           &empty_batch, sizeof(empty_batch));
        drvlite_handle_open_req bad_handle{};
        bad_handle.hdr =
            make_header(DRVLITE_OP_HANDLE_OPEN, sizeof(bad_handle));
        bad_handle.pid = 0xFFFFFFFF;
        bad_handle.access = DRVLITE_ACCESS_FULL;
        ok &= expect_error("handle-open-bad-pid", DRVLITE_OP_HANDLE_OPEN,
                           &bad_handle, sizeof(bad_handle));
        drvlite_infinity_install_req bad_hook{};
        bad_hook.hdr =
            make_header(DRVLITE_OP_INFINITY_INSTALL, sizeof(bad_hook));
        bad_hook.filter_count = DRVLITE_MAX_SSN_FILTER + 1;
        ok &= expect_error("infinity-install-bad-count",
                           DRVLITE_OP_INFINITY_INSTALL, &bad_hook,
                           sizeof(bad_hook));
        drvlite_module_dump_req bad_dump{};
        bad_dump.hdr = make_header(DRVLITE_OP_MODULE_DUMP, sizeof(bad_dump));
        bad_dump.size = DRVLITE_MAX_TRANSFER + 1;
        ok &= expect_error("module-dump-oversize", DRVLITE_OP_MODULE_DUMP,
                           &bad_dump, sizeof(bad_dump));
        drvlite::device_info info;
        const bool alive = session.query_info(&info) == DRVLITE_OK;
        std::printf("fuzz: rejected=%d driver-alive=%d\n", ok ? 1 : 0,
                    alive ? 1 : 0);
        return ok && alive ? 0 : 1;
    }
    if (command == "memtest") {
        if (argc != 3) {
            return usage();
        }
        uint32_t pid = 0;
        uint64_t address = 0;
        if (!parse_u32(argv[1], &pid) || !parse_u64(argv[2], &address)) {
            return 1;
        }
        drvlite::device_info info;
        if (session.query_info(&info) != DRVLITE_OK) {
            return 1;
        }
        uint32_t failures = 0;
        uint8_t bytes[16] = {};
        /* 4 KiB page crossing (the target buffer is 0x4000 bytes). */
        if (session.read_process(pid, address + 0xFF8, bytes, sizeof(bytes)) !=
            DRVLITE_OK) {
            std::printf("memtest: 4k-crossing read failed\n");
            ++failures;
        }
        /* Valid write/read inside the second page. */
        const uint32_t canary = 0xCAFEBABE;
        if (session.write_process(pid, address + 0x1FF8, &canary,
                                  sizeof(canary)) != DRVLITE_OK) {
            ++failures;
        }
        uint32_t observed = 0;
        if (session.read_process(pid, address + 0x1FF8, &observed,
                                 sizeof(observed)) != DRVLITE_OK ||
            observed != canary) {
            ++failures;
        }
        /* Non-present page must be an error, not a fault/BSOD. */
        if (session.read_process(pid, address + 0x100000, bytes,
                                 sizeof(bytes)) == DRVLITE_OK) {
            std::printf("memtest: unmapped read unexpectedly succeeded\n");
            ++failures;
        }
        /* Non-canonical user address must be rejected. */
        if (session.write_process(pid, 0x0000800000000000ULL, &canary,
                                  sizeof(canary)) == DRVLITE_OK) {
            ++failures;
        }
        /* 2 MiB boundary inside ntoskrnl (large-page path). */
        if (info.kernel_base == 0 ||
            session.read_kernel(info.kernel_base + 0x200000 - 8, bytes,
                                sizeof(bytes)) != DRVLITE_OK) {
            std::printf("memtest: 2m-boundary kernel read failed\n");
            ++failures;
        }
        /* Chunked large kernel read (63 KiB, header must be MZ). */
        std::vector<uint8_t> large(DRVLITE_MAX_TRANSFER);
        if (session.read_kernel(info.kernel_base, large.data(),
                                static_cast<uint32_t>(large.size())) !=
                DRVLITE_OK ||
            large[0] != 'M' || large[1] != 'Z') {
            std::printf("memtest: large kernel read failed\n");
            ++failures;
        }
        std::printf(
            "memtest: page-crossing, non-present, non-canonical, "
            "2m-boundary and chunked reads: %s\n",
            failures == 0 ? "ok" : "failed");
        return failures == 0 ? 0 : 1;
    }
    if (command == "hook-shutdown-test") {
        if (session.infinity_install({}) != DRVLITE_OK) {
            print_status("hook_install", DRVLITE_ERR_UNSUPPORTED);
            return 1;
        }
        drvlite::hook_status status;
        session.infinity_status(&status);
        const int shutdown_status = session.shutdown();
        std::printf(
            "hook-shutdown-test: installed=1 syscalls=%llu "
            "shutdown=%d\n",
            static_cast<unsigned long long>(status.total_syscalls),
            shutdown_status);
        return shutdown_status == DRVLITE_OK ? 0 : 1;
    }
    if (command == "dse-load-test") {
        if (argc < 2) {
            return usage();
        }
        uint32_t iterations = 20;
        if (argc >= 3) {
            parse_u32(argv[2], &iterations);
        }
        if (iterations == 0) {
            iterations = 20;
        }
        return run_dse_load_test(session, argv[1], iterations);
    }
    if (command == "selftest") {
        const uint32_t pid = GetCurrentProcessId();
        void* local = VirtualAlloc(nullptr, 0x4000, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_READWRITE);
        if (local == nullptr) {
            return 1;
        }
        std::memcpy(local, "DRVLITE!", 8);
        uint8_t bytes[16] = {};
        const int read_status = session.read_process(
            pid, reinterpret_cast<uint64_t>(local), bytes, 8);
        const uint32_t canary = 0x11223344;
        const int write_status = session.write_process(
            pid, reinterpret_cast<uint64_t>(local) + 0x1000, &canary, 4);
        uint32_t observed = 0;
        const int reread_status = session.read_process(
            pid, reinterpret_cast<uint64_t>(local) + 0x1000, &observed,
            sizeof(observed));
        const uint32_t canary2 = 0x55667788;
        const int write_status2 =
            session.write_process(pid, reinterpret_cast<uint64_t>(local) + 8,
                                  &canary2, sizeof(canary2));
        uint32_t observed2 = 0;
        const int reread_status2 =
            session.read_process(pid, reinterpret_cast<uint64_t>(local) + 8,
                                 &observed2, sizeof(observed2));
        drvlite::device_info info;
        session.query_info(&info);
        std::vector<uint8_t> large(DRVLITE_MAX_TRANSFER);
        const int large_status =
            session.read_kernel(info.kernel_base, large.data(),
                                static_cast<uint32_t>(large.size()));
        HANDLE handle = nullptr;
        uint32_t granted = 0;
        const int handle_status = session.open_process_handle(
            pid, DRVLITE_ACCESS_FULL, &handle, &granted);
        drvlite::dse_status dse_before;
        drvlite::dse_status dse_after;
        session.dse_query(&dse_before);
        const int dse_disable = session.dse_set(0);
        session.dse_query(&dse_after);
        const int dse_restore = session.dse_restore();
        std::printf(
            "selftest: read=%d pattern=%s write=%d reread=%d observed=0x%x "
            "samepage_write=%d samepage_read=%d observed2=0x%x "
            "large=%d large_mz=%d handle=%d granted=0x%x "
            "dse_supported=%d dse_disable=%d dse_after_patched=%d "
            "dse_before_cur=0x%x dse_after_cur=0x%x dse_restore=%d "
            "cli_pid=%u kernel_base=%s\n",
            read_status, (bytes[0] == 'D' && bytes[1] == 'R') ? "ok" : "bad",
            write_status, reread_status, observed, write_status2,
            reread_status2, observed2, large_status,
            (large[0] == 'M' && large[1] == 'Z') ? 1 : 0, handle_status,
            granted, dse_before.supported ? 1 : 0, dse_disable,
            dse_after.patched ? 1 : 0, dse_before.current, dse_after.current,
            dse_restore, pid, hex_u64(info.kernel_base).c_str());
        if (handle != nullptr) {
            CloseHandle(handle);
        }
        VirtualFree(local, 0, MEM_RELEASE);
        const bool ok =
            read_status == DRVLITE_OK && write_status == DRVLITE_OK &&
            reread_status == DRVLITE_OK && observed == canary &&
            large_status == DRVLITE_OK && handle_status == DRVLITE_OK;
        return ok ? 0 : 1;
    }
    if (command == "kernel-rw") {
        /* One-session kernel write/read test on the diagnostic page: the page
         * is allocated per driver instance, so write and verify must not be
         * split across CLI invocations (each restarts the driver). */
        drvlite::device_info info;
        if (session.query_info(&info) != DRVLITE_OK || info.test_page_va == 0) {
            print_status("kernel-rw", DRVLITE_ERR_NOT_FOUND);
            return 1;
        }
        uint8_t marker[12] = {};
        if (session.read_kernel(info.test_page_va, marker, sizeof(marker)) !=
                DRVLITE_OK ||
            std::memcmp(marker, "DRVLITE-PHYS", 12) != 0) {
            print_status("kernel-rw_read", DRVLITE_ERR_VERIFY);
            return 1;
        }
        const uint32_t canary = 0x41414141;
        if (session.write_kernel(info.test_page_va, &canary, sizeof(canary)) !=
            DRVLITE_OK) {
            print_status("kernel-rw_write", DRVLITE_ERR_IO);
            return 1;
        }
        uint32_t observed = 0;
        if (session.read_kernel(info.test_page_va, &observed,
                                sizeof(observed)) != DRVLITE_OK ||
            observed != canary) {
            print_status("kernel-rw_verify", DRVLITE_ERR_VERIFY);
            return 1;
        }
        const uint32_t original = *reinterpret_cast<const uint32_t*>(marker);
        session.write_kernel(info.test_page_va, &original, sizeof(original));
        uint32_t restored = 0;
        if (session.read_kernel(info.test_page_va, &restored,
                                sizeof(restored)) != DRVLITE_OK ||
            restored != original) {
            print_status("kernel-rw_restore", DRVLITE_ERR_VERIFY);
            return 1;
        }
        std::printf("kernel-rw: ok (va=%s marker -> canary -> restored)\n",
                    hex_u64(info.test_page_va).c_str());
        return 0;
    }
    if (command == "dse-cycle") {
        drvlite::dse_status before;
        drvlite::dse_status after_patch;
        drvlite::dse_status after_restore;
        if (session.dse_query(&before) != DRVLITE_OK) {
            return 1;
        }
        if (!before.supported) {
            std::printf("dse-cycle: supported=0\n");
            return 2;
        }
        const int disable = session.dse_set(0);
        session.dse_query(&after_patch);
        const int restore = session.dse_restore();
        session.dse_query(&after_restore);
        std::printf(
            "dse-cycle: supported=1 before=0x%x disable=%d patched=%d "
            "after=0x%x restore=%d restored=%d cur=0x%x\n",
            before.current, disable, after_patch.patched ? 1 : 0,
            after_patch.current, restore, after_restore.patched ? 1 : 0,
            after_restore.current);
        const bool ok = disable == DRVLITE_OK && after_patch.patched &&
                        after_patch.current == 0 && restore == DRVLITE_OK &&
                        !after_restore.patched &&
                        after_restore.current == before.current;
        return ok ? 0 : 1;
    }
    if (command == "shutdown") {
        const int status = session.shutdown();
        print_status("shutdown", status);
        return status == DRVLITE_OK ? 0 : 1;
    }
    if (command == "version") {
        std::printf("drvlite-cli %s (protocol %u.%u)\n",
                    drvlite::version_string(), DRVLITE_ABI_MAJOR,
                    DRVLITE_ABI_MINOR);
        return 0;
    }
    (void)timeout_ms;
    return usage();
}

bool write_bootstrap_file(const kc_bootstrap& bootstrap, const char* path) {
    FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") != 0 || file == nullptr) {
        return false;
    }
    const size_t written = fwrite(&bootstrap, 1, sizeof(bootstrap), file);
    fclose(file);
    return written == sizeof(bootstrap);
}

/* Service-load (dev/e2e) bootstrap delivery: write the kc_bootstrap as a
 * REG_BINARY value under the service's Parameters key so the driver's
 * DriverEntry can read it without a mapper. */
bool write_registry_bootstrap(const char* service, const kc_bootstrap& boot) {
    if (service == nullptr) {
        return false;
    }
    char path[512] = {};
    std::snprintf(path, sizeof(path),
                  "SYSTEM\\CurrentControlSet\\Services\\%s\\Parameters",
                  service);
    HKEY key = nullptr;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, path, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    /* Service load: the kernel resolves the shared region in the peer
     * process, so peer_pid must name this CLI process (a zero value means the
     * current process, which during DriverEntry would be the system process).
     */
    kc_bootstrap stored = boot;
    if (stored.peer_pid == 0) {
        stored.peer_pid = GetCurrentProcessId();
    }
    const LONG result =
        RegSetValueExA(key, "Bootstrap", 0, REG_BINARY,
                       reinterpret_cast<const BYTE*>(&stored), sizeof(stored));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

std::string replace_all(std::string text, const std::string& from,
                        const std::string& to) {
    size_t position = 0;
    while ((position = text.find(from, position)) != std::string::npos) {
        text.replace(position, from.size(), to);
        position += to.size();
    }
    return text;
}

/* Load/unload cycle test for the service-load dev path: attach a fresh
 * kchannel session, publish its bootstrap in the registry, start the service,
 * handshake, query, shut down and unload - all without spawning a new CLI
 * process per iteration. */
int run_service_cycle(const char* service, const char* map_command,
                      uint32_t cycles) {
    if (service == nullptr || map_command == nullptr || cycles == 0) {
        return 2;
    }
    std::string stop_command = "sc stop ";
    stop_command += service;
    uint32_t failures = 0;
    for (uint32_t index = 0; index < cycles; ++index) {
        drvlite::session session;
        drvlite::attach_options options;
        options.mode = KC_MODE_COPY;
        if (session.attach(options) != DRVLITE_OK) {
            ++failures;
            continue;
        }
        if (!write_registry_bootstrap(service, session.bootstrap())) {
            ++failures;
            session.detach();
            continue;
        }
        if (std::system(map_command) != 0) {
            ++failures;
            session.detach();
            continue;
        }
        if (session.finish(5000) != DRVLITE_OK) {
            ++failures;
        } else {
            drvlite::device_info info;
            session.query_info(&info);
        }
        session.shutdown();
        session.detach();
        std::system(stop_command.c_str());
    }
    std::printf("service-cycle: %u cycles, %u failures\n", cycles, failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return usage();
    }
    std::string command = argv[1];
    if (command == "version") {
        std::printf("drvlite-cli %s (protocol %u.%u)\n",
                    drvlite::version_string(), DRVLITE_ABI_MAJOR,
                    DRVLITE_ABI_MINOR);
        return 0;
    }
    if (command == "service-cycle") {
        bool allow = false;
        const char* service = nullptr;
        const char* map_command = nullptr;
        uint32_t cycles = 100;
        for (int index = 2; index < argc; ++index) {
            if (std::strcmp(argv[index], "--allow-kernel-live") == 0) {
                allow = true;
            } else if (std::strcmp(argv[index], "--service") == 0 &&
                       index + 1 < argc) {
                service = argv[++index];
            } else if (std::strcmp(argv[index], "--map-cmd") == 0 &&
                       index + 1 < argc) {
                map_command = argv[++index];
            } else if (std::strcmp(argv[index], "--cycles") == 0 &&
                       index + 1 < argc) {
                parse_u32(argv[++index], &cycles);
            }
        }
        const char* allow_env = std::getenv("DRVLITE_ALLOW_KERNEL_LIVE");
        allow =
            allow || (allow_env != nullptr && std::strcmp(allow_env, "1") == 0);
        if (!allow) {
            std::printf(
                "refusing to load kernel code on this machine "
                "(DRVLITE_ALLOW_KERNEL_LIVE is not set)\n");
            return 3;
        }
        if (service == nullptr || map_command == nullptr || cycles == 0) {
            std::printf(
                "usage: drvlite-cli service-cycle --service <name> "
                "--map-cmd <cmd> [--cycles N]\n");
            return 2;
        }
        return run_service_cycle(service, map_command, cycles);
    }
    if (command == "cycle-test") {
        bool allow = false;
        for (int index = 2; index < argc; ++index) {
            if (std::strcmp(argv[index], "--allow-kernel-live") == 0) {
                allow = true;
            }
        }
        const char* allow_env = std::getenv("DRVLITE_ALLOW_KERNEL_LIVE");
        allow =
            allow || (allow_env != nullptr && std::strcmp(allow_env, "1") == 0);
        if (!allow) {
            std::printf(
                "refusing to load kernel code on this machine "
                "(DRVLITE_ALLOW_KERNEL_LIVE is not set)\n");
            return 3;
        }
        const char* image_path = nullptr;
        uint32_t cycles = 100;
        for (int index = 2; index < argc; ++index) {
            if (std::strcmp(argv[index], "--image") == 0 && index + 1 < argc) {
                image_path = argv[++index];
            } else if (std::strcmp(argv[index], "--cycles") == 0 &&
                       index + 1 < argc) {
                parse_u32(argv[++index], &cycles);
            }
        }
        if (image_path == nullptr || cycles == 0) {
            std::printf(
                "usage: drvlite-cli cycle-test --image <sys> "
                "--cycles <n>\n");
            return 2;
        }
        return run_cycle_test(image_path, cycles);
    }
    if (command == "run") {
        bool allow = false;
        for (int index = 2; index < argc; ++index) {
            if (std::strcmp(argv[index], "--allow-kernel-live") == 0) {
                allow = true;
            }
        }
        const char* allow_env = std::getenv("DRVLITE_ALLOW_KERNEL_LIVE");
        allow =
            allow || (allow_env != nullptr && std::strcmp(allow_env, "1") == 0);
        if (!allow) {
            std::printf(
                "refusing to load kernel code on this machine.\n"
                "Kernel experiments are only allowed inside a bvmenv "
                "VM guest.\nRun through scripts/e2e.ps1 or set "
                "DRVLITE_ALLOW_KERNEL_LIVE=1 in the VM guest.\n");
            return 3;
        }
    }
    if (command != "run") {
        /* Every other command needs a session; tell the user how to make
         * one instead of failing deep inside the SDK. */
        std::printf("no session: use `drvlite-cli run ... -- %s ...`\n",
                    command.c_str());
        return 2;
    }

    const char* image = nullptr;
    const char* mapper = nullptr;
    const char* bootstrap_path = nullptr;
    const char* service_name = nullptr;
    bool copy_mode = false;
    uint32_t region_size = 0x40000;
    uint32_t timeout_ms = 5000;
    int index = 2;
    for (; index < argc; ++index) {
        const char* argument = argv[index];
        if (std::strcmp(argument, "--") == 0) {
            ++index;
            break;
        }
        if (std::strcmp(argument, "--image") == 0 && index + 1 < argc) {
            image = argv[++index];
        } else if (std::strcmp(argument, "--map-cmd") == 0 &&
                   index + 1 < argc) {
            mapper = argv[++index];
        } else if (std::strcmp(argument, "--bootstrap") == 0 &&
                   index + 1 < argc) {
            bootstrap_path = argv[++index];
        } else if (std::strcmp(argument, "--service") == 0 &&
                   index + 1 < argc) {
            service_name = argv[++index];
        } else if (std::strcmp(argument, "--mode") == 0 && index + 1 < argc) {
            copy_mode = std::strcmp(argv[++index], "copy") == 0;
        } else if (std::strcmp(argument, "--region") == 0 && index + 1 < argc) {
            parse_u32(argv[++index], &region_size);
        } else if (std::strcmp(argument, "--timeout-ms") == 0 &&
                   index + 1 < argc) {
            parse_u32(argv[++index], &timeout_ms);
        } else if (std::strcmp(argument, "--allow-kernel-live") == 0) {
            /* Host-safety proof, already validated above. */
            continue;
        } else {
            std::printf("unknown option: %s\n", argument);
            return usage();
        }
    }
    if (image == nullptr || mapper == nullptr) {
        std::printf("run needs --image and --map-cmd\n");
        return usage();
    }

    drvlite::session session;
    drvlite::attach_options options;
    options.region_size = region_size != 0 ? region_size : 0x40000;
    options.mode = copy_mode ? KC_MODE_COPY : KC_MODE_MAPPED;
    const int attach_status = session.attach(options);
    if (attach_status != DRVLITE_OK) {
        print_status("attach", attach_status);
        return 1;
    }
    char temp_directory[MAX_PATH] = {};
    if (GetTempPathA(MAX_PATH, temp_directory) == 0) {
        std::printf("GetTempPath failed\n");
        return 1;
    }
    char temp_file[MAX_PATH] = {};
    std::snprintf(temp_file, sizeof(temp_file), "%sdrvlite-%u.bootstrap",
                  temp_directory, GetCurrentProcessId());
    const char* bootstrap_file =
        bootstrap_path != nullptr ? bootstrap_path : temp_file;
    if (!write_bootstrap_file(session.bootstrap(), bootstrap_file)) {
        std::printf("cannot write bootstrap file\n");
        return 1;
    }
    const char* bootstrap_for_mapper = bootstrap_file;
    if (service_name != nullptr &&
        !write_registry_bootstrap(service_name, session.bootstrap())) {
        std::printf("cannot write the service bootstrap value\n");
        return 1;
    }
    std::string mapper_command = replace_all(mapper, "{image}", image);
    mapper_command =
        replace_all(mapper_command, "{bootstrap}", bootstrap_for_mapper);
    if (service_name != nullptr) {
        mapper_command = replace_all(mapper_command, "{service}", service_name);
    }
    std::printf("mapper: %s\n", mapper_command.c_str());
    const int mapper_status = std::system(mapper_command.c_str());
    DeleteFileA(temp_file);
    if (mapper_status != 0) {
        std::printf("mapper failed (exit %d)\n", mapper_status);
        return 1;
    }
    const int finish_status = session.finish(timeout_ms);
    if (finish_status != DRVLITE_OK) {
        print_status("handshake", finish_status);
        return 1;
    }
    if (index >= argc) {
        std::printf("driver attached; no subcommand given\n");
        return usage();
    }
    return run_command(session, argc - index, argv + index, timeout_ms);
}

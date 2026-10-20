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

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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
    if (command == "info") {
        drvlite::device_info info;
        const int status = session.query_info(&info);
        if (status != DRVLITE_OK) {
            print_status("query_info", status);
            return 1;
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
        if (argc != 3) {
            return usage();
        }
        uint64_t address = 0;
        if (!parse_u64(argv[1], &address)) {
            std::printf("invalid address\n");
            return 1;
        }
        const std::vector<uint8_t> bytes = parse_hex(argv[2]);
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
        if (argc != 4) {
            return usage();
        }
        uint32_t pid = 0;
        uint64_t address = 0;
        if (!parse_u32(argv[1], &pid) || !parse_u64(argv[2], &address)) {
            std::printf("invalid arguments\n");
            return 1;
        }
        const std::vector<uint8_t> bytes = parse_hex(argv[3]);
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

std::string replace_all(std::string text, const std::string& from,
                        const std::string& to) {
    size_t position = 0;
    while ((position = text.find(from, position)) != std::string::npos) {
        text.replace(position, from.size(), to);
        position += to.size();
    }
    return text;
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
        } else if (std::strcmp(argument, "--region") == 0 && index + 1 < argc) {
            parse_u32(argv[++index], &region_size);
        } else if (std::strcmp(argument, "--timeout-ms") == 0 &&
                   index + 1 < argc) {
            parse_u32(argv[++index], &timeout_ms);
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
    if (!write_bootstrap_file(session.bootstrap(), temp_file)) {
        std::printf("cannot write bootstrap file\n");
        return 1;
    }
    const char* bootstrap_for_mapper =
        bootstrap_path != nullptr ? bootstrap_path : temp_file;
    std::string mapper_command = replace_all(mapper, "{image}", image);
    mapper_command =
        replace_all(mapper_command, "{bootstrap}", bootstrap_for_mapper);
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

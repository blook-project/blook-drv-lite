/*
 * drvlite-example - worked example for the blook-drv-lite SDK.
 *
 * It performs the complete flow:
 *   1. create the kchannel session,
 *   2. manual-map the driver through an external mapper command,
 *   3. finish the handshake,
 *   4. read a process' memory through the CR3 walk,
 *   5. toggle Driver Signature Enforcement, and
 *   6. shut the driver down again.
 *
 * Build: xmake build drvlite-example
 * Run:   drvlite-example --image drvlite.sys \
 *            --map-cmd "kdu map --driver {image} --bootstrap {bootstrap}" \
 *            --pid 1234 --address 0x7ff600000000
 */
#include <blook/drvlite/sdk.h>

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

std::string replace_all(std::string text, const std::string& from,
                        const std::string& to) {
    size_t position = 0;
    while ((position = text.find(from, position)) != std::string::npos) {
        text.replace(position, from.size(), to);
        position += to.size();
    }
    return text;
}

bool write_file(const char* path, const void* data, size_t size) {
    FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") != 0 || file == nullptr) {
        return false;
    }
    const size_t written = fwrite(data, 1, size, file);
    fclose(file);
    return written == size;
}

}  // namespace

int main(int argc, char** argv) {
    const char* image = nullptr;
    const char* mapper = nullptr;
    uint32_t pid = 0;
    uint64_t address = 0;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--image") == 0 && index + 1 < argc) {
            image = argv[++index];
        } else if (std::strcmp(argv[index], "--map-cmd") == 0 &&
                   index + 1 < argc) {
            mapper = argv[++index];
        } else if (std::strcmp(argv[index], "--pid") == 0 && index + 1 < argc) {
            pid =
                static_cast<uint32_t>(std::strtoul(argv[++index], nullptr, 0));
        } else if (std::strcmp(argv[index], "--address") == 0 &&
                   index + 1 < argc) {
            address = std::strtoull(argv[++index], nullptr, 0);
        }
    }
    if (image == nullptr || mapper == nullptr || pid == 0 || address == 0) {
        std::printf(
            "usage: drvlite-example --image <sys> --map-cmd <cmd> "
            "--pid <pid> --address <va>\n");
        return 2;
    }

    drvlite::session session;
    if (session.attach() != DRVLITE_OK) {
        std::printf("attach failed\n");
        return 1;
    }
    char temp_directory[MAX_PATH] = {};
    char bootstrap_path[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, temp_directory);
    std::snprintf(bootstrap_path, sizeof(bootstrap_path),
                  "%sdrvlite-example-%u.bootstrap", temp_directory,
                  GetCurrentProcessId());
    if (!write_file(bootstrap_path, &session.bootstrap(),
                    sizeof(kc_bootstrap))) {
        std::printf("cannot write bootstrap\n");
        return 1;
    }
    std::string command = replace_all(mapper, "{image}", image);
    command = replace_all(command, "{bootstrap}", bootstrap_path);
    std::printf("mapper: %s\n", command.c_str());
    const int mapper_status = std::system(command.c_str());
    DeleteFileA(bootstrap_path);
    if (mapper_status != 0) {
        std::printf("mapper failed\n");
        return 1;
    }
    if (session.finish(5000) != DRVLITE_OK) {
        std::printf("handshake failed\n");
        return 1;
    }

    drvlite::device_info info;
    if (session.query_info(&info) != DRVLITE_OK) {
        std::printf("query_info failed\n");
        return 1;
    }
    std::printf("driver at 0x%llx (nt build %u, abi %u.%u)\n",
                static_cast<unsigned long long>(info.driver_base),
                info.nt_build, info.abi >> 16, info.abi & 0xFFFF);

    /* Read part of the process image through the validated CR3 walk. */
    uint8_t header[0x40] = {};
    if (session.read_process(pid, address, header, sizeof(header)) !=
        DRVLITE_OK) {
        std::printf("read_process failed (pid %u, va 0x%llx)\n", pid,
                    static_cast<unsigned long long>(address));
    } else {
        std::printf("read %zu bytes at 0x%llx: %02x %02x %02x %02x\n",
                    sizeof(header), static_cast<unsigned long long>(address),
                    header[0], header[1], header[2], header[3]);
    }

    /* Temporarily allow unsigned drivers, then restore the original value. */
    drvlite::dse_status dse;
    if (session.dse_query(&dse) == DRVLITE_OK && dse.supported) {
        std::printf("DSE at 0x%llx current 0x%x original 0x%x\n",
                    static_cast<unsigned long long>(dse.address), dse.current,
                    dse.original);
        if (session.dse_set(0) == DRVLITE_OK) {
            std::printf("DSE disabled (verified), loading would succeed now\n");
        }
        session.dse_restore();
    } else {
        std::printf("DSE override not supported on this build\n");
    }

    session.shutdown();
    return 0;
}

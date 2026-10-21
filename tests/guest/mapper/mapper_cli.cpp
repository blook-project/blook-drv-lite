/*
 * drvlite-mapper-cli - user-mode front end for the test mapper driver.
 *
 *   drvlite-mapper-cli --image <driver.sys> --bootstrap <file>
 *
 * Used by drvlite-cli's --map-cmd template during the e2e run.
 */
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "mapper_abi.h"

namespace {

bool read_file(const char* path, std::vector<uint8_t>* bytes) {
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

}  // namespace

int main(int argc, char** argv) {
    /* Host safety: this tool maps kernel code, so it only runs inside a VM
     * guest. scripts/e2e.ps1 sets the proof variable for its guest commands. */
    bool allow = false;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--allow-kernel-live") == 0) {
            allow = true;
        }
    }
    const char* allow_env = std::getenv("DRVLITE_ALLOW_KERNEL_LIVE");
    allow = allow || (allow_env != nullptr && std::strcmp(allow_env, "1") == 0);
    if (!allow) {
        std::printf(
            "refusing to map kernel code on this machine.\n"
            "Only run this inside a bvmenv VM guest "
            "(DRVLITE_ALLOW_KERNEL_LIVE=1).\n");
        return 3;
    }
    const char* image_path = nullptr;
    const char* bootstrap_path = nullptr;
    const char* unmap_text = nullptr;
    const char* probe_text = nullptr;
    bool sweep = false;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--image") == 0 && index + 1 < argc) {
            image_path = argv[++index];
        } else if (std::strcmp(argv[index], "--bootstrap") == 0 &&
                   index + 1 < argc) {
            bootstrap_path = argv[++index];
        } else if (std::strcmp(argv[index], "--unmap") == 0 &&
                   index + 1 < argc) {
            unmap_text = argv[++index];
        } else if (std::strcmp(argv[index], "--probe") == 0 &&
                   index + 1 < argc) {
            probe_text = argv[++index];
        } else if (std::strcmp(argv[index], "--sweep") == 0) {
            sweep = true;
        }
    }
    if (sweep) {
        HANDLE device = CreateFileA(
            DRVLITE_MAPPER_WIN32_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (device == INVALID_HANDLE_VALUE) {
            std::printf("cannot open mapper device (error %lu)\n",
                        GetLastError());
            return 1;
        }
        std::vector<uint8_t> in(512, 0);
        std::vector<uint8_t> out(512, 0);
        for (unsigned int function = 0x800; function <= 0x820; ++function) {
            const DWORD code = static_cast<DWORD>((0x8A00u << 16) | (3u << 14) |
                                                  (function << 2) | 3u);
            DWORD returned = 0;
            SetLastError(0);
            const BOOL ok = DeviceIoControl(
                device, code, in.data(), static_cast<DWORD>(in.size()),
                out.data(), static_cast<DWORD>(out.size()), &returned, nullptr);
            drvlite_map_response echo{};
            std::memcpy(&echo, out.data(), sizeof(echo));
            std::printf(
                "sweep fn=0x%x code=0x%x ok=%d error=%lu ret=%lu "
                "st=0x%x method=0x%x\n",
                function, code, ok, GetLastError(), returned, echo.status,
                echo.method);
        }
        CloseHandle(device);
        return 0;
    }
    if (probe_text != nullptr) {
        HANDLE device = CreateFileA(
            DRVLITE_MAPPER_WIN32_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (device == INVALID_HANDLE_VALUE) {
            std::printf("cannot open mapper device (error %lu)\n",
                        GetLastError());
            return 1;
        }
        std::vector<uint8_t> in(512, 0);
        std::vector<uint8_t> out(512, 0);
        DWORD returned = 0;
        const DWORD code =
            static_cast<DWORD>(std::strtoul(probe_text, nullptr, 0));
        SetLastError(0);
        const BOOL ok = DeviceIoControl(
            device, code, in.data(), static_cast<DWORD>(in.size()), out.data(),
            static_cast<DWORD>(out.size()), &returned, nullptr);
        std::printf("probe 0x%x ok=%d error=%lu returned=%lu\n", code, ok,
                    GetLastError(), returned);
        CloseHandle(device);
        return ok ? 0 : 1;
    }
    if (unmap_text != nullptr) {
        HANDLE device = CreateFileA(
            DRVLITE_MAPPER_WIN32_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (device == INVALID_HANDLE_VALUE) {
            std::printf("cannot open mapper device (error %lu)\n",
                        GetLastError());
            return 1;
        }
        drvlite_unmap_request request{};
        request.mapped_base = static_cast<unsigned long long>(
            std::strtoull(unmap_text, nullptr, 0));
        std::vector<uint8_t> unmap_out(256, 0);
        std::vector<uint8_t> unmap_in(256, 0);
        std::memcpy(unmap_in.data(), &request, sizeof(request));
        DWORD returned = 0;
        const BOOL ok = DeviceIoControl(
            device, static_cast<DWORD>(IOCTL_DRVLITE_MAPPER_UNMAP),
            unmap_in.data(), static_cast<DWORD>(unmap_in.size()),
            unmap_out.data(), static_cast<DWORD>(unmap_out.size()), &returned,
            nullptr);
        const DWORD error = GetLastError();
        CloseHandle(device);
        if (!ok) {
            std::printf("unmap ioctl failed (error %lu)\n", error);
            return 1;
        }
        unsigned int unmap_status = 0;
        std::memcpy(&unmap_status, unmap_out.data(), sizeof(unmap_status));
        if (unmap_status != 0) {
            std::printf("unmap status 0x%08x\n", unmap_status);
            return 1;
        }
        std::printf("unmapped 0x%llx\n", request.mapped_base);
        return 0;
    }
    if (image_path == nullptr || bootstrap_path == nullptr) {
        std::printf(
            "usage: drvlite-mapper-cli --image <sys> --bootstrap "
            "<file> | --unmap <base>\n");
        return 2;
    }
    std::vector<uint8_t> image;
    std::vector<uint8_t> bootstrap;
    if (!read_file(image_path, &image) ||
        !read_file(bootstrap_path, &bootstrap)) {
        std::printf("cannot read input files\n");
        return 1;
    }
    if (bootstrap.size() > 0x1000 || image.size() > (16u << 20)) {
        std::printf("input too large\n");
        return 1;
    }

    const size_t request_size =
        sizeof(drvlite_map_request) + image.size() + bootstrap.size();
    std::vector<uint8_t> request(request_size, 0);
    auto* header = reinterpret_cast<drvlite_map_request*>(request.data());
    header->image_size = static_cast<unsigned int>(image.size());
    header->bootstrap_size = static_cast<unsigned int>(bootstrap.size());
    header->flags = 1;
    header->image_offset = sizeof(drvlite_map_request);
    header->bootstrap_offset = sizeof(drvlite_map_request) + image.size();
    std::memcpy(request.data() + header->image_offset, image.data(),
                image.size());
    std::memcpy(request.data() + header->bootstrap_offset, bootstrap.data(),
                bootstrap.size());

    HANDLE device =
        CreateFileA(DRVLITE_MAPPER_WIN32_NAME, GENERIC_READ | GENERIC_WRITE, 0,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (device == INVALID_HANDLE_VALUE) {
        std::printf("cannot open mapper device (error %lu)\n", GetLastError());
        return 1;
    }
    std::printf("dbg req=%zu rsp=%zu img=%zu boot=%zu ioctl=0x%x\n",
                sizeof(drvlite_map_request), sizeof(drvlite_map_response),
                image.size(), bootstrap.size(),
                static_cast<unsigned>(IOCTL_DRVLITE_MAPPER_MAP));
    std::vector<uint8_t> out(256, 0);
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(
        device, static_cast<DWORD>(IOCTL_DRVLITE_MAPPER_MAP), request.data(),
        static_cast<DWORD>(request.size()), out.data(),
        static_cast<DWORD>(out.size()), &returned, nullptr);
    drvlite_map_response response{};
    std::memcpy(&response, out.data(), sizeof(response));
    const DWORD error = GetLastError();
    CloseHandle(device);
    if (!ok) {
        std::printf(
            "map ioctl failed (error %lu) returned=%lu diag=0x%x "
            "method=%u input=0x%llx out_size=%u\n",
            error, returned, response.status, response.method,
            static_cast<unsigned long long>(response.mapped_base),
            response.image_size);
        return 1;
    }
    if (response.status != 0) {
        std::printf("entry returned 0x%08x\n", response.status);
        return 1;
    }
    std::printf("mapped 0x%llx (%u bytes, method %u)\n", response.mapped_base,
                response.image_size, response.method);
    return 0;
}

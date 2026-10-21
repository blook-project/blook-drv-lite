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
    const char* image_path = nullptr;
    const char* bootstrap_path = nullptr;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--image") == 0 && index + 1 < argc) {
            image_path = argv[++index];
        } else if (std::strcmp(argv[index], "--bootstrap") == 0 &&
                   index + 1 < argc) {
            bootstrap_path = argv[++index];
        }
    }
    if (image_path == nullptr || bootstrap_path == nullptr) {
        std::printf(
            "usage: drvlite-mapper-cli --image <sys> --bootstrap <file>\n");
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
    drvlite_map_response response{};
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(
        device, static_cast<DWORD>(IOCTL_DRVLITE_MAPPER_MAP), request.data(),
        static_cast<DWORD>(request.size()), &response,
        static_cast<DWORD>(sizeof(response)), &returned, nullptr);
    const DWORD error = GetLastError();
    CloseHandle(device);
    if (!ok) {
        std::printf("map ioctl failed (error %lu)\n", error);
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

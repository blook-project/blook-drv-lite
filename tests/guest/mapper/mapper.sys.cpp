/*
 * drvlite-mapper - minimal test-only manual mapper.
 *
 * Exposes one device, receives a driver image and a kchannel bootstrap blob
 * from user mode, maps the image into non-paged executable kernel memory
 * (sections, relocations, imports), and calls its entry point with a kernel
 * copy of the bootstrap. It exists only so the e2e suite can validate the
 * manual-map load model without depending on a vulnerable driver.
 *
 * The PE parsing here is deliberately self contained: the WDK kernel headers
 * do not expose the image structures.
 */
#include <ntddk.h>
#include <intrin.h>

#include "mapper_abi.h"

/* Fixed width aliases (the kernel headers do not provide <stdint.h>). */
using byte_t = unsigned char;
using word_t = unsigned short;
using dword_t = unsigned int;
using qword_t = unsigned long long;
using int64_t_ = long long;

namespace {

constexpr ULONG kMapperTag = 'pamD';
constexpr dword_t kMaxSections = 96;
constexpr dword_t kMaxImageSize = 16u << 20;

struct mapper_device_extension {
    PDEVICE_OBJECT device;
};

/* Images mapped by this harness, so the e2e stress loop can unmap them. */
constexpr dword_t kMaxMappedImages = 128;
struct mapped_image_record {
    qword_t base;
    dword_t size;
};
mapped_image_record g_mapped_images[kMaxMappedImages];
KSPIN_LOCK g_mapped_lock;

/* Raw little-endian readers. */
word_t read_u16(const byte_t* data, dword_t offset) {
    return static_cast<word_t>(data[offset] | (data[offset + 1] << 8));
}

dword_t read_u32(const byte_t* data, dword_t offset) {
    return static_cast<dword_t>(data[offset]) |
           (static_cast<dword_t>(data[offset + 1]) << 8) |
           (static_cast<dword_t>(data[offset + 2]) << 16) |
           (static_cast<dword_t>(data[offset + 3]) << 24);
}

qword_t read_u64(const byte_t* data, dword_t offset) {
    return static_cast<qword_t>(read_u32(data, offset)) |
           (static_cast<qword_t>(read_u32(data, offset + 4)) << 32);
}

struct section_record {
    dword_t virtual_address;
    dword_t virtual_size;
    dword_t raw_offset;
    dword_t raw_size;
};

struct pe_headers {
    dword_t nt_offset;
    dword_t size_of_image;
    dword_t size_of_headers;
    dword_t entry_rva;
    dword_t data_directory_rva[16];
    dword_t data_directory_size[16];
    qword_t image_base;
    dword_t section_count;
    section_record sections[kMaxSections];
};

bool parse_headers(const byte_t* image, dword_t size, pe_headers* headers) {
    if (image == nullptr || headers == nullptr || size < 0x40) {
        return false;
    }
    if (read_u16(image, 0) != 0x5A4D) { /* MZ */
        return false;
    }
    headers->nt_offset = read_u32(image, 0x3C);
    const dword_t nt = headers->nt_offset;
    if (nt < 0x40 || nt + 24 > size) {
        return false;
    }
    if (read_u32(image, nt) != 0x00004550) { /* PE\0\0 */
        return false;
    }
    const dword_t file_header = nt + 4;
    const dword_t optional = file_header + 20;
    const word_t optional_size = read_u16(image, file_header + 16);
    if (optional_size < 112 || optional + optional_size > size) {
        return false;
    }
    if (read_u16(image, optional) != 0x20B) { /* PE32+ */
        return false;
    }
    headers->entry_rva = read_u32(image, optional + 16);
    headers->image_base = read_u64(image, optional + 24);
    headers->size_of_image = read_u32(image, optional + 56);
    headers->size_of_headers = read_u32(image, optional + 60);
    if (headers->size_of_image == 0 || headers->size_of_image > kMaxImageSize) {
        return false;
    }
    const dword_t directory_count = read_u32(image, optional + 108);
    if (directory_count > 16) {
        return false;
    }
    for (dword_t index = 0; index < directory_count; ++index) {
        headers->data_directory_rva[index] =
            read_u32(image, optional + 112 + index * 8);
        headers->data_directory_size[index] =
            read_u32(image, optional + 112 + index * 8 + 4);
    }

    headers->section_count = read_u16(image, file_header + 2);
    if (headers->section_count == 0 || headers->section_count > kMaxSections) {
        return false;
    }
    const dword_t sections = optional + optional_size;
    if (sections + headers->section_count * 40 > size) {
        return false;
    }
    for (dword_t index = 0; index < headers->section_count; ++index) {
        const dword_t entry = sections + index * 40;
        section_record& record = headers->sections[index];
        record.virtual_size = read_u32(image, entry + 8);
        record.virtual_address = read_u32(image, entry + 12);
        record.raw_size = read_u32(image, entry + 16);
        record.raw_offset = read_u32(image, entry + 20);
    }
    return true;
}

bool apply_relocations(byte_t* mapped, const pe_headers& headers,
                       int64_t_ delta) {
    if (delta == 0) {
        return true;
    }
    const dword_t directory_rva = headers.data_directory_rva[5];
    const dword_t directory_size = headers.data_directory_size[5];
    if (directory_rva == 0 || directory_size == 0 ||
        directory_rva >= headers.size_of_image ||
        directory_size > headers.size_of_image - directory_rva) {
        return false; /* an image without relocations cannot be moved */
    }
    const dword_t limit = headers.size_of_image;
    byte_t* cursor = mapped + directory_rva;
    byte_t* end = cursor + directory_size;
    while (cursor + 8 <= end) {
        const dword_t page_rva = read_u32(cursor, 0);
        const dword_t block_size = read_u32(cursor, 4);
        if (block_size < 8) {
            return false;
        }
        if (cursor + block_size > end) {
            return false;
        }
        const dword_t entries = (block_size - 8) / 2;
        for (dword_t index = 0; index < entries; ++index) {
            const word_t entry = read_u16(cursor, 8 + index * 2);
            const dword_t type = entry >> 12;
            const dword_t offset = entry & 0xFFF;
            if (type == 0) { /* IMAGE_REL_BASED_ABSOLUTE */
                continue;
            }
            if (type != 10) { /* IMAGE_REL_BASED_DIR64 */
                return false;
            }
            const dword_t rva = page_rva + offset;
            if (rva + sizeof(qword_t) > limit) {
                return false;
            }
            qword_t* value = reinterpret_cast<qword_t*>(mapped + rva);
            *value = static_cast<qword_t>(*value + delta);
        }
        cursor += block_size;
    }
    return true;
}

bool ascii_equal_ignore_case(const char* left, const char* right) {
    for (;;) {
        char a = *left++;
        char b = *right++;
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a + 32);
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<char>(b + 32);
        }
        if (a != b) {
            return false;
        }
        if (a == '\0') {
            return true;
        }
    }
}

bool resolve_export(const char* name, void** address) {
    if (name == nullptr || address == nullptr) {
        return false;
    }
    wchar_t wide[256] = {};
    dword_t length = 0;
    while (name[length] != '\0' && length < 255) {
        wide[length] = static_cast<wchar_t>(name[length]);
        ++length;
    }
    wide[length] = L'\0';
    UNICODE_STRING symbol;
    RtlInitUnicodeString(&symbol, wide);
    *address = MmGetSystemRoutineAddress(&symbol);
    return *address != nullptr;
}

bool resolve_imports(byte_t* mapped, const pe_headers& headers) {
    const dword_t directory_rva = headers.data_directory_rva[1];
    const dword_t directory_size = headers.data_directory_size[1];
    if (directory_rva == 0) {
        return true;
    }
    const dword_t limit = headers.size_of_image;
    if (directory_rva >= limit || directory_size > limit - directory_rva) {
        return false;
    }
    byte_t* descriptor = mapped + directory_rva;
    for (dword_t guard = 0; guard < 256; ++guard, descriptor += 20) {
        const dword_t name_rva = read_u32(descriptor, 12);
        if (name_rva == 0) {
            break;
        }
        if (name_rva >= limit) {
            return false;
        }
        const char* module_name =
            reinterpret_cast<const char*>(mapped + name_rva);
        if (!(ascii_equal_ignore_case(module_name, "ntoskrnl.exe") ||
              ascii_equal_ignore_case(module_name, "ntkrnlmp.exe") ||
              ascii_equal_ignore_case(module_name, "hal.dll"))) {
            return false;
        }
        dword_t thunk_rva = read_u32(descriptor, 0);
        if (thunk_rva == 0) {
            thunk_rva = read_u32(descriptor, 16);
        }
        const dword_t first_thunk = read_u32(descriptor, 16);
        if (thunk_rva == 0 || thunk_rva >= limit || first_thunk >= limit) {
            return false;
        }
        const byte_t* thunks = mapped + thunk_rva;
        byte_t* iat = mapped + first_thunk;
        for (dword_t index = 0;; ++index) {
            const dword_t thunk_offset = index * 8;
            if (thunk_rva + thunk_offset + 8 > limit ||
                first_thunk + thunk_offset + 8 > limit) {
                return false;
            }
            const qword_t thunk = read_u64(thunks, thunk_offset);
            if (thunk == 0) {
                break;
            }
            if ((thunk & 0x8000000000000000ULL) != 0) {
                return false; /* ordinal imports are not supported */
            }
            const dword_t hint_rva = static_cast<dword_t>(thunk);
            if (hint_rva + 2 >= limit) {
                return false;
            }
            const char* name =
                reinterpret_cast<const char*>(mapped + hint_rva + 2);
            void* function = nullptr;
            if (!resolve_export(name, &function)) {
                return false;
            }
            const qword_t value = reinterpret_cast<qword_t>(function);
            RtlCopyMemory(iat + thunk_offset, &value, sizeof(value));
        }
    }
    return true;
}

NTSTATUS call_entry(byte_t* mapped, const pe_headers& headers,
                    const void* bootstrap) {
    if (headers.entry_rva == 0 || headers.entry_rva >= headers.size_of_image) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    typedef NTSTATUS(NTAPI * entry_fn)(PVOID, PVOID);
    auto entry = reinterpret_cast<entry_fn>(mapped + headers.entry_rva);
    __wbinvd();
    int registers[4] = {};
    __cpuid(registers, 0);
    return entry(const_cast<void*>(bootstrap), nullptr);
}

/* Reads a whole file into a non-paged pool buffer. */
NTSTATUS read_whole_file(const wchar_t* path, void** buffer, ULONG* size) {
    *buffer = nullptr;
    *size = 0;
    UNICODE_STRING file_name;
    RtlInitUnicodeString(&file_name, path);
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &file_name,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               nullptr, nullptr);
    HANDLE handle = nullptr;
    IO_STATUS_BLOCK status_block{};
    NTSTATUS status =
        ZwCreateFile(&handle, GENERIC_READ, &attributes, &status_block, nullptr,
                     FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
                     FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    FILE_STANDARD_INFORMATION file_info{};
    status = ZwQueryInformationFile(handle, &status_block, &file_info,
                                    sizeof(file_info), FileStandardInformation);
    if (!NT_SUCCESS(status) || file_info.EndOfFile.QuadPart <= 0 ||
        file_info.EndOfFile.QuadPart > static_cast<LONGLONG>(kMaxImageSize)) {
        ZwClose(handle);
        return STATUS_INVALID_PARAMETER;
    }
    const ULONG length = static_cast<ULONG>(file_info.EndOfFile.QuadPart);
    void* data = ExAllocatePool2(POOL_FLAG_NON_PAGED, length, kMapperTag);
    if (data == nullptr) {
        ZwClose(handle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    status = ZwReadFile(handle, nullptr, nullptr, nullptr, &status_block, data,
                        length, nullptr, nullptr);
    ZwClose(handle);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(data, kMapperTag);
        return status;
    }
    *buffer = data;
    *size = length;
    return STATUS_SUCCESS;
}

void* g_mapped = nullptr;
void* g_bootstrap = nullptr;

void unload(PDRIVER_OBJECT driver_object) {
    UNREFERENCED_PARAMETER(driver_object);
    if (g_mapped != nullptr) {
        ExFreePoolWithTag(g_mapped, kMapperTag);
        g_mapped = nullptr;
    }
    if (g_bootstrap != nullptr) {
        ExFreePoolWithTag(g_bootstrap, kMapperTag);
        g_bootstrap = nullptr;
    }
    DbgPrint("[drvlite-loader] unloaded\n");
}

/* Maps the e2e driver image from the driver directory and calls its entry
 * point with a kernel copy of the bootstrap file. The file-based load avoids
 * an IOCTL surface entirely, which keeps the harness minimal. */
NTSTATUS load_and_call() {
    const wchar_t* image_path =
        L"\\SystemRoot\\System32\\drivers\\drvlite-lite.sys";
    const wchar_t* bootstrap_path =
        L"\\SystemRoot\\System32\\drivers\\drvlite-boot.bin";
    void* image = nullptr;
    ULONG image_size = 0;
    NTSTATUS status = read_whole_file(image_path, &image, &image_size);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[drvlite-loader] read image failed 0x%x\n", status);
        return STATUS_ACCESS_DENIED;
    }
    ULONG bootstrap_size = 0;
    const NTSTATUS bootstrap_status =
        read_whole_file(bootstrap_path, &g_bootstrap, &bootstrap_size);
    if (!NT_SUCCESS(bootstrap_status)) {
        DbgPrint("[drvlite-loader] no bootstrap (0x%x); mapping only\n",
                 bootstrap_status);
        g_bootstrap = nullptr;
    }
    const auto* image_bytes = static_cast<const byte_t*>(image);
    pe_headers headers{};
    if (!parse_headers(image_bytes, image_size, &headers)) {
        status = STATUS_INVALID_IMAGE_FORMAT;
        goto fail;
    }
    {
        const dword_t mapped_size = headers.size_of_image;
        void* mapped = ExAllocatePool2(POOL_FLAG_NON_PAGED_EXECUTE, mapped_size,
                                       kMapperTag);
        dword_t method = 1;
        if (mapped == nullptr) {
            mapped =
                ExAllocatePool2(POOL_FLAG_NON_PAGED, mapped_size, kMapperTag);
            method = 2;
        }
        if (mapped == nullptr) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto fail;
        }
        RtlZeroMemory(mapped, mapped_size);
        auto* destination = static_cast<byte_t*>(mapped);
        const dword_t headers_size = headers.size_of_headers < mapped_size
                                         ? headers.size_of_headers
                                         : mapped_size;
        RtlCopyMemory(destination, image_bytes, headers_size);
        for (dword_t index = 0; index < headers.section_count; ++index) {
            const section_record& section = headers.sections[index];
            if (section.virtual_address >= mapped_size) {
                continue;
            }
            dword_t to_copy = section.raw_size;
            if (to_copy > section.virtual_size) {
                to_copy = section.virtual_size;
            }
            if (section.virtual_address + to_copy > mapped_size) {
                to_copy = mapped_size - section.virtual_address;
            }
            if (to_copy == 0) {
                continue;
            }
            if (section.raw_offset + to_copy > image_size) {
                status = STATUS_INVALID_IMAGE_FORMAT;
                ExFreePoolWithTag(mapped, kMapperTag);
                goto fail;
            }
            RtlCopyMemory(destination + section.virtual_address,
                          image_bytes + section.raw_offset, to_copy);
        }
        const int64_t_ delta = reinterpret_cast<int64_t_>(mapped) -
                               static_cast<int64_t_>(headers.image_base);
        if (!apply_relocations(destination, headers, delta)) {
            status = STATUS_INVALID_IMAGE_FORMAT;
            ExFreePoolWithTag(mapped, kMapperTag);
            goto fail;
        }
        if (!resolve_imports(destination, headers)) {
            status = STATUS_NOT_FOUND;
            ExFreePoolWithTag(mapped, kMapperTag);
            goto fail;
        }
        /* Bisect switch: false = map only (proves the PE mapping path). */
        constexpr bool kCallEntry = false;
        if (kCallEntry && g_bootstrap != nullptr) {
            status = call_entry(destination, headers, g_bootstrap);
            if (!NT_SUCCESS(status)) {
                DbgPrint("[drvlite-loader] entry failed 0x%x\n", status);
                ExFreePoolWithTag(mapped, kMapperTag);
                goto fail;
            }
        }
        g_mapped = mapped;
        DbgPrint("[drvlite-loader] mapped at %p (method %u)\n", mapped, method);
    }
    ExFreePoolWithTag(image, kMapperTag);
    return STATUS_SUCCESS;

fail:
    if (g_bootstrap != nullptr) {
        ExFreePoolWithTag(g_bootstrap, kMapperTag);
        g_bootstrap = nullptr;
    }
    ExFreePoolWithTag(image, kMapperTag);
    return status;
}

}  // namespace

namespace {

/* Map and call from a system thread: DriverEntry returns immediately so the
 * service manager never waits on the mapped driver's own startup
 * handshake. */
VOID map_thread(PVOID context) {
    UNREFERENCED_PARAMETER(context);
    const NTSTATUS status = load_and_call();
    DbgPrint("[drvlite-loader] map thread status=0x%x\n", status);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

}  // namespace

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object,
                                PUNICODE_STRING registry_path) {
    UNREFERENCED_PARAMETER(registry_path);
    driver_object->DriverUnload = &unload;
    HANDLE thread = nullptr;
    const NTSTATUS status =
        PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, nullptr, nullptr,
                             nullptr, map_thread, nullptr);
    if (NT_SUCCESS(status) && thread != nullptr) {
        ZwClose(thread);
    }
    return STATUS_SUCCESS;
}
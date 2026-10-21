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

void unload(PDRIVER_OBJECT driver_object);
NTSTATUS create_close(PDEVICE_OBJECT device, PIRP irp);
NTSTATUS device_control(PDEVICE_OBJECT device, PIRP irp);

}  // namespace

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object,
                                PUNICODE_STRING registry_path) {
    UNREFERENCED_PARAMETER(registry_path);
    KeInitializeSpinLock(&g_mapped_lock);
    UNICODE_STRING device_name;
    RtlInitUnicodeString(&device_name, L"\\Device\\DrvliteMapper");
    PDEVICE_OBJECT device = nullptr;
    NTSTATUS status = IoCreateDevice(
        driver_object, sizeof(mapper_device_extension), &device_name,
        DRVLITE_MAPPER_DEVICE_TYPE, 0, FALSE, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    device->Flags |= DO_BUFFERED_IO;
    auto* extension =
        static_cast<mapper_device_extension*>(device->DeviceExtension);
    extension->device = device;
    UNICODE_STRING symlink;
    RtlInitUnicodeString(&symlink, L"\\??\\DrvliteMapper");
    status = IoCreateSymbolicLink(&symlink, &device_name);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(device);
        return status;
    }
    driver_object->MajorFunction[IRP_MJ_CREATE] = &create_close;
    driver_object->MajorFunction[IRP_MJ_CLOSE] = &create_close;
    driver_object->MajorFunction[IRP_MJ_DEVICE_CONTROL] = &device_control;
    driver_object->DriverUnload = &unload;
    DbgPrint("[drvlite-mapper] ready\n");
    return STATUS_SUCCESS;
}

namespace {

void unload(PDRIVER_OBJECT driver_object) {
    UNICODE_STRING symlink;
    RtlInitUnicodeString(&symlink, L"\\??\\DrvliteMapper");
    IoDeleteSymbolicLink(&symlink);
    if (driver_object->DeviceObject != nullptr) {
        IoDeleteDevice(driver_object->DeviceObject);
    }
}

NTSTATUS create_close(PDEVICE_OBJECT device, PIRP irp) {
    UNREFERENCED_PARAMETER(device);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS complete_irp(PIRP irp, NTSTATUS status, ULONG information) {
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = information;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS device_control(PDEVICE_OBJECT device, PIRP irp) {
    UNREFERENCED_PARAMETER(device);
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    if (stack->MajorFunction != IRP_MJ_DEVICE_CONTROL) {
        return complete_irp(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
    if (stack->Parameters.DeviceIoControl.IoControlCode ==
        IOCTL_DRVLITE_MAPPER_UNMAP) {
        const ULONG input_size =
            stack->Parameters.DeviceIoControl.InputBufferLength;
        const ULONG output_size =
            stack->Parameters.DeviceIoControl.OutputBufferLength;
        if (irp->AssociatedIrp.SystemBuffer == nullptr ||
            input_size < sizeof(drvlite_unmap_request) ||
            output_size < sizeof(drvlite_unmap_response)) {
            return complete_irp(irp, STATUS_BUFFER_TOO_SMALL, 0);
        }
        const auto* request = static_cast<const drvlite_unmap_request*>(
            irp->AssociatedIrp.SystemBuffer);
        auto* response = static_cast<drvlite_unmap_response*>(
            irp->AssociatedIrp.SystemBuffer);
        const qword_t base = request->mapped_base;
        NTSTATUS status = STATUS_NOT_FOUND;
        if (base != 0 && base >= 0xFFFF800000000000ULL) {
            KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g_mapped_lock);
            for (dword_t index = 0; index < kMaxMappedImages; ++index) {
                if (g_mapped_images[index].base == base) {
                    g_mapped_images[index].base = 0;
                    g_mapped_images[index].size = 0;
                    status = STATUS_SUCCESS;
                    break;
                }
            }
            KeReleaseSpinLock(&g_mapped_lock, irql);
            if (NT_SUCCESS(status)) {
                ExFreePoolWithTag(reinterpret_cast<void*>(base), kMapperTag);
            }
        }
        RtlZeroMemory(response, sizeof(*response));
        response->status = static_cast<unsigned int>(status);
        return complete_irp(irp, status, sizeof(*response));
    }
    if (stack->Parameters.DeviceIoControl.IoControlCode !=
        IOCTL_DRVLITE_MAPPER_MAP) {
        return complete_irp(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
    const ULONG input_size =
        stack->Parameters.DeviceIoControl.InputBufferLength;
    const ULONG output_size =
        stack->Parameters.DeviceIoControl.OutputBufferLength;
    if (irp->AssociatedIrp.SystemBuffer == nullptr ||
        input_size < sizeof(drvlite_map_request) ||
        output_size < sizeof(drvlite_map_response)) {
        return complete_irp(irp, STATUS_BUFFER_TOO_SMALL, 0);
    }
    const auto* request = static_cast<const drvlite_map_request*>(
        irp->AssociatedIrp.SystemBuffer);
    if (request->image_size == 0 || request->image_size > kMaxImageSize ||
        request->bootstrap_size == 0 || request->bootstrap_size > 0x1000) {
        return complete_irp(irp, STATUS_INVALID_PARAMETER, 0);
    }
    if (request->image_offset != sizeof(drvlite_map_request) ||
        request->bootstrap_offset <
            request->image_offset + request->image_size ||
        request->bootstrap_offset + request->bootstrap_size > input_size) {
        return complete_irp(irp, STATUS_INVALID_PARAMETER, 0);
    }
    const auto* image_bytes =
        static_cast<const byte_t*>(irp->AssociatedIrp.SystemBuffer) +
        request->image_offset;
    const auto* bootstrap_bytes =
        static_cast<const byte_t*>(irp->AssociatedIrp.SystemBuffer) +
        request->bootstrap_offset;

    /* The entry point must see a kernel copy of the bootstrap, never the
     * system buffer. Keep it alive for the lifetime of the image. */
    void* kernel_bootstrap = ExAllocatePool2(
        POOL_FLAG_NON_PAGED, request->bootstrap_size, kMapperTag);
    if (kernel_bootstrap == nullptr) {
        return complete_irp(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    }
    RtlCopyMemory(kernel_bootstrap, bootstrap_bytes, request->bootstrap_size);

    pe_headers headers{};
    if (!parse_headers(image_bytes, request->image_size, &headers)) {
        return complete_irp(irp, STATUS_INVALID_IMAGE_FORMAT, 0);
    }
    const dword_t image_size = headers.size_of_image;
    void* mapped =
        ExAllocatePool2(POOL_FLAG_NON_PAGED_EXECUTE, image_size, kMapperTag);
    dword_t method = 1;
    if (mapped == nullptr) {
        mapped = ExAllocatePool2(POOL_FLAG_NON_PAGED, image_size, kMapperTag);
        method = 2;
    }
    if (mapped == nullptr) {
        return complete_irp(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    }
    RtlZeroMemory(mapped, image_size);
    auto* destination = static_cast<byte_t*>(mapped);

    const dword_t headers_size = headers.size_of_headers < image_size
                                     ? headers.size_of_headers
                                     : image_size;
    RtlCopyMemory(destination, image_bytes, headers_size);
    for (dword_t index = 0; index < headers.section_count; ++index) {
        const section_record& section = headers.sections[index];
        if (section.virtual_address >= image_size) {
            continue;
        }
        dword_t to_copy = section.raw_size;
        if (to_copy > section.virtual_size) {
            to_copy = section.virtual_size;
        }
        if (section.virtual_address + to_copy > image_size) {
            to_copy = image_size - section.virtual_address;
        }
        if (to_copy == 0) {
            continue;
        }
        if (section.raw_offset + to_copy > request->image_size) {
            return complete_irp(irp, STATUS_INVALID_IMAGE_FORMAT, 0);
        }
        RtlCopyMemory(destination + section.virtual_address,
                      image_bytes + section.raw_offset, to_copy);
    }

    const int64_t_ delta = reinterpret_cast<int64_t_>(mapped) -
                           static_cast<int64_t_>(headers.image_base);
    if (!apply_relocations(destination, headers, delta)) {
        return complete_irp(irp, STATUS_INVALID_IMAGE_FORMAT, 0);
    }
    if (!resolve_imports(destination, headers)) {
        return complete_irp(irp, STATUS_NOT_FOUND, 0);
    }

    const NTSTATUS entry_status =
        call_entry(destination, headers, kernel_bootstrap);
    auto* response =
        static_cast<drvlite_map_response*>(irp->AssociatedIrp.SystemBuffer);
    RtlZeroMemory(response, sizeof(*response));
    response->status = static_cast<unsigned int>(entry_status);
    response->method = method;
    response->mapped_base = reinterpret_cast<unsigned long long>(mapped);
    response->image_size = image_size;
    if (!NT_SUCCESS(entry_status)) {
        /* The driver failed to initialise; do not leave a mapped image. */
        ExFreePoolWithTag(mapped, kMapperTag);
        return complete_irp(irp, entry_status, sizeof(*response));
    }
    {
        KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g_mapped_lock);
        for (dword_t index = 0; index < kMaxMappedImages; ++index) {
            if (g_mapped_images[index].base == 0) {
                g_mapped_images[index].base = reinterpret_cast<qword_t>(mapped);
                g_mapped_images[index].size = image_size;
                break;
            }
        }
        KeReleaseSpinLock(&g_mapped_lock, irql);
    }
    DbgPrint("[drvlite-mapper] mapped at %p (method %u), entry status 0x%x\n",
             mapped, method, entry_status);
    return complete_irp(irp, STATUS_SUCCESS, sizeof(*response));
}

}  // namespace

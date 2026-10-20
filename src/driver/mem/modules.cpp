#include "modules.h"

#include "../state.h"
#include "../util.h"
#include "kva.h"
#include "phys.h"
#include "process.h"

namespace drvlite {
namespace {

/* KLDR_DATA_TABLE_ENTRY (stable x64 layout, only the read fields). */
struct kldr_data_table_entry {
    LIST_ENTRY in_load_order_links;   /* 0x00 */
    LIST_ENTRY in_memory_order_links; /* 0x10 */
    LIST_ENTRY in_init_order_links;   /* 0x20 */
    PVOID dll_base;                   /* 0x30 */
    PVOID entry_point;                /* 0x38 */
    ULONG size_of_image;              /* 0x40 */
    UNICODE_STRING full_dll_name;     /* 0x48 */
    UNICODE_STRING base_dll_name;     /* 0x58 */
};

static_assert(sizeof(kldr_data_table_entry) == 0x68,
              "KLDR_DATA_TABLE_ENTRY layout changed");

/* PEB / PEB_LDR_DATA / LDR_DATA_TABLE_ENTRY offsets used by the CR3 walk. */
constexpr u64 kPebLdr = 0x18;
constexpr u64 kLdrInLoadOrderList = 0x10;
constexpr u64 kLdrEntryInLoadOrderLinks = 0x00;
constexpr u64 kLdrEntryDllBase = 0x30;
constexpr u64 kLdrEntrySizeOfImage = 0x40;
constexpr u64 kLdrEntryFullDllName = 0x48;
constexpr u64 kLdrEntryBaseDllName = 0x58;

const char* base_name_of(const char* path) {
    const char* base = path;
    for (const char* cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            base = cursor + 1;
        }
    }
    return base;
}

}  // namespace

u32 kernel_modules(module_entry* output, u32 capacity) {
    if (output == nullptr || capacity == 0 || PsLoadedModuleList == nullptr) {
        return 0;
    }
    u32 count = 0;
    __try {
        for (PLIST_ENTRY link = PsLoadedModuleList->Flink;
             link != PsLoadedModuleList && count < capacity;
             link = link->Flink) {
            auto* entry = CONTAINING_RECORD(link, kldr_data_table_entry,
                                            in_load_order_links);
            module_entry& item = output[count];
            item.base = reinterpret_cast<u64>(entry->dll_base);
            item.size = entry->size_of_image;
            item.flags = 0;
            narrow_copy(entry->full_dll_name, item.name, sizeof(item.name));
            if (item.name[0] == '\0') {
                narrow_copy(entry->base_dll_name, item.name, sizeof(item.name));
            }
            ++count;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return count;
    }
    return count;
}

bool kernel_module_find(const char* base_name, u64* base, u32* size) {
    if (base_name == nullptr || base == nullptr || size == nullptr ||
        PsLoadedModuleList == nullptr) {
        return false;
    }
    bool found = false;
    __try {
        for (PLIST_ENTRY link = PsLoadedModuleList->Flink;
             link != PsLoadedModuleList; link = link->Flink) {
            auto* entry = CONTAINING_RECORD(link, kldr_data_table_entry,
                                            in_load_order_links);
            char name[DRVLITE_MODULE_NAME_MAX] = {};
            narrow_copy(entry->base_dll_name, name, sizeof(name));
            if (name[0] == '\0') {
                narrow_copy(entry->full_dll_name, name, sizeof(name));
            }
            const char* candidate = base_name_of(name);
            bool equal = true;
            usize index = 0;
            for (; base_name[index] != '\0'; ++index) {
                if (candidate[index] != ascii_lower(base_name[index])) {
                    equal = false;
                    break;
                }
            }
            if (equal && candidate[index] == '\0') {
                *base = reinterpret_cast<u64>(entry->dll_base);
                *size = entry->size_of_image;
                found = true;
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return found;
}

u32 process_modules(PEPROCESS process, u64 cr3, module_entry* output,
                    u32 capacity) {
    if (process == nullptr || cr3 == 0 || output == nullptr || capacity == 0) {
        return 0;
    }
    const u64 peb = reinterpret_cast<u64>(PsGetProcessPeb(process));
    if (!is_user_pointer(peb)) {
        return 0;
    }
    const core::phys_reader reader = core_phys_reader();
    u64 ldr = 0;
    if (!core::read_u64(reader, cr3, peb + kPebLdr, &ldr) ||
        !is_user_pointer(ldr)) {
        return 0;
    }
    const u64 head = ldr + kLdrInLoadOrderList;
    u64 link = 0;
    if (!core::read_u64(reader, cr3, head, &link)) {
        return 0;
    }
    u32 count = 0;
    for (u32 guard = 0; guard < 1024 && count < capacity; ++guard) {
        if (link == head) {
            break;
        }
        const u64 entry = link - kLdrEntryInLoadOrderLinks;
        module_entry& item = output[count];
        u64 dll_base = 0;
        u32 size_of_image = 0;
        if (!core::read_u64(reader, cr3, entry + kLdrEntryDllBase, &dll_base) ||
            !core::read_u64(reader, cr3, entry + kLdrEntrySizeOfImage,
                            reinterpret_cast<u64*>(&size_of_image))) {
            break;
        }
        /* Read the base name (UNICODE_STRING: length/max/buffer). */
        u16 name_length = 0;
        if (!core::read_virtual(reader, cr3, entry + kLdrEntryBaseDllName,
                                &name_length, sizeof(name_length), false)
                 .copied) {
            break;
        }
        u64 name_buffer = 0;
        if (!core::read_u64(reader, cr3, entry + kLdrEntryBaseDllName + 8,
                            &name_buffer)) {
            break;
        }
        item.base = dll_base;
        item.size = size_of_image;
        item.flags = 0;
        item.name[0] = '\0';
        if (name_length >= 2 && name_length <= 2 * 255 &&
            is_user_pointer(name_buffer)) {
            u16 wide[256] = {};
            if (core::read_virtual(reader, cr3, name_buffer, wide, name_length,
                                   false)
                    .copied == name_length) {
                UNICODE_STRING name{};
                name.Buffer = reinterpret_cast<PWCH>(wide);
                name.Length = name_length;
                name.MaximumLength = name_length;
                narrow_copy(name, item.name, sizeof(item.name));
            }
        }
        ++count;
        u64 next = 0;
        if (!core::read_u64(reader, cr3, link, &next)) {
            break;
        }
        link = next;
    }
    return count;
}

bool module_dump(u32 pid, u64 address, void* output, u32 size, u32* copied) {
    if (copied != nullptr) {
        *copied = 0;
    }
    if (output == nullptr || size == 0) {
        return false;
    }
    if (pid == 0) {
        core::read_result result;
        if (!read_kernel_tolerant(address, output, size, &result)) {
            return false;
        }
        if (copied != nullptr) {
            *copied = static_cast<u32>(result.copied);
        }
        return result.error == core::walk_code::ok || result.copied != 0 ||
               result.zero_filled == size;
    }
    process_ref process;
    if (!process_open(pid, &process)) {
        return false;
    }
    core::read_result result;
    const bool ok =
        process_read(&process, address, output, size, true, &result);
    process_close(&process);
    if (copied != nullptr) {
        *copied = static_cast<u32>(result.copied);
    }
    return ok;
}

}  // namespace drvlite

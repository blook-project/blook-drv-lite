#include "dispatch.h"

#include "dse/dse.h"
#include "handle/handle.h"
#include "hook/infinity.h"
#include "ipc/adapter.h"
#include "mem/kva.h"
#include "mem/modules.h"
#include "mem/phys.h"
#include "mem/process.h"
#include "state.h"

namespace drvlite {
namespace {

constexpr u32 kResponseHeaderSize = sizeof(drvlite_rsp_header);

kc_status transport_status(u32 status) {
    if (status <= static_cast<u32>(KC_ERR_UNSUPPORTED)) {
        return static_cast<kc_status>(status);
    }
    return KC_ERR_INTERNAL;
}

bool request_ok(const void* input, u32 input_length, u16 opcode,
                u32 expected_size) {
    const core::header_check check =
        core::check_request(input, input_length, opcode, expected_size);
    return check.code == core::codec_code::ok;
}

bool capacity_ok(u32 value, u32 needed) {
    return value >= needed;
}

void fill_header(void* output, u16 opcode, u32 size, u32 status) {
    auto* header = static_cast<drvlite_rsp_header*>(output);
    header->magic = DRVLITE_RSP_MAGIC;
    header->abi_major = DRVLITE_ABI_MAJOR;
    header->abi_minor = DRVLITE_ABI_MINOR;
    header->op = opcode;
    header->reserved = 0;
    header->status = status;
    header->size = size;
}

kc_status finish(void* output, u32* output_length, u16 opcode, u32 size,
                 u32 status) {
    fill_header(output, opcode, size, status);
    *output_length = size;
    return transport_status(status);
}

kc_status fail(u32* output_length, u32 status) {
    if (output_length != nullptr) {
        *output_length = 0;
    }
    return transport_status(status);
}

/* Validates a canonical user range: both ends must be user addresses. */
bool user_range(u64 address, u32 size) {
    if (size == 0) {
        return false;
    }
    if (!is_user_pointer(address)) {
        return false;
    }
    return is_user_pointer(address + size - 1);
}

bool kernel_range(u64 address, u32 size) {
    if (size == 0) {
        return false;
    }
    if (!is_kernel_pointer(address)) {
        return false;
    }
    return is_kernel_pointer(address + size - 1);
}

/* ---------------- individual opcodes ---------------- */

kc_status query_info(const void* input, u32 input_length, void* output,
                     u32* output_capacity, u32* output_length) {
    if (!request_ok(input, input_length, DRVLITE_OP_QUERY_INFO,
                    sizeof(drvlite_query_info_req))) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const u32 need = sizeof(drvlite_query_info_rsp);
    if (!capacity_ok(*output_capacity, need)) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    auto* response = static_cast<drvlite_query_info_rsp*>(output);
    RtlZeroMemory(response, need);
    const driver_state& global = state();
    response->nt_build = global.nt_build;
    response->abi = (DRVLITE_ABI_MAJOR << 16) | DRVLITE_ABI_MINOR;
    response->features = global.features;
    response->driver_base = global.driver_base;
    response->driver_size = global.driver_size;
    response->kernel_base = global.kernel_base;
    response->kernel_size = global.kernel_size;
    response->client_eprocess = as_u64(global.client_process);
    response->client_pid = global.client_pid;
    response->test_page_va = as_u64(global.test_page);
    response->test_page_pa = global.test_page_pa;
    return finish(output, output_length, DRVLITE_OP_QUERY_INFO, need,
                  DRVLITE_OK);
}

kc_status mem_read(bool kernel, const void* input, u32 input_length,
                   void* output, u32* output_capacity, u32* output_length,
                   u16 opcode) {
    if (!request_ok(input, input_length, opcode, sizeof(drvlite_mem_req))) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const auto* request = static_cast<const drvlite_mem_req*>(input);
    if (request->size == 0 || request->size > DRVLITE_MAX_TRANSFER) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    if (kernel ? !kernel_range(request->address, request->size)
               : !core::is_canonical(request->address)) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const u32 data_offset = static_cast<u32>(offsetof(drvlite_mem_rsp, data));
    const u32 need = data_offset + request->size;
    if (!capacity_ok(*output_capacity, need)) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    auto* response = static_cast<drvlite_mem_rsp*>(output);
    u8* data = reinterpret_cast<u8*>(response) + data_offset;
    const bool ok = kernel ? read_kernel(request->address, data, request->size)
                           : phys_read(request->address, data, request->size);
    if (!ok) {
        return fail(output_length, DRVLITE_ERR_IO);
    }
    response->address = request->address;
    response->size = request->size;
    response->flags = 0;
    return finish(output, output_length, opcode, need, DRVLITE_OK);
}

kc_status mem_write(bool kernel, const void* input, u32 input_length,
                    void* output, u32* output_capacity, u32* output_length,
                    u16 opcode) {
    if (!request_ok(input, input_length, opcode, sizeof(drvlite_mem_req))) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const auto* request = static_cast<const drvlite_mem_req*>(input);
    if (request->size == 0 || request->size > DRVLITE_MAX_TRANSFER) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    if (input_length != sizeof(drvlite_mem_req) + request->size) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const u8* data =
        reinterpret_cast<const u8*>(request) + sizeof(drvlite_mem_req);
    const u32 need = static_cast<u32>(offsetof(drvlite_mem_rsp, data));
    if (!capacity_ok(*output_capacity, need)) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    bool ok = false;
    if (kernel) {
        ok = kernel_range(request->address, request->size) &&
             write_kernel(request->address, data, request->size);
    } else {
        u64 end = 0;
        ok = core::checked_add(request->address, request->size, &end) &&
             phys_write(request->address, data, request->size);
    }
    if (!ok) {
        return fail(output_length, DRVLITE_ERR_IO);
    }
    auto* response = static_cast<drvlite_mem_rsp*>(output);
    response->address = request->address;
    response->size = request->size;
    response->flags = 0;
    return finish(output, output_length, opcode, need, DRVLITE_OK);
}

kc_status process_mem(bool write, const void* input, u32 input_length,
                      void* output, u32* output_capacity, u32* output_length,
                      u16 opcode) {
    if (!request_ok(input, input_length, opcode,
                    sizeof(drvlite_process_mem_req))) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const auto* request = static_cast<const drvlite_process_mem_req*>(input);
    if (request->size == 0 || request->size > DRVLITE_MAX_TRANSFER ||
        !user_range(request->address, request->size)) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    process_ref process;
    if (!process_open(request->pid, &process)) {
        return fail(output_length, DRVLITE_ERR_NOT_FOUND);
    }
    u32 status = DRVLITE_OK;
    if (write) {
        if (input_length != sizeof(drvlite_process_mem_req) + request->size) {
            process_close(&process);
            return fail(output_length, DRVLITE_ERR_PARAM);
        }
        const u8* data = reinterpret_cast<const u8*>(request) +
                         sizeof(drvlite_process_mem_req);
        if (!process_write(&process, request->address, data, request->size)) {
            status = DRVLITE_ERR_IO;
        }
    } else {
        const u32 data_offset =
            static_cast<u32>(offsetof(drvlite_process_mem_rsp, data));
        const u32 need = data_offset + request->size;
        if (!capacity_ok(*output_capacity, need)) {
            process_close(&process);
            return fail(output_length, DRVLITE_ERR_PARAM);
        }
        auto* response = static_cast<drvlite_process_mem_rsp*>(output);
        u8* data = reinterpret_cast<u8*>(response) + data_offset;
        core::read_result result;
        if (!process_read(&process, request->address, data, request->size,
                          false, &result)) {
            status = DRVLITE_ERR_IO;
        } else {
            response->pid = request->pid;
            response->size = request->size;
            response->address = request->address;
            response->flags = 0;
        }
        if (status != KC_OK) {
            process_close(&process);
            return fail(output_length, status);
        }
        process_close(&process);
        return finish(output, output_length, opcode, need, DRVLITE_OK);
    }
    process_close(&process);
    if (status != KC_OK) {
        return fail(output_length, status);
    }
    const u32 need = static_cast<u32>(offsetof(drvlite_process_mem_rsp, data));
    if (!capacity_ok(*output_capacity, need)) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    auto* response = static_cast<drvlite_process_mem_rsp*>(output);
    response->pid = request->pid;
    response->size = request->size;
    response->address = request->address;
    response->flags = 0;
    return finish(output, output_length, opcode, need, DRVLITE_OK);
}

kc_status batch_read(const void* input, u32 input_length, void* output,
                     u32* output_capacity, u32* output_length) {
    if (!request_ok(input, input_length, DRVLITE_OP_MEM_BATCH_READ,
                    sizeof(drvlite_batch_read_req))) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const auto* request = static_cast<const drvlite_batch_read_req*>(input);
    if (request->count == 0 || request->count > DRVLITE_MAX_BATCH_RANGES) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    u64 total = 0;
    if (!core::batch_total(request->ranges, request->count, &total)) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    for (u32 index = 0; index < request->count; ++index) {
        if (!user_range(request->ranges[index].address,
                        request->ranges[index].size)) {
            return fail(output_length, DRVLITE_ERR_PARAM);
        }
    }
    const u32 data_offset =
        static_cast<u32>(offsetof(drvlite_batch_read_rsp, data));
    const u32 need = data_offset + static_cast<u32>(total);
    if (!capacity_ok(*output_capacity, need)) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    auto* response = static_cast<drvlite_batch_read_rsp*>(output);
    response->pid = request->pid;
    response->count = request->count;
    response->total = static_cast<u32>(total);
    response->failed = 0;
    u8* data = reinterpret_cast<u8*>(response) + data_offset;
    process_ref process;
    const bool process_ok = process_open(request->pid, &process);
    u32 offset = 0;
    for (u32 index = 0; index < request->count; ++index) {
        const drvlite_range& range = request->ranges[index];
        bool ok = false;
        if (process_ok) {
            core::read_result result;
            ok = process_read(&process, range.address, data + offset,
                              range.size, false, &result);
        }
        if (!ok) {
            response->failed += 1;
            response->status[index] = DRVLITE_ERR_RANGE;
            RtlZeroMemory(data + offset, range.size);
        } else {
            response->status[index] = DRVLITE_OK;
        }
        offset += range.size;
    }
    if (process_ok) {
        process_close(&process);
    }
    return finish(output, output_length, DRVLITE_OP_MEM_BATCH_READ, need,
                  DRVLITE_OK);
}

kc_status process_query(const void* input, u32 input_length, void* output,
                        u32* output_capacity, u32* output_length) {
    if (!request_ok(input, input_length, DRVLITE_OP_MEM_PROCESS_QUERY,
                    sizeof(drvlite_process_query_req))) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const auto* request = static_cast<const drvlite_process_query_req*>(input);
    process_ref process;
    if (!process_open(request->pid, &process)) {
        return fail(output_length, DRVLITE_ERR_NOT_FOUND);
    }
    const u32 need = sizeof(drvlite_process_query_rsp);
    if (!capacity_ok(*output_capacity, need)) {
        process_close(&process);
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const bool resolved = process_resolve(&process);
    auto* response = static_cast<drvlite_process_query_rsp*>(output);
    RtlZeroMemory(response, need);
    response->pid = process.pid;
    response->flags = resolved ? DRVLITE_PROCESS_FLAG_CR3_VALIDATED : 0;
    response->eprocess = as_u64(process.process);
    response->cr3 = process.cr3;
    response->peb = process.peb;
    response->image_base = process.image_base;
    response->image_size = process.image_size;
    process_close(&process);
    return finish(output, output_length, DRVLITE_OP_MEM_PROCESS_QUERY, need,
                  resolved ? DRVLITE_OK : DRVLITE_ERR_NOT_FOUND);
}

kc_status module_list(const void* input, u32 input_length, void* output,
                      u32* output_capacity, u32* output_length) {
    if (!request_ok(input, input_length, DRVLITE_OP_MODULE_LIST,
                    sizeof(drvlite_module_list_req))) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const auto* request = static_cast<const drvlite_module_list_req*>(input);
    const u32 offset =
        static_cast<u32>(offsetof(drvlite_module_list_rsp, modules));
    if (*output_capacity < offset) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    u32 capacity = (*output_capacity - offset) / sizeof(drvlite_module_info);
    if (request->max_count != 0 && request->max_count < capacity) {
        capacity = request->max_count;
    }
    if (capacity == 0) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    auto* response = static_cast<drvlite_module_list_rsp*>(output);
    response->pid = request->pid;
    u32 count = 0;
    if (request->pid == 0) {
        count = kernel_modules(
            reinterpret_cast<module_entry*>(response->modules), capacity);
    } else {
        process_ref process;
        if (!process_open(request->pid, &process)) {
            return fail(output_length, DRVLITE_ERR_NOT_FOUND);
        }
        u32 modules = 0;
        if (process_resolve(&process)) {
            modules = process_modules(
                process.process, process.cr3,
                reinterpret_cast<module_entry*>(response->modules), capacity);
        }
        process_close(&process);
        count = modules;
    }
    response->count = count;
    response->total = count;
    response->flags = 0;
    const u32 need = offset + count * sizeof(drvlite_module_info);
    return finish(output, output_length, DRVLITE_OP_MODULE_LIST, need,
                  DRVLITE_OK);
}

kc_status module_dump_op(const void* input, u32 input_length, void* output,
                         u32* output_capacity, u32* output_length) {
    if (!request_ok(input, input_length, DRVLITE_OP_MODULE_DUMP,
                    sizeof(drvlite_module_dump_req))) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const auto* request = static_cast<const drvlite_module_dump_req*>(input);
    if (request->size == 0 || request->size > DRVLITE_MAX_TRANSFER) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const u32 data_offset =
        static_cast<u32>(offsetof(drvlite_module_dump_rsp, data));
    const u32 need = data_offset + request->size;
    if (!capacity_ok(*output_capacity, need)) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    auto* response = static_cast<drvlite_module_dump_rsp*>(output);
    u8* data = reinterpret_cast<u8*>(response) + data_offset;
    u32 copied = 0;
    RtlZeroMemory(data, request->size);
    if (!module_dump(request->pid, request->address, data, request->size,
                     &copied)) {
        return fail(output_length, DRVLITE_ERR_IO);
    }
    response->pid = request->pid;
    response->size = request->size;
    response->copied = copied;
    response->flags = 0;
    response->address = request->address;
    return finish(output, output_length, DRVLITE_OP_MODULE_DUMP, need,
                  DRVLITE_OK);
}

kc_status dse_query(const void* input, u32 input_length, void* output,
                    u32* output_capacity, u32* output_length, u16 opcode) {
    if (!request_ok(input, input_length, opcode, sizeof(drvlite_dse_req))) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const u32 need = sizeof(drvlite_dse_rsp);
    if (!capacity_ok(*output_capacity, need)) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    auto* response = static_cast<drvlite_dse_rsp*>(output);
    RtlZeroMemory(response, need);
    dse_fill_status(response);
    return finish(output, output_length, opcode, need, DRVLITE_OK);
}

kc_status dse_control(const void* input, u32 input_length, void* output,
                      u32* output_capacity, u32* output_length, u16 opcode) {
    if (!request_ok(input, input_length, opcode, sizeof(drvlite_dse_req))) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const auto* request = static_cast<const drvlite_dse_req*>(input);
    if (!dse_available()) {
        return fail(output_length, DRVLITE_ERR_UNSUPPORTED);
    }
    const bool ok =
        opcode == DRVLITE_OP_DSE_SET ? dse_set(request->value) : dse_restore();
    const u32 need = sizeof(drvlite_dse_rsp);
    if (!capacity_ok(*output_capacity, need)) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    auto* response = static_cast<drvlite_dse_rsp*>(output);
    RtlZeroMemory(response, need);
    dse_fill_status(response);
    return finish(output, output_length, opcode, need,
                  ok ? DRVLITE_OK : DRVLITE_ERR_VERIFY);
}

kc_status handle_open_op(const void* input, u32 input_length, void* output,
                         u32* output_capacity, u32* output_length) {
    if (!request_ok(input, input_length, DRVLITE_OP_HANDLE_OPEN,
                    sizeof(drvlite_handle_open_req))) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const auto* request = static_cast<const drvlite_handle_open_req*>(input);
    const u32 need = sizeof(drvlite_handle_open_rsp);
    if (!capacity_ok(*output_capacity, need)) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    handle_result result;
    if (!handle_open(request->pid, request->type, request->tid, request->access,
                     &result)) {
        return fail(output_length, DRVLITE_ERR_ACCESS);
    }
    auto* response = static_cast<drvlite_handle_open_rsp*>(output);
    RtlZeroMemory(response, need);
    response->handle = result.handle;
    response->object = result.object;
    response->requested = result.requested;
    response->granted = result.granted;
    return finish(output, output_length, DRVLITE_OP_HANDLE_OPEN, need,
                  DRVLITE_OK);
}

kc_status handle_close_op(const void* input, u32 input_length, void* output,
                          u32* output_capacity, u32* output_length) {
    if (!request_ok(input, input_length, DRVLITE_OP_HANDLE_CLOSE,
                    sizeof(drvlite_handle_close_req))) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const auto* request = static_cast<const drvlite_handle_close_req*>(input);
    const u32 need = sizeof(drvlite_handle_close_rsp);
    if (!capacity_ok(*output_capacity, need)) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const bool closed = handle_close(request->pid, request->handle);
    auto* response = static_cast<drvlite_handle_close_rsp*>(output);
    RtlZeroMemory(response, need);
    response->closed = closed ? 1u : 0u;
    return finish(output, output_length, DRVLITE_OP_HANDLE_CLOSE, need,
                  closed ? DRVLITE_OK : DRVLITE_ERR_IO);
}

kc_status infinity_status_op(const void* input, u32 input_length, void* output,
                             u32* output_capacity, u32* output_length) {
    if (!request_ok(input, input_length, DRVLITE_OP_INFINITY_STATUS,
                    sizeof(drvlite_infinity_remove_req))) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const u32 need = sizeof(drvlite_infinity_status_rsp);
    if (!capacity_ok(*output_capacity, need)) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    auto* response = static_cast<drvlite_infinity_status_rsp*>(output);
    RtlZeroMemory(response, need);
    infinity_fill_status(response);
    return finish(output, output_length, DRVLITE_OP_INFINITY_STATUS, need,
                  DRVLITE_OK);
}

kc_status infinity_install_op(const void* input, u32 input_length, void* output,
                              u32* output_capacity, u32* output_length) {
    if (!request_ok(input, input_length, DRVLITE_OP_INFINITY_INSTALL,
                    sizeof(drvlite_infinity_install_req))) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const auto* request =
        static_cast<const drvlite_infinity_install_req*>(input);
    if (request->filter_count > DRVLITE_MAX_SSN_FILTER) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const u32 need = sizeof(drvlite_infinity_status_rsp);
    if (!capacity_ok(*output_capacity, need)) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const bool ok =
        infinity_install(request->ssn_filter, request->filter_count);
    auto* response = static_cast<drvlite_infinity_status_rsp*>(output);
    RtlZeroMemory(response, need);
    infinity_fill_status(response);
    return finish(output, output_length, DRVLITE_OP_INFINITY_INSTALL, need,
                  ok ? DRVLITE_OK : DRVLITE_ERR_UNSUPPORTED);
}

kc_status infinity_remove_op(const void* input, u32 input_length, void* output,
                             u32* output_capacity, u32* output_length) {
    if (!request_ok(input, input_length, DRVLITE_OP_INFINITY_REMOVE,
                    sizeof(drvlite_infinity_remove_req))) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const u32 need = sizeof(drvlite_infinity_status_rsp);
    if (!capacity_ok(*output_capacity, need)) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const bool ok = infinity_remove();
    auto* response = static_cast<drvlite_infinity_status_rsp*>(output);
    RtlZeroMemory(response, need);
    infinity_fill_status(response);
    return finish(output, output_length, DRVLITE_OP_INFINITY_REMOVE, need,
                  ok ? DRVLITE_OK : DRVLITE_ERR_INTERNAL);
}

kc_status hwbp_hide_op(const void* input, u32 input_length, void* output,
                       u32* output_capacity, u32* output_length) {
    if (!request_ok(input, input_length, DRVLITE_OP_HWBP_HIDE_SET,
                    sizeof(drvlite_hwbp_hide_req))) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const auto* request = static_cast<const drvlite_hwbp_hide_req*>(input);
    const u32 need = sizeof(drvlite_hwbp_hide_rsp);
    if (!capacity_ok(*output_capacity, need)) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const u32 pid = request->pid != 0 ? request->pid : state().client_pid;
    if (!infinity_set_hide(request->enable != 0, pid,
                           request->get_context_ssn)) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    auto* response = static_cast<drvlite_hwbp_hide_rsp*>(output);
    RtlZeroMemory(response, need);
    response->enabled = request->enable != 0 ? 1u : 0u;
    response->pid = pid;
    response->get_context_ssn = request->get_context_ssn & 0xFFF;
    response->hidden_queries =
        static_cast<u32>(state().infinity.hidden_queries);
    return finish(output, output_length, DRVLITE_OP_HWBP_HIDE_SET, need,
                  DRVLITE_OK);
}

kc_status shield_op(const void* input, u32 input_length, void* output,
                    u32* output_capacity, u32* output_length) {
    if (!request_ok(input, input_length, DRVLITE_OP_SYSCALL_SHIELD_SET,
                    sizeof(drvlite_syscall_shield_req))) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const auto* request = static_cast<const drvlite_syscall_shield_req*>(input);
    if (request->ssn_count > DRVLITE_MAX_SSN_FILTER) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const u32 need = sizeof(drvlite_syscall_shield_rsp);
    if (!capacity_ok(*output_capacity, need)) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const u32 pid = request->pid != 0 ? request->pid : state().client_pid;
    if (!infinity_set_shield(request->enable != 0, pid, request->ssns,
                             request->ssn_count)) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    auto* response = static_cast<drvlite_syscall_shield_rsp*>(output);
    RtlZeroMemory(response, need);
    response->enabled = request->enable != 0 ? 1u : 0u;
    response->pid = pid;
    response->blocked_calls = static_cast<u32>(state().infinity.blocked_calls);
    return finish(output, output_length, DRVLITE_OP_SYSCALL_SHIELD_SET, need,
                  DRVLITE_OK);
}

kc_status shutdown_op(const void* input, u32 input_length, void* output,
                      u32* output_capacity, u32* output_length) {
    if (!request_ok(input, input_length, DRVLITE_OP_SHUTDOWN,
                    sizeof(drvlite_shutdown_req))) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    const u32 need = sizeof(drvlite_shutdown_rsp);
    if (!capacity_ok(*output_capacity, need)) {
        return fail(output_length, DRVLITE_ERR_PARAM);
    }
    cleanup_all();
    auto* response = static_cast<drvlite_shutdown_rsp*>(output);
    RtlZeroMemory(response, need);
    response->stage = 1;
    response->restored = 1;
    const kc_status status =
        finish(output, output_length, DRVLITE_OP_SHUTDOWN, need, DRVLITE_OK);
    /* Destroy the transport from the worker thread (documented safe): the
     * reply is delivered before the worker exits. */
    adapter_stop();
    return status;
}

}  // namespace

kc_status drvlite_dispatch(void* context, u16 opcode, const void* input,
                           u32 input_length, void* output, u32* output_capacity,
                           u32* output_length) {
    UNREFERENCED_PARAMETER(context);
    if (output == nullptr || output_capacity == nullptr ||
        output_length == nullptr) {
        return KC_ERR_PARAM;
    }
    *output_length = 0;
    switch (opcode) {
        case DRVLITE_OP_QUERY_INFO:
            return query_info(input, input_length, output, output_capacity,
                              output_length);
        case DRVLITE_OP_MEM_PHYS_READ:
            return mem_read(false, input, input_length, output, output_capacity,
                            output_length, opcode);
        case DRVLITE_OP_MEM_PHYS_WRITE:
            return mem_write(false, input, input_length, output,
                             output_capacity, output_length, opcode);
        case DRVLITE_OP_MEM_KERNEL_READ:
            return mem_read(true, input, input_length, output, output_capacity,
                            output_length, opcode);
        case DRVLITE_OP_MEM_KERNEL_WRITE:
            return mem_write(true, input, input_length, output, output_capacity,
                             output_length, opcode);
        case DRVLITE_OP_MEM_PROCESS_READ:
            return process_mem(false, input, input_length, output,
                               output_capacity, output_length, opcode);
        case DRVLITE_OP_MEM_PROCESS_WRITE:
            return process_mem(true, input, input_length, output,
                               output_capacity, output_length, opcode);
        case DRVLITE_OP_MEM_BATCH_READ:
            return batch_read(input, input_length, output, output_capacity,
                              output_length);
        case DRVLITE_OP_MEM_PROCESS_QUERY:
            return process_query(input, input_length, output, output_capacity,
                                 output_length);
        case DRVLITE_OP_MODULE_LIST:
            return module_list(input, input_length, output, output_capacity,
                               output_length);
        case DRVLITE_OP_MODULE_DUMP:
            return module_dump_op(input, input_length, output, output_capacity,
                                  output_length);
        case DRVLITE_OP_DSE_QUERY:
            return dse_query(input, input_length, output, output_capacity,
                             output_length, opcode);
        case DRVLITE_OP_DSE_SET:
        case DRVLITE_OP_DSE_RESTORE:
            return dse_control(input, input_length, output, output_capacity,
                               output_length, opcode);
        case DRVLITE_OP_HANDLE_OPEN:
            return handle_open_op(input, input_length, output, output_capacity,
                                  output_length);
        case DRVLITE_OP_HANDLE_CLOSE:
            return handle_close_op(input, input_length, output, output_capacity,
                                   output_length);
        case DRVLITE_OP_INFINITY_STATUS:
            return infinity_status_op(input, input_length, output,
                                      output_capacity, output_length);
        case DRVLITE_OP_INFINITY_INSTALL:
            return infinity_install_op(input, input_length, output,
                                       output_capacity, output_length);
        case DRVLITE_OP_INFINITY_REMOVE:
            return infinity_remove_op(input, input_length, output,
                                      output_capacity, output_length);
        case DRVLITE_OP_HWBP_HIDE_SET:
            return hwbp_hide_op(input, input_length, output, output_capacity,
                                output_length);
        case DRVLITE_OP_SYSCALL_SHIELD_SET:
            return shield_op(input, input_length, output, output_capacity,
                             output_length);
        case DRVLITE_OP_SHUTDOWN:
            return shutdown_op(input, input_length, output, output_capacity,
                               output_length);
        default:
            return KC_ERR_NOT_FOUND;
    }
}

}  // namespace drvlite

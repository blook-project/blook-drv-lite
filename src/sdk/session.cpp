#include <blook/drvlite/sdk.h>

#include <cstring>

namespace drvlite {
namespace {

template <typename T>
const T* as_struct(const std::vector<uint8_t>& buffer) {
    return reinterpret_cast<const T*>(buffer.data());
}

int check_reply(const std::vector<uint8_t>& buffer, uint32_t length,
                uint16_t opcode, uint32_t minimum_size) {
    if (length < minimum_size) {
        return DRVLITE_ERR_PARAM;
    }
    drvlite_rsp_header header{};
    std::memcpy(&header, buffer.data(), sizeof(header));
    if (header.magic != DRVLITE_RSP_MAGIC) {
        return DRVLITE_ERR_MAGIC;
    }
    if (header.abi_major != DRVLITE_ABI_MAJOR ||
        header.abi_minor != DRVLITE_ABI_MINOR) {
        return DRVLITE_ERR_VERSION;
    }
    if (header.op != opcode) {
        return DRVLITE_ERR_PARAM;
    }
    if (header.size < minimum_size || header.size > length) {
        return DRVLITE_ERR_PARAM;
    }
    return static_cast<int>(header.status);
}

void fill_request(drvlite_req_header* header, uint16_t opcode, uint32_t size) {
    header->magic = DRVLITE_REQ_MAGIC;
    header->abi_major = DRVLITE_ABI_MAJOR;
    header->abi_minor = DRVLITE_ABI_MINOR;
    header->op = opcode;
    header->reserved = 0;
    header->size = size;
}

}  // namespace

const char* status_string(int status) {
    return drvlite_status_str(status);
}

const char* version_string() {
    return DRVLITE_VERSION;
}

uint32_t ntdll_syscall_number(const char* export_name) {
    if (export_name == nullptr) {
        return 0;
    }
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll == nullptr) {
        return 0;
    }
    const auto* stub =
        reinterpret_cast<const uint8_t*>(GetProcAddress(ntdll, export_name));
    if (stub == nullptr) {
        return 0;
    }
    /* Windows 10/11: 4C 8B D1 B8 <ssn32> (mov r10, rcx; mov eax, ssn). */
    if (stub[0] == 0x4C && stub[1] == 0x8B && stub[2] == 0xD1 &&
        stub[3] == 0xB8) {
        uint32_t ssn = 0;
        std::memcpy(&ssn, stub + 4, sizeof(ssn));
        return ssn & 0xFFF;
    }
    /* Older builds: B8 <ssn32>. */
    if (stub[0] == 0xB8) {
        uint32_t ssn = 0;
        std::memcpy(&ssn, stub + 1, sizeof(ssn));
        return ssn & 0xFFF;
    }
    return 0;
}

session::session() = default;

session::~session() {
    for (const auto& handle : handles_) {
        if (handle.first != nullptr && handle.first != INVALID_HANDLE_VALUE) {
            CloseHandle(handle.first);
        }
    }
    handles_.clear();
    detach();
}

int session::attach(const attach_options& options) {
    if (client_ != nullptr) {
        return DRVLITE_ERR_ALREADY;
    }
    kc_attach_opts kc_options{};
    kc_options.region_size =
        options.region_size != 0 ? options.region_size : 0x40000;
    kc_options.mode = options.mode;
    kc_options.cipher = options.cipher;
    kc_options.peer_pid = options.peer_pid;
    kc_options.flags = options.flags;
    kc_options.debug_tag = "drvlite-sdk";
    const kc_status status = kc_attach(&kc_options, &client_);
    if (status != KC_OK || client_ == nullptr) {
        client_ = nullptr;
        return static_cast<int>(status);
    }
    if (kc_get_bootstrap(client_, &bootstrap_) != KC_OK) {
        kc_detach(client_);
        client_ = nullptr;
        return DRVLITE_ERR_INTERNAL;
    }
    attached_ = true;
    return DRVLITE_OK;
}

int session::finish(uint32_t timeout_ms) {
    if (client_ == nullptr) {
        return DRVLITE_ERR_STATE;
    }
    const kc_status status = kc_attach_finish(client_, timeout_ms);
    if (status != KC_OK) {
        return static_cast<int>(status);
    }
    active_ = true;
    return DRVLITE_OK;
}

void session::detach() {
    if (client_ != nullptr) {
        kc_detach(client_);
        client_ = nullptr;
    }
    active_ = false;
    attached_ = false;
}

int session::device_call(uint16_t opcode, const void* input,
                         uint32_t input_length, void* output,
                         uint32_t output_capacity, uint32_t* output_length,
                         uint32_t min_reply_size) const {
    if (client_ == nullptr) {
        return DRVLITE_ERR_STATE;
    }
    uint32_t length = 0;
    const kc_status status = kc_call(client_, opcode, input, input_length,
                                     output, output_capacity, &length, 5000);
    if (status != KC_OK) {
        return static_cast<int>(status);
    }
    if (output_length != nullptr) {
        *output_length = length;
    }
    if (min_reply_size != 0 && length < min_reply_size) {
        return DRVLITE_ERR_PARAM;
    }
    return DRVLITE_OK;
}

int session::call(uint16_t opcode, const void* input, uint32_t input_length,
                  void* output, uint32_t output_capacity,
                  uint32_t* output_length, uint32_t timeout_ms) {
    if (client_ == nullptr) {
        return DRVLITE_ERR_STATE;
    }
    uint32_t length = 0;
    const kc_status status =
        kc_call(client_, opcode, input, input_length, output, output_capacity,
                &length, timeout_ms);
    if (status != KC_OK) {
        return static_cast<int>(status);
    }
    if (output_length != nullptr) {
        *output_length = length;
    }
    return DRVLITE_OK;
}

int session::query_info(device_info* out) const {
    if (out == nullptr) {
        return DRVLITE_ERR_PARAM;
    }
    drvlite_query_info_req request{};
    fill_request(&request.hdr, DRVLITE_OP_QUERY_INFO, sizeof(request));
    std::vector<uint8_t> reply(sizeof(drvlite_query_info_rsp));
    const int status =
        device_call(DRVLITE_OP_QUERY_INFO, &request, sizeof(request),
                    reply.data(), static_cast<uint32_t>(reply.size()), nullptr,
                    sizeof(drvlite_query_info_rsp));
    if (status != DRVLITE_OK) {
        return status;
    }
    const drvlite_query_info_rsp* response =
        as_struct<drvlite_query_info_rsp>(reply);
    out->nt_build = response->nt_build;
    out->abi = response->abi;
    out->features = response->features;
    out->driver_base = response->driver_base;
    out->driver_size = response->driver_size;
    out->kernel_base = response->kernel_base;
    out->kernel_size = response->kernel_size;
    out->client_eprocess = response->client_eprocess;
    out->client_pid = response->client_pid;
    return DRVLITE_OK;
}

int session::process_info(uint32_t pid, process_details* out) const {
    if (out == nullptr) {
        return DRVLITE_ERR_PARAM;
    }
    drvlite_process_query_req request{};
    fill_request(&request.hdr, DRVLITE_OP_MEM_PROCESS_QUERY, sizeof(request));
    request.pid = pid;
    std::vector<uint8_t> reply(sizeof(drvlite_process_query_rsp));
    const int status =
        device_call(DRVLITE_OP_MEM_PROCESS_QUERY, &request, sizeof(request),
                    reply.data(), static_cast<uint32_t>(reply.size()), nullptr,
                    sizeof(drvlite_process_query_rsp));
    if (status != DRVLITE_OK) {
        return status;
    }
    const auto* response = as_struct<drvlite_process_query_rsp>(reply);
    out->pid = response->pid;
    out->flags = response->flags;
    out->eprocess = response->eprocess;
    out->cr3 = response->cr3;
    out->peb = response->peb;
    out->image_base = response->image_base;
    out->image_size = response->image_size;
    out->cr3_validated =
        (response->flags & DRVLITE_PROCESS_FLAG_CR3_VALIDATED) != 0;
    return DRVLITE_OK;
}

int session::modules(uint32_t pid,
                     std::vector<drvlite_module_info>* out) const {
    if (out == nullptr) {
        return DRVLITE_ERR_PARAM;
    }
    out->clear();
    constexpr uint32_t kModulesPerReply = 16;
    const uint32_t reply_size =
        static_cast<uint32_t>(offsetof(drvlite_module_list_rsp, modules)) +
        kModulesPerReply * sizeof(drvlite_module_info);
    std::vector<uint8_t> reply(reply_size);
    uint32_t cursor = 0;
    for (;;) {
        drvlite_module_list_req request{};
        fill_request(&request.hdr, DRVLITE_OP_MODULE_LIST, sizeof(request));
        request.pid = pid;
        request.max_count = kModulesPerReply;
        request.flags = cursor;
        uint32_t length = 0;
        const int status = device_call(
            DRVLITE_OP_MODULE_LIST, &request, sizeof(request), reply.data(),
            reply_size, &length, sizeof(drvlite_module_list_rsp));
        if (status != DRVLITE_OK) {
            return status;
        }
        const auto* response = as_struct<drvlite_module_list_rsp>(reply);
        if (length < offsetof(drvlite_module_list_rsp, modules)) {
            return DRVLITE_ERR_PARAM;
        }
        const uint32_t available =
            (length - static_cast<uint32_t>(
                          offsetof(drvlite_module_list_rsp, modules))) /
            sizeof(drvlite_module_info);
        const uint32_t count =
            response->count < available ? response->count : available;
        for (uint32_t index = 0; index < count; ++index) {
            out->push_back(response->modules[index]);
        }
        if (count == 0) {
            break;
        }
        cursor += count;
        if (cursor >= response->total) {
            break;
        }
    }
    return DRVLITE_OK;
}

int session::module_dump(uint32_t pid, uint64_t address, uint32_t size,
                         std::vector<uint8_t>* data,
                         uint32_t* bytes_read) const {
    if (data == nullptr || size == 0 || size > DRVLITE_MAX_TRANSFER) {
        return DRVLITE_ERR_PARAM;
    }
    data->assign(size, 0);
    if (bytes_read != nullptr) {
        *bytes_read = 0;
    }
    drvlite_module_dump_req request{};
    fill_request(&request.hdr, DRVLITE_OP_MODULE_DUMP, sizeof(request));
    request.pid = pid;
    request.address = address;
    request.size = size;
    const uint32_t reply_size =
        static_cast<uint32_t>(offsetof(drvlite_module_dump_rsp, data)) + size;
    std::vector<uint8_t> reply(reply_size);
    uint32_t length = 0;
    const int status = device_call(
        DRVLITE_OP_MODULE_DUMP, &request, sizeof(request), reply.data(),
        reply_size, &length, offsetof(drvlite_module_dump_rsp, data));
    if (status != DRVLITE_OK) {
        return status;
    }
    const auto* response = as_struct<drvlite_module_dump_rsp>(reply);
    const uint32_t available =
        length - static_cast<uint32_t>(offsetof(drvlite_module_dump_rsp, data));
    const uint32_t copy_size =
        response->copied < available ? response->copied : available;
    std::memcpy(data->data(), response->data, copy_size);
    if (bytes_read != nullptr) {
        *bytes_read = response->copied;
    }
    return DRVLITE_OK;
}

int session::read_physical(uint64_t address, void* buffer,
                           uint32_t size) const {
    if (buffer == nullptr || size == 0 || size > DRVLITE_MAX_TRANSFER) {
        return DRVLITE_ERR_PARAM;
    }
    drvlite_mem_req request{};
    fill_request(&request.hdr, DRVLITE_OP_MEM_PHYS_READ, sizeof(request));
    request.address = address;
    request.size = size;
    const uint32_t reply_size =
        static_cast<uint32_t>(offsetof(drvlite_mem_rsp, data)) + size;
    std::vector<uint8_t> reply(reply_size);
    uint32_t length = 0;
    const int status = device_call(DRVLITE_OP_MEM_PHYS_READ, &request,
                                   sizeof(request), reply.data(), reply_size,
                                   &length, offsetof(drvlite_mem_rsp, data));
    if (status != DRVLITE_OK) {
        return status;
    }
    const auto* response = as_struct<drvlite_mem_rsp>(reply);
    std::memcpy(buffer, response->data, size);
    return DRVLITE_OK;
}

int session::write_physical(uint64_t address, const void* buffer,
                            uint32_t size) {
    if (buffer == nullptr || size == 0 || size > DRVLITE_MAX_TRANSFER) {
        return DRVLITE_ERR_PARAM;
    }
    std::vector<uint8_t> request(sizeof(drvlite_mem_req) + size, 0);
    auto* header = reinterpret_cast<drvlite_mem_req*>(request.data());
    fill_request(&header->hdr, DRVLITE_OP_MEM_PHYS_WRITE, sizeof(*header));
    header->address = address;
    header->size = size;
    std::memcpy(request.data() + sizeof(*header), buffer, size);
    std::vector<uint8_t> reply(offsetof(drvlite_mem_rsp, data));
    uint32_t length = 0;
    return device_call(DRVLITE_OP_MEM_PHYS_WRITE, request.data(),
                       static_cast<uint32_t>(request.size()), reply.data(),
                       static_cast<uint32_t>(reply.size()), &length,
                       offsetof(drvlite_mem_rsp, data));
}

int session::read_kernel(uint64_t address, void* buffer, uint32_t size) const {
    if (buffer == nullptr || size == 0 || size > DRVLITE_MAX_TRANSFER) {
        return DRVLITE_ERR_PARAM;
    }
    drvlite_mem_req request{};
    fill_request(&request.hdr, DRVLITE_OP_MEM_KERNEL_READ, sizeof(request));
    request.address = address;
    request.size = size;
    const uint32_t reply_size =
        static_cast<uint32_t>(offsetof(drvlite_mem_rsp, data)) + size;
    std::vector<uint8_t> reply(reply_size);
    uint32_t length = 0;
    const int status = device_call(DRVLITE_OP_MEM_KERNEL_READ, &request,
                                   sizeof(request), reply.data(), reply_size,
                                   &length, offsetof(drvlite_mem_rsp, data));
    if (status != DRVLITE_OK) {
        return status;
    }
    const auto* response = as_struct<drvlite_mem_rsp>(reply);
    std::memcpy(buffer, response->data, size);
    return DRVLITE_OK;
}

int session::write_kernel(uint64_t address, const void* buffer, uint32_t size) {
    if (buffer == nullptr || size == 0 || size > DRVLITE_MAX_TRANSFER) {
        return DRVLITE_ERR_PARAM;
    }
    std::vector<uint8_t> request(sizeof(drvlite_mem_req) + size, 0);
    auto* header = reinterpret_cast<drvlite_mem_req*>(request.data());
    fill_request(&header->hdr, DRVLITE_OP_MEM_KERNEL_WRITE, sizeof(*header));
    header->address = address;
    header->size = size;
    std::memcpy(request.data() + sizeof(*header), buffer, size);
    std::vector<uint8_t> reply(offsetof(drvlite_mem_rsp, data));
    uint32_t length = 0;
    return device_call(DRVLITE_OP_MEM_KERNEL_WRITE, request.data(),
                       static_cast<uint32_t>(request.size()), reply.data(),
                       static_cast<uint32_t>(reply.size()), &length,
                       offsetof(drvlite_mem_rsp, data));
}

int session::read_process(uint32_t pid, uint64_t address, void* buffer,
                          uint32_t size) const {
    if (buffer == nullptr || size == 0 || size > DRVLITE_MAX_TRANSFER) {
        return DRVLITE_ERR_PARAM;
    }
    drvlite_process_mem_req request{};
    fill_request(&request.hdr, DRVLITE_OP_MEM_PROCESS_READ, sizeof(request));
    request.pid = pid;
    request.address = address;
    request.size = size;
    const uint32_t reply_size =
        static_cast<uint32_t>(offsetof(drvlite_process_mem_rsp, data)) + size;
    std::vector<uint8_t> reply(reply_size);
    uint32_t length = 0;
    const int status = device_call(
        DRVLITE_OP_MEM_PROCESS_READ, &request, sizeof(request), reply.data(),
        reply_size, &length, offsetof(drvlite_process_mem_rsp, data));
    if (status != DRVLITE_OK) {
        return status;
    }
    const auto* response = as_struct<drvlite_process_mem_rsp>(reply);
    std::memcpy(buffer, response->data, size);
    return DRVLITE_OK;
}

int session::write_process(uint32_t pid, uint64_t address, const void* buffer,
                           uint32_t size) {
    if (buffer == nullptr || size == 0 || size > DRVLITE_MAX_TRANSFER) {
        return DRVLITE_ERR_PARAM;
    }
    std::vector<uint8_t> request(sizeof(drvlite_process_mem_req) + size, 0);
    auto* header = reinterpret_cast<drvlite_process_mem_req*>(request.data());
    fill_request(&header->hdr, DRVLITE_OP_MEM_PROCESS_WRITE, sizeof(*header));
    header->pid = pid;
    header->address = address;
    header->size = size;
    std::memcpy(request.data() + sizeof(*header), buffer, size);
    std::vector<uint8_t> reply(offsetof(drvlite_process_mem_rsp, data));
    uint32_t length = 0;
    return device_call(DRVLITE_OP_MEM_PROCESS_WRITE, request.data(),
                       static_cast<uint32_t>(request.size()), reply.data(),
                       static_cast<uint32_t>(reply.size()), &length,
                       offsetof(drvlite_process_mem_rsp, data));
}

int session::batch_read_process(uint32_t pid, const drvlite_range* ranges,
                                uint32_t count, std::vector<uint8_t>* data,
                                std::vector<uint32_t>* statuses) const {
    if (ranges == nullptr || count == 0 || count > DRVLITE_MAX_BATCH_RANGES ||
        data == nullptr || statuses == nullptr) {
        return DRVLITE_ERR_PARAM;
    }
    drvlite_batch_read_req request{};
    fill_request(&request.hdr, DRVLITE_OP_MEM_BATCH_READ, sizeof(request));
    request.pid = pid;
    request.count = count;
    uint64_t total = 0;
    for (uint32_t index = 0; index < count; ++index) {
        request.ranges[index] = ranges[index];
        total += ranges[index].size;
    }
    request.total = static_cast<uint32_t>(total);
    const uint32_t reply_size =
        static_cast<uint32_t>(offsetof(drvlite_batch_read_rsp, data)) +
        request.total;
    std::vector<uint8_t> reply(reply_size);
    uint32_t length = 0;
    const int status = device_call(
        DRVLITE_OP_MEM_BATCH_READ, &request, sizeof(request), reply.data(),
        reply_size, &length, offsetof(drvlite_batch_read_rsp, data));
    if (status != DRVLITE_OK) {
        return status;
    }
    const auto* response = as_struct<drvlite_batch_read_rsp>(reply);
    data->assign(response->data, response->data + response->total);
    statuses->assign(response->status, response->status + count);
    return DRVLITE_OK;
}

int session::dse_query(dse_status* out) const {
    if (out == nullptr) {
        return DRVLITE_ERR_PARAM;
    }
    drvlite_dse_req request{};
    fill_request(&request.hdr, DRVLITE_OP_DSE_QUERY, sizeof(request));
    std::vector<uint8_t> reply(sizeof(drvlite_dse_rsp));
    uint32_t length = 0;
    const int status = device_call(
        DRVLITE_OP_DSE_QUERY, &request, sizeof(request), reply.data(),
        static_cast<uint32_t>(reply.size()), &length, sizeof(drvlite_dse_rsp));
    if (status != DRVLITE_OK) {
        return status;
    }
    const auto* response = as_struct<drvlite_dse_rsp>(reply);
    out->supported = response->supported != 0;
    out->patched = response->patched != 0;
    out->current = response->current;
    out->original = response->original;
    out->nt_build = response->nt_build;
    out->error = response->flags;
    out->address = response->address;
    out->ci_base = response->ci_base;
    out->ci_size = response->ci_size;
    out->via = response->reserved;
    return DRVLITE_OK;
}

int session::dse_set(uint32_t value) {
    drvlite_dse_req request{};
    fill_request(&request.hdr, DRVLITE_OP_DSE_SET, sizeof(request));
    request.value = value;
    std::vector<uint8_t> reply(sizeof(drvlite_dse_rsp));
    uint32_t length = 0;
    return device_call(DRVLITE_OP_DSE_SET, &request, sizeof(request),
                       reply.data(), static_cast<uint32_t>(reply.size()),
                       &length, sizeof(drvlite_dse_rsp));
}

int session::dse_restore() {
    drvlite_dse_req request{};
    fill_request(&request.hdr, DRVLITE_OP_DSE_RESTORE, sizeof(request));
    std::vector<uint8_t> reply(sizeof(drvlite_dse_rsp));
    uint32_t length = 0;
    return device_call(DRVLITE_OP_DSE_RESTORE, &request, sizeof(request),
                       reply.data(), static_cast<uint32_t>(reply.size()),
                       &length, sizeof(drvlite_dse_rsp));
}

int session::open_process_handle(uint32_t pid, uint32_t access, HANDLE* out,
                                 uint32_t* granted) {
    if (out == nullptr) {
        return DRVLITE_ERR_PARAM;
    }
    drvlite_handle_open_req request{};
    fill_request(&request.hdr, DRVLITE_OP_HANDLE_OPEN, sizeof(request));
    request.pid = pid;
    request.access = access;
    request.type = DRVLITE_HANDLE_TYPE_PROCESS;
    std::vector<uint8_t> reply(sizeof(drvlite_handle_open_rsp));
    uint32_t length = 0;
    const int status =
        device_call(DRVLITE_OP_HANDLE_OPEN, &request, sizeof(request),
                    reply.data(), static_cast<uint32_t>(reply.size()), &length,
                    sizeof(drvlite_handle_open_rsp));
    if (status != DRVLITE_OK) {
        return status;
    }
    const auto* response = as_struct<drvlite_handle_open_rsp>(reply);
    *out = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(response->handle));
    if (granted != nullptr) {
        *granted = response->granted;
    }
    handles_.emplace_back(*out, pid);
    return DRVLITE_OK;
}

int session::open_thread_handle(uint32_t pid, uint32_t tid, uint32_t access,
                                HANDLE* out, uint32_t* granted) {
    if (out == nullptr) {
        return DRVLITE_ERR_PARAM;
    }
    drvlite_handle_open_req request{};
    fill_request(&request.hdr, DRVLITE_OP_HANDLE_OPEN, sizeof(request));
    request.pid = pid;
    request.tid = tid;
    request.access = access;
    request.type = DRVLITE_HANDLE_TYPE_THREAD;
    std::vector<uint8_t> reply(sizeof(drvlite_handle_open_rsp));
    uint32_t length = 0;
    const int status =
        device_call(DRVLITE_OP_HANDLE_OPEN, &request, sizeof(request),
                    reply.data(), static_cast<uint32_t>(reply.size()), &length,
                    sizeof(drvlite_handle_open_rsp));
    if (status != DRVLITE_OK) {
        return status;
    }
    const auto* response = as_struct<drvlite_handle_open_rsp>(reply);
    *out = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(response->handle));
    if (granted != nullptr) {
        *granted = response->granted;
    }
    handles_.emplace_back(*out, pid);
    return DRVLITE_OK;
}

int session::close_handle(HANDLE handle, uint32_t pid) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return DRVLITE_ERR_PARAM;
    }
    drvlite_handle_close_req request{};
    fill_request(&request.hdr, DRVLITE_OP_HANDLE_CLOSE, sizeof(request));
    request.handle = static_cast<uint64_t>(reinterpret_cast<ULONG_PTR>(handle));
    request.pid = pid;
    std::vector<uint8_t> reply(sizeof(drvlite_handle_close_rsp));
    uint32_t length = 0;
    const int status =
        device_call(DRVLITE_OP_HANDLE_CLOSE, &request, sizeof(request),
                    reply.data(), static_cast<uint32_t>(reply.size()), &length,
                    sizeof(drvlite_handle_close_rsp));
    if (status == DRVLITE_OK) {
        for (auto it = handles_.begin(); it != handles_.end(); ++it) {
            if (it->first == handle) {
                handles_.erase(it);
                break;
            }
        }
    }
    return status;
}

int session::infinity_status(hook_status* out) const {
    if (out == nullptr) {
        return DRVLITE_ERR_PARAM;
    }
    drvlite_infinity_remove_req request{};
    fill_request(&request.hdr, DRVLITE_OP_INFINITY_STATUS, sizeof(request));
    std::vector<uint8_t> reply(sizeof(drvlite_infinity_status_rsp));
    uint32_t length = 0;
    const int status =
        device_call(DRVLITE_OP_INFINITY_STATUS, &request, sizeof(request),
                    reply.data(), static_cast<uint32_t>(reply.size()), &length,
                    sizeof(drvlite_infinity_status_rsp));
    if (status != DRVLITE_OK) {
        return status;
    }
    const auto* response = as_struct<drvlite_infinity_status_rsp>(reply);
    out->installed = response->installed != 0;
    out->supported = response->supported != 0;
    out->entry = response->entry;
    out->dispatch = response->dispatch;
    out->total_syscalls = response->total_syscalls;
    out->filtered_hits = response->filtered_hits;
    out->handled_calls = response->handled_calls;
    out->spliced_bytes = response->spliced_bytes;
    out->flags = response->flags;
    return DRVLITE_OK;
}

int session::infinity_install(const std::vector<uint32_t>& ssn_filter) {
    if (ssn_filter.size() > DRVLITE_MAX_SSN_FILTER) {
        return DRVLITE_ERR_PARAM;
    }
    drvlite_infinity_install_req request{};
    fill_request(&request.hdr, DRVLITE_OP_INFINITY_INSTALL, sizeof(request));
    request.filter_count = static_cast<uint32_t>(ssn_filter.size());
    for (size_t index = 0; index < ssn_filter.size(); ++index) {
        request.ssn_filter[index] = ssn_filter[index];
    }
    std::vector<uint8_t> reply(sizeof(drvlite_infinity_status_rsp));
    uint32_t length = 0;
    return device_call(DRVLITE_OP_INFINITY_INSTALL, &request, sizeof(request),
                       reply.data(), static_cast<uint32_t>(reply.size()),
                       &length, sizeof(drvlite_infinity_status_rsp));
}

int session::infinity_remove() {
    drvlite_infinity_remove_req request{};
    fill_request(&request.hdr, DRVLITE_OP_INFINITY_REMOVE, sizeof(request));
    std::vector<uint8_t> reply(sizeof(drvlite_infinity_status_rsp));
    uint32_t length = 0;
    return device_call(DRVLITE_OP_INFINITY_REMOVE, &request, sizeof(request),
                       reply.data(), static_cast<uint32_t>(reply.size()),
                       &length, sizeof(drvlite_infinity_status_rsp));
}

int session::hide_debug_registers(bool enable, uint32_t pid) {
    drvlite_hwbp_hide_req request{};
    fill_request(&request.hdr, DRVLITE_OP_HWBP_HIDE_SET, sizeof(request));
    request.enable = enable ? 1u : 0u;
    request.pid = pid;
    request.get_context_ssn = ntdll_syscall_number("NtGetContextThread");
    if (request.get_context_ssn == 0) {
        return DRVLITE_ERR_NOT_FOUND;
    }
    std::vector<uint8_t> reply(sizeof(drvlite_hwbp_hide_rsp));
    uint32_t length = 0;
    return device_call(DRVLITE_OP_HWBP_HIDE_SET, &request, sizeof(request),
                       reply.data(), static_cast<uint32_t>(reply.size()),
                       &length, sizeof(drvlite_hwbp_hide_rsp));
}

int session::set_syscall_shield(bool enable, uint32_t pid,
                                const std::vector<uint32_t>& ssns) {
    if (ssns.size() > DRVLITE_MAX_SSN_FILTER) {
        return DRVLITE_ERR_PARAM;
    }
    drvlite_syscall_shield_req request{};
    fill_request(&request.hdr, DRVLITE_OP_SYSCALL_SHIELD_SET, sizeof(request));
    request.enable = enable ? 1u : 0u;
    request.pid = pid;
    request.ssn_count = static_cast<uint32_t>(ssns.size());
    for (size_t index = 0; index < ssns.size(); ++index) {
        request.ssns[index] = ssns[index];
    }
    std::vector<uint8_t> reply(sizeof(drvlite_syscall_shield_rsp));
    uint32_t length = 0;
    return device_call(DRVLITE_OP_SYSCALL_SHIELD_SET, &request, sizeof(request),
                       reply.data(), static_cast<uint32_t>(reply.size()),
                       &length, sizeof(drvlite_syscall_shield_rsp));
}

int session::shutdown() {
    drvlite_shutdown_req request{};
    fill_request(&request.hdr, DRVLITE_OP_SHUTDOWN, sizeof(request));
    std::vector<uint8_t> reply(sizeof(drvlite_shutdown_rsp));
    uint32_t length = 0;
    const int status =
        device_call(DRVLITE_OP_SHUTDOWN, &request, sizeof(request),
                    reply.data(), static_cast<uint32_t>(reply.size()), &length,
                    sizeof(drvlite_shutdown_rsp));
    handles_.clear();
    active_ = false;
    return status;
}

}  // namespace drvlite

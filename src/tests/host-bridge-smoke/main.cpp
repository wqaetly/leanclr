#include "host/leanclr_host_bridge.h"

#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
struct MockHandleRecord
{
    std::string type_name;
    std::string debug_name;
    uint32_t ref_count = 1;
    bool alive = true;
};

struct MockDispatchTask
{
    uint64_t ticket = 0;
    LeanClrHostBridgeDispatchCallbackFn callback = nullptr;
    void* callback_data = nullptr;
};

struct MockHostState
{
    int log_count = 0;
    int invoke_count = 0;
    LeanClrHostHandle next_handle = 100;
    uint64_t next_dispatch_ticket = 1;
    bool is_pumping_main_thread = false;
    std::string last_entry;
    std::unordered_map<LeanClrHostHandle, MockHandleRecord> handles;
    std::deque<MockDispatchTask> dispatch_queue;
    std::vector<int> dispatch_order;
};

struct MockDispatchPayload
{
    MockHostState* state = nullptr;
    int value = 0;
    bool fail = false;
};

void mock_log(void* user_data, int32_t level, const char* message)
{
    auto* state = static_cast<MockHostState*>(user_data);
    if (state == nullptr || message == nullptr)
    {
        return;
    }

    if (level >= 0)
    {
        ++state->log_count;
    }
}

LeanClrHostBridgeStatus mock_invoke_managed_entry(void* user_data,
                                                  const char* assembly_name,
                                                  const char* type_name,
                                                  const char* method_name,
                                                  LeanClrHostBridgeError* error)
{
    auto* state = static_cast<MockHostState*>(user_data);
    if (state == nullptr || assembly_name == nullptr || type_name == nullptr || method_name == nullptr)
    {
        LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT, "managed entry request is incomplete");
        return LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT;
    }

    ++state->invoke_count;
    state->last_entry = std::string(assembly_name) + "!" + type_name + "::" + method_name;
    LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_OK, nullptr);
    return LEANCLR_HOST_BRIDGE_OK;
}

MockHandleRecord* find_live_handle(MockHostState* state, LeanClrHostHandle handle, LeanClrHostBridgeError* error)
{
    if (state == nullptr || handle == 0)
    {
        LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT, "host handle request is incomplete");
        return nullptr;
    }

    auto it = state->handles.find(handle);
    if (it == state->handles.end() || !it->second.alive || it->second.ref_count == 0)
    {
        LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_OBJECT_DISPOSED, "host handle is no longer alive");
        return nullptr;
    }

    return &it->second;
}

LeanClrHostBridgeStatus mock_create_handle(void* user_data,
                                           const char* type_name,
                                           const char* debug_name,
                                           LeanClrHostHandle* out_handle,
                                           LeanClrHostBridgeError* error)
{
    auto* state = static_cast<MockHostState*>(user_data);
    if (state == nullptr || type_name == nullptr || out_handle == nullptr)
    {
        LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT, "host handle creation request is incomplete");
        return LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT;
    }

    const LeanClrHostHandle handle = state->next_handle++;
    MockHandleRecord record;
    record.type_name = type_name;
    record.debug_name = debug_name == nullptr ? "" : debug_name;
    state->handles.emplace(handle, record);
    *out_handle = handle;

    LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_OK, nullptr);
    return LEANCLR_HOST_BRIDGE_OK;
}

LeanClrHostBridgeStatus mock_retain_handle(void* user_data, LeanClrHostHandle handle, LeanClrHostBridgeError* error)
{
    auto* state = static_cast<MockHostState*>(user_data);
    auto* record = find_live_handle(state, handle, error);
    if (record == nullptr)
    {
        return error == nullptr ? LEANCLR_HOST_BRIDGE_OBJECT_DISPOSED : error->status;
    }

    ++record->ref_count;
    LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_OK, nullptr);
    return LEANCLR_HOST_BRIDGE_OK;
}

LeanClrHostBridgeStatus mock_release_handle(void* user_data, LeanClrHostHandle handle, LeanClrHostBridgeError* error)
{
    auto* state = static_cast<MockHostState*>(user_data);
    if (state == nullptr || handle == 0)
    {
        LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT, "host handle release request is incomplete");
        return LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT;
    }

    auto it = state->handles.find(handle);
    if (it == state->handles.end())
    {
        LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_OBJECT_DISPOSED, "host handle is unknown");
        return LEANCLR_HOST_BRIDGE_OBJECT_DISPOSED;
    }

    if (it->second.ref_count > 0)
    {
        --it->second.ref_count;
    }
    if (it->second.ref_count == 0)
    {
        it->second.alive = false;
    }

    LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_OK, nullptr);
    return LEANCLR_HOST_BRIDGE_OK;
}

LeanClrHostBridgeStatus mock_is_handle_alive(void* user_data,
                                             LeanClrHostHandle handle,
                                             int32_t* out_alive,
                                             LeanClrHostBridgeError* error)
{
    auto* state = static_cast<MockHostState*>(user_data);
    if (state == nullptr || out_alive == nullptr)
    {
        LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT, "host handle query request is incomplete");
        return LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT;
    }

    auto it = state->handles.find(handle);
    *out_alive = it != state->handles.end() && it->second.alive && it->second.ref_count > 0 ? 1 : 0;
    LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_OK, nullptr);
    return LEANCLR_HOST_BRIDGE_OK;
}

LeanClrHostBridgeStatus mock_notify_handle_destroyed(void* user_data,
                                                     LeanClrHostHandle handle,
                                                     LeanClrHostBridgeError* error)
{
    auto* state = static_cast<MockHostState*>(user_data);
    if (state == nullptr || handle == 0)
    {
        LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT, "host handle destroy notification is incomplete");
        return LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT;
    }

    auto it = state->handles.find(handle);
    if (it == state->handles.end())
    {
        LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_OBJECT_DISPOSED, "host handle is unknown");
        return LEANCLR_HOST_BRIDGE_OBJECT_DISPOSED;
    }

    it->second.alive = false;
    it->second.ref_count = 0;
    LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_OK, nullptr);
    return LEANCLR_HOST_BRIDGE_OK;
}

LeanClrHostBridgeStatus mock_dispatch_callback(void* callback_data, LeanClrHostBridgeError* error)
{
    auto* payload = static_cast<MockDispatchPayload*>(callback_data);
    if (payload == nullptr || payload->state == nullptr)
    {
        LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT, "main-thread callback payload is incomplete");
        return LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT;
    }

    payload->state->dispatch_order.push_back(payload->value);
    if (payload->fail)
    {
        LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_MANAGED_EXCEPTION, "managed continuation failed");
        return LEANCLR_HOST_BRIDGE_MANAGED_EXCEPTION;
    }

    LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_OK, nullptr);
    return LEANCLR_HOST_BRIDGE_OK;
}

LeanClrHostBridgeStatus mock_post_to_main_thread(void* user_data,
                                                 LeanClrHostBridgeDispatchCallbackFn callback,
                                                 void* callback_data,
                                                 uint64_t* out_ticket,
                                                 LeanClrHostBridgeError* error)
{
    auto* state = static_cast<MockHostState*>(user_data);
    if (state == nullptr || callback == nullptr || out_ticket == nullptr)
    {
        LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT, "main-thread post request is incomplete");
        return LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT;
    }

    MockDispatchTask task;
    task.ticket = state->next_dispatch_ticket++;
    task.callback = callback;
    task.callback_data = callback_data;
    state->dispatch_queue.push_back(task);
    *out_ticket = task.ticket;

    LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_OK, nullptr);
    return LEANCLR_HOST_BRIDGE_OK;
}

LeanClrHostBridgeStatus mock_pump_main_thread(void* user_data,
                                              uint32_t max_items,
                                              uint32_t* out_executed,
                                              LeanClrHostBridgeError* error)
{
    auto* state = static_cast<MockHostState*>(user_data);
    if (state == nullptr || out_executed == nullptr)
    {
        LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT, "main-thread pump request is incomplete");
        return LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT;
    }

    *out_executed = 0;
    state->is_pumping_main_thread = true;
    while (!state->dispatch_queue.empty() && (max_items == 0 || *out_executed < max_items))
    {
        const auto task = state->dispatch_queue.front();
        state->dispatch_queue.pop_front();
        ++(*out_executed);

        auto status = task.callback(task.callback_data, error);
        if (status != LEANCLR_HOST_BRIDGE_OK)
        {
            state->is_pumping_main_thread = false;
            return status;
        }
    }

    state->is_pumping_main_thread = false;
    LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_OK, nullptr);
    return LEANCLR_HOST_BRIDGE_OK;
}

LeanClrHostBridgeStatus mock_call_main_thread_sync(void* user_data,
                                                   LeanClrHostBridgeDispatchCallbackFn callback,
                                                   void* callback_data,
                                                   LeanClrHostBridgeError* error)
{
    auto* state = static_cast<MockHostState*>(user_data);
    if (state == nullptr || callback == nullptr)
    {
        LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT, "main-thread sync request is incomplete");
        return LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT;
    }
    if (state->is_pumping_main_thread)
    {
        LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_REENTRANT_CALL, "synchronous main-thread call is not allowed while pumping");
        return LEANCLR_HOST_BRIDGE_REENTRANT_CALL;
    }

    return callback(callback_data, error);
}

bool require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "host-bridge-smoke: " << message << std::endl;
        return false;
    }

    return true;
}

LeanClrHostBridgeFunctions make_mock_functions(MockHostState* state)
{
    LeanClrHostBridgeFunctions functions{};
    functions.size = sizeof(functions);
    functions.abi_version = LEANCLR_HOST_BRIDGE_ABI_VERSION;
    functions.capabilities = LEANCLR_HOST_BRIDGE_CAP_LOGGING |
                             LEANCLR_HOST_BRIDGE_CAP_INVOKE_MANAGED_ENTRY |
                             LEANCLR_HOST_BRIDGE_CAP_HANDLE_REGISTRY |
                             LEANCLR_HOST_BRIDGE_CAP_MAIN_THREAD_DISPATCH;
    functions.user_data = state;
    functions.log = mock_log;
    functions.invoke_managed_entry = mock_invoke_managed_entry;
    functions.create_handle = mock_create_handle;
    functions.retain_handle = mock_retain_handle;
    functions.release_handle = mock_release_handle;
    functions.is_handle_alive = mock_is_handle_alive;
    functions.notify_handle_destroyed = mock_notify_handle_destroyed;
    functions.post_to_main_thread = mock_post_to_main_thread;
    functions.pump_main_thread = mock_pump_main_thread;
    functions.call_main_thread_sync = mock_call_main_thread_sync;
    return functions;
}

bool run_abi_skeleton()
{
    MockHostState state;
    auto functions = make_mock_functions(&state);
    LeanClrHostBridgeError error{};

    auto status = LeanClrHostBridge_ValidateFunctions(
        &functions,
        LEANCLR_HOST_BRIDGE_CAP_LOGGING | LEANCLR_HOST_BRIDGE_CAP_INVOKE_MANAGED_ENTRY,
        &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_OK && error.status == LEANCLR_HOST_BRIDGE_OK,
                 "valid function table should be accepted"))
    {
        return false;
    }

    functions.log(functions.user_data, 1, "host bridge mock initialized");
    if (!require(state.log_count == 1, "logging callback was not invoked"))
    {
        return false;
    }

    status = functions.invoke_managed_entry(functions.user_data,
                                           "ManagedNet10.HostBridgeSmoke",
                                           "ManagedNet10.HostBridgeSmoke.Program",
                                           "Run",
                                           &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_OK && state.invoke_count == 1,
                 "managed entry callback was not invoked"))
    {
        return false;
    }
    if (!require(state.last_entry == "ManagedNet10.HostBridgeSmoke!ManagedNet10.HostBridgeSmoke.Program::Run",
                 "managed entry identity was not preserved"))
    {
        return false;
    }

    auto invalid_version = functions;
    invalid_version.abi_version = LEANCLR_HOST_BRIDGE_ABI_VERSION + 1u;
    status = LeanClrHostBridge_ValidateFunctions(&invalid_version, LEANCLR_HOST_BRIDGE_CAP_LOGGING, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_UNSUPPORTED_VERSION && error.message != nullptr,
                 "unsupported ABI version should return a diagnostic"))
    {
        return false;
    }

    auto missing_capability = functions;
    missing_capability.capabilities = LEANCLR_HOST_BRIDGE_CAP_LOGGING;
    status = LeanClrHostBridge_ValidateFunctions(
        &missing_capability,
        LEANCLR_HOST_BRIDGE_CAP_LOGGING | LEANCLR_HOST_BRIDGE_CAP_INVOKE_MANAGED_ENTRY,
        &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_MISSING_CAPABILITY && error.message != nullptr,
                 "missing capability should return a diagnostic"))
    {
        return false;
    }

    auto missing_callback = functions;
    missing_callback.invoke_managed_entry = nullptr;
    status = LeanClrHostBridge_ValidateFunctions(&missing_callback, LEANCLR_HOST_BRIDGE_CAP_INVOKE_MANAGED_ENTRY, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT && error.message != nullptr,
                 "missing callback should return a diagnostic"))
    {
        return false;
    }

    return true;
}

bool run_handle_registry()
{
    MockHostState state;
    auto functions = make_mock_functions(&state);
    LeanClrHostBridgeError error{};

    auto status = LeanClrHostBridge_ValidateFunctions(&functions, LEANCLR_HOST_BRIDGE_CAP_HANDLE_REGISTRY, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_OK, "handle registry function table should be accepted"))
    {
        return false;
    }

    LeanClrHostHandle handle = 0;
    status = functions.create_handle(functions.user_data, "Mock.Node", "Player", &handle, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_OK && handle != 0, "handle creation should return an opaque handle"))
    {
        return false;
    }
    if (!require(state.handles[handle].type_name == "Mock.Node" && state.handles[handle].debug_name == "Player",
                 "handle metadata should stay on the host side"))
    {
        return false;
    }

    int32_t alive = 0;
    status = functions.is_handle_alive(functions.user_data, handle, &alive, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_OK && alive == 1, "created handle should be alive"))
    {
        return false;
    }

    status = functions.retain_handle(functions.user_data, handle, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_OK && state.handles[handle].ref_count == 2,
                 "retain should increment the host-side ref count"))
    {
        return false;
    }

    status = functions.release_handle(functions.user_data, handle, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_OK && state.handles[handle].ref_count == 1,
                 "release should decrement the host-side ref count"))
    {
        return false;
    }

    status = functions.notify_handle_destroyed(functions.user_data, handle, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_OK, "host destroy notification should invalidate the handle"))
    {
        return false;
    }

    status = functions.is_handle_alive(functions.user_data, handle, &alive, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_OK && alive == 0, "destroyed handle should not be alive"))
    {
        return false;
    }

    status = functions.retain_handle(functions.user_data, handle, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_OBJECT_DISPOSED && error.message != nullptr,
                 "retain after host destroy should return an ObjectDisposed-style diagnostic"))
    {
        return false;
    }

    status = functions.release_handle(functions.user_data, handle, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_OK, "release after host destroy should be idempotent"))
    {
        return false;
    }
    status = functions.release_handle(functions.user_data, handle, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_OK, "duplicate release should stay idempotent"))
    {
        return false;
    }

    LeanClrHostHandle released_handle = 0;
    status = functions.create_handle(functions.user_data, "Mock.Resource", "Temp", &released_handle, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_OK && released_handle != 0,
                 "second handle creation should succeed"))
    {
        return false;
    }
    status = functions.release_handle(functions.user_data, released_handle, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_OK, "final release should succeed"))
    {
        return false;
    }
    status = functions.release_handle(functions.user_data, released_handle, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_OK, "repeat final release should be idempotent"))
    {
        return false;
    }

    auto missing_registry = functions;
    missing_registry.create_handle = nullptr;
    status = LeanClrHostBridge_ValidateFunctions(&missing_registry, LEANCLR_HOST_BRIDGE_CAP_HANDLE_REGISTRY, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT && error.message != nullptr,
                 "missing handle registry callback should return a diagnostic"))
    {
        return false;
    }

    return true;
}

bool run_dispatcher()
{
    MockHostState state;
    auto functions = make_mock_functions(&state);
    LeanClrHostBridgeError error{};

    auto status = LeanClrHostBridge_ValidateFunctions(&functions, LEANCLR_HOST_BRIDGE_CAP_MAIN_THREAD_DISPATCH, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_OK, "dispatcher function table should be accepted"))
    {
        return false;
    }

    MockDispatchPayload first{&state, 1, false};
    MockDispatchPayload second{&state, 2, false};
    uint64_t first_ticket = 0;
    uint64_t second_ticket = 0;
    status = functions.post_to_main_thread(functions.user_data, mock_dispatch_callback, &first, &first_ticket, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_OK && first_ticket == 1, "first main-thread post should return a ticket"))
    {
        return false;
    }
    status = functions.post_to_main_thread(functions.user_data, mock_dispatch_callback, &second, &second_ticket, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_OK && second_ticket == 2, "second main-thread post should return the next ticket"))
    {
        return false;
    }

    uint32_t executed = 0;
    status = functions.pump_main_thread(functions.user_data, 1, &executed, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_OK && executed == 1 && state.dispatch_order.size() == 1 && state.dispatch_order[0] == 1,
                 "single-item pump should execute only the first queued continuation"))
    {
        return false;
    }

    status = functions.pump_main_thread(functions.user_data, 0, &executed, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_OK && executed == 1 && state.dispatch_order.size() == 2 && state.dispatch_order[1] == 2,
                 "next pump should preserve FIFO ordering"))
    {
        return false;
    }

    MockDispatchPayload sync_payload{&state, 3, false};
    status = functions.call_main_thread_sync(functions.user_data, mock_dispatch_callback, &sync_payload, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_OK && state.dispatch_order.size() == 3 && state.dispatch_order[2] == 3,
                 "synchronous main-thread call should return callback results"))
    {
        return false;
    }

    state.is_pumping_main_thread = true;
    status = functions.call_main_thread_sync(functions.user_data, mock_dispatch_callback, &sync_payload, &error);
    state.is_pumping_main_thread = false;
    if (!require(status == LEANCLR_HOST_BRIDGE_REENTRANT_CALL && error.message != nullptr,
                 "synchronous reentry while pumping should be rejected"))
    {
        return false;
    }

    MockDispatchPayload failing{&state, 4, true};
    uint64_t failing_ticket = 0;
    status = functions.post_to_main_thread(functions.user_data, mock_dispatch_callback, &failing, &failing_ticket, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_OK && failing_ticket == 3, "failing continuation should still be queued"))
    {
        return false;
    }
    status = functions.pump_main_thread(functions.user_data, 1, &executed, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_MANAGED_EXCEPTION && executed == 1 && error.message != nullptr,
                 "managed exception should be returned as dispatcher diagnostic"))
    {
        return false;
    }

    auto missing_dispatcher = functions;
    missing_dispatcher.post_to_main_thread = nullptr;
    status = LeanClrHostBridge_ValidateFunctions(&missing_dispatcher, LEANCLR_HOST_BRIDGE_CAP_MAIN_THREAD_DISPATCH, &error);
    if (!require(status == LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT && error.message != nullptr,
                 "missing dispatcher callback should return a diagnostic"))
    {
        return false;
    }

    return true;
}
} // namespace

int main(int argc, char** argv)
{
    std::string scenario = "AbiSkeleton";
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--scenario") == 0 && i + 1 < argc)
        {
            scenario = argv[++i];
        }
    }

    if (scenario == "AbiSkeleton")
    {
        if (!run_abi_skeleton())
        {
            return 1;
        }

        std::cout << "ok! host bridge AbiSkeleton" << std::endl;
        return 0;
    }

    if (scenario == "HandleRegistry")
    {
        if (!run_handle_registry())
        {
            return 1;
        }

        std::cout << "ok! host bridge HandleRegistry" << std::endl;
        return 0;
    }

    if (scenario == "Dispatcher")
    {
        if (!run_dispatcher())
        {
            return 1;
        }

        std::cout << "ok! host bridge Dispatcher" << std::endl;
        return 0;
    }

    std::cerr << "host-bridge-smoke: unsupported scenario: " << scenario << std::endl;
    return 2;
}

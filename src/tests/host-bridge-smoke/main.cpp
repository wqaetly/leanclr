#include "host/leanclr_host_bridge.h"

#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>

namespace
{
struct MockHandleRecord
{
    std::string type_name;
    std::string debug_name;
    uint32_t ref_count = 1;
    bool alive = true;
};

struct MockHostState
{
    int log_count = 0;
    int invoke_count = 0;
    LeanClrHostHandle next_handle = 100;
    std::string last_entry;
    std::unordered_map<LeanClrHostHandle, MockHandleRecord> handles;
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
                             LEANCLR_HOST_BRIDGE_CAP_HANDLE_REGISTRY;
    functions.user_data = state;
    functions.log = mock_log;
    functions.invoke_managed_entry = mock_invoke_managed_entry;
    functions.create_handle = mock_create_handle;
    functions.retain_handle = mock_retain_handle;
    functions.release_handle = mock_release_handle;
    functions.is_handle_alive = mock_is_handle_alive;
    functions.notify_handle_destroyed = mock_notify_handle_destroyed;
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

    std::cerr << "host-bridge-smoke: unsupported scenario: " << scenario << std::endl;
    return 2;
}

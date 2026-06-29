#include "host/leanclr_host_bridge.h"

#include <cstring>
#include <iostream>
#include <string>

namespace
{
struct MockHostState
{
    int log_count = 0;
    int invoke_count = 0;
    std::string last_entry;
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
    functions.capabilities = LEANCLR_HOST_BRIDGE_CAP_LOGGING | LEANCLR_HOST_BRIDGE_CAP_INVOKE_MANAGED_ENTRY;
    functions.user_data = state;
    functions.log = mock_log;
    functions.invoke_managed_entry = mock_invoke_managed_entry;
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

    std::cerr << "host-bridge-smoke: unsupported scenario: " << scenario << std::endl;
    return 2;
}

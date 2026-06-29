#ifndef LEANCLR_HOST_BRIDGE_H
#define LEANCLR_HOST_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LEANCLR_HOST_BRIDGE_ABI_VERSION 1u

typedef uint64_t LeanClrHostHandle;

typedef enum LeanClrHostBridgeStatus
{
    LEANCLR_HOST_BRIDGE_OK = 0,
    LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT = 1,
    LEANCLR_HOST_BRIDGE_UNSUPPORTED_VERSION = 2,
    LEANCLR_HOST_BRIDGE_MISSING_CAPABILITY = 3,
    LEANCLR_HOST_BRIDGE_HOST_FAILURE = 4
} LeanClrHostBridgeStatus;

typedef enum LeanClrHostBridgeCapability
{
    LEANCLR_HOST_BRIDGE_CAP_LOGGING = 1ull << 0,
    LEANCLR_HOST_BRIDGE_CAP_INVOKE_MANAGED_ENTRY = 1ull << 1,
    LEANCLR_HOST_BRIDGE_CAP_HANDLE_REGISTRY = 1ull << 2,
    LEANCLR_HOST_BRIDGE_CAP_MAIN_THREAD_DISPATCH = 1ull << 3,
    LEANCLR_HOST_BRIDGE_CAP_EVENT_CALLBACK = 1ull << 4
} LeanClrHostBridgeCapability;

typedef struct LeanClrHostBridgeError
{
    LeanClrHostBridgeStatus status;
    const char* message;
} LeanClrHostBridgeError;

typedef void (*LeanClrHostBridgeLogFn)(void* user_data, int32_t level, const char* message);

typedef LeanClrHostBridgeStatus (*LeanClrHostBridgeInvokeManagedEntryFn)(void* user_data,
                                                                         const char* assembly_name,
                                                                         const char* type_name,
                                                                         const char* method_name,
                                                                         LeanClrHostBridgeError* error);

typedef struct LeanClrHostBridgeFunctions
{
    uint32_t size;
    uint32_t abi_version;
    uint64_t capabilities;
    void* user_data;
    LeanClrHostBridgeLogFn log;
    LeanClrHostBridgeInvokeManagedEntryFn invoke_managed_entry;
} LeanClrHostBridgeFunctions;

static inline void LeanClrHostBridge_SetError(LeanClrHostBridgeError* error,
                                              LeanClrHostBridgeStatus status,
                                              const char* message)
{
    if (error == 0)
    {
        return;
    }

    error->status = status;
    error->message = message;
}

static inline LeanClrHostBridgeStatus LeanClrHostBridge_ValidateFunctions(const LeanClrHostBridgeFunctions* functions,
                                                                          uint64_t required_capabilities,
                                                                          LeanClrHostBridgeError* error)
{
    if (functions == 0)
    {
        LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT, "host bridge function table is null");
        return LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT;
    }

    if (functions->size < sizeof(LeanClrHostBridgeFunctions))
    {
        LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT, "host bridge function table is too small");
        return LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT;
    }

    if (functions->abi_version != LEANCLR_HOST_BRIDGE_ABI_VERSION)
    {
        LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_UNSUPPORTED_VERSION, "host bridge ABI version is not supported");
        return LEANCLR_HOST_BRIDGE_UNSUPPORTED_VERSION;
    }

    if ((functions->capabilities & required_capabilities) != required_capabilities)
    {
        LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_MISSING_CAPABILITY, "host bridge capability is missing");
        return LEANCLR_HOST_BRIDGE_MISSING_CAPABILITY;
    }

    if (((functions->capabilities & LEANCLR_HOST_BRIDGE_CAP_LOGGING) != 0 && functions->log == 0) ||
        ((functions->capabilities & LEANCLR_HOST_BRIDGE_CAP_INVOKE_MANAGED_ENTRY) != 0 && functions->invoke_managed_entry == 0))
    {
        LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT, "host bridge function pointer is missing");
        return LEANCLR_HOST_BRIDGE_INVALID_ARGUMENT;
    }

    LeanClrHostBridge_SetError(error, LEANCLR_HOST_BRIDGE_OK, 0);
    return LEANCLR_HOST_BRIDGE_OK;
}

#ifdef __cplusplus
}
#endif

#endif

#include "system_runtime_compilerservices_unsafe.h"

#include <cstring>

#include "interp/eval_stack_op.h"
#include "interp/interp_defs.h"

namespace leanclr
{
namespace intrinsics
{
namespace
{

RtResult<size_t> get_first_generic_arg_size(const metadata::RtMethodInfo* method) noexcept
{
    if (method == nullptr || method->generic_method == nullptr || method->generic_method->generic_context.method_inst == nullptr)
    {
        RET_ERR(RtErr::ExecutionEngine);
    }

    const metadata::RtGenericInst* method_inst = method->generic_method->generic_context.method_inst;
    if (method_inst->generic_arg_count < 1)
    {
        RET_ERR(RtErr::ExecutionEngine);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(interp::ReduceTypeAndSize, type_and_size,
                                            interp::InterpDefs::get_reduce_type_and_size_by_typesig(method_inst->generic_args[0]));
    RET_OK(type_and_size.byte_size);
}

intptr_t get_integer_offset_param(const metadata::RtMethodInfo* method, const interp::RtStackObject* params, size_t index) noexcept
{
    const metadata::RtTypeSig* param_type = method->parameters[index];
    switch (param_type->ele_type)
    {
    case metadata::RtElementType::I4:
        return static_cast<intptr_t>(interp::EvalStackOp::get_param<int32_t>(params, index));
    case metadata::RtElementType::U4:
        return static_cast<intptr_t>(interp::EvalStackOp::get_param<uint32_t>(params, index));
    case metadata::RtElementType::U:
        return static_cast<intptr_t>(interp::EvalStackOp::get_param<uintptr_t>(params, index));
    case metadata::RtElementType::I:
    default:
        return static_cast<intptr_t>(interp::EvalStackOp::get_param<intptr_t>(params, index));
    }
}

void* add_bytes_to_pointer(void* source, intptr_t byte_offset) noexcept
{
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(source) + static_cast<uintptr_t>(byte_offset));
}

} // namespace

RtResult<void*> SystemRuntimeCompilerServicesUnsafe::as_pointer(void* location) noexcept
{
    RET_OK(location);
}

RtResult<void*> SystemRuntimeCompilerServicesUnsafe::as(void* source) noexcept
{
    RET_OK(source);
}

RtResultVoid SystemRuntimeCompilerServicesUnsafe::add(const metadata::RtMethodInfo* method, const interp::RtStackObject* params,
                                                      interp::RtStackObject* ret) noexcept
{
    void* source = interp::EvalStackOp::get_param<void*>(params, 0);
    intptr_t element_offset = get_integer_offset_param(method, params, 1);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(size_t, element_size, get_first_generic_arg_size(method));
    interp::EvalStackOp::set_return(ret, add_bytes_to_pointer(source, element_offset * static_cast<intptr_t>(element_size)));
    RET_VOID_OK();
}

RtResultVoid SystemRuntimeCompilerServicesUnsafe::add_byte_offset(const metadata::RtMethodInfo* method, const interp::RtStackObject* params,
                                                                  interp::RtStackObject* ret) noexcept
{
    void* source = interp::EvalStackOp::get_param<void*>(params, 0);
    intptr_t byte_offset = get_integer_offset_param(method, params, 1);
    interp::EvalStackOp::set_return(ret, add_bytes_to_pointer(source, byte_offset));
    RET_VOID_OK();
}

RtResultVoid SystemRuntimeCompilerServicesUnsafe::subtract_byte_offset(const metadata::RtMethodInfo* method, const interp::RtStackObject* params,
                                                                       interp::RtStackObject* ret) noexcept
{
    void* source = interp::EvalStackOp::get_param<void*>(params, 0);
    intptr_t byte_offset = get_integer_offset_param(method, params, 1);
    interp::EvalStackOp::set_return(ret, add_bytes_to_pointer(source, -byte_offset));
    RET_VOID_OK();
}

RtResultVoid SystemRuntimeCompilerServicesUnsafe::bit_cast(const metadata::RtMethodInfo* method, const interp::RtStackObject* params,
                                                           interp::RtStackObject* ret) noexcept
{
    if (method == nullptr || method->generic_method == nullptr || method->generic_method->generic_context.method_inst == nullptr)
    {
        RET_ERR(RtErr::ExecutionEngine);
    }

    const metadata::RtGenericInst* method_inst = method->generic_method->generic_context.method_inst;
    if (method_inst->generic_arg_count != 2)
    {
        RET_ERR(RtErr::ExecutionEngine);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(interp::ReduceTypeAndSize, source_type_and_size,
                                            interp::InterpDefs::get_reduce_type_and_size_by_typesig(method_inst->generic_args[0]));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(interp::ReduceTypeAndSize, target_type_and_size,
                                            interp::InterpDefs::get_reduce_type_and_size_by_typesig(method_inst->generic_args[1]));

    if (source_type_and_size.byte_size != target_type_and_size.byte_size)
    {
        RET_ERR(RtErr::NotSupported);
    }

    std::memcpy(ret, params, source_type_and_size.byte_size);
    RET_VOID_OK();
}

static RtResultVoid as_pointer_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                       interp::RtStackObject* ret) noexcept
{
    void* location = interp::EvalStackOp::get_param<void*>(params, 0);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(void*, result, SystemRuntimeCompilerServicesUnsafe::as_pointer(location));
    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

static RtResultVoid as_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                               interp::RtStackObject* ret) noexcept
{
    void* source = interp::EvalStackOp::get_param<void*>(params, 0);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(void*, result, SystemRuntimeCompilerServicesUnsafe::as(source));
    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

static RtResultVoid bit_cast_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo* method, const interp::RtStackObject* params,
                                     interp::RtStackObject* ret) noexcept
{
    RET_ERR_ON_FAIL(SystemRuntimeCompilerServicesUnsafe::bit_cast(method, params, ret));
    RET_VOID_OK();
}

static RtResultVoid add_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo* method, const interp::RtStackObject* params,
                                interp::RtStackObject* ret) noexcept
{
    RET_ERR_ON_FAIL(SystemRuntimeCompilerServicesUnsafe::add(method, params, ret));
    RET_VOID_OK();
}

static RtResultVoid add_byte_offset_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo* method, const interp::RtStackObject* params,
                                           interp::RtStackObject* ret) noexcept
{
    RET_ERR_ON_FAIL(SystemRuntimeCompilerServicesUnsafe::add_byte_offset(method, params, ret));
    RET_VOID_OK();
}

static RtResultVoid subtract_byte_offset_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo* method,
                                                const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    RET_ERR_ON_FAIL(SystemRuntimeCompilerServicesUnsafe::subtract_byte_offset(method, params, ret));
    RET_VOID_OK();
}

static vm::IntrinsicEntry s_intrinsic_entries_system_runtime_compilerservices_unsafe[] = {
    {"System.Runtime.CompilerServices.Unsafe::AsPointer<>", (vm::IntrinsicFunction)&SystemRuntimeCompilerServicesUnsafe::as_pointer, as_pointer_invoker},
    {"System.Runtime.CompilerServices.Unsafe::As<>", (vm::IntrinsicFunction)&SystemRuntimeCompilerServicesUnsafe::as, as_invoker},
    {"System.Runtime.CompilerServices.Unsafe::As<,>", (vm::IntrinsicFunction)&SystemRuntimeCompilerServicesUnsafe::as, as_invoker},
    {"System.Runtime.CompilerServices.Unsafe::Add<>", nullptr, add_invoker},
    {"System.Runtime.CompilerServices.Unsafe::AddByteOffset<>", nullptr, add_byte_offset_invoker},
    {"System.Runtime.CompilerServices.Unsafe::SubtractByteOffset<>", nullptr, subtract_byte_offset_invoker},
    {"System.Runtime.CompilerServices.Unsafe::BitCast<,>", nullptr, bit_cast_invoker},
};

utils::Span<vm::IntrinsicEntry> SystemRuntimeCompilerServicesUnsafe::get_intrinsic_entries() noexcept
{
    return utils::Span<vm::IntrinsicEntry>(s_intrinsic_entries_system_runtime_compilerservices_unsafe,
                                           sizeof(s_intrinsic_entries_system_runtime_compilerservices_unsafe) / sizeof(vm::IntrinsicEntry));
}

} // namespace intrinsics
} // namespace leanclr

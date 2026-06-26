#include "system_runtime_compilerservices_unsafe.h"

#include "interp/eval_stack_op.h"

namespace leanclr
{
namespace intrinsics
{

RtResult<void*> SystemRuntimeCompilerServicesUnsafe::as_pointer(void* location) noexcept
{
    RET_OK(location);
}

static RtResultVoid as_pointer_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                       interp::RtStackObject* ret) noexcept
{
    void* location = interp::EvalStackOp::get_param<void*>(params, 0);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(void*, result, SystemRuntimeCompilerServicesUnsafe::as_pointer(location));
    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

static vm::IntrinsicEntry s_intrinsic_entries_system_runtime_compilerservices_unsafe[] = {
    {"System.Runtime.CompilerServices.Unsafe::AsPointer<>", (vm::IntrinsicFunction)&SystemRuntimeCompilerServicesUnsafe::as_pointer, as_pointer_invoker},
};

utils::Span<vm::IntrinsicEntry> SystemRuntimeCompilerServicesUnsafe::get_intrinsic_entries() noexcept
{
    return utils::Span<vm::IntrinsicEntry>(s_intrinsic_entries_system_runtime_compilerservices_unsafe,
                                           sizeof(s_intrinsic_entries_system_runtime_compilerservices_unsafe) / sizeof(vm::IntrinsicEntry));
}

} // namespace intrinsics
} // namespace leanclr

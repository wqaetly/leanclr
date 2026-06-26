#include "system_runtime_interopservices_memorymarshal.h"

#include "interp/eval_stack_op.h"
#include "vm/rt_array.h"

namespace leanclr
{
namespace intrinsics
{

RtResult<void*> SystemRuntimeInteropServicesMemoryMarshal::get_array_data_reference(vm::RtArray* array) noexcept
{
    if (array == nullptr)
    {
        RET_ERR(RtErr::NullReference);
    }

    RET_OK(vm::Array::get_array_data_start_as_ptr_void(array));
}

/// @intrinsic: System.Runtime.InteropServices.MemoryMarshal::GetArrayDataReference<>(T[])
/// @intrinsic: System.Runtime.InteropServices.MemoryMarshal::GetArrayDataReference(System.Array)
static RtResultVoid get_array_data_reference_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                     const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    vm::RtArray* array = interp::EvalStackOp::get_param<vm::RtArray*>(params, 0);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(void*, data, SystemRuntimeInteropServicesMemoryMarshal::get_array_data_reference(array));
    interp::EvalStackOp::set_return(ret, data);
    RET_VOID_OK();
}

static vm::IntrinsicEntry s_intrinsic_entries_system_runtime_interopservices_memorymarshal[] = {
    {"System.Runtime.InteropServices.MemoryMarshal::GetArrayDataReference<>",
     (vm::IntrinsicFunction)&SystemRuntimeInteropServicesMemoryMarshal::get_array_data_reference, get_array_data_reference_invoker},
    {"System.Runtime.InteropServices.MemoryMarshal::GetArrayDataReference(System.Array)",
     (vm::IntrinsicFunction)&SystemRuntimeInteropServicesMemoryMarshal::get_array_data_reference, get_array_data_reference_invoker},
};

utils::Span<vm::IntrinsicEntry> SystemRuntimeInteropServicesMemoryMarshal::get_intrinsic_entries() noexcept
{
    return utils::Span<vm::IntrinsicEntry>(s_intrinsic_entries_system_runtime_interopservices_memorymarshal,
                                           sizeof(s_intrinsic_entries_system_runtime_interopservices_memorymarshal) / sizeof(vm::IntrinsicEntry));
}

} // namespace intrinsics
} // namespace leanclr

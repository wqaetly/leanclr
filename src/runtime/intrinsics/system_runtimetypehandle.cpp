#include "system_runtimetypehandle.h"

#include "vm/class.h"
#include "vm/reflection.h"

namespace leanclr
{
namespace intrinsics
{

RtResult<vm::RtReflectionRuntimeType*> SystemRuntimeTypeHandle::get_runtime_type(const metadata::RtClass* method_table) noexcept
{
    if (method_table == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionType*, ref_type, vm::Reflection::get_klass_reflection_object(method_table));
    RET_OK(reinterpret_cast<vm::RtReflectionRuntimeType*>(ref_type));
}

RtResult<vm::RtReflectionRuntimeType*> SystemRuntimeTypeHandle::get_runtime_type_from_handle(void* runtime_type_handle) noexcept
{
    if (runtime_type_handle == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    auto runtime_type_klass = vm::Class::get_corlib_types().cls_runtimetype;
    auto direct_runtime_type = reinterpret_cast<vm::RtReflectionRuntimeType*>(runtime_type_handle);
    if (direct_runtime_type->reflection_type.header.klass == runtime_type_klass)
    {
        RET_OK(direct_runtime_type);
    }

    auto runtime_type = *reinterpret_cast<vm::RtReflectionRuntimeType**>(runtime_type_handle);
    if (runtime_type == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    RET_OK(runtime_type);
}

/// @intrinsic: System.RuntimeTypeHandle::GetRuntimeType(System.Runtime.CompilerServices.MethodTable*)
static RtResultVoid get_runtime_type_invoker(metadata::RtManagedMethodPointer methodPtr, const metadata::RtMethodInfo* method,
                                             const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    (void)methodPtr;
    (void)method;
    const metadata::RtClass* method_table = interp::EvalStackOp::get_param<const metadata::RtClass*>(params, 0);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionRuntimeType*, runtime_type, SystemRuntimeTypeHandle::get_runtime_type(method_table));
    interp::EvalStackOp::set_return(ret, runtime_type);
    RET_VOID_OK();
}

/// @intrinsic: System.RuntimeTypeHandle::GetRuntimeType()
static RtResultVoid get_runtime_type_from_handle_invoker(metadata::RtManagedMethodPointer methodPtr, const metadata::RtMethodInfo* method,
                                                         const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    (void)methodPtr;
    (void)method;
    void* runtime_type_handle = interp::EvalStackOp::get_param<void*>(params, 0);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionRuntimeType*, runtime_type,
                                            SystemRuntimeTypeHandle::get_runtime_type_from_handle(runtime_type_handle));
    interp::EvalStackOp::set_return(ret, runtime_type);
    RET_VOID_OK();
}

static vm::IntrinsicEntry s_intrinsic_entries_system_runtimetypehandle[] = {
    {"System.RuntimeTypeHandle::GetRuntimeType(System.Runtime.CompilerServices.MethodTable*)",
     (vm::IntrinsicFunction)&SystemRuntimeTypeHandle::get_runtime_type, get_runtime_type_invoker},
    {"System.RuntimeTypeHandle::GetRuntimeType()", (vm::IntrinsicFunction)&SystemRuntimeTypeHandle::get_runtime_type_from_handle,
     get_runtime_type_from_handle_invoker},
};

utils::Span<vm::IntrinsicEntry> SystemRuntimeTypeHandle::get_intrinsic_entries() noexcept
{
    constexpr size_t entry_count = sizeof(s_intrinsic_entries_system_runtimetypehandle) / sizeof(s_intrinsic_entries_system_runtimetypehandle[0]);
    return utils::Span<vm::IntrinsicEntry>(s_intrinsic_entries_system_runtimetypehandle, entry_count);
}

} // namespace intrinsics
} // namespace leanclr

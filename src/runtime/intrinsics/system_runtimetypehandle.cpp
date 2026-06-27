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

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtClass*, klass,
                                            vm::Reflection::get_class_from_net10_method_table(method_table));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionType*, ref_type, vm::Reflection::get_klass_reflection_object(klass));
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

RtResult<bool> SystemRuntimeTypeHandle::can_cast_to(vm::RtReflectionRuntimeType* source_type, vm::RtReflectionRuntimeType* target_type) noexcept
{
    if (source_type == nullptr || target_type == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    const metadata::RtTypeSig* source_type_sig = source_type->reflection_type.type_handle;
    const metadata::RtTypeSig* target_type_sig = target_type->reflection_type.type_handle;
    if (source_type_sig == target_type_sig)
    {
        RET_OK(true);
    }
    if (source_type_sig->is_by_ref() || target_type_sig->is_by_ref())
    {
        RET_OK(false);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, source_class, vm::Class::get_class_from_typesig(source_type_sig));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, target_class, vm::Class::get_class_from_typesig(target_type_sig));
    RET_ERR_ON_FAIL(vm::Class::initialize_super_types(source_class));
    RET_ERR_ON_FAIL(vm::Class::initialize_interfaces(source_class));
    RET_ERR_ON_FAIL(vm::Class::initialize_super_types(target_class));
    RET_ERR_ON_FAIL(vm::Class::initialize_interfaces(target_class));
    RET_OK(vm::Class::is_assignable_from(source_class, target_class));
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

/// @intrinsic: System.RuntimeTypeHandle::CanCastTo(System.RuntimeType,System.RuntimeType)
static RtResultVoid can_cast_to_invoker(metadata::RtManagedMethodPointer methodPtr, const metadata::RtMethodInfo* method,
                                        const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    (void)methodPtr;
    (void)method;
    auto source_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType*>(params, 0);
    auto target_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType*>(params, 1);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, result, SystemRuntimeTypeHandle::can_cast_to(source_type, target_type));
    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

static vm::IntrinsicEntry s_intrinsic_entries_system_runtimetypehandle[] = {
    {"System.RuntimeTypeHandle::GetRuntimeType(System.Runtime.CompilerServices.MethodTable*)",
     (vm::IntrinsicFunction)&SystemRuntimeTypeHandle::get_runtime_type, get_runtime_type_invoker},
    {"System.RuntimeTypeHandle::GetRuntimeType()", (vm::IntrinsicFunction)&SystemRuntimeTypeHandle::get_runtime_type_from_handle,
     get_runtime_type_from_handle_invoker},
    {"System.RuntimeTypeHandle::CanCastTo(System.RuntimeType,System.RuntimeType)",
     (vm::IntrinsicFunction)&SystemRuntimeTypeHandle::can_cast_to, can_cast_to_invoker},
    {"System.RuntimeTypeHandle::CanCastTo", (vm::IntrinsicFunction)&SystemRuntimeTypeHandle::can_cast_to, can_cast_to_invoker},
};

utils::Span<vm::IntrinsicEntry> SystemRuntimeTypeHandle::get_intrinsic_entries() noexcept
{
    constexpr size_t entry_count = sizeof(s_intrinsic_entries_system_runtimetypehandle) / sizeof(s_intrinsic_entries_system_runtimetypehandle[0]);
    return utils::Span<vm::IntrinsicEntry>(s_intrinsic_entries_system_runtimetypehandle, entry_count);
}

} // namespace intrinsics
} // namespace leanclr

#include "system_type.h"

#include "vm/class.h"
#include "vm/reflection.h"
#include "vm/type.h"

namespace leanclr
{
namespace intrinsics
{

RtResult<vm::RtReflectionRuntimeType*> SystemType::get_type_from_handle(const metadata::RtTypeSig* type_handle) noexcept
{
    if (type_handle == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_handle));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionType*, ref_type, vm::Reflection::get_klass_reflection_object(klass));
    RET_OK(reinterpret_cast<vm::RtReflectionRuntimeType*>(ref_type));
}

RtResult<bool> SystemType::get_is_value_type(vm::RtReflectionRuntimeType* runtime_type) noexcept
{
    if (runtime_type == nullptr)
    {
        RET_ERR(RtErr::NullReference);
    }

    return vm::Type::is_value_type(runtime_type->reflection_type.type_handle);
}

/// @intrinsic: System.Type::GetTypeFromHandle(System.RuntimeTypeHandle)
static RtResultVoid get_type_from_handle_invoker(metadata::RtManagedMethodPointer methodPtr, const metadata::RtMethodInfo* method,
                                                 const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    (void)methodPtr;
    (void)method;
    const metadata::RtTypeSig* type_handle = interp::EvalStackOp::get_param<const metadata::RtTypeSig*>(params, 0);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionRuntimeType*, runtime_type, SystemType::get_type_from_handle(type_handle));
    interp::EvalStackOp::set_return(ret, runtime_type);
    RET_VOID_OK();
}

/// @intrinsic: System.Type::get_IsValueType
static RtResultVoid get_is_value_type_invoker(metadata::RtManagedMethodPointer methodPtr, const metadata::RtMethodInfo* method,
                                              const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    (void)methodPtr;
    (void)method;
    auto runtime_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType*>(params, 0);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, is_value_type, SystemType::get_is_value_type(runtime_type));
    interp::EvalStackOp::set_return(ret, is_value_type);
    RET_VOID_OK();
}

static vm::IntrinsicEntry s_intrinsic_entries_system_type[] = {
    {"System.Type::GetTypeFromHandle(System.RuntimeTypeHandle)", (vm::IntrinsicFunction)&SystemType::get_type_from_handle,
     get_type_from_handle_invoker},
    {"System.Type::get_IsValueType", (vm::IntrinsicFunction)&SystemType::get_is_value_type, get_is_value_type_invoker},
    {"System.RuntimeType::get_IsValueType", (vm::IntrinsicFunction)&SystemType::get_is_value_type, get_is_value_type_invoker},
    {"System.RuntimeType::IsValueTypeImpl()", (vm::IntrinsicFunction)&SystemType::get_is_value_type, get_is_value_type_invoker},
};

utils::Span<vm::IntrinsicEntry> SystemType::get_intrinsic_entries() noexcept
{
    constexpr size_t entry_count = sizeof(s_intrinsic_entries_system_type) / sizeof(s_intrinsic_entries_system_type[0]);
    return utils::Span<vm::IntrinsicEntry>(s_intrinsic_entries_system_type, entry_count);
}

} // namespace intrinsics
} // namespace leanclr

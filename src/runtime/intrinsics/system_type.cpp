#include "system_type.h"

#include "metadata/metadata_compare.h"
#include "vm/class.h"
#include "vm/reflection.h"
#include "vm/type.h"

namespace leanclr
{
namespace intrinsics
{
namespace
{

bool is_plausible_pointer(const void* value) noexcept
{
    auto address = reinterpret_cast<uintptr_t>(value);
    return address >= 0x10000 && (address % alignof(void*) == 0);
}

bool is_runtime_type_object(const void* value, const metadata::RtClass* runtime_type_klass) noexcept
{
    if (!is_plausible_pointer(value))
    {
        return false;
    }

    auto obj = reinterpret_cast<const vm::RtObject*>(value);
    return obj->klass == runtime_type_klass;
}

} // namespace

RtResult<vm::RtReflectionRuntimeType*> SystemType::get_type_from_handle(const void* type_handle) noexcept
{
    if (type_handle == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    auto runtime_type_klass = vm::Class::get_corlib_types().cls_runtimetype;
    if (is_runtime_type_object(type_handle, runtime_type_klass))
    {
        RET_OK(reinterpret_cast<vm::RtReflectionRuntimeType*>(const_cast<void*>(type_handle)));
    }

    auto type_object = *reinterpret_cast<void* const*>(type_handle);
    if (is_runtime_type_object(type_object, runtime_type_klass))
    {
        RET_OK(reinterpret_cast<vm::RtReflectionRuntimeType*>(type_object));
    }

    auto type_sig = reinterpret_cast<const metadata::RtTypeSig*>(type_handle);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));
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

RtResult<bool> SystemType::equals(vm::RtReflectionRuntimeType* left, vm::RtReflectionRuntimeType* right) noexcept
{
    if (left == right)
    {
        RET_OK(true);
    }
    if (left == nullptr || right == nullptr)
    {
        RET_OK(false);
    }
    const auto& corlib_types = vm::Class::get_corlib_types();
    if (left->reflection_type.header.klass != corlib_types.cls_runtimetype || right->reflection_type.header.klass != corlib_types.cls_runtimetype)
    {
        RET_OK(false);
    }

    RET_OK(metadata::MetadataCompare::is_typesig_equal_ignore_attrs(
        left->reflection_type.type_handle,
        right->reflection_type.type_handle,
        false));
}

/// @intrinsic: System.Type::GetTypeFromHandle(System.RuntimeTypeHandle)
static RtResultVoid get_type_from_handle_invoker(metadata::RtManagedMethodPointer methodPtr, const metadata::RtMethodInfo* method,
                                                 const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    (void)methodPtr;
    (void)method;
    const void* type_handle = interp::EvalStackOp::get_param<const void*>(params, 0);

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

/// @intrinsic: System.Type::op_Equality(System.Type,System.Type)
static RtResultVoid equals_invoker(metadata::RtManagedMethodPointer methodPtr, const metadata::RtMethodInfo* method,
                                   const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    (void)methodPtr;
    (void)method;
    auto left = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType*>(params, 0);
    auto right = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType*>(params, 1);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, result, SystemType::equals(left, right));
    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

/// @intrinsic: System.Type::op_Inequality(System.Type,System.Type)
static RtResultVoid not_equals_invoker(metadata::RtManagedMethodPointer methodPtr, const metadata::RtMethodInfo* method,
                                       const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    (void)methodPtr;
    (void)method;
    auto left = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType*>(params, 0);
    auto right = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType*>(params, 1);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, result, SystemType::equals(left, right));
    interp::EvalStackOp::set_return(ret, !result);
    RET_VOID_OK();
}

static vm::IntrinsicEntry s_intrinsic_entries_system_type[] = {
    {"System.Type::GetTypeFromHandle(System.RuntimeTypeHandle)", (vm::IntrinsicFunction)&SystemType::get_type_from_handle,
     get_type_from_handle_invoker},
    {"System.Type::get_IsValueType", (vm::IntrinsicFunction)&SystemType::get_is_value_type, get_is_value_type_invoker},
    {"System.RuntimeType::get_IsValueType", (vm::IntrinsicFunction)&SystemType::get_is_value_type, get_is_value_type_invoker},
    {"System.RuntimeType::IsValueTypeImpl()", (vm::IntrinsicFunction)&SystemType::get_is_value_type, get_is_value_type_invoker},
    {"System.Type::op_Equality(System.Type,System.Type)", (vm::IntrinsicFunction)&SystemType::equals, equals_invoker},
    {"System.Type::op_Equality", (vm::IntrinsicFunction)&SystemType::equals, equals_invoker},
    {"System.Type::op_Inequality(System.Type,System.Type)", (vm::IntrinsicFunction)&SystemType::equals, not_equals_invoker},
    {"System.Type::op_Inequality", (vm::IntrinsicFunction)&SystemType::equals, not_equals_invoker},
};

utils::Span<vm::IntrinsicEntry> SystemType::get_intrinsic_entries() noexcept
{
    constexpr size_t entry_count = sizeof(s_intrinsic_entries_system_type) / sizeof(s_intrinsic_entries_system_type[0]);
    return utils::Span<vm::IntrinsicEntry>(s_intrinsic_entries_system_type, entry_count);
}

} // namespace intrinsics
} // namespace leanclr

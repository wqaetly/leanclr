#include "system_runtimetype.h"

#include <cctype>
#include <cstring>

#include "interp/eval_stack_op.h"
#include "utils/string_builder.h"
#include "vm/class.h"
#include "vm/customattribute.h"
#include "vm/field.h"
#include "vm/method.h"
#include "vm/object.h"
#include "vm/reflection.h"
#include "vm/runtime.h"
#include "vm/rt_array.h"
#include "vm/rt_string.h"
#include "utils/rt_vector.h"

namespace leanclr
{
namespace intrinsics
{
namespace
{
constexpr int32_t BINDING_FLAGS_IGNORE_CASE = 0x1;
constexpr int32_t BINDING_FLAGS_DECLARED_ONLY = 0x2;
constexpr int32_t BINDING_FLAGS_INSTANCE = 0x4;
constexpr int32_t BINDING_FLAGS_STATIC = 0x8;
constexpr int32_t BINDING_FLAGS_PUBLIC = 0x10;
constexpr int32_t BINDING_FLAGS_NON_PUBLIC = 0x20;
constexpr int32_t BINDING_FLAGS_FLATTEN_HIERARCHY = 0x40;

static bool is_ascii_case_insensitive_equal(const char* left, const char* right) noexcept
{
    while (*left != '\0' && *right != '\0')
    {
        char left_ch = static_cast<char>(std::tolower(static_cast<unsigned char>(*left)));
        char right_ch = static_cast<char>(std::tolower(static_cast<unsigned char>(*right)));
        if (left_ch != right_ch)
        {
            return false;
        }
        ++left;
        ++right;
    }
    return *left == *right;
}

static bool field_name_matches(const char* field_name, const char* search_name, int32_t binding_flags) noexcept
{
    if ((binding_flags & BINDING_FLAGS_IGNORE_CASE) != 0)
    {
        return is_ascii_case_insensitive_equal(field_name, search_name);
    }
    return std::strcmp(field_name, search_name) == 0;
}

static bool field_matches_binding_flags(const metadata::RtFieldInfo* field, const metadata::RtClass* declaring_klass,
                                        const metadata::RtClass* target_klass, int32_t binding_flags) noexcept
{
    if (vm::Field::is_public(field))
    {
        if ((binding_flags & BINDING_FLAGS_PUBLIC) == 0)
        {
            return false;
        }
    }
    else
    {
        if ((binding_flags & BINDING_FLAGS_NON_PUBLIC) == 0)
        {
            return false;
        }
        if (vm::Field::is_private(field) && declaring_klass != target_klass)
        {
            return false;
        }
    }

    if (vm::Field::is_static_included_literal_and_rva(field))
    {
        if ((binding_flags & BINDING_FLAGS_STATIC) == 0)
        {
            return false;
        }
        if (declaring_klass != target_klass && (binding_flags & BINDING_FLAGS_FLATTEN_HIERARCHY) == 0)
        {
            return false;
        }
    }
    else if ((binding_flags & BINDING_FLAGS_INSTANCE) == 0)
    {
        return false;
    }

    return true;
}

static bool method_matches_binding_flags(const metadata::RtMethodInfo* method, const metadata::RtClass* declaring_klass,
                                         const metadata::RtClass* target_klass, int32_t binding_flags) noexcept
{
    if (vm::Method::is_ctor_or_cctor(method))
    {
        return false;
    }

    if (vm::Method::is_public(method))
    {
        if ((binding_flags & BINDING_FLAGS_PUBLIC) == 0)
        {
            return false;
        }
    }
    else
    {
        if ((binding_flags & BINDING_FLAGS_NON_PUBLIC) == 0)
        {
            return false;
        }
        if (vm::Method::is_private(method) && declaring_klass != target_klass)
        {
            return false;
        }
    }

    if (vm::Method::is_static(method))
    {
        if ((binding_flags & BINDING_FLAGS_STATIC) == 0)
        {
            return false;
        }
        if (declaring_klass != target_klass && (binding_flags & BINDING_FLAGS_FLATTEN_HIERARCHY) == 0)
        {
            return false;
        }
    }
    else if ((binding_flags & BINDING_FLAGS_INSTANCE) == 0)
    {
        return false;
    }

    return true;
}

static RtResult<bool> is_generic_type_by_typesig(const metadata::RtTypeSig* type_sig) noexcept
{
    if (type_sig == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    if (type_sig->is_by_ref())
    {
        RET_OK(false);
    }

    if (type_sig->ele_type == metadata::RtElementType::GenericInst)
    {
        RET_OK(true);
    }

    if (type_sig->ele_type != metadata::RtElementType::Class && type_sig->ele_type != metadata::RtElementType::ValueType)
    {
        RET_OK(false);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));
    RET_OK(vm::Class::is_generic(klass));
}

static RtResult<bool> is_generic_type_definition_by_typesig(const metadata::RtTypeSig* type_sig) noexcept
{
    if (type_sig == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    if (type_sig->is_by_ref() || type_sig->ele_type != metadata::RtElementType::Class &&
                                     type_sig->ele_type != metadata::RtElementType::ValueType)
    {
        RET_OK(false);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));
    RET_OK(vm::Class::is_generic(klass));
}
} // namespace

RtResult<vm::RtReflectionField*> SystemRuntimeType::get_field(vm::RtReflectionRuntimeType* runtime_type, vm::RtString* name,
                                                              int32_t binding_flags) noexcept
{
    if (runtime_type == nullptr)
    {
        RET_ERR(RtErr::NullReference);
    }
    if (name == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    const metadata::RtTypeSig* type_sig = runtime_type->reflection_type.type_handle;
    if (type_sig->by_ref)
    {
        RET_OK(nullptr);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));

    utils::Utf8StringBuilder name_buf(vm::String::get_chars_ptr(name), static_cast<size_t>(vm::String::get_length(name)));
    name_buf.sure_null_terminator_but_not_append();
    const char* search_name = name_buf.get_const_chars();

    const metadata::RtClass* current_klass = klass;
    while (current_klass != nullptr)
    {
        RET_ERR_ON_FAIL(vm::Class::initialize_fields(const_cast<metadata::RtClass*>(current_klass)));
        for (uint32_t i = 0; i < current_klass->field_count; ++i)
        {
            const metadata::RtFieldInfo* field = current_klass->fields + i;
            if (!field_name_matches(field->name, search_name, binding_flags))
            {
                continue;
            }
            if (!field_matches_binding_flags(field, current_klass, klass, binding_flags))
            {
                continue;
            }
            return vm::Reflection::get_field_reflection_object(field, klass);
        }

        if ((binding_flags & BINDING_FLAGS_DECLARED_ONLY) != 0)
        {
            break;
        }
        current_klass = current_klass->parent;
    }

    RET_OK(nullptr);
}

RtResult<vm::RtArray*> SystemRuntimeType::get_methods(vm::RtReflectionRuntimeType* runtime_type, int32_t binding_flags) noexcept
{
    if (runtime_type == nullptr)
    {
        RET_ERR(RtErr::NullReference);
    }

    const auto& corlib_types = vm::Class::get_corlib_types();
    const metadata::RtTypeSig* type_sig = runtime_type->reflection_type.type_handle;
    if (type_sig->by_ref)
    {
        return LEANCLR_NEW_EMPTY_SZARRAY_BY_ELE_KLASS_INTERNAL(corlib_types.cls_reflection_method,
                                                               "SystemRuntimeType::get_methods");
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));

    utils::Vector<const metadata::RtMethodInfo*> methods;
    const metadata::RtClass* current_klass = klass;
    while (current_klass != nullptr)
    {
        RET_ERR_ON_FAIL(vm::Class::initialize_methods(const_cast<metadata::RtClass*>(current_klass)));
        for (uint32_t i = 0; i < current_klass->method_count; ++i)
        {
            const metadata::RtMethodInfo* method = current_klass->methods[i];
            if (!method_matches_binding_flags(method, current_klass, klass, binding_flags))
            {
                continue;
            }
            methods.push_back(method);
        }

        if ((binding_flags & BINDING_FLAGS_DECLARED_ONLY) != 0)
        {
            break;
        }
        current_klass = current_klass->parent;
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(
        vm::RtArray*, result,
        LEANCLR_NEW_SZARRAY_FROM_ELE_KLASS_INTERNAL(corlib_types.cls_reflection_method, static_cast<int32_t>(methods.size()),
                                                    "SystemRuntimeType::get_methods"));
    for (int32_t i = 0; i < static_cast<int32_t>(methods.size()); ++i)
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionMethod*, ref_method,
                                                vm::Reflection::get_method_reflection_object(methods[static_cast<size_t>(i)], klass));
        vm::Array::set_array_data_at<vm::RtReflectionMethod*>(result, i, ref_method);
    }

    RET_OK(result);
}

RtResult<vm::RtArray*> SystemRuntimeType::get_custom_attributes(vm::RtReflectionRuntimeType* runtime_type,
                                                                vm::RtReflectionRuntimeType* attribute_type, bool inherit) noexcept
{
    (void)inherit;

    if (runtime_type == nullptr)
    {
        RET_ERR(RtErr::NullReference);
    }

    metadata::RtClass* attr_klass = nullptr;
    if (attribute_type != nullptr)
    {
        UNWRAP_OR_RET_ERR_ON_FAIL(attr_klass, vm::Class::get_class_from_typesig(attribute_type->reflection_type.type_handle));
    }

    const metadata::RtTypeSig* type_sig = runtime_type->reflection_type.type_handle;
    if (type_sig->by_ref)
    {
        return LEANCLR_NEW_EMPTY_SZARRAY_BY_ELE_KLASS_INTERNAL(vm::Class::get_corlib_types().cls_attribute,
                                                               "SystemRuntimeType::get_custom_attributes");
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));
    return vm::CustomAttribute::get_customattributes_on_target_token(klass->image, klass->token, attr_klass);
}

RtResult<vm::RtReflectionRuntimeType*> SystemRuntimeType::get_parent_type(vm::RtReflectionRuntimeType* runtime_type) noexcept
{
    if (runtime_type == nullptr)
    {
        RET_ERR(RtErr::NullReference);
    }

    const metadata::RtTypeSig* type_sig = runtime_type->reflection_type.type_handle;
    if (type_sig->by_ref)
    {
        RET_OK(nullptr);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));
    if (klass->parent == nullptr)
    {
        RET_OK(nullptr);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionType*, parent_type, vm::Reflection::get_klass_reflection_object(klass->parent));
    RET_OK(reinterpret_cast<vm::RtReflectionRuntimeType*>(parent_type));
}

RtResult<bool> SystemRuntimeType::get_is_actual_interface(vm::RtReflectionRuntimeType* runtime_type) noexcept
{
    if (runtime_type == nullptr)
    {
        RET_ERR(RtErr::NullReference);
    }

    const metadata::RtTypeSig* type_sig = runtime_type->reflection_type.type_handle;
    if (type_sig->by_ref)
    {
        RET_OK(false);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));
    RET_OK(vm::Class::is_interface(klass));
}

RtResult<bool> SystemRuntimeType::get_is_actual_enum(vm::RtReflectionRuntimeType* runtime_type) noexcept
{
    if (runtime_type == nullptr)
    {
        RET_ERR(RtErr::NullReference);
    }

    const metadata::RtTypeSig* type_sig = runtime_type->reflection_type.type_handle;
    if (type_sig->by_ref)
    {
        RET_OK(false);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));
    RET_OK(vm::Class::is_enum_type(klass));
}

RtResult<bool> SystemRuntimeType::is_delegate(vm::RtReflectionRuntimeType* runtime_type) noexcept
{
    if (runtime_type == nullptr)
    {
        RET_ERR(RtErr::NullReference);
    }

    const metadata::RtTypeSig* type_sig = runtime_type->reflection_type.type_handle;
    if (type_sig->by_ref)
    {
        RET_OK(false);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));
    RET_ERR_ON_FAIL(vm::Class::initialize_super_types(klass));
    RET_OK(vm::Class::is_multicastdelegate_subclass(klass));
}

RtResult<bool> SystemRuntimeType::get_is_generic_type(vm::RtReflectionRuntimeType* runtime_type) noexcept
{
    if (runtime_type == nullptr)
    {
        RET_ERR(RtErr::NullReference);
    }

    auto type_sig = runtime_type->reflection_type.type_handle;
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, result, is_generic_type_by_typesig(type_sig));
    RET_OK(result);
}

RtResult<bool> SystemRuntimeType::get_is_generic_type_definition(vm::RtReflectionRuntimeType* runtime_type) noexcept
{
    if (runtime_type == nullptr)
    {
        RET_ERR(RtErr::NullReference);
    }

    auto type_sig = runtime_type->reflection_type.type_handle;
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, result, is_generic_type_definition_by_typesig(type_sig));
    RET_OK(result);
}

RtResult<vm::RtObject*> SystemRuntimeType::create_instance(vm::RtReflectionRuntimeType* runtime_type) noexcept
{
    if (runtime_type == nullptr)
    {
        RET_ERR(RtErr::NullReference);
    }

    const metadata::RtTypeSig* type_sig = runtime_type->reflection_type.type_handle;
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));
    RET_ERR_ON_FAIL(vm::Class::initialize_all(klass));

    const metadata::RtMethodInfo* ctor = vm::Method::find_matched_method_in_class_by_name_and_param_count(klass, ".ctor", 0);
    if (ctor != nullptr && !vm::Class::is_value_type(klass))
    {
        return vm::Runtime::invoke_object_arguments_without_run_cctor(ctor, nullptr, nullptr, 0);
    }

    return LEANCLR_CREATE_INSTANCE_INTERNAL(type_sig, "SystemRuntimeType::create_instance");
}

RtResultVoid SystemRuntimeType::call_default_struct_constructor(vm::RtReflectionRuntimeType* runtime_type, void* data) noexcept
{
    if (runtime_type == nullptr)
    {
        RET_ERR(RtErr::NullReference);
    }
    if (data == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    const metadata::RtTypeSig* type_sig = runtime_type->reflection_type.type_handle;
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));
    RET_ERR_ON_FAIL(vm::Class::initialize_methods(klass));
    const metadata::RtMethodInfo* ctor = vm::Method::find_matched_method_in_class_by_name_and_param_count(klass, ".ctor", 0);
    if (ctor == nullptr)
    {
        RET_VOID_OK();
    }

    interp::RtStackObject args[1]{};
    args[0].ptr = data;
    RET_ERR_ON_FAIL(vm::Runtime::invoke_stackobject_arguments_with_run_cctor(ctor, args, nullptr));
    RET_VOID_OK();
}

/// @intrinsic: System.RuntimeType::GetField(System.String,System.Reflection.BindingFlags)
static RtResultVoid get_field_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                      interp::RtStackObject* ret) noexcept
{
    auto runtime_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType*>(params, 0);
    auto name = interp::EvalStackOp::get_param<vm::RtString*>(params, 1);
    int32_t binding_flags = interp::EvalStackOp::get_param<int32_t>(params, 2);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionField*, field, SystemRuntimeType::get_field(runtime_type, name, binding_flags));
    interp::EvalStackOp::set_return(ret, field);
    RET_VOID_OK();
}

/// @intrinsic: System.RuntimeType::GetMethods(System.Reflection.BindingFlags)
static RtResultVoid get_methods_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                        interp::RtStackObject* ret) noexcept
{
    auto runtime_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType*>(params, 0);
    int32_t binding_flags = interp::EvalStackOp::get_param<int32_t>(params, 1);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, methods, SystemRuntimeType::get_methods(runtime_type, binding_flags));
    interp::EvalStackOp::set_return(ret, methods);
    RET_VOID_OK();
}

/// @intrinsic: System.RuntimeType::GetCustomAttributes(System.Type,System.Boolean)
static RtResultVoid get_custom_attributes_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                                  interp::RtStackObject* ret) noexcept
{
    auto runtime_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType*>(params, 0);
    auto attribute_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType*>(params, 1);
    bool inherit = interp::EvalStackOp::get_param<bool>(params, 2);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, attributes, SystemRuntimeType::get_custom_attributes(runtime_type, attribute_type, inherit));
    interp::EvalStackOp::set_return(ret, attributes);
    RET_VOID_OK();
}

/// @intrinsic: System.RuntimeType::GetParentType()
static RtResultVoid get_parent_type_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                            interp::RtStackObject* ret) noexcept
{
    auto runtime_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType*>(params, 0);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionRuntimeType*, parent_type, SystemRuntimeType::get_parent_type(runtime_type));
    interp::EvalStackOp::set_return(ret, parent_type);
    RET_VOID_OK();
}

/// @intrinsic: System.RuntimeType::get_IsActualEnum()
static RtResultVoid get_is_actual_enum_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                               interp::RtStackObject* ret) noexcept
{
    auto runtime_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType*>(params, 0);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, is_enum, SystemRuntimeType::get_is_actual_enum(runtime_type));
    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(is_enum));
    RET_VOID_OK();
}

/// @intrinsic: System.RuntimeType::get_IsActualInterface()
static RtResultVoid get_is_actual_interface_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                                    interp::RtStackObject* ret) noexcept
{
    auto runtime_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType*>(params, 0);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, is_interface, SystemRuntimeType::get_is_actual_interface(runtime_type));
    interp::EvalStackOp::set_return(ret, is_interface);
    RET_VOID_OK();
}

/// @intrinsic: System.RuntimeType::IsDelegate()
static RtResultVoid is_delegate_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                        interp::RtStackObject* ret) noexcept
{
    auto runtime_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType*>(params, 0);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, result, SystemRuntimeType::is_delegate(runtime_type));
    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(result));
    RET_VOID_OK();
}

/// @intrinsic: System.RuntimeType::get_IsGenericType()
static RtResultVoid get_is_generic_type_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                                interp::RtStackObject* ret) noexcept
{
    auto runtime_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType*>(params, 0);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, is_generic_type, SystemRuntimeType::get_is_generic_type(runtime_type));
    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(is_generic_type));
    RET_VOID_OK();
}

/// @intrinsic: System.RuntimeType::get_IsGenericTypeDefinition()
static RtResultVoid get_is_generic_type_definition_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                           const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    auto runtime_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType*>(params, 0);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, is_generic_type_definition,
                                            SystemRuntimeType::get_is_generic_type_definition(runtime_type));
    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(is_generic_type_definition));
    RET_VOID_OK();
}

/// @intrinsic: System.RuntimeType::CreateInstanceOfT()
static RtResultVoid create_instance_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                            interp::RtStackObject* ret) noexcept
{
    auto runtime_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType*>(params, 0);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, obj, SystemRuntimeType::create_instance(runtime_type));
    interp::EvalStackOp::set_return(ret, obj);
    RET_VOID_OK();
}

/// @intrinsic: System.RuntimeType::CreateInstanceDefaultCtor(System.Boolean,System.Boolean)
static RtResultVoid create_instance_default_ctor_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                         const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    auto runtime_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType*>(params, 0);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, obj, SystemRuntimeType::create_instance(runtime_type));
    interp::EvalStackOp::set_return(ret, obj);
    RET_VOID_OK();
}

/// @intrinsic: System.RuntimeType::CallDefaultStructConstructor(System.Byte&)
static RtResultVoid call_default_struct_constructor_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                            const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto runtime_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType*>(params, 0);
    void* data = interp::EvalStackOp::get_param<void*>(params, 1);

    RET_ERR_ON_FAIL(SystemRuntimeType::call_default_struct_constructor(runtime_type, data));
    RET_VOID_OK();
}

static vm::IntrinsicEntry s_intrinsic_entries_system_runtimetype[] = {
    {"System.RuntimeType::GetField(System.String,System.Reflection.BindingFlags)", (vm::IntrinsicFunction)&SystemRuntimeType::get_field, get_field_invoker},
    {"System.RuntimeType::GetMethods(System.Reflection.BindingFlags)", (vm::IntrinsicFunction)&SystemRuntimeType::get_methods, get_methods_invoker},
    {"System.RuntimeType::GetCustomAttributes(System.Type,System.Boolean)", (vm::IntrinsicFunction)&SystemRuntimeType::get_custom_attributes,
     get_custom_attributes_invoker},
    {"System.RuntimeType::get_BaseType", (vm::IntrinsicFunction)&SystemRuntimeType::get_parent_type, get_parent_type_invoker},
    {"System.RuntimeType::get_BaseType()", (vm::IntrinsicFunction)&SystemRuntimeType::get_parent_type, get_parent_type_invoker},
    {"System.RuntimeType::GetBaseType()", (vm::IntrinsicFunction)&SystemRuntimeType::get_parent_type, get_parent_type_invoker},
    {"System.RuntimeType::GetParentType()", (vm::IntrinsicFunction)&SystemRuntimeType::get_parent_type, get_parent_type_invoker},
    {"System.RuntimeType::get_IsActualEnum", (vm::IntrinsicFunction)&SystemRuntimeType::get_is_actual_enum, get_is_actual_enum_invoker},
    {"System.RuntimeType::get_IsActualEnum()", (vm::IntrinsicFunction)&SystemRuntimeType::get_is_actual_enum, get_is_actual_enum_invoker},
    {"System.RuntimeType::get_IsActualInterface", (vm::IntrinsicFunction)&SystemRuntimeType::get_is_actual_interface, get_is_actual_interface_invoker},
    {"System.RuntimeType::get_IsActualInterface()", (vm::IntrinsicFunction)&SystemRuntimeType::get_is_actual_interface, get_is_actual_interface_invoker},
    {"System.RuntimeType::IsDelegate()", (vm::IntrinsicFunction)&SystemRuntimeType::is_delegate, is_delegate_invoker},
    {"System.RuntimeType::get_IsGenericType", (vm::IntrinsicFunction)&SystemRuntimeType::get_is_generic_type, get_is_generic_type_invoker},
    {"System.RuntimeType::get_IsGenericType()", (vm::IntrinsicFunction)&SystemRuntimeType::get_is_generic_type, get_is_generic_type_invoker},
    {"System.RuntimeType::get_IsGenericTypeDefinition",
     (vm::IntrinsicFunction)&SystemRuntimeType::get_is_generic_type_definition, get_is_generic_type_definition_invoker},
    {"System.RuntimeType::get_IsGenericTypeDefinition()",
     (vm::IntrinsicFunction)&SystemRuntimeType::get_is_generic_type_definition, get_is_generic_type_definition_invoker},
    {"System.RuntimeType::CreateInstanceOfT()", (vm::IntrinsicFunction)&SystemRuntimeType::create_instance, create_instance_invoker},
    {"System.RuntimeType::CreateInstanceDefaultCtor(System.Boolean,System.Boolean)", (vm::IntrinsicFunction)&SystemRuntimeType::create_instance,
     create_instance_default_ctor_invoker},
    {"System.RuntimeType::CallDefaultStructConstructor(System.Byte&)",
     (vm::IntrinsicFunction)&SystemRuntimeType::call_default_struct_constructor, call_default_struct_constructor_invoker},
};

utils::Span<vm::IntrinsicEntry> SystemRuntimeType::get_intrinsic_entries() noexcept
{
    return utils::Span<vm::IntrinsicEntry>(s_intrinsic_entries_system_runtimetype,
                                           sizeof(s_intrinsic_entries_system_runtimetype) / sizeof(vm::IntrinsicEntry));
}

} // namespace intrinsics
} // namespace leanclr

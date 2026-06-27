#include "system_reflection_runtimepropertyinfo.h"
#include "icall_base.h"
#include "vm/class.h"
#include "vm/field.h"
#include "vm/object.h"
#include "vm/property.h"
#include "vm/reflection.h"
#include "vm/rt_array.h"
#include "vm/rt_string.h"
#include "metadata/module_def.h"

namespace leanclr
{
namespace icalls
{
namespace
{
constexpr int32_t METHOD_ATTRIBUTE_PRIVATE = 0x0001;
constexpr int32_t METHOD_ATTRIBUTE_PUBLIC = 0x0006;
constexpr int32_t METHOD_ATTRIBUTE_MEMBER_ACCESS_MASK = 0x0007;
constexpr int32_t METHOD_ATTRIBUTE_STATIC = 0x0010;
constexpr int32_t BINDING_FLAGS_INSTANCE = 0x0004;
constexpr int32_t BINDING_FLAGS_STATIC = 0x0008;
constexpr int32_t BINDING_FLAGS_PUBLIC = 0x0010;
constexpr int32_t BINDING_FLAGS_NON_PUBLIC = 0x0020;

static RtResultVoid set_instance_field_value(vm::RtObject* obj, const char* field_name, const void* value) noexcept
{
    const metadata::RtFieldInfo* field = vm::Class::get_field_for_name(obj->klass, field_name, true);
    if (field == nullptr)
    {
        RET_ERR(RtErr::MissingField);
    }

    return vm::Field::set_instance_value(field, obj, value);
}

static int32_t get_method_binding_flags(const metadata::RtMethodInfo* method) noexcept
{
    if (method == nullptr)
    {
        return BINDING_FLAGS_NON_PUBLIC | BINDING_FLAGS_INSTANCE;
    }

    bool is_public = (method->flags & METHOD_ATTRIBUTE_MEMBER_ACCESS_MASK) == METHOD_ATTRIBUTE_PUBLIC;
    bool is_static = (method->flags & METHOD_ATTRIBUTE_STATIC) != 0;
    return (is_public ? BINDING_FLAGS_PUBLIC : BINDING_FLAGS_NON_PUBLIC) |
           (is_static ? BINDING_FLAGS_STATIC : BINDING_FLAGS_INSTANCE);
}

static RtResult<vm::RtReflectionMethod*> create_runtime_method_info(const metadata::RtMethodInfo* method,
                                                                    vm::RtReflectionRuntimeType* declaring_type,
                                                                    vm::RtObject* reflected_type_cache) noexcept
{
    if (method == nullptr)
    {
        RET_OK(nullptr);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, obj,
                                            LEANCLR_NEWOBJ_INTERNAL(vm::Class::get_corlib_types().cls_reflection_method,
                                                                    "RuntimePropertyInfo::.ctor accessor"));
    auto method_obj = reinterpret_cast<vm::RtReflectionMethod*>(obj);
    method_obj->method = method;
    method_obj->reflected_type_cache = reflected_type_cache;
    method_obj->name = nullptr;
    method_obj->to_string = nullptr;
    method_obj->parameters = nullptr;
    method_obj->return_parameter = nullptr;
    method_obj->binding_flags = get_method_binding_flags(method);
    method_obj->method_attributes = static_cast<int32_t>(method->flags);
    method_obj->signature = nullptr;
    method_obj->declaring_type = declaring_type;
    method_obj->keepalive = nullptr;
    method_obj->invoker = nullptr;
    RET_OK(method_obj);
}

static RtResult<const metadata::RtPropertyInfo*> get_property_from_runtime_type(vm::RtReflectionRuntimeType* declaring_type,
                                                                               int32_t property_token) noexcept
{
    if (declaring_type == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    metadata::RtToken token = metadata::RtToken::decode(static_cast<metadata::EncodedTokenId>(property_token));
    if (token.table_type != metadata::TableType::Property || token.rid == 0)
    {
        RET_ERR(RtErr::BadImageFormat);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass,
                                            vm::Class::get_class_from_typesig(declaring_type->reflection_type.type_handle));
    RET_ERR_ON_FAIL(vm::Class::initialize_properties(klass));
    for (uint16_t i = 0; i < klass->property_count; ++i)
    {
        const metadata::RtPropertyInfo* property = klass->properties + i;
        if (property->token == static_cast<metadata::EncodedTokenId>(property_token))
        {
            RET_OK(property);
        }
    }

    RET_ERR(RtErr::MissingField);
}

static int32_t get_property_binding_flags(const metadata::RtPropertyInfo* property, bool* is_private) noexcept
{
    bool any_public = false;
    bool any_static = false;
    bool all_private = true;
    const metadata::RtMethodInfo* methods[] = {property->get_method, property->set_method};
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i)
    {
        const metadata::RtMethodInfo* method = methods[i];
        if (method == nullptr)
        {
            continue;
        }

        int32_t visibility = method->flags & METHOD_ATTRIBUTE_MEMBER_ACCESS_MASK;
        if (visibility == METHOD_ATTRIBUTE_PUBLIC)
        {
            any_public = true;
            all_private = false;
        }
        else if (visibility != METHOD_ATTRIBUTE_PRIVATE)
        {
            all_private = false;
        }
        if ((method->flags & METHOD_ATTRIBUTE_STATIC) != 0)
        {
            any_static = true;
        }
    }

    if (is_private != nullptr)
    {
        *is_private = all_private;
    }
    return (any_public ? BINDING_FLAGS_PUBLIC : BINDING_FLAGS_NON_PUBLIC) |
           (any_static ? BINDING_FLAGS_STATIC : BINDING_FLAGS_INSTANCE);
}
} // namespace

// ========== PInfo enum ==========
enum class PInfo : int32_t
{
    Attributes = 0x1,
    GetMethod = 0x2,
    SetMethod = 0x4,
    ReflectedType = 0x8,
    DeclaringType = 0x10,
    Name = 0x20,
};

// ========== Implementation Functions ==========

RtResult<vm::RtReflectionProperty*> SystemReflectionRuntimePropertyInfo::internal_from_handle_type(metadata::RtPropertyInfo* property,
                                                                                                   const metadata::RtTypeSig* type_sig) noexcept
{
    const metadata::RtClass* property_parent = property->parent;
    if (type_sig == nullptr)
    {
        return vm::Reflection::get_property_reflection_object(property, property_parent);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtClass*, cur_klass, vm::Class::get_class_from_typesig(type_sig));
    while (cur_klass != nullptr)
    {
        if (cur_klass == property_parent)
        {
            return vm::Reflection::get_property_reflection_object(property, cur_klass);
        }
        cur_klass = cur_klass->parent;
    }
    RET_OK(nullptr);
}

RtResultVoid SystemReflectionRuntimePropertyInfo::get_property_info(vm::RtReflectionProperty* property, vm::RtMonoPropertyInfo* result_info,
                                                                    int32_t pinfo) noexcept
{
    if (pinfo & static_cast<int32_t>(PInfo::Attributes))
    {
        result_info->attrs = property->property->flags;
    }
    const metadata::RtClass* reflected_klass = property->klass;
    const metadata::RtPropertyInfo* prop = property->property;

    if (pinfo & static_cast<int32_t>(PInfo::GetMethod))
    {
        if (prop->get_method != nullptr)
        {
            DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionMethod*, get_method,
                                                    vm::Reflection::get_method_reflection_object(prop->get_method, reflected_klass));
            result_info->get_method = get_method;
        }
        else
        {
            result_info->get_method = nullptr;
        }
    }
    if (pinfo & static_cast<int32_t>(PInfo::SetMethod))
    {
        if (prop->set_method != nullptr)
        {
            DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionMethod*, set_method,
                                                    vm::Reflection::get_method_reflection_object(prop->set_method, reflected_klass));
            result_info->set_method = set_method;
        }
        else
        {
            result_info->set_method = nullptr;
        }
    }
    if (pinfo & static_cast<int32_t>(PInfo::ReflectedType))
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionType*, parent, vm::Reflection::get_klass_reflection_object(reflected_klass));
        result_info->parent = parent;
    }
    if (pinfo & static_cast<int32_t>(PInfo::DeclaringType))
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionType*, declaring_type, vm::Reflection::get_klass_reflection_object(prop->parent));
        result_info->declaring_type = declaring_type;
    }
    if (pinfo & static_cast<int32_t>(PInfo::Name))
    {
        vm::RtString* name = vm::String::create_string_from_utf8cstr(prop->name);
        result_info->name = name;
    }
    RET_VOID_OK();
}

RtResult<vm::RtArray*> SystemReflectionRuntimePropertyInfo::get_type_modifiers(vm::RtReflectionProperty* property, bool optional) noexcept
{
    const metadata::RtPropertyInfo* prop = property->property;
    metadata::RtModuleDef* mod = prop->parent->image;

    utils::Vector<metadata::RtClass*> modifiers;
    RET_ERR_ON_FAIL(vm::Property::get_property_modifiers(prop, optional, modifiers));

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(
        vm::RtArray*, modifier_type_arr,
        LEANCLR_NEW_SZARRAY_FROM_ELE_KLASS_INTERNAL(vm::Class::get_corlib_types().cls_systemtype, static_cast<int32_t>(modifiers.size()), "icalls::SystemReflectionRuntimePropertyInfo::get_type_modifiers"));

    for (size_t i = 0; i < modifiers.size(); ++i)
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionType*, type_obj, vm::Reflection::get_klass_reflection_object(modifiers[i]));
        vm::Array::set_array_data_at(modifier_type_arr, static_cast<int32_t>(i), type_obj);
    }
    RET_OK(modifier_type_arr);
}

RtResult<vm::RtObject*> SystemReflectionRuntimePropertyInfo::get_default_value(vm::RtReflectionProperty* property) noexcept
{
    return vm::Property::get_const_object(property->property);
}

RtResult<int32_t> SystemReflectionRuntimePropertyInfo::get_metadata_token(vm::RtReflectionProperty* property) noexcept
{
    RET_OK(static_cast<int32_t>(property->property->token));
}

RtResult<vm::RtObject*> SystemReflectionRuntimePropertyInfo::create_net10_property_info(
    const metadata::RtPropertyInfo* property,
    vm::RtReflectionRuntimeType* declaring_type,
    vm::RtObject* reflected_type_cache,
    bool* is_private) noexcept
{
    if (property == nullptr || declaring_type == nullptr || reflected_type_cache == nullptr || is_private == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionMethod*, getter,
                                            create_runtime_method_info(property->get_method, declaring_type, reflected_type_cache));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionMethod*, setter,
                                            create_runtime_method_info(property->set_method, declaring_type, reflected_type_cache));

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, obj,
                                            LEANCLR_NEWOBJ_INTERNAL(vm::Class::get_corlib_types().cls_reflection_property,
                                                                    "RuntimePropertyInfo::.ctor net10"));

    int32_t token = static_cast<int32_t>(property->token);
    vm::RtString* name = nullptr;
    void* utf8_name = const_cast<char*>(property->name);
    int32_t flags = static_cast<int32_t>(property->flags);
    vm::RtArray* other_methods = nullptr;
    int32_t binding_flags = get_property_binding_flags(property, is_private);
    vm::RtObject* signature = nullptr;
    vm::RtArray* parameters = nullptr;

    RET_ERR_ON_FAIL(set_instance_field_value(obj, "m_token", &token));
    RET_ERR_ON_FAIL(set_instance_field_value(obj, "m_name", &name));
    RET_ERR_ON_FAIL(set_instance_field_value(obj, "m_utf8name", &utf8_name));
    RET_ERR_ON_FAIL(set_instance_field_value(obj, "m_flags", &flags));
    RET_ERR_ON_FAIL(set_instance_field_value(obj, "m_reflectedTypeCache", &reflected_type_cache));
    RET_ERR_ON_FAIL(set_instance_field_value(obj, "m_getterMethod", &getter));
    RET_ERR_ON_FAIL(set_instance_field_value(obj, "m_setterMethod", &setter));
    RET_ERR_ON_FAIL(set_instance_field_value(obj, "m_otherMethod", &other_methods));
    RET_ERR_ON_FAIL(set_instance_field_value(obj, "m_declaringType", &declaring_type));
    RET_ERR_ON_FAIL(set_instance_field_value(obj, "m_bindingFlags", &binding_flags));
    RET_ERR_ON_FAIL(set_instance_field_value(obj, "m_signature", &signature));
    RET_ERR_ON_FAIL(set_instance_field_value(obj, "m_parameters", &parameters));

    RET_OK(obj);
}

// ========== Invoker Functions ==========

/// @icall: System.Reflection.RuntimePropertyInfo::internal_from_handle_type(System.IntPtr,System.IntPtr)
static RtResultVoid internal_from_handle_type_invoker_runtimepropertyinfo(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                                          const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    metadata::RtPropertyInfo* property = EvalStackOp::get_param<metadata::RtPropertyInfo*>(params, 0);
    const metadata::RtTypeSig* type_sig = EvalStackOp::get_param<const metadata::RtTypeSig*>(params, 1);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionProperty*, result,
                                            SystemReflectionRuntimePropertyInfo::internal_from_handle_type(property, type_sig));
    EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

/// @icall:
/// System.Reflection.RuntimePropertyInfo::System.Reflection.RuntimePropertyInfo::get_property_info
static RtResultVoid get_property_info_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                              interp::RtStackObject* ret) noexcept
{
    (void)ret;
    vm::RtReflectionProperty* property = EvalStackOp::get_param<vm::RtReflectionProperty*>(params, 0);
    vm::RtMonoPropertyInfo* result_info_ptr = EvalStackOp::get_param<vm::RtMonoPropertyInfo*>(params, 1);
    int32_t pinfo = EvalStackOp::get_param<int32_t>(params, 2);
    return SystemReflectionRuntimePropertyInfo::get_property_info(property, result_info_ptr, pinfo);
}

/// @icall: System.Reflection.RuntimePropertyInfo::GetTypeModifiers(System.Reflection.RuntimePropertyInfo,System.Boolean)
static RtResultVoid runtimepropertyinfo_get_type_modifiers_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                                   const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    vm::RtReflectionProperty* property = EvalStackOp::get_param<vm::RtReflectionProperty*>(params, 0);
    bool optional = EvalStackOp::get_param<bool>(params, 1);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, result, SystemReflectionRuntimePropertyInfo::get_type_modifiers(property, optional));
    EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

/// @icall: System.Reflection.RuntimePropertyInfo::get_default_value(System.Reflection.RuntimePropertyInfo)
static RtResultVoid get_default_value_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                              interp::RtStackObject* ret) noexcept
{
    vm::RtReflectionProperty* property = EvalStackOp::get_param<vm::RtReflectionProperty*>(params, 0);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, result, SystemReflectionRuntimePropertyInfo::get_default_value(property));
    EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

/// @icall: System.Reflection.RuntimePropertyInfo::get_metadata_token(System.Reflection.RuntimePropertyInfo)
static RtResultVoid get_metadata_token_invoker_system_reflection_runtimepropertyinfo(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                                                     const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    vm::RtReflectionProperty* property = EvalStackOp::get_param<vm::RtReflectionProperty*>(params, 0);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(int32_t, result, SystemReflectionRuntimePropertyInfo::get_metadata_token(property));
    EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

/// @newobj: System.Reflection.RuntimePropertyInfo::.ctor(System.Int32,System.RuntimeType,System.RuntimeType/RuntimeTypeCache,System.Boolean&)
static RtResultVoid newobj_runtime_property_info_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo* ctor,
                                                         const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    int32_t property_token = EvalStackOp::get_param<int32_t>(params, 0);
    auto declaring_type = EvalStackOp::get_param<vm::RtReflectionRuntimeType*>(params, 1);
    auto reflected_type_cache = EvalStackOp::get_param<vm::RtObject*>(params, 2);
    auto is_private = EvalStackOp::get_param<bool*>(params, 3);
    if (declaring_type == nullptr || reflected_type_cache == nullptr || is_private == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtPropertyInfo*, property,
                                            get_property_from_runtime_type(declaring_type, property_token));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, property_obj_raw,
                                            SystemReflectionRuntimePropertyInfo::create_net10_property_info(
                                                property, declaring_type, reflected_type_cache, is_private));
    EvalStackOp::set_return(ret, property_obj_raw);
    RET_VOID_OK();
}

// ========== Registration ==========

static vm::InternalCallEntry s_internal_call_entries_system_reflection_runtimepropertyinfo[] = {
    {"System.Reflection.RuntimePropertyInfo::internal_from_handle_type(System.IntPtr,System.IntPtr)",
     (vm::InternalCallFunction)&SystemReflectionRuntimePropertyInfo::internal_from_handle_type, internal_from_handle_type_invoker_runtimepropertyinfo},
    {"System.Reflection.RuntimePropertyInfo::get_property_info", (vm::InternalCallFunction)&SystemReflectionRuntimePropertyInfo::get_property_info,
     get_property_info_invoker},
    {"System.Reflection.RuntimePropertyInfo::GetTypeModifiers(System.Reflection.RuntimePropertyInfo,System.Boolean)",
     (vm::InternalCallFunction)&SystemReflectionRuntimePropertyInfo::get_type_modifiers, runtimepropertyinfo_get_type_modifiers_invoker},
    {"System.Reflection.RuntimePropertyInfo::get_default_value(System.Reflection.RuntimePropertyInfo)",
     (vm::InternalCallFunction)&SystemReflectionRuntimePropertyInfo::get_default_value, get_default_value_invoker},
    {"System.Reflection.RuntimePropertyInfo::get_metadata_token(System.Reflection.RuntimePropertyInfo)",
     (vm::InternalCallFunction)&SystemReflectionRuntimePropertyInfo::get_metadata_token, get_metadata_token_invoker_system_reflection_runtimepropertyinfo},
};

utils::Span<vm::InternalCallEntry> SystemReflectionRuntimePropertyInfo::get_internal_call_entries() noexcept
{
    constexpr size_t entry_count =
        sizeof(s_internal_call_entries_system_reflection_runtimepropertyinfo) / sizeof(s_internal_call_entries_system_reflection_runtimepropertyinfo[0]);
    return utils::Span<vm::InternalCallEntry>(s_internal_call_entries_system_reflection_runtimepropertyinfo, entry_count);
}

static vm::NewobjInternalCallEntry s_newobj_internal_call_entries_system_reflection_runtimepropertyinfo[] = {
    {"System.Reflection.RuntimePropertyInfo::.ctor(System.Int32,System.RuntimeType,System.RuntimeType/RuntimeTypeCache,System.Boolean&)",
     newobj_runtime_property_info_invoker},
};

utils::Span<vm::NewobjInternalCallEntry> SystemReflectionRuntimePropertyInfo::get_newobj_internal_call_entries() noexcept
{
    constexpr size_t entry_count = sizeof(s_newobj_internal_call_entries_system_reflection_runtimepropertyinfo) /
                                   sizeof(s_newobj_internal_call_entries_system_reflection_runtimepropertyinfo[0]);
    return utils::Span<vm::NewobjInternalCallEntry>(s_newobj_internal_call_entries_system_reflection_runtimepropertyinfo, entry_count);
}

} // namespace icalls
} // namespace leanclr

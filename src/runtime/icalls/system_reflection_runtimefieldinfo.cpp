#include "system_reflection_runtimefieldinfo.h"
#include "icall_base.h"
#include "vm/class.h"
#include "vm/field.h"
#include "vm/object.h"
#include "vm/reflection.h"
#include "vm/rt_array.h"
#include "metadata/module_def.h"

namespace leanclr
{
namespace icalls
{
namespace
{
static RtResultVoid set_instance_field_value(vm::RtObject* obj, const char* field_name, const void* value) noexcept
{
    const metadata::RtFieldInfo* field = vm::Class::get_field_for_name(obj->klass, field_name, true);
    if (field == nullptr)
    {
        RET_ERR(RtErr::MissingField);
    }

    return vm::Field::set_instance_value(field, obj, value);
}

static bool is_metadata_field_handle(const metadata::RtFieldInfo* field) noexcept
{
    if (field == nullptr || field->parent == nullptr || field->type_sig == nullptr)
    {
        return false;
    }

    metadata::RtToken token = metadata::RtToken::decode(field->token);
    return token.table_type == metadata::TableType::Field && token.rid != 0;
}

static RtResult<const metadata::RtFieldInfo*> get_field_handle_from_runtime_field_info_object(const void* value) noexcept
{
    if (value == nullptr)
    {
        RET_OK(nullptr);
    }

    auto obj = reinterpret_cast<vm::RtObject*>(const_cast<void*>(value));
    const metadata::RtClass* obj_klass = obj->klass;
    if (obj_klass == nullptr)
    {
        RET_OK(nullptr);
    }

    const auto& corlib_types = vm::Class::get_corlib_types();
    bool is_runtime_field_info = obj_klass == corlib_types.cls_reflection_field;
    if (!is_runtime_field_info)
    {
        const char* ns = obj_klass->namespaze != nullptr ? obj_klass->namespaze : "";
        const char* name = obj_klass->name != nullptr ? obj_klass->name : "";
        is_runtime_field_info = std::strcmp(ns, "System") == 0 && std::strcmp(name, "RuntimeFieldInfoStub") == 0;
    }
    if (!is_runtime_field_info)
    {
        RET_OK(nullptr);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtFieldInfo*, field,
                                            vm::Reflection::get_field_info_from_reflection_object(reinterpret_cast<vm::RtReflectionField*>(obj)));
    RET_OK(is_metadata_field_handle(field) ? field : nullptr);
}

static RtResult<const metadata::RtFieldInfo*> get_runtime_field_handle_internal_param(const interp::RtStackObject* params,
                                                                                     size_t index) noexcept
{
    uintptr_t raw_value = EvalStackOp::get_param<uintptr_t>(params, index);
    if (raw_value == 0)
    {
        RET_OK(nullptr);
    }

    auto direct = reinterpret_cast<const metadata::RtFieldInfo*>(raw_value);
    if (is_metadata_field_handle(direct))
    {
        RET_OK(direct);
    }

    const void* raw = reinterpret_cast<const void*>(raw_value);
    metadata::RtClass* runtime_field_handle_klass = nullptr;
    metadata::RtModuleDef* corlib = metadata::RtModuleDef::get_corlib_module();
    if (corlib != nullptr)
    {
        auto klass_ret = corlib->get_class_by_name("System.RuntimeFieldHandleInternal", false, false);
        if (klass_ret.is_ok())
        {
            runtime_field_handle_klass = klass_ret.unwrap();
        }
    }

    auto boxed_handle = reinterpret_cast<const vm::RtObject*>(raw);
    if (runtime_field_handle_klass != nullptr && boxed_handle->klass == runtime_field_handle_klass)
    {
        const uint8_t* value_ptr = reinterpret_cast<const uint8_t*>(raw) + sizeof(vm::RtObject);
        auto field = *reinterpret_cast<const metadata::RtFieldInfo* const*>(value_ptr);
        RET_OK(is_metadata_field_handle(field) ? field : nullptr);
    }

    uintptr_t slot_value = *reinterpret_cast<const uintptr_t*>(raw);
    auto slot_field = reinterpret_cast<const metadata::RtFieldInfo*>(slot_value);
    if (is_metadata_field_handle(slot_field))
    {
        RET_OK(slot_field);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtFieldInfo*, slot_object_field,
                                            get_field_handle_from_runtime_field_info_object(reinterpret_cast<const void*>(slot_value)));
    if (slot_object_field != nullptr)
    {
        RET_OK(slot_object_field);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtFieldInfo*, raw_object_field,
                                            get_field_handle_from_runtime_field_info_object(raw));
    if (raw_object_field != nullptr)
    {
        RET_OK(raw_object_field);
    }

    RET_ERR(RtErr::BadImageFormat);
}

RtResult<const metadata::RtFieldInfo*> get_field_handle_from_object(vm::RtObject* obj) noexcept
{
    if (obj == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    const metadata::RtFieldInfo* handle = nullptr;
    const metadata::RtFieldInfo* handle_field = vm::Class::get_field_for_name(obj->klass, "m_fieldHandle", true);
    if (handle_field != nullptr)
    {
        RET_ERR_ON_FAIL(vm::Field::get_instance_value(handle_field, obj, &handle));
        if (handle != nullptr)
        {
            RET_OK(handle);
        }
    }

    return vm::Reflection::get_field_info_from_reflection_object(reinterpret_cast<vm::RtReflectionField*>(obj));
}

RtResult<bool> rt_field_info_equals(vm::RtObject* self, vm::RtObject* other) noexcept
{
    if (self == other)
    {
        RET_OK(true);
    }
    if (self == nullptr || other == nullptr || self->klass != other->klass)
    {
        RET_OK(false);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtFieldInfo*, self_field, get_field_handle_from_object(self));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtFieldInfo*, other_field, get_field_handle_from_object(other));
    RET_OK(self_field == other_field);
}
} // namespace

// ========== Implementation Functions ==========

RtResult<uint32_t> SystemReflectionRuntimeFieldInfo::get_metadata_token(vm::RtReflectionField* field) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtFieldInfo*, field_info, vm::Reflection::get_field_info_from_reflection_object(field));
    RET_OK(field_info->token);
}

RtResult<int32_t> SystemReflectionRuntimeFieldInfo::get_field_offset(vm::RtReflectionField* field) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtFieldInfo*, field_info, vm::Reflection::get_field_info_from_reflection_object(field));
    RET_OK(static_cast<int32_t>(vm::Field::get_field_offset_excludes_object_header_for_all_type(field_info)));
}

RtResult<vm::RtObject*> SystemReflectionRuntimeFieldInfo::get_raw_const_value(vm::RtReflectionField* field) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtFieldInfo*, field_info, vm::Reflection::get_field_info_from_reflection_object(field));
    return vm::Field::get_field_const_object(field_info);
}

RtResult<vm::RtObject*> SystemReflectionRuntimeFieldInfo::get_value_internal(vm::RtReflectionField* field, vm::RtObject* obj) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtFieldInfo*, field_info, vm::Reflection::get_field_info_from_reflection_object(field));
    return vm::Field::get_value_object(field_info, obj);
}

RtResult<vm::RtObject*> SystemReflectionRuntimeFieldInfo::unsafe_get_value(vm::RtReflectionField* field, vm::RtObject* obj) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtFieldInfo*, field_info, vm::Reflection::get_field_info_from_reflection_object(field));
    return vm::Field::get_value_object(field_info, obj);
}

RtResultVoid SystemReflectionRuntimeFieldInfo::set_value_internal(vm::RtReflectionField* field, vm::RtObject* obj, vm::RtObject* value) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtFieldInfo*, field_info, vm::Reflection::get_field_info_from_reflection_object(field));
    return vm::Field::set_value_object(field_info, obj, value);
}

RtResult<vm::RtReflectionType*> SystemReflectionRuntimeFieldInfo::get_parent_type(vm::RtReflectionField* field, bool declaring) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtFieldInfo*, field_info, vm::Reflection::get_field_info_from_reflection_object(field));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtClass*, reflection_klass, vm::Reflection::get_reflection_field_klass(field));
    const metadata::RtClass* parent = declaring ? field_info->parent : reflection_klass;
    return vm::Reflection::get_klass_reflection_object(parent);
}

RtResult<vm::RtReflectionType*> SystemReflectionRuntimeFieldInfo::resolve_type(vm::RtReflectionField* field) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtFieldInfo*, field_info, vm::Reflection::get_field_info_from_reflection_object(field));
    const metadata::RtTypeSig* type_sig = field_info->type_sig;
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));
    return vm::Reflection::get_klass_reflection_object(klass);
}

RtResult<vm::RtArray*> SystemReflectionRuntimeFieldInfo::get_type_modifiers(vm::RtReflectionField* field, bool optional) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtFieldInfo*, field_info, vm::Reflection::get_field_info_from_reflection_object(field));

    utils::Vector<metadata::RtClass*> modifiers;
    RET_ERR_ON_FAIL(vm::Field::get_field_modifiers(field_info, optional, modifiers));

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(
        vm::RtArray*, modifier_type_arr,
        LEANCLR_NEW_SZARRAY_FROM_ELE_KLASS_INTERNAL(vm::Class::get_corlib_types().cls_systemtype, static_cast<int32_t>(modifiers.size()), "icalls::SystemReflectionRuntimeFieldInfo::get_type_modifiers"));

    for (size_t i = 0; i < modifiers.size(); ++i)
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionType*, type_obj, vm::Reflection::get_klass_reflection_object(modifiers[i]));
        vm::Array::set_array_data_at(modifier_type_arr, static_cast<int32_t>(i), type_obj);
    }
    RET_OK(modifier_type_arr);
}

// ========== Invoker Functions ==========

/// @icall: System.Reflection.RuntimeFieldInfo::get_metadata_token
static RtResultVoid get_metadata_token_invoker_system_reflection_runtimefieldinfo(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                                                  const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    vm::RtReflectionField* field = EvalStackOp::get_param<vm::RtReflectionField*>(params, 0);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(uint32_t, result, SystemReflectionRuntimeFieldInfo::get_metadata_token(field));
    EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

/// @icall: System.Reflection.RuntimeFieldInfo::GetFieldOffset
static RtResultVoid get_field_offset_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                             interp::RtStackObject* ret) noexcept
{
    vm::RtReflectionField* field = EvalStackOp::get_param<vm::RtReflectionField*>(params, 0);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(int32_t, result, SystemReflectionRuntimeFieldInfo::get_field_offset(field));
    EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

/// @icall: System.Reflection.RuntimeFieldInfo::GetRawConstantValue
static RtResultVoid get_raw_const_value_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                                interp::RtStackObject* ret) noexcept
{
    vm::RtReflectionField* field = EvalStackOp::get_param<vm::RtReflectionField*>(params, 0);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, result, SystemReflectionRuntimeFieldInfo::get_raw_const_value(field));
    EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

/// @icall: System.Reflection.RuntimeFieldInfo::GetValueInternal
static RtResultVoid get_value_internal_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                               interp::RtStackObject* ret) noexcept
{
    vm::RtReflectionField* field = EvalStackOp::get_param<vm::RtReflectionField*>(params, 0);
    vm::RtObject* obj = EvalStackOp::get_param<vm::RtObject*>(params, 1);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, result, SystemReflectionRuntimeFieldInfo::get_value_internal(field, obj));
    EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

/// @icall: System.Reflection.RuntimeFieldInfo::UnsafeGetValue
static RtResultVoid unsafe_get_value_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                             interp::RtStackObject* ret) noexcept
{
    vm::RtReflectionField* field = EvalStackOp::get_param<vm::RtReflectionField*>(params, 0);
    vm::RtObject* obj = EvalStackOp::get_param<vm::RtObject*>(params, 1);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, result, SystemReflectionRuntimeFieldInfo::unsafe_get_value(field, obj));
    EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

/// @icall: System.Reflection.RuntimeFieldInfo::SetValueInternal(System.Reflection.FieldInfo,System.Object,System.Object)
static RtResultVoid set_value_internal_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                               interp::RtStackObject* ret) noexcept
{
    (void)ret;
    vm::RtReflectionField* field = EvalStackOp::get_param<vm::RtReflectionField*>(params, 0);
    vm::RtObject* obj = EvalStackOp::get_param<vm::RtObject*>(params, 1);
    vm::RtObject* value = EvalStackOp::get_param<vm::RtObject*>(params, 2);
    return SystemReflectionRuntimeFieldInfo::set_value_internal(field, obj, value);
}

/// @icall: System.Reflection.RuntimeFieldInfo::GetParentType
static RtResultVoid get_parent_type_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                            interp::RtStackObject* ret) noexcept
{
    vm::RtReflectionField* field = EvalStackOp::get_param<vm::RtReflectionField*>(params, 0);
    bool declaring = EvalStackOp::get_param<bool>(params, 1);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionType*, result, SystemReflectionRuntimeFieldInfo::get_parent_type(field, declaring));
    EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

/// @icall: System.Reflection.RuntimeFieldInfo::ResolveType
static RtResultVoid resolve_type_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                         interp::RtStackObject* ret) noexcept
{
    vm::RtReflectionField* field = EvalStackOp::get_param<vm::RtReflectionField*>(params, 0);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionType*, result, SystemReflectionRuntimeFieldInfo::resolve_type(field));
    EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

/// @icall: System.Reflection.RuntimeFieldInfo::GetTypeModifiers(System.Boolean)
static RtResultVoid runtimefieldinfo_get_type_modifiers_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                                const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    vm::RtReflectionField* field = EvalStackOp::get_param<vm::RtReflectionField*>(params, 0);
    bool optional = EvalStackOp::get_param<bool>(params, 1);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, result, SystemReflectionRuntimeFieldInfo::get_type_modifiers(field, optional));
    EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

/// @icall: System.Reflection.RtFieldInfo::Equals(System.Object)
static RtResultVoid rt_field_info_equals_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                                 interp::RtStackObject* ret) noexcept
{
    auto self = EvalStackOp::get_param<vm::RtObject*>(params, 0);
    auto other = EvalStackOp::get_param<vm::RtObject*>(params, 1);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, result, rt_field_info_equals(self, other));
    EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

/// @newobj: System.Reflection.RtFieldInfo::.ctor(System.RuntimeFieldHandleInternal,System.RuntimeType,System.RuntimeType/RuntimeTypeCache,System.Reflection.BindingFlags)
static RtResultVoid newobj_rt_field_info_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo* ctor,
                                                 const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtFieldInfo*, field, get_runtime_field_handle_internal_param(params, 0));
    auto declaring_type = EvalStackOp::get_param<vm::RtReflectionRuntimeType*>(params, 1);
    auto reflected_type_cache = EvalStackOp::get_param<vm::RtObject*>(params, 2);
    int32_t binding_flags = EvalStackOp::get_param<int32_t>(params, 3);
    if (field == nullptr || declaring_type == nullptr || reflected_type_cache == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, obj, LEANCLR_NEWOBJ_INTERNAL(ctor->parent, "RtFieldInfo::.ctor newobj"));

    int32_t field_attributes = static_cast<int32_t>(field->flags);
    RET_ERR_ON_FAIL(set_instance_field_value(obj, "m_bindingFlags", &binding_flags));
    RET_ERR_ON_FAIL(set_instance_field_value(obj, "m_reflectedTypeCache", &reflected_type_cache));
    RET_ERR_ON_FAIL(set_instance_field_value(obj, "m_declaringType", &declaring_type));
    RET_ERR_ON_FAIL(set_instance_field_value(obj, "m_fieldHandle", &field));
    RET_ERR_ON_FAIL(set_instance_field_value(obj, "m_fieldAttributes", &field_attributes));

    EvalStackOp::set_return(ret, obj);
    RET_VOID_OK();
}

// ========== Registration ==========

static vm::InternalCallEntry s_internal_call_entries_system_reflection_runtimefieldinfo[] = {
    {"System.Reflection.RuntimeFieldInfo::get_metadata_token", (vm::InternalCallFunction)&SystemReflectionRuntimeFieldInfo::get_metadata_token,
     get_metadata_token_invoker_system_reflection_runtimefieldinfo},
    {"System.Reflection.RtFieldInfo::Equals(System.Object)", nullptr, rt_field_info_equals_invoker},
    {"System.Reflection.RuntimeFieldInfo::GetFieldOffset", (vm::InternalCallFunction)&SystemReflectionRuntimeFieldInfo::get_field_offset,
     get_field_offset_invoker},
    {"System.Reflection.RuntimeFieldInfo::GetRawConstantValue", (vm::InternalCallFunction)&SystemReflectionRuntimeFieldInfo::get_raw_const_value,
     get_raw_const_value_invoker},
    {"System.Reflection.RuntimeFieldInfo::GetValueInternal", (vm::InternalCallFunction)&SystemReflectionRuntimeFieldInfo::get_value_internal,
     get_value_internal_invoker},
    {"System.Reflection.RuntimeFieldInfo::UnsafeGetValue", (vm::InternalCallFunction)&SystemReflectionRuntimeFieldInfo::unsafe_get_value,
     unsafe_get_value_invoker},
    {"System.Reflection.RuntimeFieldInfo::SetValueInternal(System.Reflection.FieldInfo,System.Object,System.Object)",
     (vm::InternalCallFunction)&SystemReflectionRuntimeFieldInfo::set_value_internal, set_value_internal_invoker},
    {"System.Reflection.RuntimeFieldInfo::GetParentType", (vm::InternalCallFunction)&SystemReflectionRuntimeFieldInfo::get_parent_type,
     get_parent_type_invoker},
    {"System.Reflection.RuntimeFieldInfo::ResolveType", (vm::InternalCallFunction)&SystemReflectionRuntimeFieldInfo::resolve_type, resolve_type_invoker},
    {"System.Reflection.RuntimeFieldInfo::GetTypeModifiers(System.Boolean)", (vm::InternalCallFunction)&SystemReflectionRuntimeFieldInfo::get_type_modifiers,
     runtimefieldinfo_get_type_modifiers_invoker},
};

utils::Span<vm::InternalCallEntry> SystemReflectionRuntimeFieldInfo::get_internal_call_entries() noexcept
{
    constexpr size_t entry_count =
        sizeof(s_internal_call_entries_system_reflection_runtimefieldinfo) / sizeof(s_internal_call_entries_system_reflection_runtimefieldinfo[0]);
    return utils::Span<vm::InternalCallEntry>(s_internal_call_entries_system_reflection_runtimefieldinfo, entry_count);
}

static vm::NewobjInternalCallEntry s_newobj_internal_call_entries_system_reflection_runtimefieldinfo[] = {
    {"System.Reflection.RtFieldInfo::.ctor(System.RuntimeFieldHandleInternal,System.RuntimeType,System.RuntimeType/RuntimeTypeCache,System.Reflection.BindingFlags)",
     newobj_rt_field_info_invoker},
};

utils::Span<vm::NewobjInternalCallEntry> SystemReflectionRuntimeFieldInfo::get_newobj_internal_call_entries() noexcept
{
    constexpr size_t entry_count =
        sizeof(s_newobj_internal_call_entries_system_reflection_runtimefieldinfo) / sizeof(s_newobj_internal_call_entries_system_reflection_runtimefieldinfo[0]);
    return utils::Span<vm::NewobjInternalCallEntry>(s_newobj_internal_call_entries_system_reflection_runtimefieldinfo, entry_count);
}

} // namespace icalls
} // namespace leanclr

#include "system_monocustomattrs.h"
#include "const_strs.h"
#include "metadata/module_def.h"
#include "vm/class.h"
#include "vm/customattribute.h"
#include "vm/field.h"
#include "vm/method.h"
#include "vm/object.h"
#include "vm/reflection.h"
#include "vm/rt_array.h"
#include "vm/runtime.h"
#include "interp/eval_stack_op.h"

using namespace leanclr::core;
using namespace leanclr::vm;
using namespace leanclr::metadata;
using namespace leanclr::interp;

namespace leanclr
{
namespace icalls
{

// Implementation functions

static bool is_attribute_filter_match(RtObject* attribute, const RtClass* attr_klass) noexcept
{
    return attr_klass == nullptr || Object::is_inst(attribute, attr_klass);
}

static RtResult<RtClass*> get_corlib_class(const char* full_name) noexcept
{
    metadata::RtModuleDef* corlib = Class::get_corlib_types().cls_object->image;
    return corlib->get_class_by_name(full_name, false, true);
}

static RtResult<RtObject*> create_struct_layout_attribute(const RtClass* target_klass) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(RtClass*, attr_klass,
                                            get_corlib_class("System.Runtime.InteropServices.StructLayoutAttribute"));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(RtClass*, layout_kind_klass,
                                            get_corlib_class("System.Runtime.InteropServices.LayoutKind"));
    const RtTypeSig* ctor_params[] = {layout_kind_klass->by_val};
    const RtMethodInfo* ctor = Method::find_matched_method_in_class_by_name_and_signature(attr_klass, STR_CTOR, ctor_params, 1);
    if (ctor == nullptr)
    {
        RET_ERR(RtErr::MissingMethod);
    }

    int32_t layout_kind = 3; // LayoutKind.Auto
    if ((target_klass->flags & static_cast<uint32_t>(RtTypeAttribute::SequentialLayout)) != 0)
    {
        layout_kind = 0; // LayoutKind.Sequential
    }
    else if ((target_klass->flags & static_cast<uint32_t>(RtTypeAttribute::ExplicitLayout)) != 0)
    {
        layout_kind = 2; // LayoutKind.Explicit
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(RtObject*, attr_obj,
                                            LEANCLR_NEWOBJ_INTERNAL(attr_klass, "SystemMonoCustomAttrs::create_struct_layout_attribute"));
    const void* ctor_args[] = {&layout_kind};
    RET_ERR_ON_FAIL(Runtime::invoke_with_run_cctor(ctor, attr_obj, ctor_args));

    int32_t pack = 0;
    int32_t size = 0;
    auto layout_data = target_klass->image->get_class_layout_data(target_klass->token);
    if (layout_data)
    {
        pack = static_cast<int32_t>(layout_data->packing);
        size = static_cast<int32_t>(layout_data->size);
    }

    int32_t char_set = 2; // CharSet.Ansi
    uint32_t string_format = target_klass->flags & static_cast<uint32_t>(RtTypeAttribute::StringFormatMask);
    if (string_format == static_cast<uint32_t>(RtTypeAttribute::UnicodeClass))
    {
        char_set = 3; // CharSet.Unicode
    }
    else if (string_format == static_cast<uint32_t>(RtTypeAttribute::AutoClass))
    {
        char_set = 4; // CharSet.Auto
    }

    const RtFieldInfo* pack_field = Class::get_field_for_name(attr_klass, "Pack", true);
    const RtFieldInfo* size_field = Class::get_field_for_name(attr_klass, "Size", true);
    const RtFieldInfo* char_set_field = Class::get_field_for_name(attr_klass, "CharSet", true);
    if (pack_field == nullptr || size_field == nullptr || char_set_field == nullptr)
    {
        RET_ERR(RtErr::MissingField);
    }
    RET_ERR_ON_FAIL(Field::set_instance_value(pack_field, attr_obj, &pack));
    RET_ERR_ON_FAIL(Field::set_instance_value(size_field, attr_obj, &size));
    RET_ERR_ON_FAIL(Field::set_instance_value(char_set_field, attr_obj, &char_set));
    RET_OK(attr_obj);
}

static RtResult<RtObject*> create_field_offset_attribute(const RtFieldInfo* field) noexcept
{
    auto field_offset = field->parent->image->get_field_offset(field->token);
    if (!field_offset)
    {
        RET_OK(nullptr);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(RtClass*, attr_klass,
                                            get_corlib_class("System.Runtime.InteropServices.FieldOffsetAttribute"));
    const RtTypeSig* ctor_params[] = {Class::get_corlib_types().cls_int32->by_val};
    const RtMethodInfo* ctor = Method::find_matched_method_in_class_by_name_and_signature(attr_klass, STR_CTOR, ctor_params, 1);
    if (ctor == nullptr)
    {
        RET_ERR(RtErr::MissingMethod);
    }

    int32_t offset = static_cast<int32_t>(*field_offset);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(RtObject*, attr_obj,
                                            LEANCLR_NEWOBJ_INTERNAL(attr_klass, "SystemMonoCustomAttrs::create_field_offset_attribute"));
    const void* ctor_args[] = {&offset};
    RET_ERR_ON_FAIL(Runtime::invoke_with_run_cctor(ctor, attr_obj, ctor_args));
    RET_OK(attr_obj);
}

static RtResultVoid add_pseudo_custom_attributes(utils::Vector<RtObject*>& attributes, RtObject* obj, const RtClass* attr_klass) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(RtObject*, provider_obj, Reflection::normalize_coreclr_reflection_object(obj));
    const CorLibTypes& corlib_types = Class::get_corlib_types();
    const RtClass* obj_klass = provider_obj->klass;

    if (obj_klass == corlib_types.cls_runtimetype)
    {
        RtReflectionType* type_obj = reinterpret_cast<RtReflectionType*>(provider_obj);
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(RtClass*, target_klass, Reflection::get_class_from_reflection_type_object(type_obj));
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(RtObject*, attr_obj, create_struct_layout_attribute(target_klass));
        if (is_attribute_filter_match(attr_obj, attr_klass))
        {
            attributes.push_back(attr_obj);
        }
    }
    else if (obj_klass == corlib_types.cls_reflection_field)
    {
        RtReflectionField* field_obj = reinterpret_cast<RtReflectionField*>(provider_obj);
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const RtFieldInfo*, field, Reflection::get_field_info_from_reflection_object(field_obj));
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(RtObject*, attr_obj, create_field_offset_attribute(field));
        if (attr_obj != nullptr && is_attribute_filter_match(attr_obj, attr_klass))
        {
            attributes.push_back(attr_obj);
        }
    }

    RET_VOID_OK();
}

static RtResult<RtArray*> merge_custom_attribute_arrays(RtArray* normal_attrs, const utils::Vector<RtObject*>& pseudo_attrs) noexcept
{
    const CorLibTypes& types = Class::get_corlib_types();
    int32_t normal_count = normal_attrs != nullptr ? Array::get_array_length(normal_attrs) : 0;
    int32_t total_count = normal_count + static_cast<int32_t>(pseudo_attrs.size());

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(RtArray*, result,
                                            LEANCLR_NEW_SZARRAY_FROM_ELE_KLASS_INTERNAL(types.cls_attribute, total_count,
                                                                                       "SystemMonoCustomAttrs::merge_custom_attribute_arrays"));
    for (int32_t i = 0; i < normal_count; ++i)
    {
        Array::set_array_data_at<RtObject*>(result, i, Array::get_array_data_at<RtObject*>(normal_attrs, i));
    }
    for (int32_t i = 0; i < static_cast<int32_t>(pseudo_attrs.size()); ++i)
    {
        Array::set_array_data_at<RtObject*>(result, normal_count + i, pseudo_attrs[i]);
    }
    RET_OK(result);
}

RtResult<bool> SystemMonoCustomAttrs::is_defined_internal(RtObject* obj, RtReflectionType* attribute_type) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(RtClass*, attr_klass, Reflection::get_class_from_reflection_type_object(attribute_type));
    return CustomAttribute::has_attribute(obj, attr_klass);
}

RtResult<RtArray*> SystemMonoCustomAttrs::get_custom_attributes_internal(RtObject* obj, RtReflectionType* attribute_type, bool pseudo_attrs) noexcept
{
    metadata::RtClass* attr_klass = nullptr;
    if (attribute_type != nullptr)
    {
        UNWRAP_OR_RET_ERR_ON_FAIL(attr_klass, Reflection::get_class_from_reflection_type_object(attribute_type));
    }
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(RtArray*, normal_attrs, CustomAttribute::get_customattributes_on_target_object(obj, attr_klass));
    if (!pseudo_attrs)
    {
        RET_OK(normal_attrs);
    }

    utils::Vector<RtObject*> synthesized_attrs;
    RET_ERR_ON_FAIL(add_pseudo_custom_attributes(synthesized_attrs, obj, attr_klass));
    if (synthesized_attrs.empty())
    {
        RET_OK(normal_attrs);
    }

    return merge_custom_attribute_arrays(normal_attrs, synthesized_attrs);
}

RtResult<RtArray*> SystemMonoCustomAttrs::get_custom_attributes_data_internal(RtObject* obj) noexcept
{
    return CustomAttribute::get_customattributes_data_on_target(obj);
}

// Invoker functions

/// @icall: System.MonoCustomAttrs::IsDefinedInternal
static RtResultVoid is_defined_internal_invoker(RtManagedMethodPointer, const RtMethodInfo*, const RtStackObject* params, RtStackObject* ret) noexcept
{
    RtObject* obj = EvalStackOp::get_param<RtObject*>(params, 0);
    RtReflectionType* attr_type = EvalStackOp::get_param<RtReflectionType*>(params, 1);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, result, SystemMonoCustomAttrs::is_defined_internal(obj, attr_type));
    EvalStackOp::set_return(ret, static_cast<int32_t>(result));
    RET_VOID_OK();
}

/// @icall: System.MonoCustomAttrs::GetCustomAttributesInternal
static RtResultVoid get_custom_attributes_internal_invoker(RtManagedMethodPointer, const RtMethodInfo*, const RtStackObject* params,
                                                           RtStackObject* ret) noexcept
{
    RtObject* obj = EvalStackOp::get_param<RtObject*>(params, 0);
    RtReflectionType* attr_type = EvalStackOp::get_param<RtReflectionType*>(params, 1);
    bool pseudo_attrs = EvalStackOp::get_param<bool>(params, 2);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(RtArray*, result, SystemMonoCustomAttrs::get_custom_attributes_internal(obj, attr_type, pseudo_attrs));
    EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

/// @icall: System.MonoCustomAttrs::GetCustomAttributesDataInternal(System.Reflection.ICustomAttributeProvider)
static RtResultVoid get_custom_attributes_data_internal_invoker(RtManagedMethodPointer, const RtMethodInfo*, const RtStackObject* params,
                                                                RtStackObject* ret) noexcept
{
    RtObject* obj = EvalStackOp::get_param<RtObject*>(params, 0);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(RtArray*, result, SystemMonoCustomAttrs::get_custom_attributes_data_internal(obj));
    EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

// Internal call entries

static InternalCallEntry s_internal_call_entries_system_monocustomattrs[] = {
    {"System.MonoCustomAttrs::IsDefinedInternal", (InternalCallFunction)&SystemMonoCustomAttrs::is_defined_internal, is_defined_internal_invoker},
    {"System.MonoCustomAttrs::GetCustomAttributesInternal", (InternalCallFunction)&SystemMonoCustomAttrs::get_custom_attributes_internal,
     get_custom_attributes_internal_invoker},
    {"System.MonoCustomAttrs::GetCustomAttributesDataInternal(System.Reflection.ICustomAttributeProvider)",
     (InternalCallFunction)&SystemMonoCustomAttrs::get_custom_attributes_data_internal, get_custom_attributes_data_internal_invoker},
};

utils::Span<InternalCallEntry> SystemMonoCustomAttrs::get_internal_call_entries() noexcept
{
    constexpr size_t entry_count = sizeof(s_internal_call_entries_system_monocustomattrs) / sizeof(s_internal_call_entries_system_monocustomattrs[0]);
    return utils::Span<InternalCallEntry>(s_internal_call_entries_system_monocustomattrs, entry_count);
}

} // namespace icalls
} // namespace leanclr

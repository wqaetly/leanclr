#include "coreclr_qcall.h"

#include "interp/eval_stack_op.h"
#include "interp/machine_state.h"
#include "metadata/metadata_name.h"
#include "metadata/metadata_cache.h"
#include "metadata/module_def.h"
#include "platform/rt_sys.h"
#include "utils/rt_vector.h"
#include "utils/string_builder.h"
#include "vm/assembly.h"
#include "vm/class.h"
#include "vm/field.h"
#include "vm/generic_class.h"
#include "vm/gchandle.h"
#include "vm/marshal.h"
#include "vm/method.h"
#include "vm/object.h"
#include "vm/pinvoke.h"
#include "vm/reflection.h"
#include "vm/runtime.h"
#include "vm/rt_array.h"
#include "vm/rt_string.h"
#include "vm/rt_thread.h"

namespace leanclr
{
namespace pinvokes
{
namespace
{
constexpr int32_t FORMAT_NAMESPACE = 0x00000001;
constexpr int32_t FORMAT_ASSEMBLY = 0x00000004;
constexpr int32_t CALLING_CONVENTION_STANDARD = 0x0001;
constexpr int32_t CALLING_CONVENTION_HAS_THIS = 0x0020;

struct RtOsVersionInfoEx
{
    uint32_t dwOSVersionInfoSize;
    uint32_t dwMajorVersion;
    uint32_t dwMinorVersion;
    uint32_t dwBuildNumber;
    uint32_t dwPlatformId;
    Utf16Char szCSDVersion[128];
    uint16_t wServicePackMajor;
    uint16_t wServicePackMinor;
    uint16_t wSuiteMask;
    uint8_t wProductType;
    uint8_t wReserved;
};

struct RtStackFrameHelper : public vm::RtObject
{
    vm::RtArray* rgi_offset;
    vm::RtArray* rgi_il_offset;
    vm::RtObject* dynamic_methods;
    vm::RtArray* rg_method_handle;
    vm::RtArray* rg_assembly_path;
    vm::RtArray* rg_assembly;
    vm::RtArray* rg_loaded_pe_address;
    vm::RtArray* rgi_loaded_pe_size;
    vm::RtArray* rgi_is_file_layout;
    vm::RtArray* rg_in_memory_pdb_address;
    vm::RtArray* rgi_in_memory_pdb_size;
    vm::RtArray* rgi_method_token;
    vm::RtArray* rg_filename;
    vm::RtArray* rgi_line_number;
    vm::RtArray* rgi_column_number;
    vm::RtArray* rgi_last_frame_from_foreign_exception_stack_trace;
    int32_t frame_count;
};

struct StackFrameData
{
    const metadata::RtMethodInfo* method;
    int32_t native_offset;
    int32_t il_offset;
    vm::RtString* file_name;
    int32_t line_number;
    int32_t column_number;
    bool is_last_frame_from_foreign_exception_stack_trace;
};

RtResult<const metadata::RtTypeSig*> get_type_sig_from_qcall_type_handle(void* qcall_type_handle, void* native_handle) noexcept
{
    if (native_handle != nullptr)
    {
        RET_OK(reinterpret_cast<const metadata::RtTypeSig*>(native_handle));
    }

    if (qcall_type_handle == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    auto runtime_type_klass = vm::Class::get_corlib_types().cls_runtimetype;
    auto direct_runtime_type = reinterpret_cast<vm::RtReflectionRuntimeType*>(qcall_type_handle);
    if (direct_runtime_type->reflection_type.header.klass == runtime_type_klass)
    {
        RET_OK(direct_runtime_type->reflection_type.type_handle);
    }

    auto runtime_type = *reinterpret_cast<vm::RtReflectionRuntimeType**>(qcall_type_handle);
    if (runtime_type == nullptr || runtime_type->reflection_type.header.klass != runtime_type_klass)
    {
        RET_ERR(RtErr::BadImageFormat);
    }

    RET_OK(runtime_type->reflection_type.type_handle);
}

RtResult<metadata::RtModuleDef*> get_module_from_qcall_module(void* qcall_module, void* native_handle) noexcept
{
    if (native_handle != nullptr)
    {
        RET_OK(reinterpret_cast<metadata::RtModuleDef*>(native_handle));
    }

    if (qcall_module == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    auto runtime_module_klass = vm::Class::get_corlib_types().cls_reflection_module;
    auto direct_module = reinterpret_cast<vm::RtReflectionModule*>(qcall_module);
    if (direct_module->klass == runtime_module_klass)
    {
        RET_OK(direct_module->native_handle);
    }

    auto runtime_module = *reinterpret_cast<vm::RtReflectionModule**>(qcall_module);
    if (runtime_module == nullptr || runtime_module->klass != runtime_module_klass)
    {
        RET_ERR(RtErr::BadImageFormat);
    }

    RET_OK(runtime_module->native_handle);
}

RtResultVoid append_basic_type_name(utils::Utf8StringBuilder& sb, const metadata::RtTypeSig* type_sig) noexcept
{
    switch (type_sig->ele_type)
    {
    case metadata::RtElementType::Array:
    {
        const metadata::RtArrayType* array_type = type_sig->data.array_type;
        RET_ERR_ON_FAIL(append_basic_type_name(sb, array_type->ele_type));
        sb.append_char('[');
        if (array_type->rank > 1)
        {
            sb.append_chars(',', array_type->rank - 1);
        }
        else
        {
            sb.append_char('*');
        }
        sb.append_char(']');
        break;
    }
    case metadata::RtElementType::SZArray:
        RET_ERR_ON_FAIL(append_basic_type_name(sb, type_sig->data.element_type));
        sb.append_cstr("[]");
        break;
    case metadata::RtElementType::Ptr:
        RET_ERR_ON_FAIL(append_basic_type_name(sb, type_sig->data.element_type));
        sb.append_char('*');
        break;
    case metadata::RtElementType::Var:
    case metadata::RtElementType::MVar:
        sb.append_cstr(type_sig->data.generic_param->name);
        break;
    default:
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));
        sb.append_cstr(klass->name);
        break;
    }
    }

    if (type_sig->by_ref)
    {
        sb.append_char('&');
    }

    RET_VOID_OK();
}

RtResult<vm::RtString*> construct_type_name(void* qcall_type_handle, void* native_handle, int32_t format_flags) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, type_sig,
                                            get_type_sig_from_qcall_type_handle(qcall_type_handle, native_handle));

    utils::Utf8StringBuilder sb;
    if ((format_flags & FORMAT_ASSEMBLY) != 0)
    {
        RET_ERR_ON_FAIL(metadata::MetadataName::append_type_full_name(sb, type_sig, metadata::TypeNameFormat::AssemblyQualified, false));
    }
    else if ((format_flags & FORMAT_NAMESPACE) != 0)
    {
        RET_ERR_ON_FAIL(metadata::MetadataName::append_type_full_name(sb, type_sig, metadata::TypeNameFormat::FullName, false));
    }
    else
    {
        RET_ERR_ON_FAIL(append_basic_type_name(sb, type_sig));
    }

    RET_OK(vm::String::create_string_from_utf8chars(sb.get_const_chars(), static_cast<int32_t>(sb.length())));
}

RtResult<int32_t> get_cor_element_type(void* type_handle) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, type_sig,
                                            get_type_sig_from_qcall_type_handle(type_handle, type_handle));
    RET_OK(static_cast<int32_t>(type_sig->ele_type));
}

RtResult<vm::RtArray*> get_module_types(void* qcall_module, void* native_handle) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtModuleDef*, module, get_module_from_qcall_module(qcall_module, native_handle));
    return vm::Assembly::get_types(module->get_assembly(), false);
}

RtResult<metadata::RtClass*> get_type_def_class_for_metadata_enum(metadata::RtModuleDef* module, int32_t parent_token) noexcept
{
    metadata::RtToken token = metadata::RtToken::decode(static_cast<metadata::EncodedTokenId>(parent_token));
    if (token.table_type != metadata::TableType::TypeDef || token.rid == 0)
    {
        RET_ERR(RtErr::BadImageFormat);
    }

    return module->get_class_by_type_def_rid(token.rid);
}

RtResultVoid collect_metadata_enum_tokens(metadata::RtModuleDef* module, int32_t token_type, int32_t parent_token,
                                          utils::Vector<int32_t>& tokens) noexcept
{
    if (module == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    switch (metadata::RtToken::decode_table_type(static_cast<metadata::EncodedTokenId>(token_type)))
    {
    case metadata::TableType::Method:
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass,
                                                get_type_def_class_for_metadata_enum(module, parent_token));
        RET_ERR_ON_FAIL(vm::Class::initialize_methods(klass));
        for (uint16_t i = 0; i < klass->method_count; ++i)
        {
            tokens.push_back(static_cast<int32_t>(klass->methods[i]->token));
        }
        break;
    }
    case metadata::TableType::Field:
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass,
                                                get_type_def_class_for_metadata_enum(module, parent_token));
        RET_ERR_ON_FAIL(vm::Class::initialize_fields(klass));
        for (uint16_t i = 0; i < klass->field_count; ++i)
        {
            tokens.push_back(static_cast<int32_t>(klass->fields[i].token));
        }
        break;
    }
    case metadata::TableType::Property:
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass,
                                                get_type_def_class_for_metadata_enum(module, parent_token));
        RET_ERR_ON_FAIL(vm::Class::initialize_properties(klass));
        for (uint16_t i = 0; i < klass->property_count; ++i)
        {
            tokens.push_back(static_cast<int32_t>(klass->properties[i].token));
        }
        break;
    }
    case metadata::TableType::Event:
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass,
                                                get_type_def_class_for_metadata_enum(module, parent_token));
        RET_ERR_ON_FAIL(vm::Class::initialize_events(klass));
        for (uint16_t i = 0; i < klass->event_count; ++i)
        {
            tokens.push_back(static_cast<int32_t>(klass->events[i].token));
        }
        break;
    }
    case metadata::TableType::TypeDef:
    {
        metadata::RtToken parent = metadata::RtToken::decode(static_cast<metadata::EncodedTokenId>(parent_token));
        if (parent.table_type == metadata::TableType::TypeDef && parent.rid != 0)
        {
            DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass,
                                                    get_type_def_class_for_metadata_enum(module, parent_token));
            RET_ERR_ON_FAIL(vm::Class::initialize_nested_classes(klass));
            for (uint16_t i = 0; i < klass->nested_class_count; ++i)
            {
                tokens.push_back(static_cast<int32_t>(klass->nested_classes[i]->token));
            }
        }
        else
        {
            utils::Vector<metadata::RtClass*> classes;
            RET_ERR_ON_FAIL(module->get_types(false, classes));
            for (size_t i = 0; i < classes.size(); ++i)
            {
                tokens.push_back(static_cast<int32_t>(classes[i]->token));
            }
        }
        break;
    }
    case metadata::TableType::Param:
    case metadata::TableType::CustomAttribute:
        break;
    default:
        break;
    }

    RET_VOID_OK();
}

RtResultVoid store_metadata_enum_tokens(const utils::Vector<int32_t>& tokens, int32_t* count, int32_t* result_buffer,
                                        vm::RtArray** large_result) noexcept
{
    if (count == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }
    if (*count < 0)
    {
        RET_ERR(RtErr::Argument);
    }

    int32_t result_count = static_cast<int32_t>(tokens.size());
    int32_t small_capacity = *count;
    *count = result_count;

    if (result_count == 0)
    {
        RET_VOID_OK();
    }

    if (result_count <= small_capacity)
    {
        if (result_buffer == nullptr)
        {
            RET_ERR(RtErr::ArgumentNull);
        }
        for (int32_t i = 0; i < result_count; ++i)
        {
            result_buffer[i] = tokens[static_cast<size_t>(i)];
        }
        RET_VOID_OK();
    }

    if (large_result == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, token_array,
                                            LEANCLR_NEW_SZARRAY_FROM_ELE_KLASS_INTERNAL(vm::Class::get_corlib_types().cls_int32,
                                                                                       result_count, "MetadataImport::Enum"));
    for (int32_t i = 0; i < result_count; ++i)
    {
        vm::Array::set_array_data_at<int32_t>(token_array, i, tokens[static_cast<size_t>(i)]);
    }
    *large_result = token_array;
    RET_VOID_OK();
}

RtResult<vm::RtReflectionRuntimeType*> get_runtime_type_from_type_sig(const metadata::RtTypeSig* type_sig) noexcept
{
    if (type_sig == nullptr)
    {
        RET_ERR(RtErr::BadImageFormat);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionType*, type_obj, vm::Reflection::get_type_reflection_object(type_sig));
    RET_OK(reinterpret_cast<vm::RtReflectionRuntimeType*>(type_obj));
}

RtResult<vm::RtReflectionRuntimeType*> get_generic_type_definition(void* qcall_type_handle, void* native_handle) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, type_sig,
                                            get_type_sig_from_qcall_type_handle(qcall_type_handle, native_handle));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));

    const metadata::RtClass* generic_definition_klass = nullptr;
    if (vm::Class::is_generic_inst(klass))
    {
        generic_definition_klass = vm::Class::get_generic_base_klass_of_generic_class(klass);
    }
    else if (vm::Class::is_generic(klass))
    {
        generic_definition_klass = klass;
    }
    else
    {
        generic_definition_klass = klass;
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionType*, type_obj,
                                            vm::Reflection::get_klass_reflection_object(generic_definition_klass));
    RET_OK(reinterpret_cast<vm::RtReflectionRuntimeType*>(type_obj));
}

RtResultVoid initialize_signature_from_metadata(vm::RtSignature* signature, void* raw_sig, int32_t raw_sig_size,
                                                const metadata::RtFieldInfo* field, const metadata::RtMethodInfo* method) noexcept
{
    if (signature == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    signature->sig = raw_sig;
    signature->csig = raw_sig_size;
    signature->method = method;

    if (signature->return_type_or_field_type != nullptr)
    {
        RET_VOID_OK();
    }

    const metadata::RtTypeSig* return_or_field_type = nullptr;
    const metadata::RtTypeSig* const* parameters = nullptr;
    int32_t parameter_count = 0;

    if (method != nullptr)
    {
        return_or_field_type = method->return_type;
        parameters = method->parameters;
        parameter_count = static_cast<int32_t>(method->parameter_count);
        signature->managed_calling_convention_and_arg_iterator_flags = CALLING_CONVENTION_STANDARD;
        if (vm::Method::is_instance(method))
        {
            signature->managed_calling_convention_and_arg_iterator_flags |= CALLING_CONVENTION_HAS_THIS;
        }
    }
    else if (field != nullptr)
    {
        return_or_field_type = field->type_sig;
    }
    else
    {
        RETURN_NOT_IMPLEMENTED_ERROR();
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionRuntimeType*, return_type,
                                            get_runtime_type_from_type_sig(return_or_field_type));
    signature->return_type_or_field_type = return_type;

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, arguments,
                                            LEANCLR_NEW_SZARRAY_FROM_ELE_KLASS_INTERNAL(vm::Class::get_corlib_types().cls_runtimetype,
                                                                                       parameter_count, "Signature_Init"));
    signature->arguments = arguments;
    for (int32_t i = 0; i < parameter_count; ++i)
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionRuntimeType*, parameter_type,
                                                get_runtime_type_from_type_sig(parameters[i]));
        vm::Array::set_array_data_at<vm::RtReflectionRuntimeType*>(arguments, i, parameter_type);
    }

    RET_VOID_OK();
}

RtResult<metadata::RtClass*> instantiate_type_for_generic_parameters(void* qcall_type_handle, void* native_handle, void** type_handles,
                                                                     int32_t type_handle_count) noexcept
{
    if (type_handle_count < 0 || type_handle_count > static_cast<int32_t>(metadata::RT_MAX_GENERIC_PARAM_COUNT))
    {
        RET_ERR(RtErr::Argument);
    }
    if (type_handle_count > 0 && type_handles == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, base_type_sig,
                                            get_type_sig_from_qcall_type_handle(qcall_type_handle, native_handle));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, base_klass, vm::Class::get_class_from_typesig(base_type_sig));

    uint32_t base_type_def_gid = 0;
    if (vm::Class::is_generic_inst(base_klass))
    {
        base_type_def_gid = base_type_sig->data.generic_class->base_type_def_gid;
    }
    else
    {
        base_type_def_gid = vm::Class::get_type_def_gid(base_klass);
    }

    const metadata::RtTypeSig* generic_args[metadata::RT_MAX_GENERIC_PARAM_COUNT]{};
    for (int32_t i = 0; i < type_handle_count; ++i)
    {
        if (type_handles[i] == nullptr)
        {
            RET_ERR(RtErr::ArgumentNull);
        }
        generic_args[i] = reinterpret_cast<const metadata::RtTypeSig*>(type_handles[i]);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtGenericInst*, generic_inst,
                                            metadata::MetadataCache::get_pooled_generic_inst(generic_args, static_cast<uint8_t>(type_handle_count)));
    return vm::GenericClass::get_class(base_type_def_gid, generic_inst);
}

RtResult<vm::RtObject*> create_instance_for_generic_parameters(void* qcall_type_handle, void* native_handle, void** type_handles,
                                                               int32_t type_handle_count) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass,
                                            instantiate_type_for_generic_parameters(qcall_type_handle, native_handle, type_handles, type_handle_count));
    RET_ERR_ON_FAIL(vm::Class::initialize_all(klass));

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, obj,
                                            LEANCLR_NEWOBJ_INTERNAL(klass, "RuntimeTypeHandle::CreateInstanceForAnotherGenericParameter"));

    const metadata::RtMethodInfo* ctor = vm::Method::find_matched_method_in_class_by_name_and_param_count(klass, ".ctor", 0);
    if (ctor != nullptr)
    {
        interp::RtStackObject args[1]{};
        args[0].obj = obj;
        RET_ERR_ON_FAIL(vm::Runtime::invoke_stackobject_arguments_with_run_cctor(ctor, args, nullptr));
    }

    RET_OK(obj);
}

bool is_diagnostics_stack_frame(const interp::InterpFrame* frame) noexcept
{
    const metadata::RtMethodInfo* method = frame->method;
    return method != nullptr && method->parent != nullptr && method->parent->namespaze != nullptr &&
           std::strcmp(method->parent->namespaze, "System.Diagnostics") == 0;
}

RtResultVoid collect_current_thread_stack_frames(bool need_file_info, utils::Vector<StackFrameData>& result) noexcept
{
    auto& ms = interp::MachineState::get_global_machine_state();
    auto frames = ms.get_active_frames();

    utils::Vector<StackFrameData> collected;
    for (size_t i = 0; i < frames.size(); ++i)
    {
        const interp::InterpFrame* frame = &frames[i];
        if (is_diagnostics_stack_frame(frame))
        {
            continue;
        }

        const metadata::RtMethodInfo* method = frame->method;
        StackFrameData data{method, -1, -1, nullptr, 0, 0, false};
        if (method != nullptr && method->interp_data != nullptr && frame->ip != nullptr)
        {
            data.il_offset = static_cast<int32_t>(frame->ip - method->interp_data->codes);
            metadata::PdbImage* pdb_image = method->parent->image->get_pdb_image();
            if (need_file_info && pdb_image != nullptr)
            {
                const char* pdb_file_name = nullptr;
                pdb_image->get_debug_info_for_method(method, data.il_offset, &data.il_offset, &pdb_file_name, &data.line_number, &data.column_number);
                data.file_name = pdb_file_name != nullptr ? vm::String::create_string_from_utf8cstr(pdb_file_name) : nullptr;
            }
        }
        collected.push_back(data);
    }

    for (size_t i = collected.size(); i > 0; --i)
    {
        result.push_back(collected[i - 1]);
    }

    RET_VOID_OK();
}

RtResultVoid collect_exception_stack_frames(vm::RtException* exception, utils::Vector<StackFrameData>& result) noexcept
{
    if (exception == nullptr || exception->trace_ips == nullptr)
    {
        RET_VOID_OK();
    }

    int32_t frame_count = vm::Array::get_array_length(exception->trace_ips);
    for (int32_t i = 0; i < frame_count; ++i)
    {
        auto stack_frame = reinterpret_cast<vm::RtStackFrame*>(vm::Array::get_array_data_at<vm::RtObject*>(exception->trace_ips, i));
        if (stack_frame == nullptr)
        {
            continue;
        }

        const metadata::RtMethodInfo* method = stack_frame->method != nullptr ? stack_frame->method->method : nullptr;
        result.push_back(StackFrameData{method, stack_frame->native_offset, stack_frame->il_offset, stack_frame->filename,
                                        stack_frame->line, stack_frame->column, false});
    }

    RET_VOID_OK();
}

RtResultVoid populate_stack_frame_helper(RtStackFrameHelper* helper, const utils::Vector<StackFrameData>& frames) noexcept
{
    if (helper == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    const int32_t frame_count = static_cast<int32_t>(frames.size());
    const vm::CorLibTypes& corlib_types = vm::Class::get_corlib_types();

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, offsets,
                                            LEANCLR_NEW_SZARRAY_FROM_ELE_KLASS_INTERNAL(corlib_types.cls_int32, frame_count,
                                                                                       "StackTrace_GetStackFramesInternal"));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, il_offsets,
                                            LEANCLR_NEW_SZARRAY_FROM_ELE_KLASS_INTERNAL(corlib_types.cls_int32, frame_count,
                                                                                       "StackTrace_GetStackFramesInternal"));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, method_handles,
                                            LEANCLR_NEW_SZARRAY_FROM_ELE_KLASS_INTERNAL(corlib_types.cls_intptr, frame_count,
                                                                                       "StackTrace_GetStackFramesInternal"));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, method_tokens,
                                            LEANCLR_NEW_SZARRAY_FROM_ELE_KLASS_INTERNAL(corlib_types.cls_int32, frame_count,
                                                                                       "StackTrace_GetStackFramesInternal"));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, file_names,
                                            LEANCLR_NEW_SZARRAY_FROM_ELE_KLASS_INTERNAL(corlib_types.cls_string, frame_count,
                                                                                       "StackTrace_GetStackFramesInternal"));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, line_numbers,
                                            LEANCLR_NEW_SZARRAY_FROM_ELE_KLASS_INTERNAL(corlib_types.cls_int32, frame_count,
                                                                                       "StackTrace_GetStackFramesInternal"));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, column_numbers,
                                            LEANCLR_NEW_SZARRAY_FROM_ELE_KLASS_INTERNAL(corlib_types.cls_int32, frame_count,
                                                                                       "StackTrace_GetStackFramesInternal"));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, foreign_exception_frames,
                                            LEANCLR_NEW_SZARRAY_FROM_ELE_KLASS_INTERNAL(corlib_types.cls_boolean, frame_count,
                                                                                       "StackTrace_GetStackFramesInternal"));

    for (int32_t i = 0; i < frame_count; ++i)
    {
        const StackFrameData& frame = frames[static_cast<size_t>(i)];
        vm::Array::set_array_data_at<int32_t>(offsets, i, frame.native_offset);
        vm::Array::set_array_data_at<int32_t>(il_offsets, i, frame.il_offset);
        vm::Array::set_array_data_at<void*>(method_handles, i, const_cast<metadata::RtMethodInfo*>(frame.method));
        vm::Array::set_array_data_at<int32_t>(method_tokens, i, 0);
        vm::Array::set_array_data_at<vm::RtString*>(file_names, i, frame.file_name);
        vm::Array::set_array_data_at<int32_t>(line_numbers, i, frame.line_number);
        vm::Array::set_array_data_at<int32_t>(column_numbers, i, frame.column_number);
        vm::Array::set_array_data_at<bool>(foreign_exception_frames, i, frame.is_last_frame_from_foreign_exception_stack_trace);
    }

    helper->rgi_offset = offsets;
    helper->rgi_il_offset = il_offsets;
    helper->dynamic_methods = nullptr;
    helper->rg_method_handle = method_handles;
    helper->rg_assembly_path = nullptr;
    helper->rg_assembly = nullptr;
    helper->rg_loaded_pe_address = nullptr;
    helper->rgi_loaded_pe_size = nullptr;
    helper->rgi_is_file_layout = nullptr;
    helper->rg_in_memory_pdb_address = nullptr;
    helper->rgi_in_memory_pdb_size = nullptr;
    helper->rgi_method_token = method_tokens;
    helper->rg_filename = file_names;
    helper->rgi_line_number = line_numbers;
    helper->rgi_column_number = column_numbers;
    helper->rgi_last_frame_from_foreign_exception_stack_trace = foreign_exception_frames;
    helper->frame_count = frame_count;

    RET_VOID_OK();
}

RtResultVoid get_current_thread_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                        interp::RtStackObject*) noexcept
{
    auto thread_slot = interp::EvalStackOp::get_param<vm::RtThread**>(params, 0);
    *thread_slot = vm::Thread::get_current_thread();
    RET_VOID_OK();
}

RtResultVoid is_managed_debugger_attached_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject*,
                                                  interp::RtStackObject* ret) noexcept
{
    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(0));
    RET_VOID_OK();
}

RtResultVoid debugger_log_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject*, interp::RtStackObject*) noexcept
{
    RET_VOID_OK();
}

RtResultVoid metadata_updater_is_apply_update_supported_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                                const interp::RtStackObject*, interp::RtStackObject* ret) noexcept
{
    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(0));
    RET_VOID_OK();
}

RtResultVoid kernel32_get_last_error_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject*,
                                             interp::RtStackObject* ret) noexcept
{
    interp::EvalStackOp::set_return(ret, vm::Marshal::get_last_win32_error());
    RET_VOID_OK();
}

RtResultVoid kernel32_set_last_error_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                             interp::RtStackObject*) noexcept
{
    int32_t error = interp::EvalStackOp::get_param<int32_t>(params, 0);
    vm::Marshal::set_last_win32_error(error);
    RET_VOID_OK();
}

RtResultVoid kernel32_get_environment_variable_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                       const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    vm::RtString* variable_name = interp::EvalStackOp::get_param<vm::RtString*>(params, 0);
    Utf16Char* value = interp::EvalStackOp::get_param<Utf16Char*>(params, 1);
    uint32_t value_length = interp::EvalStackOp::get_param<uint32_t>(params, 2);

    uint32_t result = platform::RtSys::get_environment_variable(variable_name != nullptr ? vm::String::get_chars_ptr(variable_name) : nullptr,
                                                               value, value_length);
    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

RtResultVoid kernel32_get_locale_info_ex_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                 const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    vm::RtString* locale_name = interp::EvalStackOp::get_param<vm::RtString*>(params, 0);
    uint32_t lc_type = interp::EvalStackOp::get_param<uint32_t>(params, 1);
    Utf16Char* locale_data = interp::EvalStackOp::get_param<Utf16Char*>(params, 2);
    int32_t locale_data_length = interp::EvalStackOp::get_param<int32_t>(params, 3);

    int32_t result = platform::RtSys::get_locale_info_ex(locale_name != nullptr ? vm::String::get_chars_ptr(locale_name) : nullptr,
                                                         lc_type, locale_data, locale_data_length);
    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

RtResultVoid globalization_load_icu_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject*,
                                            interp::RtStackObject* ret) noexcept
{
    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(0));
    RET_VOID_OK();
}

RtResultVoid advapi32_event_register_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                             interp::RtStackObject* ret) noexcept
{
    (void)interp::EvalStackOp::get_param<void*>(params, 0);
    (void)interp::EvalStackOp::get_param<void*>(params, 1);
    (void)interp::EvalStackOp::get_param<void*>(params, 2);
    auto registration_handle = interp::EvalStackOp::get_param<int64_t*>(params, 3);
    if (registration_handle != nullptr)
    {
        *registration_handle = 1;
    }
    interp::EvalStackOp::set_return(ret, static_cast<uint32_t>(0));
    RET_VOID_OK();
}

RtResultVoid advapi32_event_unregister_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject*,
                                               interp::RtStackObject* ret) noexcept
{
    interp::EvalStackOp::set_return(ret, static_cast<uint32_t>(0));
    RET_VOID_OK();
}

RtResultVoid advapi32_event_write_transfer_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                   const interp::RtStackObject*, interp::RtStackObject* ret) noexcept
{
    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(0));
    RET_VOID_OK();
}

RtResultVoid advapi32_event_activity_id_control_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                        const interp::RtStackObject*, interp::RtStackObject* ret) noexcept
{
    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(0));
    RET_VOID_OK();
}

RtResultVoid advapi32_event_set_information_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject*,
                                                    interp::RtStackObject* ret) noexcept
{
    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(0));
    RET_VOID_OK();
}

RtResultVoid environment_get_processor_count_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject*,
                                                     interp::RtStackObject* ret) noexcept
{
    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(1));
    RET_VOID_OK();
}

RtResultVoid ntdll_rtl_get_version_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                           interp::RtStackObject* ret) noexcept
{
    auto version = interp::EvalStackOp::get_param<RtOsVersionInfoEx*>(params, 0);
    if (version == nullptr)
    {
        interp::EvalStackOp::set_return(ret, static_cast<int32_t>(-1));
        RET_VOID_OK();
    }

    *version = {};
    version->dwOSVersionInfoSize = sizeof(RtOsVersionInfoEx);
    version->dwMajorVersion = 10;
    version->dwMinorVersion = 0;
    version->dwBuildNumber = 0;
    version->dwPlatformId = 2; // VER_PLATFORM_WIN32_NT
    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(0));
    RET_VOID_OK();
}

RtResultVoid eventpipe_enable_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject*,
                                      interp::RtStackObject* ret) noexcept
{
    interp::EvalStackOp::set_return(ret, static_cast<uint64_t>(0));
    RET_VOID_OK();
}

RtResultVoid eventpipe_void_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject*,
                                    interp::RtStackObject*) noexcept
{
    RET_VOID_OK();
}

RtResultVoid eventpipe_create_provider_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject*,
                                               interp::RtStackObject* ret) noexcept
{
    interp::EvalStackOp::set_return(ret, reinterpret_cast<void*>(1));
    RET_VOID_OK();
}

RtResultVoid eventpipe_define_event_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject*,
                                            interp::RtStackObject* ret) noexcept
{
    interp::EvalStackOp::set_return(ret, reinterpret_cast<void*>(1));
    RET_VOID_OK();
}

RtResultVoid eventpipe_get_provider_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject*,
                                            interp::RtStackObject* ret) noexcept
{
    interp::EvalStackOp::set_return(ret, nullptr);
    RET_VOID_OK();
}

RtResultVoid eventpipe_activity_id_control_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject*,
                                                   interp::RtStackObject* ret) noexcept
{
    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(0));
    RET_VOID_OK();
}

RtResultVoid eventpipe_bool_false_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject*,
                                          interp::RtStackObject* ret) noexcept
{
    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(0));
    RET_VOID_OK();
}

RtResultVoid get_type_handle_gc_handle_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                               interp::RtStackObject* ret) noexcept
{
    (void)interp::EvalStackOp::get_param<void*>(params, 0);
    (void)interp::EvalStackOp::get_param<void*>(params, 1);
    int32_t handle_type = interp::EvalStackOp::get_param<int32_t>(params, 2);
    void* handle = vm::GCHandle::get_target_handle(nullptr, nullptr, handle_type);
    interp::EvalStackOp::set_return(ret, vm::GCHandle::get_target_slot(handle));
    RET_VOID_OK();
}

RtResultVoid free_type_handle_gc_handle_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                                interp::RtStackObject* ret) noexcept
{
    (void)interp::EvalStackOp::get_param<void*>(params, 0);
    (void)interp::EvalStackOp::get_param<void*>(params, 1);
    auto slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 2);
    vm::GCHandle::free_handle(vm::GCHandle::get_handle_by_target_slot(slot));
    interp::EvalStackOp::set_return(ret, nullptr);
    RET_VOID_OK();
}

RtResultVoid get_frozen_stack_trace_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                            interp::RtStackObject*) noexcept
{
    (void)interp::EvalStackOp::get_param<void*>(params, 0);
    auto stack_trace_slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 1);
    *stack_trace_slot = nullptr;
    RET_VOID_OK();
}

RtResultVoid get_stack_frames_internal_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                               const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto helper_slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 0);
    bool need_file_info = interp::EvalStackOp::get_param<int32_t>(params, 1) != 0;
    auto exception_slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 2);

    if (helper_slot == nullptr || *helper_slot == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    auto helper = reinterpret_cast<RtStackFrameHelper*>(*helper_slot);
    vm::RtException* exception = exception_slot != nullptr ? reinterpret_cast<vm::RtException*>(*exception_slot) : nullptr;

    utils::Vector<StackFrameData> frames;
    if (exception != nullptr)
    {
        RET_ERR_ON_FAIL(collect_exception_stack_frames(exception, frames));
    }
    else
    {
        RET_ERR_ON_FAIL(collect_current_thread_stack_frames(need_file_info, frames));
    }

    RET_ERR_ON_FAIL(populate_stack_frame_helper(helper, frames));
    RET_VOID_OK();
}

RtResultVoid delegate_find_method_handle_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                                 interp::RtStackObject*) noexcept
{
    auto delegate_slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 0);
    auto method_info_slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 1);
    if (delegate_slot == nullptr || method_info_slot == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    auto this_delegate = reinterpret_cast<vm::RtDelegate*>(*delegate_slot);
    if (this_delegate == nullptr)
    {
        RET_ERR(RtErr::NullReference);
    }

    const metadata::RtMethodInfo* reflected_method = this_delegate->method;
    if (this_delegate->target != nullptr)
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtMethodInfo*, virtual_method,
                                                vm::Method::get_virtual_method_impl(this_delegate->target, this_delegate->method));
        reflected_method = virtual_method;
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionMethod*, method_info,
                                            vm::Reflection::get_method_reflection_object(reflected_method, reflected_method->parent));
    *method_info_slot = reinterpret_cast<vm::RtObject*>(method_info);
    RET_VOID_OK();
}

RtResultVoid construct_runtime_type_name_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                                 interp::RtStackObject*) noexcept
{
    auto qcall_type_handle = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    int32_t format_flags = interp::EvalStackOp::get_param<int32_t>(params, 2);
    auto ret_string = interp::EvalStackOp::get_param<vm::RtString**>(params, 3);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtString*, name, construct_type_name(qcall_type_handle, native_handle, format_flags));
    *ret_string = name;
    RET_VOID_OK();
}

RtResultVoid get_cor_element_type_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                          interp::RtStackObject* ret) noexcept
{
    auto type_handle = interp::EvalStackOp::get_param<void*>(params, 0);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(int32_t, result, get_cor_element_type(type_handle));
    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

RtResultVoid get_generic_type_definition_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                                 interp::RtStackObject*) noexcept
{
    auto qcall_type_handle = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto ret_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType**>(params, 2);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionRuntimeType*, generic_type_definition,
                                            get_generic_type_definition(qcall_type_handle, native_handle));
    *ret_type = generic_type_definition;
    RET_VOID_OK();
}

RtResultVoid get_module_types_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                      interp::RtStackObject* ret) noexcept
{
    auto qcall_module = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto types_slot = interp::EvalStackOp::get_param<vm::RtArray**>(params, 2);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, types, get_module_types(qcall_module, native_handle));
    if (types_slot != nullptr)
    {
        *types_slot = types;
    }
    interp::EvalStackOp::set_return(ret, types);
    RET_VOID_OK();
}

RtResultVoid metadata_import_enum_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                          interp::RtStackObject*) noexcept
{
    auto metadata_import = interp::EvalStackOp::get_param<metadata::RtModuleDef*>(params, 0);
    int32_t token_type = interp::EvalStackOp::get_param<int32_t>(params, 1);
    int32_t parent_token = interp::EvalStackOp::get_param<int32_t>(params, 2);
    auto count = interp::EvalStackOp::get_param<int32_t*>(params, 3);
    auto result_buffer = interp::EvalStackOp::get_param<int32_t*>(params, 4);
    auto large_result = interp::EvalStackOp::get_param<vm::RtArray**>(params, 5);

    utils::Vector<int32_t> tokens;
    RET_ERR_ON_FAIL(collect_metadata_enum_tokens(metadata_import, token_type, parent_token, tokens));
    RET_ERR_ON_FAIL(store_metadata_enum_tokens(tokens, count, result_buffer, large_result));
    RET_VOID_OK();
}

RtResultVoid create_instance_for_another_generic_parameter_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                                   const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto qcall_type_handle = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto type_handles = interp::EvalStackOp::get_param<void**>(params, 2);
    int32_t type_handle_count = interp::EvalStackOp::get_param<int32_t>(params, 3);
    auto ret_obj = interp::EvalStackOp::get_param<vm::RtObject**>(params, 4);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, obj,
                                            create_instance_for_generic_parameters(qcall_type_handle, native_handle, type_handles, type_handle_count));
    *ret_obj = obj;
    RET_VOID_OK();
}

RtResultVoid signature_init_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                    interp::RtStackObject*) noexcept
{
    auto signature_slot = interp::EvalStackOp::get_param<vm::RtSignature**>(params, 0);
    auto raw_sig = interp::EvalStackOp::get_param<void*>(params, 1);
    int32_t raw_sig_size = interp::EvalStackOp::get_param<int32_t>(params, 2);
    auto field = interp::EvalStackOp::get_param<const metadata::RtFieldInfo*>(params, 3);
    auto method = interp::EvalStackOp::get_param<const metadata::RtMethodInfo*>(params, 4);

    vm::RtSignature* signature = signature_slot != nullptr ? *signature_slot : nullptr;
    RET_ERR_ON_FAIL(initialize_signature_from_metadata(signature, raw_sig, raw_sig_size, field, method));
    RET_VOID_OK();
}

} // namespace

void register_coreclr_qcall_pinvokes() noexcept
{
    vm::PInvokes::register_pinvoke("System.Threading.Thread::GetCurrentThread(System.Runtime.CompilerServices.ObjectHandleOnStack)", nullptr,
                                   get_current_thread_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::GetCurrentThread", nullptr, get_current_thread_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.Debugger::IsManagedDebuggerAttached()", nullptr, is_managed_debugger_attached_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.Debugger::IsManagedDebuggerAttached", nullptr, is_managed_debugger_attached_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.Debugger::<LogInternal>g____PInvoke|10_0", nullptr, debugger_log_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.Metadata.MetadataUpdater::<IsApplyUpdateSupported>g____PInvoke|1_0()", nullptr,
                                   metadata_updater_is_apply_update_supported_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.Metadata.MetadataUpdater::<IsApplyUpdateSupported>g____PInvoke|1_0", nullptr,
                                   metadata_updater_is_apply_update_supported_invoker);
    vm::PInvokes::register_pinvoke("AssemblyNative_IsApplyUpdateSupported()", nullptr, metadata_updater_is_apply_update_supported_invoker);
    vm::PInvokes::register_pinvoke("AssemblyNative_IsApplyUpdateSupported", nullptr, metadata_updater_is_apply_update_supported_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::GetLastError()", nullptr, kernel32_get_last_error_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::GetLastError", nullptr, kernel32_get_last_error_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::GetLastError()", nullptr, kernel32_get_last_error_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::GetLastError", nullptr, kernel32_get_last_error_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::SetLastError(System.Int32)", nullptr, kernel32_set_last_error_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::SetLastError", nullptr, kernel32_set_last_error_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::SetLastError(System.Int32)", nullptr, kernel32_set_last_error_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::SetLastError", nullptr, kernel32_set_last_error_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::GetEnvironmentVariable(System.String,System.Char&,System.UInt32)", nullptr,
                                   kernel32_get_environment_variable_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::GetEnvironmentVariable", nullptr, kernel32_get_environment_variable_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::<GetEnvironmentVariable>g____PInvoke|296_0(System.String,System.Char&,System.UInt32)",
                                   nullptr, kernel32_get_environment_variable_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::<GetEnvironmentVariable>g____PInvoke|296_0", nullptr,
                                   kernel32_get_environment_variable_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::GetEnvironmentVariable(System.String,System.Char&,System.UInt32)", nullptr,
                                   kernel32_get_environment_variable_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::GetEnvironmentVariable", nullptr, kernel32_get_environment_variable_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::<GetEnvironmentVariable>g____PInvoke|296_0(System.String,System.Char&,System.UInt32)", nullptr,
                                   kernel32_get_environment_variable_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::<GetEnvironmentVariable>g____PInvoke|296_0", nullptr,
                                   kernel32_get_environment_variable_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::GetLocaleInfoEx(System.String,System.UInt32,System.Char*,System.Int32)", nullptr,
                                   kernel32_get_locale_info_ex_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::GetLocaleInfoEx", nullptr, kernel32_get_locale_info_ex_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::<GetLocaleInfoEx>g____PInvoke|34_0(System.String,System.UInt32,System.Char*,System.Int32)",
                                   nullptr, kernel32_get_locale_info_ex_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::<GetLocaleInfoEx>g____PInvoke|34_0", nullptr,
                                   kernel32_get_locale_info_ex_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::GetLocaleInfoEx(System.String,System.UInt32,System.Char*,System.Int32)", nullptr,
                                   kernel32_get_locale_info_ex_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::GetLocaleInfoEx", nullptr, kernel32_get_locale_info_ex_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::<GetLocaleInfoEx>g____PInvoke|34_0(System.String,System.UInt32,System.Char*,System.Int32)", nullptr,
                                   kernel32_get_locale_info_ex_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::<GetLocaleInfoEx>g____PInvoke|34_0", nullptr,
                                   kernel32_get_locale_info_ex_invoker);
    vm::PInvokes::register_pinvoke("Interop/Globalization::LoadICU()", nullptr, globalization_load_icu_invoker);
    vm::PInvokes::register_pinvoke("Interop/Globalization::LoadICU", nullptr, globalization_load_icu_invoker);
    vm::PInvokes::register_pinvoke("Globalization::LoadICU()", nullptr, globalization_load_icu_invoker);
    vm::PInvokes::register_pinvoke("Globalization::LoadICU", nullptr, globalization_load_icu_invoker);
    vm::PInvokes::register_pinvoke("Interop/Advapi32::EventRegister", nullptr, advapi32_event_register_invoker);
    vm::PInvokes::register_pinvoke("Advapi32::EventRegister", nullptr, advapi32_event_register_invoker);
    vm::PInvokes::register_pinvoke("Interop/Advapi32::EventUnregister", nullptr, advapi32_event_unregister_invoker);
    vm::PInvokes::register_pinvoke("Advapi32::EventUnregister", nullptr, advapi32_event_unregister_invoker);
    vm::PInvokes::register_pinvoke("Interop/Advapi32::EventWriteTransfer", nullptr, advapi32_event_write_transfer_invoker);
    vm::PInvokes::register_pinvoke("Advapi32::EventWriteTransfer", nullptr, advapi32_event_write_transfer_invoker);
    vm::PInvokes::register_pinvoke("Interop/Advapi32::EventActivityIdControl", nullptr, advapi32_event_activity_id_control_invoker);
    vm::PInvokes::register_pinvoke("Advapi32::EventActivityIdControl", nullptr, advapi32_event_activity_id_control_invoker);
    vm::PInvokes::register_pinvoke("Interop/Advapi32::EventSetInformation", nullptr, advapi32_event_set_information_invoker);
    vm::PInvokes::register_pinvoke("Advapi32::EventSetInformation", nullptr, advapi32_event_set_information_invoker);
    vm::PInvokes::register_pinvoke("System.Environment::GetProcessorCount()", nullptr, environment_get_processor_count_invoker);
    vm::PInvokes::register_pinvoke("System.Environment::GetProcessorCount", nullptr, environment_get_processor_count_invoker);
    vm::PInvokes::register_pinvoke("Interop/NtDll::<RtlGetVersion>g____PInvoke|22_0", nullptr, ntdll_rtl_get_version_invoker);
    vm::PInvokes::register_pinvoke("NtDll::<RtlGetVersion>g____PInvoke|22_0", nullptr, ntdll_rtl_get_version_invoker);
    vm::PInvokes::register_pinvoke("Interop/NtDll::RtlGetVersion", nullptr, ntdll_rtl_get_version_invoker);
    vm::PInvokes::register_pinvoke("NtDll::RtlGetVersion", nullptr, ntdll_rtl_get_version_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.Tracing.EventPipeInternal::<Enable>g____PInvoke|0_0", nullptr, eventpipe_enable_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.Tracing.EventPipeInternal::<Disable>g____PInvoke|1_0", nullptr, eventpipe_void_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.Tracing.EventPipeInternal::<CreateProvider>g____PInvoke|4_0", nullptr,
                                   eventpipe_create_provider_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.Tracing.EventPipeInternal::<DefineEvent>g____PInvoke|5_0", nullptr,
                                   eventpipe_define_event_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.Tracing.EventPipeInternal::<GetProvider>g____PInvoke|6_0", nullptr,
                                   eventpipe_get_provider_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.Tracing.EventPipeInternal::<DeleteProvider>g____PInvoke|7_0", nullptr,
                                   eventpipe_void_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.Tracing.EventPipeInternal::<EventActivityIdControl>g____PInvoke|8_0", nullptr,
                                   eventpipe_activity_id_control_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.Tracing.EventPipeInternal::<WriteEventData>g____PInvoke|9_0", nullptr,
                                   eventpipe_void_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.Tracing.EventPipeInternal::<GetSessionInfo>g____PInvoke|10_0", nullptr,
                                   eventpipe_bool_false_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.Tracing.EventPipeInternal::<GetNextEvent>g____PInvoke|11_0", nullptr,
                                   eventpipe_bool_false_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.Tracing.EventPipeInternal::<SignalSession>g____PInvoke|12_0", nullptr,
                                   eventpipe_bool_false_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.Tracing.EventPipeInternal::<WaitForSessionSignal>g____PInvoke|13_0", nullptr,
                                   eventpipe_bool_false_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeTypeHandle::GetGCHandle(System.Runtime.CompilerServices.QCallTypeHandle,System.Runtime.InteropServices.GCHandleType)", nullptr,
        get_type_handle_gc_handle_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeTypeHandle::GetGCHandle", nullptr, get_type_handle_gc_handle_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeTypeHandle::FreeGCHandle(System.Runtime.CompilerServices.QCallTypeHandle,System.IntPtr)", nullptr,
                                   free_type_handle_gc_handle_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeTypeHandle::FreeGCHandle", nullptr, free_type_handle_gc_handle_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Exception::GetFrozenStackTrace(System.Runtime.CompilerServices.ObjectHandleOnStack,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, get_frozen_stack_trace_invoker);
    vm::PInvokes::register_pinvoke("System.Exception::GetFrozenStackTrace", nullptr, get_frozen_stack_trace_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Diagnostics.StackTrace::<GetStackFramesInternal>g____PInvoke|0_0(System.Runtime.CompilerServices.ObjectHandleOnStack,System.Int32,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, get_stack_frames_internal_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.StackTrace::<GetStackFramesInternal>g____PInvoke|0_0", nullptr,
                                   get_stack_frames_internal_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Diagnostics.StackTrace::GetStackFramesInternal(System.Runtime.CompilerServices.ObjectHandleOnStack,System.Boolean,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, get_stack_frames_internal_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.StackTrace::GetStackFramesInternal", nullptr, get_stack_frames_internal_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Delegate::FindMethodHandle(System.Runtime.CompilerServices.ObjectHandleOnStack,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, delegate_find_method_handle_invoker);
    vm::PInvokes::register_pinvoke("System.Delegate::FindMethodHandle", nullptr, delegate_find_method_handle_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeTypeHandle::ConstructName(System.Runtime.CompilerServices.QCallTypeHandle,System.TypeNameFormatFlags,System.Runtime.CompilerServices.StringHandleOnStack)",
        nullptr, construct_runtime_type_name_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeTypeHandle::ConstructName", nullptr, construct_runtime_type_name_invoker);
    vm::PInvokes::register_pinvoke("System.Runtime.CompilerServices.TypeHandle::GetCorElementType(System.IntPtr)", nullptr,
                                   get_cor_element_type_invoker);
    vm::PInvokes::register_pinvoke("System.Runtime.CompilerServices.TypeHandle::GetCorElementType", nullptr, get_cor_element_type_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeTypeHandle::GetGenericTypeDefinition(System.Runtime.CompilerServices.QCallTypeHandle,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, get_generic_type_definition_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeTypeHandle::GetGenericTypeDefinition", nullptr, get_generic_type_definition_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.RuntimeModule::GetTypes(System.Runtime.CompilerServices.QCallModule,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, get_module_types_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.RuntimeModule::GetTypes", nullptr, get_module_types_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.MetadataImport::<Enum>g____PInvoke|8_0(System.IntPtr,System.Int32,System.Int32,System.Int32*,System.Int32*,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, metadata_import_enum_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.MetadataImport::<Enum>g____PInvoke|8_0", nullptr, metadata_import_enum_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeTypeHandle::CreateInstanceForAnotherGenericParameter(System.Runtime.CompilerServices.QCallTypeHandle,System.IntPtr*,System.Int32,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, create_instance_for_another_generic_parameter_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeTypeHandle::CreateInstanceForAnotherGenericParameter", nullptr,
                                   create_instance_for_another_generic_parameter_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Signature::Init(System.Runtime.CompilerServices.ObjectHandleOnStack,System.Void*,System.Int32,System.RuntimeFieldHandleInternal,System.RuntimeMethodHandleInternal)",
        nullptr, signature_init_invoker);
    vm::PInvokes::register_pinvoke("System.Signature::Init", nullptr, signature_init_invoker);
}

} // namespace pinvokes
} // namespace leanclr

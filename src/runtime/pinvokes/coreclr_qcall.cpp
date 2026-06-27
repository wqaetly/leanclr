#include "coreclr_qcall.h"

#include <cstring>

#include "icalls/system_enum.h"
#include "interp/eval_stack_op.h"
#include "interp/machine_state.h"
#include "metadata/metadata_name.h"
#include "metadata/metadata_cache.h"
#include "metadata/module_def.h"
#include "platform/bcrypt.h"
#include "platform/rt_sys.h"
#include "utils/rt_vector.h"
#include "utils/string_builder.h"
#include "vm/assembly.h"
#include "vm/array_class.h"
#include "vm/class.h"
#include "vm/delegate.h"
#include "vm/environment.h"
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
#include "vm/type.h"

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

struct RtSystemInfo
{
    uint16_t wProcessorArchitecture;
    uint16_t wReserved;
    uint32_t dwPageSize;
    uintptr_t lpMinimumApplicationAddress;
    uintptr_t lpMaximumApplicationAddress;
    uintptr_t dwActiveProcessorMask;
    uint32_t dwNumberOfProcessors;
    uint32_t dwProcessorType;
    uint32_t dwAllocationGranularity;
    uint16_t wProcessorLevel;
    uint16_t wProcessorRevision;
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

static bool is_value_type_fast_compare_blocked_field_type(metadata::RtElementType element_type) noexcept
{
    switch (element_type)
    {
    case metadata::RtElementType::R4:
    case metadata::RtElementType::R8:
    case metadata::RtElementType::Ptr:
    case metadata::RtElementType::FnPtr:
    case metadata::RtElementType::Object:
    case metadata::RtElementType::String:
    case metadata::RtElementType::Class:
    case metadata::RtElementType::Array:
    case metadata::RtElementType::SZArray:
        return true;
    default:
        return false;
    }
}

RtResult<bool> declares_value_type_equals_or_get_hash_code(const metadata::RtClass* klass) noexcept
{
    RET_ERR_ON_FAIL(vm::Class::initialize_methods(const_cast<metadata::RtClass*>(klass)));
    for (uint16_t i = 0; i < klass->method_count; ++i)
    {
        const metadata::RtMethodInfo* method = klass->methods[i];
        if (method == nullptr || method->name == nullptr)
        {
            continue;
        }

        if ((std::strcmp(method->name, "Equals") == 0 && method->parameter_count == 1) ||
            (std::strcmp(method->name, "GetHashCode") == 0 && method->parameter_count == 0))
        {
            RET_OK(true);
        }
    }

    RET_OK(false);
}

RtResult<bool> can_compare_bits_or_use_fast_get_hash_code(const metadata::RtClass* klass) noexcept
{
    if (klass == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    if (!vm::Class::is_value_type(klass))
    {
        RET_OK(false);
    }

    if (vm::Class::is_enum_type(klass))
    {
        RET_OK(true);
    }

    if (vm::Class::is_explicit_layout(klass))
    {
        RET_OK(false);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, declares_override, declares_value_type_equals_or_get_hash_code(klass));
    if (declares_override)
    {
        RET_OK(false);
    }

    RET_ERR_ON_FAIL(vm::Class::initialize_fields(const_cast<metadata::RtClass*>(klass)));
    if (vm::Class::get_has_references(klass))
    {
        RET_OK(false);
    }

    for (uint16_t i = 0; i < klass->field_count; ++i)
    {
        const metadata::RtFieldInfo* field = &klass->fields[i];
        if (!vm::Field::is_instance(field))
        {
            continue;
        }

        metadata::RtElementType element_type = field->type_sig->ele_type;
        if (is_value_type_fast_compare_blocked_field_type(element_type))
        {
            RET_OK(false);
        }

        if (element_type == metadata::RtElementType::ValueType || element_type == metadata::RtElementType::GenericInst)
        {
            DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, field_klass, vm::Class::get_class_from_typesig(field->type_sig));
            if (!vm::Class::is_value_type(field_klass))
            {
                RET_OK(false);
            }

            DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, nested_can_compare,
                                                    can_compare_bits_or_use_fast_get_hash_code(field_klass));
            if (!nested_can_compare)
            {
                RET_OK(false);
            }
        }
    }

    RET_OK(true);
}

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

RtResult<metadata::RtAssembly*> get_assembly_from_qcall_assembly(void* qcall_assembly, void* native_handle) noexcept
{
    if (native_handle != nullptr)
    {
        RET_OK(reinterpret_cast<metadata::RtAssembly*>(native_handle));
    }

    if (qcall_assembly == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    auto runtime_assembly_klass = vm::Class::get_corlib_types().cls_reflection_assembly;
    auto direct_assembly = reinterpret_cast<vm::RtReflectionAssembly*>(qcall_assembly);
    if (direct_assembly->klass == runtime_assembly_klass)
    {
        RET_OK(direct_assembly->assembly);
    }

    auto runtime_assembly = *reinterpret_cast<vm::RtReflectionAssembly**>(qcall_assembly);
    if (runtime_assembly == nullptr || runtime_assembly->klass != runtime_assembly_klass)
    {
        RET_ERR(RtErr::BadImageFormat);
    }

    RET_OK(runtime_assembly->assembly);
}

RtResult<vm::RtArray*> create_array_instance(void* qcall_type_handle, void* native_handle, int32_t rank, int32_t* lengths,
                                             int32_t* lower_bounds, bool from_array_type) noexcept
{
    if (rank <= 0 || rank > static_cast<int32_t>(metadata::RT_MAX_ARRAY_RANK) || lengths == nullptr)
    {
        RET_ERR(RtErr::Argument);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, type_sig,
                                            get_type_sig_from_qcall_type_handle(qcall_type_handle, native_handle));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));

    if (from_array_type)
    {
        if (!vm::Class::is_array_or_szarray(klass))
        {
            RET_ERR(RtErr::Argument);
        }

        if (rank == 1 && (lower_bounds == nullptr || lower_bounds[0] == 0) && vm::Class::is_szarray_class(klass))
        {
            return LEANCLR_NEW_SZARRAY_FROM_ARRAY_KLASS_INTERNAL(klass, lengths[0], "Array_CreateInstance");
        }

        return LEANCLR_NEW_MDARRAY_FROM_ARRAY_KLASS_INTERNAL(klass, lengths, lower_bounds, "Array_CreateInstance");
    }

    if (rank == 1 && (lower_bounds == nullptr || lower_bounds[0] == 0))
    {
        return LEANCLR_NEW_SZARRAY_FROM_ELE_KLASS_INTERNAL(klass, lengths[0], "Array_CreateInstance");
    }

    return LEANCLR_NEW_MDARRAY_FROM_ELE_KLASS_INTERNAL(klass, rank, lengths, lower_bounds, "Array_CreateInstance");
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

RtResult<vm::RtReflectionType*> get_runtime_assembly_type_core(metadata::RtAssembly* assembly, const char* type_name,
                                                               void** nested_type_names, int32_t nested_type_names_length, bool ignore_case) noexcept
{
    if (assembly == nullptr || assembly->mod == nullptr || type_name == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }
    if (nested_type_names_length < 0 || (nested_type_names_length > 0 && nested_type_names == nullptr))
    {
        RET_ERR(RtErr::Argument);
    }

    utils::Utf8StringBuilder full_name;
    full_name.append_cstr(type_name);
    for (int32_t i = 0; i < nested_type_names_length; ++i)
    {
        auto nested_type_name = reinterpret_cast<const char*>(nested_type_names[i]);
        if (nested_type_name == nullptr)
        {
            RET_ERR(RtErr::ArgumentNull);
        }
        full_name.append_char('+');
        full_name.append_cstr(nested_type_name);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(
        const metadata::RtTypeSig*, resolved_type_sig,
        vm::Type::resolve_assembly_qualified_name(assembly->mod, full_name.get_const_chars(), full_name.length(), ignore_case));
    if (resolved_type_sig == nullptr)
    {
        RET_OK(nullptr);
    }

    return vm::Reflection::get_type_reflection_object(resolved_type_sig);
}

RtResult<vm::RtReflectionType*> get_runtime_assembly_type_core_ignore_case(metadata::RtAssembly* assembly, const Utf16Char* type_name,
                                                                           void** nested_type_names, int32_t nested_type_names_length) noexcept
{
    if (type_name == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }
    if (nested_type_names_length < 0 || (nested_type_names_length > 0 && nested_type_names == nullptr))
    {
        RET_ERR(RtErr::Argument);
    }

    utils::Utf8StringBuilder full_name(type_name);
    for (int32_t i = 0; i < nested_type_names_length; ++i)
    {
        auto nested_type_name = reinterpret_cast<const Utf16Char*>(nested_type_names[i]);
        if (nested_type_name == nullptr)
        {
            RET_ERR(RtErr::ArgumentNull);
        }
        full_name.append_char('+');
        full_name.append_utf16_str(nested_type_name, static_cast<size_t>(utils::StringUtil::get_utf16chars_length(nested_type_name)));
    }

    full_name.sure_null_terminator_but_not_append();
    return get_runtime_assembly_type_core(assembly, full_name.get_const_chars(), nullptr, 0, true);
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
        metadata::RtToken parent = metadata::RtToken::decode(static_cast<metadata::EncodedTokenId>(parent_token));
        if (parent.table_type == metadata::TableType::Property || parent.table_type == metadata::TableType::Event)
        {
            const metadata::CliImage& cli_image = module->get_cli_image();
            uint32_t association = metadata::RtMetadata::encode_has_semantics_coded_index(parent.table_type, parent.rid);
            auto semantics_range = cli_image.find_row_range_of_owner_at_sorted_table(metadata::TableType::MethodSemantics, 2, association);
            if (semantics_range)
            {
                for (uint32_t semantics_rid = semantics_range->ridBegin; semantics_rid < semantics_range->ridEnd; ++semantics_rid)
                {
                    auto semantics_row = cli_image.read_method_semantics(semantics_rid);
                    if (!semantics_row)
                    {
                        RET_ERR(RtErr::BadImageFormat);
                    }

                    tokens.push_back(static_cast<int32_t>(metadata::RtToken::encode(metadata::TableType::Method, semantics_row->method)));
                    tokens.push_back(static_cast<int32_t>(semantics_row->semantics));
                }
            }
        }
        else
        {
            DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass,
                                                    get_type_def_class_for_metadata_enum(module, parent_token));
            RET_ERR_ON_FAIL(vm::Class::initialize_methods(klass));
            for (uint16_t i = 0; i < klass->method_count; ++i)
            {
                tokens.push_back(static_cast<int32_t>(klass->methods[i]->token));
            }
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

    if (large_result != nullptr)
    {
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

RtResult<vm::RtReflectionModule*> get_module_from_runtime_type_slot(vm::RtObject** runtime_type_slot) noexcept
{
    if (runtime_type_slot == nullptr || *runtime_type_slot == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    auto runtime_type = reinterpret_cast<vm::RtReflectionRuntimeType*>(*runtime_type_slot);
    if (runtime_type->reflection_type.header.klass != vm::Class::get_corlib_types().cls_runtimetype)
    {
        RET_ERR(RtErr::Argument);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass,
                                            vm::Class::get_class_from_typesig(runtime_type->reflection_type.type_handle));
    return vm::Reflection::get_module_reflection_object(klass->image);
}

RtResult<vm::RtArray*> get_type_instantiation(void* qcall_type_handle, void* native_handle, bool runtime_array) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, type_sig,
                                            get_type_sig_from_qcall_type_handle(qcall_type_handle, native_handle));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));

    const auto& corlib_types = vm::Class::get_corlib_types();
    metadata::RtClass* element_klass = runtime_array ? corlib_types.cls_runtimetype : corlib_types.cls_systemtype;

    if (vm::Class::is_generic(klass))
    {
        const metadata::RtGenericContainer* generic_container = klass->generic_container;
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(
            vm::RtArray*, result,
            LEANCLR_NEW_SZARRAY_FROM_ELE_KLASS_INTERNAL(element_klass, generic_container->generic_param_count,
                                                        "RuntimeTypeHandle_GetInstantiation"));
        for (uint32_t i = 0; i < generic_container->generic_param_count; ++i)
        {
            const metadata::RtGenericParam* param = &generic_container->generic_params[i];
            metadata::RtTypeSig generic_param_type_sig = metadata::RtTypeSig::new_byval_with_data(metadata::RtElementType::Var, param);
            DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionType*, type_obj,
                                                    vm::Reflection::get_type_reflection_object(&generic_param_type_sig));
            vm::Array::set_array_data_at<vm::RtReflectionType*>(result, static_cast<int32_t>(i), type_obj);
        }
        RET_OK(result);
    }

    if (type_sig->ele_type == metadata::RtElementType::GenericInst)
    {
        const metadata::RtGenericInst* generic_inst = type_sig->data.generic_class->class_inst;
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, result,
                                                LEANCLR_NEW_SZARRAY_FROM_ELE_KLASS_INTERNAL(element_klass, generic_inst->generic_arg_count,
                                                                                           "RuntimeTypeHandle_GetInstantiation"));
        for (uint8_t i = 0; i < generic_inst->generic_arg_count; ++i)
        {
            DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionType*, type_obj,
                                                    vm::Reflection::get_type_reflection_object(generic_inst->generic_args[i]));
            vm::Array::set_array_data_at<vm::RtReflectionType*>(result, i, type_obj);
        }
        RET_OK(result);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, result,
                                            LEANCLR_NEW_EMPTY_SZARRAY_BY_ELE_KLASS_INTERNAL(element_klass,
                                                                                           "RuntimeTypeHandle_GetInstantiation"));
    RET_OK(result);
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

RtResult<const metadata::RtMethodInfo*> resolve_module_method(void* qcall_module, void* native_handle, int32_t method_token,
                                                              void** type_inst_args, int32_t type_inst_count, void** method_inst_args,
                                                              int32_t method_inst_count) noexcept
{
    if (type_inst_count < 0 || type_inst_count > static_cast<int32_t>(metadata::RT_MAX_GENERIC_PARAM_COUNT) ||
        method_inst_count < 0 || method_inst_count > static_cast<int32_t>(metadata::RT_MAX_GENERIC_PARAM_COUNT))
    {
        RET_ERR(RtErr::Argument);
    }
    if ((type_inst_count > 0 && type_inst_args == nullptr) || (method_inst_count > 0 && method_inst_args == nullptr))
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtModuleDef*, module, get_module_from_qcall_module(qcall_module, native_handle));

    const metadata::RtTypeSig* type_inst_sigs[metadata::RT_MAX_GENERIC_PARAM_COUNT]{};
    for (int32_t i = 0; i < type_inst_count; ++i)
    {
        if (type_inst_args[i] == nullptr)
        {
            RET_ERR(RtErr::ArgumentNull);
        }
        type_inst_sigs[i] = reinterpret_cast<const metadata::RtTypeSig*>(type_inst_args[i]);
    }

    const metadata::RtTypeSig* method_inst_sigs[metadata::RT_MAX_GENERIC_PARAM_COUNT]{};
    for (int32_t i = 0; i < method_inst_count; ++i)
    {
        if (method_inst_args[i] == nullptr)
        {
            RET_ERR(RtErr::ArgumentNull);
        }
        method_inst_sigs[i] = reinterpret_cast<const metadata::RtTypeSig*>(method_inst_args[i]);
    }

    const metadata::RtGenericInst* class_inst = nullptr;
    if (type_inst_count > 0)
    {
        UNWRAP_OR_RET_ERR_ON_FAIL(class_inst,
                                  metadata::MetadataCache::get_pooled_generic_inst(type_inst_sigs, static_cast<uint8_t>(type_inst_count)));
    }

    const metadata::RtGenericInst* method_inst = nullptr;
    if (method_inst_count > 0)
    {
        UNWRAP_OR_RET_ERR_ON_FAIL(method_inst,
                                  metadata::MetadataCache::get_pooled_generic_inst(method_inst_sigs, static_cast<uint8_t>(method_inst_count)));
    }

    metadata::RtGenericContainerContext gcc{};
    metadata::RtGenericContext gc{class_inst, method_inst};
    metadata::RtToken token = metadata::RtToken::decode(static_cast<metadata::EncodedTokenId>(method_token));
    return module->get_method_by_token(token, gcc, &gc);
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

bool is_method_base_get_current_method_frame(const interp::InterpFrame* frame) noexcept
{
    const metadata::RtMethodInfo* method = frame->method;
    return method != nullptr && method->parent != nullptr && method->parent->namespaze != nullptr && method->parent->name != nullptr &&
           method->name != nullptr && std::strcmp(method->parent->namespaze, "System.Reflection") == 0 &&
           std::strcmp(method->parent->name, "MethodBase") == 0 && std::strcmp(method->name, "GetCurrentMethod") == 0;
}

RtResult<const metadata::RtMethodInfo*> get_current_method_for_stack_mark(vm::RtStackCrawlMark* stack_mark) noexcept
{
    vm::RtStackCrawlMark mark = stack_mark != nullptr ? *stack_mark : vm::RtStackCrawlMark::LookForMyCaller;
    int32_t caller_skip = mark == vm::RtStackCrawlMark::LookForMyCallersCaller ? 1 : 0;

    auto frames = interp::MachineState::get_global_machine_state().get_active_frames();
    for (size_t i = frames.size(); i > 0; --i)
    {
        const interp::InterpFrame* frame = &frames[i - 1];
        if (frame->method == nullptr || is_method_base_get_current_method_frame(frame))
        {
            continue;
        }

        if (caller_skip > 0)
        {
            --caller_skip;
            continue;
        }

        RET_OK(frame->method);
    }

    RET_OK(nullptr);
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

RtResultVoid kernel32_get_environment_variable_ptr_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                           const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    Utf16Char* variable_name = interp::EvalStackOp::get_param<Utf16Char*>(params, 0);
    Utf16Char* value = interp::EvalStackOp::get_param<Utf16Char*>(params, 1);
    uint32_t value_length = interp::EvalStackOp::get_param<uint32_t>(params, 2);

    uint32_t result = platform::RtSys::get_environment_variable(variable_name, value, value_length);
    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

RtResultVoid kernel32_set_environment_variable_ptr_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                           const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    Utf16Char* variable_name = interp::EvalStackOp::get_param<Utf16Char*>(params, 0);
    Utf16Char* value = interp::EvalStackOp::get_param<Utf16Char*>(params, 1);

    int32_t result = platform::RtSys::set_environment_variable(variable_name, value);
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

uint16_t get_system_processor_architecture() noexcept
{
#if defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
    return 9; // PROCESSOR_ARCHITECTURE_AMD64
#elif defined(_M_ARM64) || defined(__aarch64__)
    return 12; // PROCESSOR_ARCHITECTURE_ARM64
#elif defined(_M_IX86) || defined(__i386__)
    return 0; // PROCESSOR_ARCHITECTURE_INTEL
#else
    return 0xffff; // PROCESSOR_ARCHITECTURE_UNKNOWN
#endif
}

RtResultVoid kernel32_get_system_info_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                              const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto info = interp::EvalStackOp::get_param<RtSystemInfo*>(params, 0);
    if (info != nullptr)
    {
        std::memset(info, 0, sizeof(RtSystemInfo));
        info->wProcessorArchitecture = get_system_processor_architecture();
        info->dwPageSize = static_cast<uint32_t>(vm::Environment::get_page_size());
        info->dwActiveProcessorMask = static_cast<uintptr_t>(1);
        int32_t processor_count = vm::Environment::get_processor_count();
        info->dwNumberOfProcessors = static_cast<uint32_t>(processor_count > 0 ? processor_count : 1);
        info->dwAllocationGranularity = 65536;
    }
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

RtResultVoid gc_collect_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                interp::RtStackObject*) noexcept
{
    (void)params;
    // LeanCLR's current managed execution path cannot safely force a collection.
    // Match the .NET 10 contract shape while keeping this smoke-only call non-throwing.
    RET_VOID_OK();
}

RtResultVoid runtime_helpers_run_class_constructor_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                           const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto qcall_type_handle = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, type_sig,
                                            get_type_sig_from_qcall_type_handle(qcall_type_handle, native_handle));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));
    RET_ERR_ON_FAIL(vm::Runtime::run_class_static_constructor(klass));
    RET_VOID_OK();
}

RtResultVoid runtime_helpers_run_module_constructor_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                            const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto qcall_module = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtModuleDef*, module, get_module_from_qcall_module(qcall_module, native_handle));
    RET_ERR_ON_FAIL(vm::Runtime::run_module_static_constructor(module));
    RET_VOID_OK();
}

RtResultVoid method_base_get_current_method_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                    const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    auto stack_mark = interp::EvalStackOp::get_param<vm::RtStackCrawlMark*>(params, 0);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtMethodInfo*, method, get_current_method_for_stack_mark(stack_mark));
    interp::EvalStackOp::set_return(ret, method);
    RET_VOID_OK();
}

RtResultVoid array_create_instance_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                           interp::RtStackObject*) noexcept
{
    auto qcall_type_handle = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    int32_t rank = interp::EvalStackOp::get_param<int32_t>(params, 2);
    auto lengths = interp::EvalStackOp::get_param<int32_t*>(params, 3);
    auto lower_bounds = interp::EvalStackOp::get_param<int32_t*>(params, 4);
    bool from_array_type = interp::EvalStackOp::get_param<int32_t>(params, 5) != 0;
    auto ret_array = interp::EvalStackOp::get_param<vm::RtArray**>(params, 6);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, array,
                                            create_array_instance(qcall_type_handle, native_handle, rank, lengths, lower_bounds, from_array_type));
    *ret_array = array;
    RET_VOID_OK();
}

RtResultVoid enum_get_values_and_names_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                               interp::RtStackObject*) noexcept
{
    auto qcall_type_handle = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto values = interp::EvalStackOp::get_param<vm::RtArray**>(params, 2);
    auto names = interp::EvalStackOp::get_param<vm::RtArray**>(params, 3);
    bool get_names = interp::EvalStackOp::get_param<int32_t>(params, 4) != 0;
    RET_ERR_ON_FAIL(icalls::SystemEnum::get_enum_values_and_names_qcall(qcall_type_handle, native_handle, values, names, get_names));
    RET_VOID_OK();
}

RtResultVoid method_table_can_compare_bits_or_use_fast_get_hash_code_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                                             const interp::RtStackObject* params,
                                                                             interp::RtStackObject* ret) noexcept
{
    auto method_table = interp::EvalStackOp::get_param<const metadata::RtClass*>(params, 0);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, can_compare,
                                            can_compare_bits_or_use_fast_get_hash_code(method_table));
    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(can_compare));
    RET_VOID_OK();
}

RtResultVoid bcrypt_gen_random_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                       interp::RtStackObject* ret) noexcept
{
    intptr_t algo_handle = interp::EvalStackOp::get_param<intptr_t>(params, 0);
    auto buffer = interp::EvalStackOp::get_param<uint8_t*>(params, 1);
    int32_t length = interp::EvalStackOp::get_param<int32_t>(params, 2);
    int32_t flags = interp::EvalStackOp::get_param<int32_t>(params, 3);

    platform::Bcrypt::gen_random(algo_handle, buffer, length, flags);
    interp::EvalStackOp::set_return(ret, static_cast<uint32_t>(0));
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

RtResultVoid ntdll_nt_query_system_information_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                       const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    (void)interp::EvalStackOp::get_param<int32_t>(params, 0);
    void* system_information = interp::EvalStackOp::get_param<void*>(params, 1);
    uint32_t system_information_length = interp::EvalStackOp::get_param<uint32_t>(params, 2);
    uint32_t* return_length = interp::EvalStackOp::get_param<uint32_t*>(params, 3);

    if (system_information != nullptr && system_information_length != 0)
    {
        std::memset(system_information, 0, system_information_length);
    }
    if (return_length != nullptr)
    {
        *return_length = 0;
    }

    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(-1));
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

    const metadata::RtMethodInfo* reflected_method = vm::Delegate::get_target_method(this_delegate);
    if (reflected_method == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }
    if (this_delegate->target != nullptr)
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtMethodInfo*, virtual_method,
                                                vm::Method::get_virtual_method_impl(this_delegate->target, reflected_method));
        reflected_method = virtual_method;
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionMethod*, method_info,
                                            vm::Reflection::get_method_reflection_object(reflected_method, reflected_method->parent));
    *method_info_slot = reinterpret_cast<vm::RtObject*>(method_info);
    RET_VOID_OK();
}

RtResultVoid delegate_bind_to_method_info_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                                  interp::RtStackObject* ret) noexcept
{
    auto delegate_slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 0);
    auto target_slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 1);
    auto method = interp::EvalStackOp::get_param<const metadata::RtMethodInfo*>(params, 2);
    (void)interp::EvalStackOp::get_param<void*>(params, 3);
    (void)interp::EvalStackOp::get_param<void*>(params, 4);
    (void)interp::EvalStackOp::get_param<int32_t>(params, 5);

    if (delegate_slot == nullptr || *delegate_slot == nullptr || method == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    auto multicast_delegate = reinterpret_cast<vm::RtMulticastDelegate*>(*delegate_slot);
    vm::RtObject* target = target_slot != nullptr ? *target_slot : nullptr;
    RET_ERR_ON_FAIL(vm::Delegate::constructor_delegate(multicast_delegate, target, method));
    bool is_virtual_method = !vm::Method::is_devirtualed(method);
    if (is_virtual_method && target != nullptr)
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtMethodInfo*, virtual_method,
                                                vm::Method::get_virtual_method_impl(target, method));
        vm::Delegate::set_target_method(&multicast_delegate->dele, virtual_method);
    }

    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(1));
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

RtResultVoid runtime_type_handle_get_module_slow_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                         const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto runtime_type_slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 0);
    auto module_slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 1);
    if (module_slot == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionModule*, module, get_module_from_runtime_type_slot(runtime_type_slot));
    *module_slot = reinterpret_cast<vm::RtObject*>(module);
    RET_VOID_OK();
}

RtResultVoid runtime_type_handle_get_instantiation_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                           const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto qcall_type_handle = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto types_slot = interp::EvalStackOp::get_param<vm::RtArray**>(params, 2);
    bool runtime_array = interp::EvalStackOp::get_param<int32_t>(params, 3) != 0;

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, instantiation,
                                            get_type_instantiation(qcall_type_handle, native_handle, runtime_array));
    if (types_slot != nullptr)
    {
        *types_slot = instantiation;
    }
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

RtResultVoid assembly_get_type_core_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                            interp::RtStackObject*) noexcept
{
    auto qcall_assembly = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto type_name = interp::EvalStackOp::get_param<const char*>(params, 2);
    auto nested_type_names = interp::EvalStackOp::get_param<void**>(params, 3);
    int32_t nested_type_names_length = interp::EvalStackOp::get_param<int32_t>(params, 4);
    auto ret_type = interp::EvalStackOp::get_param<vm::RtReflectionType**>(params, 5);
    if (ret_type == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtAssembly*, assembly,
                                            get_assembly_from_qcall_assembly(qcall_assembly, native_handle));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(
        vm::RtReflectionType*, type, get_runtime_assembly_type_core(assembly, type_name, nested_type_names, nested_type_names_length, false));
    *ret_type = type;
    RET_VOID_OK();
}

RtResultVoid assembly_get_type_core_ignore_case_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                        const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto qcall_assembly = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto type_name = interp::EvalStackOp::get_param<const Utf16Char*>(params, 2);
    auto nested_type_names = interp::EvalStackOp::get_param<void**>(params, 3);
    int32_t nested_type_names_length = interp::EvalStackOp::get_param<int32_t>(params, 4);
    auto ret_type = interp::EvalStackOp::get_param<vm::RtReflectionType**>(params, 5);
    if (ret_type == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtAssembly*, assembly,
                                            get_assembly_from_qcall_assembly(qcall_assembly, native_handle));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionType*, type,
                                            get_runtime_assembly_type_core_ignore_case(assembly, type_name, nested_type_names,
                                                                                       nested_type_names_length));
    *ret_type = type;
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

RtResultVoid module_handle_resolve_method_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                                  interp::RtStackObject* ret) noexcept
{
    auto qcall_module = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    int32_t method_token = interp::EvalStackOp::get_param<int32_t>(params, 2);
    auto type_inst_args = interp::EvalStackOp::get_param<void**>(params, 3);
    int32_t type_inst_count = interp::EvalStackOp::get_param<int32_t>(params, 4);
    auto method_inst_args = interp::EvalStackOp::get_param<void**>(params, 5);
    int32_t method_inst_count = interp::EvalStackOp::get_param<int32_t>(params, 6);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtMethodInfo*, method,
                                            resolve_module_method(qcall_module, native_handle, method_token, type_inst_args, type_inst_count,
                                                                  method_inst_args, method_inst_count));
    interp::EvalStackOp::set_return(ret, method);
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

RtResultVoid runtime_type_handle_internal_alloc_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                        const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto method_table = interp::EvalStackOp::get_param<void*>(params, 0);
    auto result_slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 1);
    if (result_slot == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, type_sig,
                                            get_type_sig_from_qcall_type_handle(method_table, method_table));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));
    RET_ERR_ON_FAIL(vm::Class::initialize_all(klass));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, obj,
                                            LEANCLR_NEWOBJ_INTERNAL(klass, "RuntimeTypeHandle_InternalAlloc"));
    *result_slot = obj;
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

RtResultVoid runtime_method_handle_get_method_body_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                           const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto method = interp::EvalStackOp::get_param<const metadata::RtMethodInfo*>(params, 0);
    (void)interp::EvalStackOp::get_param<void*>(params, 1);
    (void)interp::EvalStackOp::get_param<void*>(params, 2);
    auto result = interp::EvalStackOp::get_param<vm::RtReflectionMethodBody**>(params, 3);

    if (result == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionMethodBody*, body, vm::Method::create_reflection_method_body(method));
    *result = body;
    RET_VOID_OK();
}

RtResultVoid runtime_method_handle_invoke_method_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                         const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto target_slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 0);
    auto args = interp::EvalStackOp::get_param<void**>(params, 1);
    auto signature_slot = interp::EvalStackOp::get_param<vm::RtSignature**>(params, 2);
    bool is_constructor = interp::EvalStackOp::get_param<int32_t>(params, 3) != 0;
    auto result_slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 4);

    if (signature_slot == nullptr || *signature_slot == nullptr || result_slot == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    vm::RtSignature* signature = *signature_slot;
    const metadata::RtMethodInfo* method = signature->method;
    if (method == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }
    if (is_constructor || method->parameter_count != 0 || args != nullptr)
    {
        RET_ERR(RtErr::NotSupported);
    }

    vm::RtObject* exception = nullptr;
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, result,
                                            vm::Reflection::invoke_method(method, target_slot != nullptr ? *target_slot : nullptr, nullptr, &exception));
    if (exception != nullptr)
    {
        vm::Exception::set_current_exception(reinterpret_cast<vm::RtException*>(exception));
        RET_ERR(RtErr::ManagedException);
    }

    *result_slot = result;
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
    vm::PInvokes::register_pinvoke("Interop/Kernel32::<GetEnvironmentVariable>g____PInvoke|296_0(System.UInt16*,System.Char*,System.UInt32)",
                                   nullptr, kernel32_get_environment_variable_ptr_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::<GetEnvironmentVariable>g____PInvoke|296_0", nullptr,
                                   kernel32_get_environment_variable_ptr_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::GetEnvironmentVariable(System.String,System.Char&,System.UInt32)", nullptr,
                                   kernel32_get_environment_variable_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::GetEnvironmentVariable", nullptr, kernel32_get_environment_variable_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::<GetEnvironmentVariable>g____PInvoke|296_0(System.String,System.Char&,System.UInt32)", nullptr,
                                   kernel32_get_environment_variable_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::<GetEnvironmentVariable>g____PInvoke|296_0(System.UInt16*,System.Char*,System.UInt32)", nullptr,
                                   kernel32_get_environment_variable_ptr_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::<GetEnvironmentVariable>g____PInvoke|296_0", nullptr,
                                   kernel32_get_environment_variable_ptr_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::<GetEnvironmentVariable>g____PInvoke|296_0(System.UInt16*,System.Char*,System.UInt32)", nullptr,
                                   kernel32_get_environment_variable_ptr_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::<GetEnvironmentVariable>g____PInvoke|296_0", nullptr,
                                   kernel32_get_environment_variable_ptr_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::<SetEnvironmentVariable>g____PInvoke|316_0(System.UInt16*,System.UInt16*)",
                                   nullptr, kernel32_set_environment_variable_ptr_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::<SetEnvironmentVariable>g____PInvoke|316_0", nullptr,
                                   kernel32_set_environment_variable_ptr_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::<SetEnvironmentVariable>g____PInvoke|316_0(System.UInt16*,System.UInt16*)",
                                   nullptr, kernel32_set_environment_variable_ptr_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::<SetEnvironmentVariable>g____PInvoke|316_0", nullptr,
                                   kernel32_set_environment_variable_ptr_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::<SetEnvironmentVariable>g____PInvoke|316_0(System.UInt16*,System.UInt16*)",
                                   nullptr, kernel32_set_environment_variable_ptr_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::<SetEnvironmentVariable>g____PInvoke|316_0", nullptr,
                                   kernel32_set_environment_variable_ptr_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::SetEnvironmentVariable(System.UInt16*,System.UInt16*)",
                                   nullptr, kernel32_set_environment_variable_ptr_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::SetEnvironmentVariable", nullptr,
                                   kernel32_set_environment_variable_ptr_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::SetEnvironmentVariable(System.UInt16*,System.UInt16*)", nullptr,
                                   kernel32_set_environment_variable_ptr_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::SetEnvironmentVariable", nullptr,
                                   kernel32_set_environment_variable_ptr_invoker);
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
    vm::PInvokes::register_pinvoke("Interop/Kernel32::GetSystemInfo(Interop/Kernel32/SYSTEM_INFO*)", nullptr,
                                   kernel32_get_system_info_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::GetSystemInfo", nullptr, kernel32_get_system_info_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::GetSystemInfo(Interop/Kernel32/SYSTEM_INFO*)", nullptr,
                                   kernel32_get_system_info_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::GetSystemInfo", nullptr, kernel32_get_system_info_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::GetSystemInfo(Interop/Kernel32/SYSTEM_INFO*)", nullptr,
                                   kernel32_get_system_info_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::GetSystemInfo", nullptr, kernel32_get_system_info_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::GetNativeSystemInfo(Interop/Kernel32/SYSTEM_INFO*)", nullptr,
                                   kernel32_get_system_info_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::GetNativeSystemInfo", nullptr, kernel32_get_system_info_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::GetNativeSystemInfo(Interop/Kernel32/SYSTEM_INFO*)", nullptr,
                                   kernel32_get_system_info_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::GetNativeSystemInfo", nullptr, kernel32_get_system_info_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::GetNativeSystemInfo(Interop/Kernel32/SYSTEM_INFO*)", nullptr,
                                   kernel32_get_system_info_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::GetNativeSystemInfo", nullptr, kernel32_get_system_info_invoker);
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
    vm::PInvokes::register_pinvoke("System.GC::<_Collect>g____PInvoke|8_0(System.Int32,System.Int32,System.Byte)", nullptr,
                                   gc_collect_invoker);
    vm::PInvokes::register_pinvoke("System.GC::<_Collect>g____PInvoke|8_0", nullptr, gc_collect_invoker);
    vm::PInvokes::register_pinvoke("System.GC::_Collect", nullptr, gc_collect_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Runtime.CompilerServices.RuntimeHelpers::RunClassConstructor(System.Runtime.CompilerServices.QCallTypeHandle)", nullptr,
        runtime_helpers_run_class_constructor_invoker);
    vm::PInvokes::register_pinvoke("System.Runtime.CompilerServices.RuntimeHelpers::RunClassConstructor", nullptr,
                                   runtime_helpers_run_class_constructor_invoker);
    vm::PInvokes::register_pinvoke("ReflectionInvocation_RunClassConstructor", nullptr,
                                   runtime_helpers_run_class_constructor_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Runtime.CompilerServices.RuntimeHelpers::RunModuleConstructor(System.Runtime.CompilerServices.QCallModule)", nullptr,
        runtime_helpers_run_module_constructor_invoker);
    vm::PInvokes::register_pinvoke("System.Runtime.CompilerServices.RuntimeHelpers::RunModuleConstructor", nullptr,
                                   runtime_helpers_run_module_constructor_invoker);
    vm::PInvokes::register_pinvoke("ReflectionInvocation_RunModuleConstructor", nullptr,
                                   runtime_helpers_run_module_constructor_invoker);
    vm::PInvokes::register_pinvoke("MethodBase_GetCurrentMethod", nullptr, method_base_get_current_method_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.MethodBase::GetCurrentMethod(System.Runtime.CompilerServices.StackCrawlMarkHandle)", nullptr,
                                   method_base_get_current_method_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.MethodBase::GetCurrentMethod", nullptr, method_base_get_current_method_invoker);
    vm::PInvokes::register_pinvoke("Array_CreateInstance", nullptr, array_create_instance_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Array::<InternalCreate>g____PInvoke|0_0(System.Runtime.CompilerServices.QCallTypeHandle,System.Int32,System.Int32*,System.Int32*,System.Boolean,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, array_create_instance_invoker);
    vm::PInvokes::register_pinvoke("System.Array::<InternalCreate>g____PInvoke|0_0", nullptr, array_create_instance_invoker);
    vm::PInvokes::register_pinvoke("System.Array::InternalCreate", nullptr, array_create_instance_invoker);
    vm::PInvokes::register_pinvoke("Enum_GetValuesAndNames", nullptr, enum_get_values_and_names_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Enum::GetEnumValuesAndNames(System.Runtime.CompilerServices.QCallTypeHandle,System.Runtime.CompilerServices.ObjectHandleOnStack,System.Runtime.CompilerServices.ObjectHandleOnStack,System.Int32)",
        nullptr, enum_get_values_and_names_invoker);
    vm::PInvokes::register_pinvoke("System.Enum::GetEnumValuesAndNames", nullptr, enum_get_values_and_names_invoker);
    vm::PInvokes::register_pinvoke("MethodTable_CanCompareBitsOrUseFastGetHashCode", nullptr,
                                   method_table_can_compare_bits_or_use_fast_get_hash_code_invoker);
    vm::PInvokes::register_pinvoke(
        "System.ValueType::<CanCompareBitsOrUseFastGetHashCodeHelper>g____PInvoke|2_0(System.Runtime.CompilerServices.MethodTable*)",
        nullptr, method_table_can_compare_bits_or_use_fast_get_hash_code_invoker);
    vm::PInvokes::register_pinvoke("System.ValueType::<CanCompareBitsOrUseFastGetHashCodeHelper>g____PInvoke|2_0", nullptr,
                                   method_table_can_compare_bits_or_use_fast_get_hash_code_invoker);
    vm::PInvokes::register_pinvoke("BCrypt::BCryptGenRandom(System.IntPtr,System.Byte*,System.Int32,System.Int32)", nullptr,
                                   bcrypt_gen_random_invoker);
    vm::PInvokes::register_pinvoke("BCrypt::BCryptGenRandom", nullptr, bcrypt_gen_random_invoker);
    vm::PInvokes::register_pinvoke("Interop/BCrypt::BCryptGenRandom(System.IntPtr,System.Byte*,System.Int32,System.Int32)", nullptr,
                                   bcrypt_gen_random_invoker);
    vm::PInvokes::register_pinvoke("Interop/BCrypt::BCryptGenRandom", nullptr, bcrypt_gen_random_invoker);
    vm::PInvokes::register_pinvoke("Interop/NtDll::<RtlGetVersion>g____PInvoke|22_0", nullptr, ntdll_rtl_get_version_invoker);
    vm::PInvokes::register_pinvoke("NtDll::<RtlGetVersion>g____PInvoke|22_0", nullptr, ntdll_rtl_get_version_invoker);
    vm::PInvokes::register_pinvoke("Interop/NtDll::RtlGetVersion", nullptr, ntdll_rtl_get_version_invoker);
    vm::PInvokes::register_pinvoke("NtDll::RtlGetVersion", nullptr, ntdll_rtl_get_version_invoker);
    vm::PInvokes::register_pinvoke("Interop/NtDll::NtQuerySystemInformation(System.Int32,System.Void*,System.UInt32,System.UInt32*)",
                                   nullptr, ntdll_nt_query_system_information_invoker);
    vm::PInvokes::register_pinvoke("Interop/NtDll::NtQuerySystemInformation", nullptr,
                                   ntdll_nt_query_system_information_invoker);
    vm::PInvokes::register_pinvoke("NtDll::NtQuerySystemInformation(System.Int32,System.Void*,System.UInt32,System.UInt32*)", nullptr,
                                   ntdll_nt_query_system_information_invoker);
    vm::PInvokes::register_pinvoke("NtDll::NtQuerySystemInformation", nullptr, ntdll_nt_query_system_information_invoker);
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
    vm::PInvokes::register_pinvoke("System.Delegate::<BindToMethodInfo>g____PInvoke|21_0", nullptr,
                                   delegate_bind_to_method_info_invoker);
    vm::PInvokes::register_pinvoke("System.Delegate::BindToMethodInfo", nullptr, delegate_bind_to_method_info_invoker);
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
        "System.RuntimeTypeHandle::GetModuleSlow(System.Runtime.CompilerServices.ObjectHandleOnStack,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, runtime_type_handle_get_module_slow_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeTypeHandle::GetModuleSlow", nullptr, runtime_type_handle_get_module_slow_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeTypeHandle::GetInstantiation(System.Runtime.CompilerServices.QCallTypeHandle,System.Runtime.CompilerServices.ObjectHandleOnStack,System.Int32)",
        nullptr, runtime_type_handle_get_instantiation_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeTypeHandle::GetInstantiation", nullptr, runtime_type_handle_get_instantiation_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.RuntimeModule::GetTypes(System.Runtime.CompilerServices.QCallModule,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, get_module_types_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.RuntimeModule::GetTypes", nullptr, get_module_types_invoker);
    vm::PInvokes::register_pinvoke("AssemblyNative_GetTypeCore", nullptr, assembly_get_type_core_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.RuntimeAssembly::<GetTypeCore>g____PInvoke|25_0(System.Runtime.CompilerServices.QCallAssembly,System.Byte*,System.IntPtr*,System.Int32,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, assembly_get_type_core_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.RuntimeAssembly::<GetTypeCore>g____PInvoke|25_0", nullptr,
                                   assembly_get_type_core_invoker);
    vm::PInvokes::register_pinvoke("AssemblyNative_GetTypeCoreIgnoreCase", nullptr, assembly_get_type_core_ignore_case_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.RuntimeAssembly::<GetTypeCoreIgnoreCase>g____PInvoke|26_0(System.Runtime.CompilerServices.QCallAssembly,System.UInt16*,System.IntPtr*,System.Int32,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, assembly_get_type_core_ignore_case_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.RuntimeAssembly::<GetTypeCoreIgnoreCase>g____PInvoke|26_0", nullptr,
                                   assembly_get_type_core_ignore_case_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.MetadataImport::<Enum>g____PInvoke|8_0(System.IntPtr,System.Int32,System.Int32,System.Int32*,System.Int32*,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, metadata_import_enum_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.MetadataImport::<Enum>g____PInvoke|8_0", nullptr, metadata_import_enum_invoker);
    vm::PInvokes::register_pinvoke(
        "System.ModuleHandle::ResolveMethod(System.Runtime.CompilerServices.QCallModule,System.Int32,System.IntPtr*,System.Int32,System.IntPtr*,System.Int32)",
        nullptr, module_handle_resolve_method_invoker);
    vm::PInvokes::register_pinvoke("System.ModuleHandle::ResolveMethod", nullptr, module_handle_resolve_method_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeTypeHandle::CreateInstanceForAnotherGenericParameter(System.Runtime.CompilerServices.QCallTypeHandle,System.IntPtr*,System.Int32,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, create_instance_for_another_generic_parameter_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeTypeHandle::CreateInstanceForAnotherGenericParameter", nullptr,
                                   create_instance_for_another_generic_parameter_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeTypeHandle::InternalAlloc(System.Runtime.CompilerServices.MethodTable*,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, runtime_type_handle_internal_alloc_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeTypeHandle::InternalAlloc", nullptr, runtime_type_handle_internal_alloc_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Signature::Init(System.Runtime.CompilerServices.ObjectHandleOnStack,System.Void*,System.Int32,System.RuntimeFieldHandleInternal,System.RuntimeMethodHandleInternal)",
        nullptr, signature_init_invoker);
    vm::PInvokes::register_pinvoke("System.Signature::Init", nullptr, signature_init_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeMethodHandle::GetMethodBody(System.RuntimeMethodHandleInternal,System.Runtime.CompilerServices.QCallTypeHandle,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, runtime_method_handle_get_method_body_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeMethodHandle::GetMethodBody", nullptr, runtime_method_handle_get_method_body_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeMethodHandle::InvokeMethod(System.Runtime.CompilerServices.ObjectHandleOnStack,System.Void**,System.Runtime.CompilerServices.ObjectHandleOnStack,System.Boolean,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, runtime_method_handle_invoke_method_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeMethodHandle::InvokeMethod", nullptr, runtime_method_handle_invoke_method_invoker);
    vm::PInvokes::register_pinvoke("RuntimeMethodHandle_InvokeMethod", nullptr, runtime_method_handle_invoke_method_invoker);
}

} // namespace pinvokes
} // namespace leanclr

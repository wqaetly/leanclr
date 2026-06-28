#include "coreclr_qcall.h"

#include <cstring>
#include <limits>

#include "alloc/general_allocation.h"
#include "gc/gc_roots.h"
#include "icalls/system_enum.h"
#include "icalls/system_runtimemethodhandle.h"
#include "interp/eval_stack_op.h"
#include "interp/machine_state.h"
#include "metadata/metadata_name.h"
#include "metadata/metadata_cache.h"
#include "metadata/module_def.h"
#include "platform/bcrypt.h"
#include "platform/kernel32.h"
#include "platform/rt_file.h"
#include "platform/rt_sys.h"
#include "utils/rt_vector.h"
#include "utils/string_builder.h"
#include "vm/assembly.h"
#include "vm/array_class.h"
#include "vm/class.h"
#include "vm/customattribute.h"
#include "vm/delegate.h"
#include "vm/environment.h"
#include "vm/field.h"
#include "vm/generic_method.h"
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
#include "vm/stacktrace.h"
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

static uint8_t fold_ascii_case(uint8_t value) noexcept
{
    return value >= static_cast<uint8_t>('A') && value <= static_cast<uint8_t>('Z')
        ? static_cast<uint8_t>(value + ('a' - 'A'))
        : value;
}

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

class RtObjectSlotRootGuard
{
  public:
    ~RtObjectSlotRootGuard()
    {
        for (size_t i = 0; i < slots_.size(); ++i)
        {
            gc::GcRoots::unregister_slot(slots_[i]);
        }
    }

    void register_slot(vm::RtObject** slot)
    {
        if (slot != nullptr)
        {
            gc::GcRoots::register_slot(slot);
            slots_.push_back(slot);
        }
    }

  private:
    utils::Vector<vm::RtObject**> slots_;
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

struct RtIntPtrSpan
{
    void** pointer;
    int32_t length;
};

struct NativeAssemblyNameParts
{
    const Utf16Char* name;
    uint16_t major;
    uint16_t minor;
    uint16_t build;
    uint16_t revision;
    const Utf16Char* culture_name;
    const uint8_t* public_key_or_token;
    int32_t public_key_or_token_length;
    uint32_t flags;
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

RtResult<vm::RtArray*> create_array_instance(void* qcall_type_handle, void* native_handle, int32_t rank, int32_t* lengths,
                                             int32_t* lower_bounds, bool from_array_type) noexcept
{
    if (rank <= 0 || rank > static_cast<int32_t>(metadata::RT_MAX_ARRAY_RANK) || lengths == nullptr)
    {
        RET_ERR(RtErr::Argument);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, type_sig,
                                            vm::Reflection::get_type_sig_from_qcall_type_handle(qcall_type_handle, native_handle));
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
                                            vm::Reflection::get_type_sig_from_qcall_type_handle(qcall_type_handle, native_handle));

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
                                            vm::Reflection::get_type_sig_from_qcall_type_handle(type_handle, type_handle));
    RET_OK(static_cast<int32_t>(type_sig->ele_type));
}

RtResult<vm::RtArray*> get_module_types(void* qcall_module, void* native_handle) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtModuleDef*, module, vm::Reflection::get_module_from_qcall_module(qcall_module, native_handle));
    return vm::Assembly::get_types(module->get_assembly(), false);
}

RtResult<int32_t> get_module_md_stream_version(void* qcall_module, void* native_handle) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtModuleDef*, module, vm::Reflection::get_module_from_qcall_module(qcall_module, native_handle));
    (void)module;
    RET_OK(0);
}

RtResult<vm::RtArray*> get_assembly_modules(void* qcall_assembly, void* native_handle) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtAssembly*, assembly, vm::Reflection::get_assembly_from_qcall_assembly(qcall_assembly, native_handle));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(
        vm::RtArray*, module_array,
        LEANCLR_NEW_SZARRAY_FROM_ELE_KLASS_INTERNAL(vm::Class::get_corlib_types().cls_reflection_module, 1, "RuntimeAssembly_GetModules"));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionModule*, module, vm::Reflection::get_module_reflection_object(assembly->mod));
    vm::Array::set_array_data_at<vm::RtReflectionModule*>(module_array, 0, module);
    RET_OK(module_array);
}

RtResult<vm::RtString*> get_assembly_full_name(void* qcall_assembly, void* native_handle) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtAssembly*, assembly, vm::Reflection::get_assembly_from_qcall_assembly(qcall_assembly, native_handle));
    if (assembly->mod == nullptr)
    {
        RET_ERR(RtErr::BadImageFormat);
    }

    utils::Utf8StringBuilder full_name;
    metadata::MetadataName::append_assembly_name(full_name, assembly->mod->get_assembly_name());
    RET_OK(vm::String::create_string_from_utf8chars(full_name.get_const_chars(), static_cast<int32_t>(full_name.length())));
}

RtResult<vm::RtString*> get_assembly_image_runtime_version(void* qcall_assembly, void* native_handle) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtAssembly*, assembly, vm::Reflection::get_assembly_from_qcall_assembly(qcall_assembly, native_handle));
    if (assembly->mod == nullptr)
    {
        RET_ERR(RtErr::BadImageFormat);
    }

    RET_OK(vm::String::create_string_from_utf8cstr("v4.0.30319"));
}

RtResult<vm::RtReflectionMethod*> get_assembly_entry_point(void* qcall_assembly, void* native_handle) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtAssembly*, assembly, vm::Reflection::get_assembly_from_qcall_assembly(qcall_assembly, native_handle));
    if (assembly->mod == nullptr)
    {
        RET_ERR(RtErr::BadImageFormat);
    }

    metadata::EncodedTokenId entrypoint_token = assembly->mod->get_entrypoint_token();
    if (entrypoint_token == 0)
    {
        RET_OK(nullptr);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtMethodInfo*, method,
                                            assembly->mod->get_method_by_rid(metadata::RtToken::decode_rid(entrypoint_token)));
    return vm::Reflection::get_method_reflection_object(method, method->parent);
}

RtResult<vm::RtArray*> get_assembly_manifest_resource_names(void* qcall_assembly, void* native_handle) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtAssembly*, assembly, vm::Reflection::get_assembly_from_qcall_assembly(qcall_assembly, native_handle));
    if (assembly->mod == nullptr)
    {
        RET_ERR(RtErr::BadImageFormat);
    }

    return LEANCLR_NEW_EMPTY_SZARRAY_BY_ELE_KLASS_INTERNAL(
        vm::Class::get_corlib_types().cls_string, "RuntimeAssembly_GetManifestResourceNames");
}

RtResult<const metadata::RtAssemblyName*> get_qcall_assembly_name(void* qcall_assembly, void* native_handle) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtAssembly*, assembly, vm::Reflection::get_assembly_from_qcall_assembly(qcall_assembly, native_handle));
    if (assembly->mod == nullptr)
    {
        RET_ERR(RtErr::BadImageFormat);
    }

    RET_OK(&assembly->mod->get_assembly_name());
}

RtResult<vm::RtArray*> get_assembly_referenced_assemblies(void* qcall_assembly, void* native_handle) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtAssembly*, assembly, vm::Reflection::get_assembly_from_qcall_assembly(qcall_assembly, native_handle));
    if (assembly->mod == nullptr)
    {
        RET_ERR(RtErr::BadImageFormat);
    }

    utils::Vector<metadata::RtAssembly*> referenced_assemblies;
    RET_ERR_ON_FAIL(assembly->mod->get_reference_assemblies(referenced_assemblies));

    metadata::RtModuleDef* corlib = metadata::RtModuleDef::get_corlib_module();
    if (corlib == nullptr)
    {
        RET_ERR(RtErr::BadImageFormat);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, assembly_name_klass,
                                            corlib->get_class_by_name("System.Reflection.AssemblyName", false, true));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(
        vm::RtArray*, result,
        LEANCLR_NEW_SZARRAY_FROM_ELE_KLASS_INTERNAL(assembly_name_klass, static_cast<int32_t>(referenced_assemblies.size()),
                                                   "RuntimeAssembly_GetReferencedAssemblies"));

    for (size_t i = 0; i < referenced_assemblies.size(); ++i)
    {
        metadata::RtAssembly* referenced_assembly = referenced_assemblies[i];
        if (referenced_assembly == nullptr || referenced_assembly->mod == nullptr)
        {
            RET_ERR(RtErr::BadImageFormat);
        }

        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, assembly_name,
                                                vm::Reflection::create_runtime_assembly_name_object(
                                                    referenced_assembly->mod->get_assembly_name()));
        vm::Array::set_array_data_at<vm::RtObject*>(result, static_cast<int32_t>(i), assembly_name);
    }

    RET_OK(result);
}

RtResult<vm::RtString*> get_assembly_simple_name(void* qcall_assembly, void* native_handle) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtAssemblyName*, assembly_name,
                                            get_qcall_assembly_name(qcall_assembly, native_handle));
    RET_OK(vm::String::create_string_from_utf8cstr(assembly_name->name));
}

RtResult<vm::RtString*> get_assembly_locale(void* qcall_assembly, void* native_handle) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtAssemblyName*, assembly_name,
                                            get_qcall_assembly_name(qcall_assembly, native_handle));
    if (assembly_name->culture == nullptr || assembly_name->culture[0] == '\0')
    {
        RET_OK(nullptr);
    }

    RET_OK(vm::String::create_string_from_utf8cstr(assembly_name->culture));
}

RtResult<vm::RtArray*> get_assembly_public_key(void* qcall_assembly, void* native_handle) noexcept
{
    (void)qcall_assembly;
    (void)native_handle;
    RET_OK(nullptr);
}

RtResult<vm::RtReflectionAssembly*> load_runtime_assembly(const NativeAssemblyNameParts* name_parts, bool throw_on_file_not_found) noexcept
{
    if (name_parts == nullptr || name_parts->name == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    utils::Utf8StringBuilder assembly_name(name_parts->name);
    assembly_name.sure_null_terminator_but_not_append();
    if (assembly_name.length() == 0)
    {
        RET_ERR(RtErr::Argument);
    }

    auto loaded = vm::Assembly::load_by_name(assembly_name.get_const_chars());
    if (loaded.is_err())
    {
        if (!throw_on_file_not_found)
        {
            RET_OK(nullptr);
        }
        RET_ERR(loaded.unwrap_err());
    }

    return vm::Reflection::get_assembly_reflection_object(loaded.unwrap());
}

static bool is_same_or_nested_within(const metadata::RtClass* klass, const metadata::RtClass* enclosing) noexcept
{
    for (const metadata::RtClass* current = klass; current != nullptr; current = current->declaring_class)
    {
        if (current == enclosing)
        {
            return true;
        }
    }
    return false;
}

static RtResult<bool> is_subclass_or_same(const metadata::RtClass* klass, const metadata::RtClass* parent) noexcept
{
    if (klass == nullptr || parent == nullptr)
    {
        RET_OK(false);
    }

    RET_ERR_ON_FAIL(vm::Class::initialize_super_types(const_cast<metadata::RtClass*>(klass)));
    RET_ERR_ON_FAIL(vm::Class::initialize_super_types(const_cast<metadata::RtClass*>(parent)));
    RET_OK(vm::Class::is_subclass_of_initialized(klass, parent, false));
}

static RtResult<bool> is_class_visible_from_source(const metadata::RtClass* klass, const metadata::RtClass* source_klass,
                                                   const metadata::RtModuleDef* source_module) noexcept
{
    if (klass == nullptr)
    {
        RET_OK(false);
    }

    const metadata::RtClass* declaring_class = klass->declaring_class;
    bool same_module = source_module != nullptr && klass->image == source_module;
    uint32_t visibility = klass->flags & static_cast<uint32_t>(metadata::RtTypeAttribute::VisibilityMask);

    switch (static_cast<metadata::RtTypeAttribute>(visibility))
    {
    case metadata::RtTypeAttribute::Public:
        RET_OK(true);
    case metadata::RtTypeAttribute::NotPublic:
        RET_OK(same_module);
    case metadata::RtTypeAttribute::NestedPublic:
        if (declaring_class == nullptr)
        {
            RET_OK(true);
        }
        return is_class_visible_from_source(declaring_class, source_klass, source_module);
    case metadata::RtTypeAttribute::NestedPrivate:
        RET_OK(same_module && source_klass != nullptr && is_same_or_nested_within(source_klass, declaring_class));
    case metadata::RtTypeAttribute::NestedAssembly:
        if (!same_module)
        {
            RET_OK(false);
        }
        return declaring_class != nullptr ? is_class_visible_from_source(declaring_class, source_klass, source_module)
                                          : RtResult<bool>::Ok(true);
    case metadata::RtTypeAttribute::NestedFamily:
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, family_visible, is_subclass_or_same(source_klass, declaring_class));
        if (!family_visible)
        {
            RET_OK(false);
        }
        return declaring_class != nullptr ? is_class_visible_from_source(declaring_class, source_klass, source_module)
                                          : RtResult<bool>::Ok(true);
    }
    case metadata::RtTypeAttribute::NestedFamAndAssem:
    {
        if (!same_module)
        {
            RET_OK(false);
        }
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, family_visible, is_subclass_or_same(source_klass, declaring_class));
        if (!family_visible)
        {
            RET_OK(false);
        }
        return declaring_class != nullptr ? is_class_visible_from_source(declaring_class, source_klass, source_module)
                                          : RtResult<bool>::Ok(true);
    }
    case metadata::RtTypeAttribute::NestedFamOrAssem:
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, family_visible, is_subclass_or_same(source_klass, declaring_class));
        if (!same_module && !family_visible)
        {
            RET_OK(false);
        }
        return declaring_class != nullptr ? is_class_visible_from_source(declaring_class, source_klass, source_module)
                                          : RtResult<bool>::Ok(true);
    }
    default:
        RET_OK(false);
    }
}

static RtResult<bool> is_method_visible_from_source(const metadata::RtMethodInfo* method, const metadata::RtClass* source_klass,
                                                    const metadata::RtModuleDef* source_module) noexcept
{
    if (method == nullptr || method->parent == nullptr)
    {
        RET_OK(false);
    }

    uint16_t access = method->flags & static_cast<uint16_t>(metadata::RtMethodAttribute::MemberAccessMask);
    bool same_module = source_module != nullptr && method->parent->image == source_module;

    switch (static_cast<metadata::RtMethodAttribute>(access))
    {
    case metadata::RtMethodAttribute::Public:
        RET_OK(true);
    case metadata::RtMethodAttribute::Private:
        RET_OK(source_klass != nullptr && is_same_or_nested_within(source_klass, method->parent));
    case metadata::RtMethodAttribute::Assembly:
        RET_OK(same_module);
    case metadata::RtMethodAttribute::Family:
        return is_subclass_or_same(source_klass, method->parent);
    case metadata::RtMethodAttribute::FamAndAssem:
    {
        if (!same_module)
        {
            RET_OK(false);
        }
        return is_subclass_or_same(source_klass, method->parent);
    }
    case metadata::RtMethodAttribute::FamOrAssem:
    {
        if (same_module)
        {
            RET_OK(true);
        }
        return is_subclass_or_same(source_klass, method->parent);
    }
    default:
        RET_OK(false);
    }
}

static RtResult<bool> is_ca_visible_from_decorated_type(void* attr_qcall_type_handle, void* attr_native_handle,
                                                       const metadata::RtMethodInfo* attr_ctor, void* source_qcall_type_handle,
                                                       void* source_native_handle, void* source_qcall_module,
                                                       void* source_native_module) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, attr_type_sig,
                                            vm::Reflection::get_type_sig_from_qcall_type_handle(attr_qcall_type_handle, attr_native_handle));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, attr_klass, vm::Class::get_class_from_typesig(attr_type_sig));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtModuleDef*, source_module,
                                            vm::Reflection::get_module_from_qcall_module(source_qcall_module, source_native_module));

    const metadata::RtClass* source_klass = nullptr;
    if (source_qcall_type_handle != nullptr || source_native_handle != nullptr)
    {
        auto source_type_sig_result = vm::Reflection::get_type_sig_from_qcall_type_handle(source_qcall_type_handle, source_native_handle);
        if (source_type_sig_result.is_ok())
        {
            auto source_klass_result = vm::Class::get_class_from_typesig(source_type_sig_result.unwrap());
            if (source_klass_result.is_ok())
            {
                source_klass = source_klass_result.unwrap();
            }
        }
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, type_visible,
                                            is_class_visible_from_source(attr_klass, source_klass, source_module));
    if (!type_visible)
    {
        RET_OK(false);
    }

    return is_method_visible_from_source(attr_ctor, source_klass, source_module);
}

static RtResult<metadata::RtCustomAttributeRawData> find_custom_attribute_raw_data_by_blob(metadata::RtModuleDef* module,
                                                                                          const metadata::RtMethodInfo* ctor,
                                                                                          const uint8_t* blob_start,
                                                                                          const uint8_t* blob_end) noexcept
{
    if (module == nullptr || ctor == nullptr || blob_start == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    uint32_t custom_attribute_count = module->get_table_row_num(metadata::TableType::CustomAttribute);
    for (uint32_t rid = 1; rid <= custom_attribute_count; ++rid)
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtCustomAttributeRawData, raw_data,
                                                module->get_custom_attribute_raw_data(rid));
        if (raw_data.ctor != ctor)
        {
            continue;
        }

        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL3(utils::BinaryReader, reader,
                                                 module->get_decoded_blob_reader(raw_data.dataBlobIndex));
        const uint8_t* data = reader.data();
        const uint8_t* data_end = data + reader.length();
        if (data == blob_start && (blob_end == nullptr || data_end == blob_end))
        {
            RET_OK(raw_data);
        }
    }

    RET_ERR(RtErr::BadImageFormat);
}

static RtResult<vm::RtObject*> create_custom_attribute_instance(metadata::RtModuleDef* module, vm::RtObject* ctor_object,
                                                               void** blob_start, void* blob_end, int32_t* named_arg_count) noexcept
{
    if (blob_start == nullptr || *blob_start == nullptr || named_arg_count == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtMethodInfo*, ctor,
                                            vm::Reflection::get_method_info_from_handle_arg(ctor_object));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtCustomAttributeRawData, raw_data,
                                            find_custom_attribute_raw_data_by_blob(module, ctor,
                                                                                  reinterpret_cast<const uint8_t*>(*blob_start),
                                                                                  reinterpret_cast<const uint8_t*>(blob_end)));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, attribute,
                                            vm::CustomAttribute::read_custom_attribute(module, &raw_data));

    *blob_start = blob_end;
    *named_arg_count = 0;
    RET_OK(attribute);
}

RtResult<int32_t> get_runtime_type_fields(void* method_table, RtIntPtrSpan data, int32_t* used_count) noexcept
{
    if (used_count == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }
    if (method_table == nullptr)
    {
        *used_count = 0;
        RET_OK(1);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, type_sig,
                                            vm::Reflection::get_type_sig_from_qcall_type_handle(method_table, method_table));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));
    RET_ERR_ON_FAIL(vm::Class::initialize_fields(klass));

    utils::Vector<const metadata::RtFieldInfo*> fields;
    fields.reserve(klass->field_count);
    for (uint32_t i = 0; i < klass->field_count; ++i)
    {
        const metadata::RtFieldInfo* field = klass->fields + i;
        if ((field->flags & static_cast<uint32_t>(metadata::RtFieldAttribute::Literal)) == 0)
        {
            fields.push_back(field);
        }
    }

    int32_t count = static_cast<int32_t>(fields.size());
    *used_count = count;
    if (count > data.length)
    {
        RET_OK(0);
    }
    if (count > 0 && data.pointer == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    for (int32_t i = 0; i < count; ++i)
    {
        data.pointer[i] = const_cast<metadata::RtFieldInfo*>(fields[static_cast<size_t>(i)]);
    }

    RET_OK(1);
}

RtResult<vm::RtArray*> get_runtime_type_interfaces(void* method_table) noexcept
{
    if (method_table == nullptr)
    {
        return LEANCLR_NEW_EMPTY_SZARRAY_BY_ELE_KLASS_INTERNAL(vm::Class::get_corlib_types().cls_systemtype,
                                                               "RuntimeTypeHandle_GetInterfaces");
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, type_sig,
                                            vm::Reflection::get_type_sig_from_qcall_type_handle(method_table, method_table));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));
    RET_ERR_ON_FAIL(vm::Class::initialize_interfaces(klass));

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(
        vm::RtArray*, interface_array,
        LEANCLR_NEW_SZARRAY_FROM_ELE_KLASS_INTERNAL(vm::Class::get_corlib_types().cls_systemtype, klass->interface_count,
                                                    "RuntimeTypeHandle_GetInterfaces"));
    for (uint32_t i = 0; i < klass->interface_count; ++i)
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionType*, interface_reflection,
                                                vm::Reflection::get_klass_reflection_object(klass->interfaces[i]));
        vm::Array::set_array_data_at<vm::RtReflectionType*>(interface_array, static_cast<int32_t>(i), interface_reflection);
    }

    RET_OK(interface_array);
}

RtResult<int32_t> get_rva_field_info(const metadata::RtFieldInfo* field, void** data, uint32_t* length) noexcept
{
    if (data == nullptr || length == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    *data = nullptr;
    *length = 0;

    if (field == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    if (!vm::Field::is_static_rva(field))
    {
        RET_OK(0);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const uint8_t*, rva_data, vm::Field::get_field_rva_data(field));
    if (rva_data == nullptr)
    {
        RET_OK(0);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(size_t, field_size, vm::Field::get_field_size(field));
    if (field_size > static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
    {
        RET_ERR(RtErr::Argument);
    }

    *data = const_cast<uint8_t*>(rva_data);
    *length = static_cast<uint32_t>(field_size);
    RET_OK(1);
}

RtResult<const void*> get_declaring_type_handle(void* type_handle) noexcept
{
    if (type_handle == nullptr)
    {
        RET_OK(nullptr);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, type_sig,
                                            vm::Reflection::get_type_sig_from_qcall_type_handle(type_handle, type_handle));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, declaring_klass, vm::Type::get_declaring_type(type_sig));
    if (declaring_klass == nullptr)
    {
        RET_OK(nullptr);
    }

    return vm::Reflection::get_net10_method_table(declaring_klass->by_val);
}

RtResult<int32_t> get_module_token(void* qcall_module, void* native_handle) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtModuleDef*, module, vm::Reflection::get_module_from_qcall_module(qcall_module, native_handle));
    RET_OK(static_cast<int32_t>(module->get_module_token()));
}

RtResult<vm::RtReflectionRuntimeType*> get_module_runtime_type(void* qcall_module, void* native_handle) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtModuleDef*, module, vm::Reflection::get_module_from_qcall_module(qcall_module, native_handle));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionModule*, reflection_module,
                                            vm::Reflection::get_module_reflection_object(module));
    RET_OK(reflection_module->runtime_type);
}

RtResult<vm::RtString*> get_runtime_module_name(void* qcall_module, void* native_handle) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtModuleDef*, module, vm::Reflection::get_module_from_qcall_module(qcall_module, native_handle));
    const char* name = module->get_name();
    if (name == nullptr || name[0] == '\0')
    {
        name = module->get_name_no_ext();
    }
    if (name == nullptr)
    {
        RET_ERR(RtErr::BadImageFormat);
    }

    return vm::String::create_string_from_utf8cstr(name);
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
    {
        metadata::RtToken parent = metadata::RtToken::decode(static_cast<metadata::EncodedTokenId>(parent_token));
        if (parent.table_type != metadata::TableType::Method || parent.rid == 0)
        {
            RET_ERR(RtErr::BadImageFormat);
        }

        const metadata::CliImage& cli_image = module->get_cli_image();
        auto method_row = cli_image.read_method(parent.rid);
        if (!method_row)
        {
            RET_ERR(RtErr::BadImageFormat);
        }

        uint32_t param_count = module->get_table_row_num(metadata::TableType::Param);
        uint32_t end_rid = param_count + 1;
        uint32_t method_count = module->get_table_row_num(metadata::TableType::Method);
        if (parent.rid < method_count)
        {
            auto next_method_row = cli_image.read_method(parent.rid + 1);
            if (!next_method_row)
            {
                RET_ERR(RtErr::BadImageFormat);
            }
            end_rid = next_method_row->param_list;
        }

        for (uint32_t param_rid = method_row->param_list; param_rid < end_rid; ++param_rid)
        {
            tokens.push_back(static_cast<int32_t>(metadata::RtToken::encode(metadata::TableType::Param, param_rid)));
        }
        break;
    }
    case metadata::TableType::CustomAttribute:
    {
        if (parent_token == 0)
        {
            break;
        }

        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL3(metadata::RtCustomAttributeRidRange, rid_range,
                                                 module->get_custom_attribute_rid_range(
                                                     static_cast<metadata::EncodedTokenId>(parent_token)));
        for (uint32_t i = 0; i < rid_range.count; ++i)
        {
            uint32_t ca_rid = rid_range.start_rid + i;
            tokens.push_back(static_cast<int32_t>(metadata::RtToken::encode(metadata::TableType::CustomAttribute, ca_rid)));
        }
        break;
    }
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

RtResult<vm::RtReflectionRuntimeType*> get_generic_type_definition(void* qcall_type_handle, void* native_handle) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, type_sig,
                                            vm::Reflection::get_type_sig_from_qcall_type_handle(qcall_type_handle, native_handle));
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
    auto type_sig_result = vm::Reflection::get_type_sig_from_runtime_type_object(runtime_type);
    if (!type_sig_result.is_ok())
    {
        RET_ERR(RtErr::Argument);
    }

    const metadata::RtTypeSig* type_sig = type_sig_result.unwrap();
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass,
                                            vm::Class::get_class_from_typesig(type_sig));
    return vm::Reflection::get_module_reflection_object(klass->image);
}

RtResult<vm::RtArray*> get_type_instantiation(void* qcall_type_handle, void* native_handle, bool runtime_array) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, type_sig,
                                            vm::Reflection::get_type_sig_from_qcall_type_handle(qcall_type_handle, native_handle));
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
    metadata::RtPropertySig property_sig;

    if (method != nullptr)
    {
        if (method->token != metadata::RtToken::Invalid)
        {
            metadata::RtModuleDef* module = method->parent->image;
            auto method_row = module->get_cli_image().read_method(metadata::RtToken::decode_rid(method->token));
            if (method_row.has_value())
            {
                DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL2(utils::BinaryReader, method_sig_reader,
                                                         module->get_decoded_blob_reader(method_row->signature));
                signature->sig = const_cast<uint8_t*>(method_sig_reader.data());
                signature->csig = static_cast<int32_t>(method_sig_reader.length());
            }
            else if (metadata::RtToken::decode_table_type(method->token) == metadata::TableType::Method)
            {
                RET_ERR(RtErr::BadImageFormat);
            }
        }
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
        if (field->token != metadata::RtToken::Invalid)
        {
            metadata::RtModuleDef* module = field->parent->image;
            auto field_row = module->get_cli_image().read_field(metadata::RtToken::decode_rid(field->token));
            if (field_row.has_value())
            {
                DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL2(utils::BinaryReader, field_sig_reader,
                                                         module->get_decoded_blob_reader(field_row->signature));
                signature->sig = const_cast<uint8_t*>(field_sig_reader.data());
                signature->csig = static_cast<int32_t>(field_sig_reader.length());
            }
            else if (metadata::RtToken::decode_table_type(field->token) == metadata::TableType::Field)
            {
                RET_ERR(RtErr::BadImageFormat);
            }
        }
        return_or_field_type = field->type_sig;
    }
    else
    {
        if (raw_sig == nullptr || raw_sig_size < 0 || signature->declaring_type == nullptr)
        {
            RET_ERR(RtErr::BadImageFormat);
        }

        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(
            const metadata::RtTypeSig*, declaring_type_sig,
            vm::Reflection::get_type_sig_from_runtime_type_object(signature->declaring_type));
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(
            metadata::RtClass*, declaring_klass,
            vm::Class::get_class_from_typesig(declaring_type_sig));
        if (declaring_klass == nullptr || declaring_klass->image == nullptr)
        {
            RET_ERR(RtErr::BadImageFormat);
        }

        utils::BinaryReader reader(raw_sig, static_cast<size_t>(raw_sig_size));
        UNWRAP_OR_RET_ERR_ON_FAIL(
            property_sig,
            declaring_klass->image->read_property_sig(reader, vm::Class::get_generic_container_context(declaring_klass), nullptr));
        return_or_field_type = property_sig.type_sig;
        parameters = property_sig.params.data();
        parameter_count = static_cast<int32_t>(property_sig.params.size());
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionRuntimeType*, return_type,
                                            vm::Reflection::get_runtime_type_from_type_sig(return_or_field_type));
    signature->return_type_or_field_type = return_type;

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, arguments,
                                            LEANCLR_NEW_SZARRAY_FROM_ELE_KLASS_INTERNAL(vm::Class::get_corlib_types().cls_runtimetype,
                                                                                       parameter_count, "Signature_Init"));
    signature->arguments = arguments;
    for (int32_t i = 0; i < parameter_count; ++i)
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionRuntimeType*, parameter_type,
                                                vm::Reflection::get_runtime_type_from_type_sig(parameters[i]));
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
                                            vm::Reflection::get_type_sig_from_qcall_type_handle(qcall_type_handle, native_handle));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, base_klass, vm::Class::get_class_from_typesig(base_type_sig));

    uint32_t base_type_def_gid = 0;
    const metadata::RtClass* generic_definition_klass = base_klass;
    if (vm::Class::is_generic_inst(base_klass))
    {
        generic_definition_klass = vm::Class::get_generic_base_klass_of_generic_class(base_klass);
        base_type_def_gid = base_type_sig->data.generic_class->base_type_def_gid;
    }
    else if (!vm::Class::is_generic(base_klass))
    {
        RET_ERR(RtErr::Argument);
    }
    else
    {
        base_type_def_gid = vm::Class::get_type_def_gid(base_klass);
    }

    const metadata::RtGenericContainer* generic_container = generic_definition_klass->generic_container;
    if (generic_container == nullptr || type_handle_count != generic_container->generic_param_count)
    {
        RET_ERR(RtErr::Argument);
    }

    const metadata::RtTypeSig* generic_args[metadata::RT_MAX_GENERIC_PARAM_COUNT]{};
    for (int32_t i = 0; i < type_handle_count; ++i)
    {
        if (type_handles[i] == nullptr)
        {
            RET_ERR(RtErr::ArgumentNull);
        }
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, generic_arg,
                                                vm::Reflection::get_type_sig_from_net10_type_handle(type_handles[i]));
        generic_args[i] = generic_arg;
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtGenericInst*, generic_inst,
                                            metadata::MetadataCache::get_pooled_generic_inst(generic_args, static_cast<uint8_t>(type_handle_count)));
    return vm::GenericClass::get_class(base_type_def_gid, generic_inst);
}

RtResult<vm::RtReflectionRuntimeType*> instantiate_runtime_type(void* qcall_type_handle, void* native_handle, void** type_handles,
                                                               int32_t type_handle_count) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass,
                                            instantiate_type_for_generic_parameters(qcall_type_handle, native_handle, type_handles,
                                                                                    type_handle_count));
    return vm::Reflection::get_runtime_type_from_type_sig(klass->by_val);
}

static RtResultVoid validate_runtime_type_element_shape(const metadata::RtTypeSig* type_sig) noexcept
{
    if (type_sig == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }
    if (type_sig->is_by_ref() || type_sig->ele_type == metadata::RtElementType::TypedByRef)
    {
        RET_ERR(RtErr::TypeLoad);
    }

    RET_VOID_OK();
}

RtResult<vm::RtReflectionRuntimeType*> make_array_runtime_type(void* qcall_type_handle, void* native_handle, int32_t rank) noexcept
{
    if (rank <= 0 || rank > static_cast<int32_t>(metadata::RT_MAX_ARRAY_RANK))
    {
        RET_ERR(RtErr::TypeLoad);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, type_sig,
                                            vm::Reflection::get_type_sig_from_qcall_type_handle(qcall_type_handle, native_handle));
    RET_ERR_ON_FAIL(validate_runtime_type_element_shape(type_sig));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, array_klass,
                                            vm::ArrayClass::get_array_class_from_element_type(type_sig, static_cast<uint8_t>(rank)));
    return vm::Reflection::get_runtime_type_from_type_sig(array_klass->by_val);
}

RtResult<vm::RtReflectionRuntimeType*> make_szarray_runtime_type(void* qcall_type_handle, void* native_handle) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, type_sig,
                                            vm::Reflection::get_type_sig_from_qcall_type_handle(qcall_type_handle, native_handle));
    RET_ERR_ON_FAIL(validate_runtime_type_element_shape(type_sig));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, array_klass,
                                            vm::ArrayClass::get_szarray_class_from_element_typesig(type_sig));
    return vm::Reflection::get_runtime_type_from_type_sig(array_klass->by_val);
}

RtResult<vm::RtReflectionRuntimeType*> make_byref_runtime_type(void* qcall_type_handle, void* native_handle) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, type_sig,
                                            vm::Reflection::get_type_sig_from_qcall_type_handle(qcall_type_handle, native_handle));
    if (type_sig->is_by_ref() || type_sig->ele_type == metadata::RtElementType::TypedByRef)
    {
        RET_ERR(RtErr::TypeLoad);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));
    return vm::Reflection::get_runtime_type_from_type_sig(vm::Class::get_by_ref_type_sig(klass));
}

RtResult<vm::RtReflectionRuntimeType*> make_pointer_runtime_type(void* qcall_type_handle, void* native_handle) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, type_sig,
                                            vm::Reflection::get_type_sig_from_qcall_type_handle(qcall_type_handle, native_handle));
    RET_ERR_ON_FAIL(validate_runtime_type_element_shape(type_sig));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, ptr_klass,
                                            vm::Class::get_ptr_class_by_element_typesig(type_sig));
    return vm::Reflection::get_runtime_type_from_type_sig(ptr_klass->by_val);
}

RtResult<const metadata::RtGenericInst*> get_pooled_generic_inst_from_type_handles(void** type_handles, int32_t type_handle_count) noexcept
{
    if (type_handle_count == 0)
    {
        RET_OK(nullptr);
    }
    if (type_handles == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    const metadata::RtTypeSig* type_sigs[metadata::RT_MAX_GENERIC_PARAM_COUNT]{};
    for (int32_t i = 0; i < type_handle_count; ++i)
    {
        if (type_handles[i] == nullptr)
        {
            RET_ERR(RtErr::ArgumentNull);
        }
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, type_sig,
                                                vm::Reflection::get_type_sig_from_net10_type_handle(type_handles[i]));
        type_sigs[i] = type_sig;
    }

    return metadata::MetadataCache::get_pooled_generic_inst(type_sigs, static_cast<uint8_t>(type_handle_count));
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

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtModuleDef*, module, vm::Reflection::get_module_from_qcall_module(qcall_module, native_handle));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtGenericInst*, class_inst,
                                            get_pooled_generic_inst_from_type_handles(type_inst_args, type_inst_count));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtGenericInst*, method_inst,
                                            get_pooled_generic_inst_from_type_handles(method_inst_args, method_inst_count));

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

RtResult<const metadata::RtFieldInfo*> resolve_module_field(void* qcall_module, void* native_handle, int32_t field_token,
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

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtModuleDef*, module, vm::Reflection::get_module_from_qcall_module(qcall_module, native_handle));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtGenericInst*, class_inst,
                                            get_pooled_generic_inst_from_type_handles(type_inst_args, type_inst_count));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtGenericInst*, method_inst,
                                            get_pooled_generic_inst_from_type_handles(method_inst_args, method_inst_count));

    metadata::RtGenericContainerContext gcc{};
    metadata::RtGenericContext gc{class_inst, method_inst};
    metadata::RtToken token = metadata::RtToken::decode(static_cast<metadata::EncodedTokenId>(field_token));
    return module->get_field_by_token(token, gcc, &gc);
}

RtResult<vm::RtReflectionRuntimeType*> resolve_module_type(void* qcall_module, void* native_handle, int32_t type_token,
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

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtModuleDef*, module, vm::Reflection::get_module_from_qcall_module(qcall_module, native_handle));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtGenericInst*, class_inst,
                                            get_pooled_generic_inst_from_type_handles(type_inst_args, type_inst_count));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtGenericInst*, method_inst,
                                            get_pooled_generic_inst_from_type_handles(method_inst_args, method_inst_count));

    metadata::RtGenericContainerContext gcc{};
    metadata::RtGenericContext gc{class_inst, method_inst};
    metadata::RtToken token = metadata::RtToken::decode(static_cast<metadata::EncodedTokenId>(type_token));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, type_sig,
                                            module->get_typesig_by_type_def_ref_spec_token(token, gcc, &gc));
    return vm::Reflection::get_runtime_type_from_type_sig(type_sig);
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

bool is_assembly_stack_walk_helper_frame(const interp::InterpFrame* frame) noexcept
{
    const metadata::RtMethodInfo* method = frame->method;
    if (method == nullptr || method->parent == nullptr || method->parent->namespaze == nullptr || method->parent->name == nullptr ||
        method->name == nullptr)
    {
        return false;
    }

    return std::strcmp(method->parent->namespaze, "System.Reflection") == 0 && std::strcmp(method->parent->name, "Assembly") == 0 &&
           (std::strcmp(method->name, "GetExecutingAssembly") == 0 || std::strcmp(method->name, "GetCallingAssembly") == 0);
}

RtResult<const metadata::RtMethodInfo*> get_method_for_stack_mark(vm::RtStackCrawlMark* stack_mark, bool skip_method_base_helpers,
                                                                  bool skip_assembly_helpers) noexcept
{
    vm::RtStackCrawlMark mark = stack_mark != nullptr ? *stack_mark : vm::RtStackCrawlMark::LookForMyCaller;
    int32_t caller_skip = mark == vm::RtStackCrawlMark::LookForMyCallersCaller ? 1 : 0;

    auto frames = interp::MachineState::get_global_machine_state().get_active_frames();
    for (size_t i = frames.size(); i > 0; --i)
    {
        const interp::InterpFrame* frame = &frames[i - 1];
        if (frame->method == nullptr || (skip_method_base_helpers && is_method_base_get_current_method_frame(frame)) ||
            (skip_assembly_helpers && is_assembly_stack_walk_helper_frame(frame)))
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

RtResult<const metadata::RtMethodInfo*> get_current_method_for_stack_mark(vm::RtStackCrawlMark* stack_mark) noexcept
{
    return get_method_for_stack_mark(stack_mark, true, false);
}

RtResult<vm::RtReflectionAssembly*> get_executing_assembly_for_stack_mark(vm::RtStackCrawlMark* stack_mark) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtMethodInfo*, method,
                                            get_method_for_stack_mark(stack_mark, false, true));
    metadata::RtAssembly* assembly = method != nullptr && method->parent != nullptr && method->parent->image != nullptr
                                         ? method->parent->image->get_assembly()
                                         : vm::Assembly::get_corlib();
    return vm::Reflection::get_assembly_reflection_object(assembly);
}

RtResult<vm::RtReflectionAssembly*> get_entry_assembly() noexcept
{
    utils::Vector<metadata::RtModuleDef*> modules;
    metadata::RtModuleDef::get_registered_modules(modules);
    for (metadata::RtModuleDef* mod : modules)
    {
        if (mod != nullptr && mod->get_entrypoint_token() != 0)
        {
            return vm::Reflection::get_assembly_reflection_object(mod->get_assembly());
        }
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
        auto* stack_frame = vm::Array::get_array_data_at<vm::RtObject*>(exception->trace_ips, i);
        if (stack_frame == nullptr)
        {
            continue;
        }

        vm::RtReflectionMethod* reflection_method = nullptr;
        int32_t native_offset = -1;
        int32_t il_offset = -1;
        vm::RtString* file_name = nullptr;
        int32_t line_number = 0;
        int32_t column_number = 0;
        bool is_last_frame_from_foreign_exception_stack_trace = false;
        RET_ERR_ON_FAIL(vm::StackTrace::get_stack_frame_data(stack_frame, &reflection_method, &native_offset, &il_offset, &file_name, &line_number,
                                                            &column_number, &is_last_frame_from_foreign_exception_stack_trace));

        const metadata::RtMethodInfo* method = nullptr;
        if (reflection_method != nullptr)
        {
            UNWRAP_OR_RET_ERR_ON_FAIL(method, vm::Reflection::get_method_info_from_reflection_object(reflection_method));
        }
        result.push_back(StackFrameData{method, native_offset, il_offset, file_name, line_number, column_number,
                                        is_last_frame_from_foreign_exception_stack_trace});
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

int32_t g_next_coreclr_thread_id = 1;

RtResult<uint8_t*> get_instance_field_data(vm::RtObject* obj, const char* field_name) noexcept
{
    if (obj == nullptr || field_name == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    auto klass = const_cast<metadata::RtClass*>(obj->klass);
    if (klass == nullptr)
    {
        RET_ERR(RtErr::NullReference);
    }

    RET_ERR_ON_FAIL(vm::Class::initialize_fields(klass));
    const metadata::RtFieldInfo* field = vm::Class::get_field_for_name(klass, field_name, true);
    if (field == nullptr)
    {
        RET_ERR(RtErr::MissingField);
    }

    RET_OK(reinterpret_cast<uint8_t*>(obj) + vm::Field::get_instance_field_offset_includes_object_header_for_all_type(field));
}

void set_instance_int32_field_if_present(vm::RtObject* obj, const char* field_name, int32_t value) noexcept
{
    if (obj == nullptr || obj->klass == nullptr)
    {
        return;
    }

    const metadata::RtFieldInfo* field = vm::Class::get_field_for_name(obj->klass, field_name, true);
    if (field == nullptr)
    {
        return;
    }

    uint8_t* data = reinterpret_cast<uint8_t*>(obj) + vm::Field::get_instance_field_offset_includes_object_header_for_all_type(field);
    *reinterpret_cast<int32_t*>(data) = value;
}

void set_instance_bool_field_if_present(vm::RtObject* obj, const char* field_name, bool value) noexcept
{
    if (obj == nullptr || obj->klass == nullptr)
    {
        return;
    }

    const metadata::RtFieldInfo* field = vm::Class::get_field_for_name(obj->klass, field_name, true);
    if (field == nullptr)
    {
        return;
    }

    uint8_t* data = reinterpret_cast<uint8_t*>(obj) + vm::Field::get_instance_field_offset_includes_object_header_for_all_type(field);
    *reinterpret_cast<bool*>(data) = value;
}

RtResultVoid ensure_coreclr_thread_initialized(vm::RtObject* thread, bool current_thread) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(uint8_t*, internal_thread_data, get_instance_field_data(thread, "_DONT_USE_InternalThread"));
    auto internal_thread_slot = reinterpret_cast<intptr_t*>(internal_thread_data);
    if (*internal_thread_slot != 0)
    {
        RET_VOID_OK();
    }

    auto internal_thread = alloc::GeneralAllocation::malloc_any_zeroed<vm::RtInternalThread>();
    auto native_thread = alloc::GeneralAllocation::malloc_any_zeroed<vm::RtNativeThread>();
    if (internal_thread == nullptr || native_thread == nullptr)
    {
        alloc::GeneralAllocation::free(internal_thread);
        alloc::GeneralAllocation::free(native_thread);
        RET_ERR(RtErr::OutOfMemory);
    }

    int32_t managed_thread_id = current_thread ? 1 : ++g_next_coreclr_thread_id;
    internal_thread->handle = native_thread;
    internal_thread->thread_id = managed_thread_id;
    internal_thread->managed_id = managed_thread_id;
    internal_thread->state = current_thread ? vm::RtThreadState::Running : vm::RtThreadState::Unstarted;
    internal_thread->priority = static_cast<int32_t>(vm::ThreadPriority::Normal);

    *internal_thread_slot = reinterpret_cast<intptr_t>(internal_thread);
    set_instance_int32_field_if_present(thread, "_priority", static_cast<int32_t>(vm::ThreadPriority::Normal));
    set_instance_int32_field_if_present(thread, "_managedThreadId", managed_thread_id);
    set_instance_bool_field_if_present(thread, "_isDead", false);
    set_instance_bool_field_if_present(thread, "_isThreadPool", false);

    RET_VOID_OK();
}

RtResult<vm::RtInternalThread*> get_coreclr_internal_thread(vm::RtObject* thread) noexcept
{
    RET_ERR_ON_FAIL(ensure_coreclr_thread_initialized(thread, false));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(uint8_t*, internal_thread_data, get_instance_field_data(thread, "_DONT_USE_InternalThread"));
    auto internal_thread = reinterpret_cast<vm::RtInternalThread*>(*reinterpret_cast<intptr_t*>(internal_thread_data));
    if (internal_thread == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }
    RET_OK(internal_thread);
}

vm::RtInternalThread* get_coreclr_internal_thread_from_handle(intptr_t thread_handle) noexcept
{
    return reinterpret_cast<vm::RtInternalThread*>(thread_handle);
}

RtResultVoid get_current_thread_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                        interp::RtStackObject*) noexcept
{
    auto thread_slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 0);
    if (thread_slot == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    vm::RtObject* current_thread = reinterpret_cast<vm::RtObject*>(vm::Thread::get_current_thread());
    RET_ERR_ON_FAIL(ensure_coreclr_thread_initialized(current_thread, true));
    *thread_slot = current_thread;
    RET_VOID_OK();
}

RtResultVoid md_utf8_string_equals_case_insensitive_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                            const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    auto lhs = interp::EvalStackOp::get_param<const uint8_t*>(params, 0);
    auto rhs = interp::EvalStackOp::get_param<const uint8_t*>(params, 1);
    int32_t count = interp::EvalStackOp::get_param<int32_t>(params, 2);

    int32_t equals = 0;
    if (count >= 0 && (count == 0 || (lhs != nullptr && rhs != nullptr)))
    {
        equals = 1;
        for (int32_t i = 0; i < count; ++i)
        {
            if (fold_ascii_case(lhs[i]) != fold_ascii_case(rhs[i]))
            {
                equals = 0;
                break;
            }
        }
    }

    interp::EvalStackOp::set_return(ret, equals);
    RET_VOID_OK();
}

RtResultVoid thread_initialize_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                       interp::RtStackObject*) noexcept
{
    auto thread_slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 0);
    if (thread_slot == nullptr || *thread_slot == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    return ensure_coreclr_thread_initialized(*thread_slot, false);
}

RtResultVoid thread_get_is_background_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                              interp::RtStackObject* ret) noexcept
{
    intptr_t thread_handle = interp::EvalStackOp::get_param<intptr_t>(params, 0);
    vm::RtInternalThread* thread = get_coreclr_internal_thread_from_handle(thread_handle);
    int32_t is_background =
        thread != nullptr && (static_cast<int32_t>(thread->state) & static_cast<int32_t>(vm::RtThreadState::Background)) != 0 ? 1 : 0;
    interp::EvalStackOp::set_return(ret, is_background);
    RET_VOID_OK();
}

RtResultVoid thread_set_is_background_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                              interp::RtStackObject*) noexcept
{
    intptr_t thread_handle = interp::EvalStackOp::get_param<intptr_t>(params, 0);
    int32_t value = interp::EvalStackOp::get_param<int32_t>(params, 1);
    vm::RtInternalThread* thread = get_coreclr_internal_thread_from_handle(thread_handle);
    if (thread == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    int32_t state = static_cast<int32_t>(thread->state);
    if (value != 0)
    {
        state |= static_cast<int32_t>(vm::RtThreadState::Background);
    }
    else
    {
        state &= ~static_cast<int32_t>(vm::RtThreadState::Background);
    }
    thread->state = static_cast<vm::RtThreadState>(state);
    RET_VOID_OK();
}

RtResultVoid thread_get_thread_state_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                             interp::RtStackObject* ret) noexcept
{
    intptr_t thread_handle = interp::EvalStackOp::get_param<intptr_t>(params, 0);
    vm::RtInternalThread* thread = get_coreclr_internal_thread_from_handle(thread_handle);
    int32_t state = thread != nullptr ? static_cast<int32_t>(thread->state) : static_cast<int32_t>(vm::RtThreadState::Stopped);
    interp::EvalStackOp::set_return(ret, state);
    RET_VOID_OK();
}

RtResultVoid thread_set_priority_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                         interp::RtStackObject*) noexcept
{
    auto thread_slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 0);
    int32_t priority = interp::EvalStackOp::get_param<int32_t>(params, 1);
    if (thread_slot == nullptr || *thread_slot == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtInternalThread*, thread, get_coreclr_internal_thread(*thread_slot));
    thread->priority = priority;
    set_instance_int32_field_if_present(*thread_slot, "_priority", priority);
    RET_VOID_OK();
}

RtResultVoid thread_start_internal_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                           interp::RtStackObject*) noexcept
{
    intptr_t thread_handle = interp::EvalStackOp::get_param<intptr_t>(params, 0);
    (void)interp::EvalStackOp::get_param<int32_t>(params, 1);
    int32_t priority = interp::EvalStackOp::get_param<int32_t>(params, 2);
    int32_t is_thread_pool = interp::EvalStackOp::get_param<int32_t>(params, 3);
    (void)interp::EvalStackOp::get_param<Utf16Char*>(params, 4);
    vm::RtInternalThread* thread = get_coreclr_internal_thread_from_handle(thread_handle);
    if (thread == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    int32_t state = static_cast<int32_t>(thread->state);
    state &= ~static_cast<int32_t>(vm::RtThreadState::Unstarted);
    thread->state = static_cast<vm::RtThreadState>(state);
    thread->priority = priority;
    thread->threadpool_thread = is_thread_pool != 0;
    RET_VOID_OK();
}

RtResultVoid thread_inform_thread_name_change_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                      const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    (void)interp::EvalStackOp::get_param<intptr_t>(params, 0);
    (void)interp::EvalStackOp::get_param<Utf16Char*>(params, 1);
    (void)interp::EvalStackOp::get_param<int32_t>(params, 2);
    RET_VOID_OK();
}

RtResultVoid thread_sleep_internal_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                           interp::RtStackObject*) noexcept
{
    int32_t milliseconds = interp::EvalStackOp::get_param<int32_t>(params, 0);
    vm::Thread::sleep(milliseconds);
    RET_VOID_OK();
}

RtResultVoid thread_spin_wait_internal_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                               interp::RtStackObject*) noexcept
{
    (void)interp::EvalStackOp::get_param<int32_t>(params, 0);
    RET_VOID_OK();
}

RtResultVoid thread_yield_internal_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject*,
                                           interp::RtStackObject* ret) noexcept
{
    interp::EvalStackOp::set_return(ret, vm::Thread::yield_internal() ? 1 : 0);
    RET_VOID_OK();
}

RtResultVoid thread_get_current_os_thread_id_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject*,
                                                     interp::RtStackObject* ret) noexcept
{
    interp::EvalStackOp::set_return(ret, static_cast<uint64_t>(1));
    RET_VOID_OK();
}

RtResultVoid thread_interrupt_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                      interp::RtStackObject*) noexcept
{
    (void)interp::EvalStackOp::get_param<intptr_t>(params, 0);
    RET_VOID_OK();
}

RtResultVoid thread_join_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                 interp::RtStackObject* ret) noexcept
{
    (void)interp::EvalStackOp::get_param<vm::RtObject**>(params, 0);
    (void)interp::EvalStackOp::get_param<int32_t>(params, 1);
    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(1));
    RET_VOID_OK();
}

RtResultVoid thread_poll_gc_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject*, interp::RtStackObject*) noexcept
{
    RET_VOID_OK();
}

RtResultVoid is_managed_debugger_attached_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject*,
                                                   interp::RtStackObject* ret) noexcept
{
    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(0));
    RET_VOID_OK();
}

RtResultVoid is_debugger_logging_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject*,
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

RtResultVoid kernel32_query_performance_frequency_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                          const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    int64_t* frequency = interp::EvalStackOp::get_param<int64_t*>(params, 0);
#if LEANCLR_PLATFORM_WIN
    int32_t result = platform::Kernel32::query_performance_frequency(frequency) ? 1 : 0;
#else
    (void)frequency;
    int32_t result = 0;
#endif
    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

RtResultVoid kernel32_query_performance_counter_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                        const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    int64_t* counter = interp::EvalStackOp::get_param<int64_t*>(params, 0);
#if LEANCLR_PLATFORM_WIN
    int32_t result = platform::Kernel32::query_performance_counter(counter) ? 1 : 0;
#else
    (void)counter;
    int32_t result = 0;
#endif
    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

RtResultVoid kernel32_get_console_cp_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject*,
                                             interp::RtStackObject* ret) noexcept
{
    interp::EvalStackOp::set_return(ret, static_cast<uint32_t>(platform::Kernel32::get_console_cp()));
    RET_VOID_OK();
}

RtResultVoid kernel32_get_console_output_cp_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject*,
                                                    interp::RtStackObject* ret) noexcept
{
    interp::EvalStackOp::set_return(ret, static_cast<uint32_t>(platform::Kernel32::get_console_output_cp()));
    RET_VOID_OK();
}

RtResultVoid kernel32_get_std_handle_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                             interp::RtStackObject* ret) noexcept
{
    int32_t std_handle = interp::EvalStackOp::get_param<int32_t>(params, 0);
    interp::EvalStackOp::set_return(ret, platform::Kernel32::get_std_handle(std_handle));
    RET_VOID_OK();
}

RtResultVoid kernel32_write_file_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                         interp::RtStackObject* ret) noexcept
{
    intptr_t handle = interp::EvalStackOp::get_param<intptr_t>(params, 0);
    auto buffer = interp::EvalStackOp::get_param<const uint8_t*>(params, 1);
    int32_t count = interp::EvalStackOp::get_param<int32_t>(params, 2);
    auto bytes_written = interp::EvalStackOp::get_param<int32_t*>(params, 3);
    (void)interp::EvalStackOp::get_param<intptr_t>(params, 4);

    int32_t error = 0;
    int32_t written = os::File::write(handle, buffer, count, &error);
    if (bytes_written != nullptr)
    {
        *bytes_written = written > 0 ? written : 0;
    }
    if (written < 0)
    {
        vm::Marshal::set_last_win32_error(error);
        interp::EvalStackOp::set_return(ret, 0);
        RET_VOID_OK();
    }

    interp::EvalStackOp::set_return(ret, 1);
    RET_VOID_OK();
}

RtResultVoid kernel32_get_file_type_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                            interp::RtStackObject* ret) noexcept
{
    intptr_t handle = interp::EvalStackOp::get_param<intptr_t>(params, 0);
    int32_t error = 0;
    int32_t file_type = os::File::get_file_type(handle, &error);
    if (file_type == os::File::FileTypeUnknown && error != 0)
    {
        vm::Marshal::set_last_win32_error(error);
    }
    interp::EvalStackOp::set_return(ret, file_type);
    RET_VOID_OK();
}

RtResultVoid kernel32_initialize_critical_section_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                          const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    void* critical_section = interp::EvalStackOp::get_param<void*>(params, 0);
    platform::Kernel32::initialize_critical_section(critical_section);
    RET_VOID_OK();
}

RtResultVoid kernel32_delete_critical_section_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                                      interp::RtStackObject*) noexcept
{
    void* critical_section = interp::EvalStackOp::get_param<void*>(params, 0);
    platform::Kernel32::delete_critical_section(critical_section);
    RET_VOID_OK();
}

RtResultVoid kernel32_enter_critical_section_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                                     interp::RtStackObject*) noexcept
{
    void* critical_section = interp::EvalStackOp::get_param<void*>(params, 0);
    platform::Kernel32::enter_critical_section(critical_section);
    RET_VOID_OK();
}

RtResultVoid kernel32_leave_critical_section_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                                     interp::RtStackObject*) noexcept
{
    void* critical_section = interp::EvalStackOp::get_param<void*>(params, 0);
    platform::Kernel32::leave_critical_section(critical_section);
    RET_VOID_OK();
}

RtResultVoid kernel32_initialize_condition_variable_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                            const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    void* condition_variable = interp::EvalStackOp::get_param<void*>(params, 0);
    platform::Kernel32::initialize_condition_variable(condition_variable);
    RET_VOID_OK();
}

RtResultVoid kernel32_sleep_condition_variable_cs_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                          const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    void* condition_variable = interp::EvalStackOp::get_param<void*>(params, 0);
    void* critical_section = interp::EvalStackOp::get_param<void*>(params, 1);
    int32_t milliseconds = interp::EvalStackOp::get_param<int32_t>(params, 2);
    int32_t result = platform::Kernel32::sleep_condition_variable_cs(condition_variable, critical_section, milliseconds) ? 1 : 0;
    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

RtResultVoid kernel32_wake_condition_variable_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                       const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    void* condition_variable = interp::EvalStackOp::get_param<void*>(params, 0);
    platform::Kernel32::wake_condition_variable(condition_variable);
    RET_VOID_OK();
}

RtResultVoid kernel32_create_io_completion_port_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                        const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    intptr_t file_handle = interp::EvalStackOp::get_param<intptr_t>(params, 0);
    intptr_t existing_completion_port = interp::EvalStackOp::get_param<intptr_t>(params, 1);
    uintptr_t completion_key = interp::EvalStackOp::get_param<uintptr_t>(params, 2);
    int32_t number_of_concurrent_threads = interp::EvalStackOp::get_param<int32_t>(params, 3);
    intptr_t result = platform::Kernel32::create_io_completion_port(file_handle, existing_completion_port, completion_key,
                                                                    number_of_concurrent_threads);
    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

RtResultVoid kernel32_post_queued_completion_status_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                            const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    intptr_t completion_port = interp::EvalStackOp::get_param<intptr_t>(params, 0);
    uint32_t number_of_bytes_transferred = interp::EvalStackOp::get_param<uint32_t>(params, 1);
    uintptr_t completion_key = interp::EvalStackOp::get_param<uintptr_t>(params, 2);
    intptr_t overlapped = interp::EvalStackOp::get_param<intptr_t>(params, 3);
    int32_t result =
        platform::Kernel32::post_queued_completion_status(completion_port, number_of_bytes_transferred, completion_key, overlapped) ? 1 : 0;
    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

RtResultVoid kernel32_get_queued_completion_status_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                           const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    intptr_t completion_port = interp::EvalStackOp::get_param<intptr_t>(params, 0);
    uint32_t* number_of_bytes_transferred = interp::EvalStackOp::get_param<uint32_t*>(params, 1);
    uintptr_t* completion_key = interp::EvalStackOp::get_param<uintptr_t*>(params, 2);
    intptr_t* overlapped = interp::EvalStackOp::get_param<intptr_t*>(params, 3);
    int32_t milliseconds = interp::EvalStackOp::get_param<int32_t>(params, 4);
    int32_t result = platform::Kernel32::get_queued_completion_status(completion_port, number_of_bytes_transferred, completion_key, overlapped,
                                                                      milliseconds) ? 1 : 0;
    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

RtResultVoid kernel32_get_queued_completion_status_ex_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                              const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    intptr_t completion_port = interp::EvalStackOp::get_param<intptr_t>(params, 0);
    void* completion_port_entries = interp::EvalStackOp::get_param<void*>(params, 1);
    int32_t count = interp::EvalStackOp::get_param<int32_t>(params, 2);
    int32_t* number_of_entries_removed = interp::EvalStackOp::get_param<int32_t*>(params, 3);
    int32_t milliseconds = interp::EvalStackOp::get_param<int32_t>(params, 4);
    int32_t alertable = interp::EvalStackOp::get_param<int32_t>(params, 5);
    int32_t result = platform::Kernel32::get_queued_completion_status_ex(completion_port, completion_port_entries, count, number_of_entries_removed,
                                                                         milliseconds, alertable) ? 1 : 0;
    interp::EvalStackOp::set_return(ret, result);
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

RtResultVoid kernel32_lc_map_string_ex_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                               const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    Utf16Char* locale_name = interp::EvalStackOp::get_param<Utf16Char*>(params, 0);
    uint32_t map_flags = interp::EvalStackOp::get_param<uint32_t>(params, 1);
    Utf16Char* source = interp::EvalStackOp::get_param<Utf16Char*>(params, 2);
    int32_t source_length = interp::EvalStackOp::get_param<int32_t>(params, 3);
    void* destination = interp::EvalStackOp::get_param<void*>(params, 4);
    int32_t destination_length = interp::EvalStackOp::get_param<int32_t>(params, 5);
    void* version_information = interp::EvalStackOp::get_param<void*>(params, 6);
    void* reserved = interp::EvalStackOp::get_param<void*>(params, 7);
    intptr_t sort_handle = interp::EvalStackOp::get_param<intptr_t>(params, 8);

    int32_t result = platform::RtSys::lc_map_string_ex(locale_name, map_flags, source, source_length, destination, destination_length,
                                                       version_information, reserved, sort_handle);
    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

RtResultVoid kernel32_find_nls_string_ex_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                 const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    Utf16Char* locale_name = interp::EvalStackOp::get_param<Utf16Char*>(params, 0);
    uint32_t find_flags = interp::EvalStackOp::get_param<uint32_t>(params, 1);
    Utf16Char* source = interp::EvalStackOp::get_param<Utf16Char*>(params, 2);
    int32_t source_length = interp::EvalStackOp::get_param<int32_t>(params, 3);
    Utf16Char* value = interp::EvalStackOp::get_param<Utf16Char*>(params, 4);
    int32_t value_length = interp::EvalStackOp::get_param<int32_t>(params, 5);
    int32_t* found_length = interp::EvalStackOp::get_param<int32_t*>(params, 6);
    void* version_information = interp::EvalStackOp::get_param<void*>(params, 7);
    void* reserved = interp::EvalStackOp::get_param<void*>(params, 8);
    intptr_t sort_handle = interp::EvalStackOp::get_param<intptr_t>(params, 9);

    int32_t result = platform::RtSys::find_nls_string_ex(locale_name, find_flags, source, source_length, value, value_length,
                                                         found_length, version_information, reserved, sort_handle);
    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

RtResultVoid kernel32_find_string_ordinal_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                  const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    uint32_t find_flags = interp::EvalStackOp::get_param<uint32_t>(params, 0);
    Utf16Char* source = interp::EvalStackOp::get_param<Utf16Char*>(params, 1);
    int32_t source_length = interp::EvalStackOp::get_param<int32_t>(params, 2);
    Utf16Char* value = interp::EvalStackOp::get_param<Utf16Char*>(params, 3);
    int32_t value_length = interp::EvalStackOp::get_param<int32_t>(params, 4);
    int32_t ignore_case = interp::EvalStackOp::get_param<int32_t>(params, 5);

    int32_t result = platform::RtSys::find_string_ordinal(find_flags, source, source_length, value, value_length, ignore_case);
    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

RtResultVoid kernel32_compare_string_ex_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    Utf16Char* locale_name = interp::EvalStackOp::get_param<Utf16Char*>(params, 0);
    uint32_t compare_flags = interp::EvalStackOp::get_param<uint32_t>(params, 1);
    Utf16Char* string1 = interp::EvalStackOp::get_param<Utf16Char*>(params, 2);
    int32_t string1_length = interp::EvalStackOp::get_param<int32_t>(params, 3);
    Utf16Char* string2 = interp::EvalStackOp::get_param<Utf16Char*>(params, 4);
    int32_t string2_length = interp::EvalStackOp::get_param<int32_t>(params, 5);
    void* version_information = interp::EvalStackOp::get_param<void*>(params, 6);
    void* reserved = interp::EvalStackOp::get_param<void*>(params, 7);
    intptr_t sort_handle = interp::EvalStackOp::get_param<intptr_t>(params, 8);

    int32_t result = platform::RtSys::compare_string_ex(locale_name, compare_flags, string1, string1_length, string2, string2_length,
                                                        version_information, reserved, sort_handle);
    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

RtResultVoid string_intern_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                   interp::RtStackObject*) noexcept
{
    auto string_slot = interp::EvalStackOp::get_param<vm::RtString**>(params, 0);
    if (string_slot != nullptr && *string_slot != nullptr)
    {
        *string_slot = vm::String::intern_string(*string_slot);
    }
    RET_VOID_OK();
}

RtResultVoid string_is_interned_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                        interp::RtStackObject*) noexcept
{
    auto string_slot = interp::EvalStackOp::get_param<vm::RtString**>(params, 0);
    if (string_slot != nullptr)
    {
        *string_slot = vm::String::get_interned_string(*string_slot);
    }
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
                                            vm::Reflection::get_type_sig_from_qcall_type_handle(qcall_type_handle, native_handle));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));
    RET_ERR_ON_FAIL(vm::Runtime::run_class_static_constructor(klass));
    RET_VOID_OK();
}

RtResultVoid runtime_helpers_run_module_constructor_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                            const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto qcall_module = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtModuleDef*, module, vm::Reflection::get_module_from_qcall_module(qcall_module, native_handle));
    RET_ERR_ON_FAIL(vm::Runtime::run_module_static_constructor(module));
    RET_VOID_OK();
}

RtResultVoid runtime_helpers_allocate_uninitialized_clone_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                                  const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto obj_slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 0);
    if (obj_slot == nullptr || *obj_slot == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    vm::RtObject* source = *obj_slot;
    const metadata::RtClass* klass = source->klass;
    if (vm::Class::is_string_class(klass))
    {
        RET_ERR(RtErr::Argument);
    }

    vm::RtObject* clone = nullptr;
    if (vm::Class::is_array_or_szarray(klass))
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, array_clone,
                                                LEANCLR_CLONE_INTERNAL(source, "RuntimeHelpers_AllocateUninitializedClone"));
        clone = array_clone;
    }
    else
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, new_obj,
                                                LEANCLR_NEWOBJ_INTERNAL(klass, "RuntimeHelpers_AllocateUninitializedClone"));
        clone = new_obj;
    }

    *obj_slot = clone;
    RET_VOID_OK();
}

RtResultVoid buffer_memmove_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                    interp::RtStackObject*) noexcept
{
    auto destination = interp::EvalStackOp::get_param<uint8_t*>(params, 0);
    auto source = interp::EvalStackOp::get_param<const uint8_t*>(params, 1);
    uintptr_t length = interp::EvalStackOp::get_param<uintptr_t>(params, 2);
    if (length != 0 && (destination == nullptr || source == nullptr))
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    std::memmove(destination, source, static_cast<size_t>(length));
    RET_VOID_OK();
}

RtResultVoid buffer_clear_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                  interp::RtStackObject*) noexcept
{
    auto destination = interp::EvalStackOp::get_param<void*>(params, 0);
    uintptr_t length = interp::EvalStackOp::get_param<uintptr_t>(params, 1);
    if (length != 0 && destination == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    std::memset(destination, 0, static_cast<size_t>(length));
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

RtResultVoid assembly_get_executing_assembly_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                     const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto stack_mark = interp::EvalStackOp::get_param<vm::RtStackCrawlMark*>(params, 0);
    auto ret_assembly = interp::EvalStackOp::get_param<vm::RtObject**>(params, 1);
    if (ret_assembly == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionAssembly*, assembly, get_executing_assembly_for_stack_mark(stack_mark));
    *ret_assembly = reinterpret_cast<vm::RtObject*>(assembly);
    RET_VOID_OK();
}

RtResultVoid assembly_get_entry_assembly_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                                 interp::RtStackObject*) noexcept
{
    auto ret_assembly = interp::EvalStackOp::get_param<vm::RtObject**>(params, 0);
    if (ret_assembly == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionAssembly*, assembly, get_entry_assembly());
    *ret_assembly = reinterpret_cast<vm::RtObject*>(assembly);
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
    auto method_table = interp::EvalStackOp::get_param<const void*>(params, 0);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtClass*, klass,
                                            vm::Reflection::get_class_from_net10_method_table(method_table));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, can_compare,
                                            can_compare_bits_or_use_fast_get_hash_code(klass));
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
    auto method_arg = interp::EvalStackOp::get_param<const void*>(params, 2);
    (void)interp::EvalStackOp::get_param<void*>(params, 3);
    (void)interp::EvalStackOp::get_param<void*>(params, 4);
    (void)interp::EvalStackOp::get_param<int32_t>(params, 5);

    if (delegate_slot == nullptr || *delegate_slot == nullptr || method_arg == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtMethodInfo*, method,
                                            vm::Reflection::get_method_info_from_handle_arg(method_arg));

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

RtResultVoid get_rva_field_info_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                        interp::RtStackObject* ret) noexcept
{
    auto field_arg = interp::EvalStackOp::get_param<const void*>(params, 0);
    auto data = interp::EvalStackOp::get_param<void**>(params, 1);
    auto length = interp::EvalStackOp::get_param<uint32_t*>(params, 2);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtFieldInfo*, field,
                                            vm::Reflection::get_field_info_from_handle_arg(field_arg));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(int32_t, result, get_rva_field_info(field, data, length));
    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

RtResultVoid runtime_field_handle_get_value_direct_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                           const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto field_arg = interp::EvalStackOp::get_param<const void*>(params, 0);
    auto typed_ref = interp::EvalStackOp::get_param<vm::RtTypedReference*>(params, 1);
    (void)interp::EvalStackOp::get_param<void*>(params, 2);
    (void)interp::EvalStackOp::get_param<void*>(params, 3);
    (void)interp::EvalStackOp::get_param<void*>(params, 4);
    (void)interp::EvalStackOp::get_param<void*>(params, 5);
    auto result_slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 6);
    if (typed_ref == nullptr || result_slot == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtFieldInfo*, field,
                                            vm::Reflection::get_field_info_from_handle_arg(field_arg));
    vm::RtObject* result = nullptr;
    if (vm::Class::is_reference_type(field->parent))
    {
        auto target = *reinterpret_cast<vm::RtObject**>(const_cast<void*>(typed_ref->value));
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, value,
                                                vm::Field::get_value_object(field, target));
        result = value;
    }
    else
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, value,
                                                vm::Field::get_value_direct(field, const_cast<void*>(typed_ref->value)));
        result = value;
    }

    *result_slot = result;
    RET_VOID_OK();
}

RtResultVoid runtime_field_handle_set_value_direct_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                           const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto field_arg = interp::EvalStackOp::get_param<const void*>(params, 0);
    auto typed_ref = interp::EvalStackOp::get_param<vm::RtTypedReference*>(params, 1);
    auto value_slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 2);
    (void)interp::EvalStackOp::get_param<void*>(params, 3);
    (void)interp::EvalStackOp::get_param<void*>(params, 4);
    (void)interp::EvalStackOp::get_param<void*>(params, 5);
    (void)interp::EvalStackOp::get_param<void*>(params, 6);
    if (typed_ref == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtFieldInfo*, field,
                                            vm::Reflection::get_field_info_from_handle_arg(field_arg));
    vm::RtObject* value = value_slot != nullptr ? *value_slot : nullptr;
    if (vm::Class::is_reference_type(field->parent))
    {
        auto target = *reinterpret_cast<vm::RtObject**>(const_cast<void*>(typed_ref->value));
        return vm::Field::set_value_object(field, target, value);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, is_reference_type, vm::Type::is_reference_type(field->type_sig));
    void* ptr_field_value = is_reference_type ? static_cast<void*>(&value) : static_cast<void*>(value ? value + 1 : nullptr);
    return vm::Field::set_value_direct(field, const_cast<void*>(typed_ref->value), ptr_field_value);
}

RtResultVoid runtime_field_handle_set_value_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                    const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto field_arg = interp::EvalStackOp::get_param<const void*>(params, 0);
    auto obj_slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 1);
    auto value_slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 2);
    (void)interp::EvalStackOp::get_param<void*>(params, 3);
    (void)interp::EvalStackOp::get_param<void*>(params, 4);
    (void)interp::EvalStackOp::get_param<void*>(params, 5);
    (void)interp::EvalStackOp::get_param<void*>(params, 6);
    auto is_class_initialized = interp::EvalStackOp::get_param<int32_t*>(params, 7);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtFieldInfo*, field,
                                            vm::Reflection::get_field_info_from_handle_arg(field_arg));
    vm::RtObject* obj = obj_slot != nullptr ? *obj_slot : nullptr;
    vm::RtObject* value = value_slot != nullptr ? *value_slot : nullptr;
    RET_ERR_ON_FAIL(vm::Field::set_value_object(field, obj, value));
    if (is_class_initialized != nullptr)
    {
        *is_class_initialized = 1;
    }
    RET_VOID_OK();
}

RtResultVoid get_declaring_type_handle_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                               const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    auto type_handle = interp::EvalStackOp::get_param<void*>(params, 0);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const void*, declaring_type_handle,
                                            get_declaring_type_handle(type_handle));
    interp::EvalStackOp::set_return(ret, declaring_type_handle);
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

RtResultVoid runtime_method_handle_get_method_instantiation_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                                    const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto method_arg = interp::EvalStackOp::get_param<const void*>(params, 0);
    auto types_slot = interp::EvalStackOp::get_param<vm::RtArray**>(params, 1);
    bool runtime_array = interp::EvalStackOp::get_param<int32_t>(params, 2) != 0;
    if (types_slot == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtMethodInfo*, method,
                                            vm::Reflection::get_method_info_from_handle_arg(method_arg));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, instantiation,
                                            icalls::SystemRuntimeMethodHandle::get_method_instantiation(method, runtime_array));
    *types_slot = instantiation;
    RET_VOID_OK();
}

RtResultVoid runtime_method_handle_get_stub_if_needed_slow_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                                   const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    auto method_arg = interp::EvalStackOp::get_param<const void*>(params, 0);
    (void)interp::EvalStackOp::get_param<void*>(params, 1);
    (void)interp::EvalStackOp::get_param<void*>(params, 2);
    auto method_instantiation_slot = interp::EvalStackOp::get_param<vm::RtArray**>(params, 3);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtMethodInfo*, method,
                                            vm::Reflection::get_method_info_from_handle_arg(method_arg));
    if (method_instantiation_slot != nullptr && *method_instantiation_slot != nullptr)
    {
        vm::RtArray* method_instantiation = *method_instantiation_slot;
        uint8_t generic_param_count = static_cast<uint8_t>(vm::Array::get_array_length(method_instantiation));
        const metadata::RtTypeSig** arg_list =
            static_cast<const metadata::RtTypeSig**>(alloca(sizeof(metadata::RtTypeSig*) * generic_param_count));
        for (uint8_t i = 0; i < generic_param_count; ++i)
        {
            auto arg_obj = vm::Array::get_array_data_at<vm::RtReflectionType*>(method_instantiation, i);
            UNWRAP_OR_RET_ERR_ON_FAIL(arg_list[i], vm::Reflection::get_type_sig_from_reflection_type_object(arg_obj));
        }

        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtGenericInst*, method_inst,
                                                metadata::MetadataCache::get_pooled_generic_inst(arg_list, generic_param_count));
        const metadata::RtGenericInst* class_inst =
            method->generic_method != nullptr ? method->generic_method->generic_context.class_inst : nullptr;
        const metadata::RtMethodInfo* base_method = method;
        if (method->generic_method != nullptr)
        {
            UNWRAP_OR_RET_ERR_ON_FAIL(base_method, vm::Method::get_method_by_method_def_gid(method->generic_method->base_method_gid));
        }

        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtMethodInfo*, inflated_method,
                                                vm::GenericMethod::get_method(base_method, class_inst, method_inst));
        interp::EvalStackOp::set_return(ret, inflated_method);
        RET_VOID_OK();
    }

    interp::EvalStackOp::set_return(ret, method);
    RET_VOID_OK();
}

RtResultVoid runtime_method_handle_strip_method_instantiation_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                                      const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto method_arg = interp::EvalStackOp::get_param<const void*>(params, 0);
    auto out_method_slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 1);
    if (out_method_slot == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtMethodInfo*, method,
                                            vm::Reflection::get_method_info_from_handle_arg(method_arg));
    if (method->generic_method != nullptr && method->generic_method->generic_context.method_inst != nullptr)
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtMethodInfo*, base_method,
                                                vm::Method::get_method_by_method_def_gid(method->generic_method->base_method_gid));
        const metadata::RtMethodInfo* stripped_method = base_method;
        const metadata::RtGenericInst* class_inst = method->generic_method->generic_context.class_inst;
        if (class_inst != nullptr)
        {
            UNWRAP_OR_RET_ERR_ON_FAIL(stripped_method, vm::GenericMethod::get_method(base_method, class_inst, nullptr));
        }

        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionMethod*, reflection_method,
                                                vm::Reflection::get_method_reflection_object(stripped_method, stripped_method->parent));
        *out_method_slot = reinterpret_cast<vm::RtObject*>(reflection_method);
    }

    RET_VOID_OK();
}

RtResultVoid runtime_type_handle_get_method_at_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                       const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    auto method_table = interp::EvalStackOp::get_param<const void*>(params, 0);
    int32_t slot = interp::EvalStackOp::get_param<int32_t>(params, 1);
    if (slot < 0)
    {
        RET_ERR(RtErr::ArgumentOutOfRange);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, type_sig,
                                            vm::Reflection::get_type_sig_from_runtime_type_handle_arg(method_table));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));
    RET_ERR_ON_FAIL(vm::Class::initialize_methods(klass));
    RET_ERR_ON_FAIL(vm::Class::initialize_vtables(klass));

    const metadata::RtMethodInfo* result = nullptr;
    if (slot < klass->vtable_count)
    {
        result = klass->vtable[slot].method_impl;
    }
    else
    {
        for (uint16_t i = 0; i < klass->method_count; ++i)
        {
            const metadata::RtMethodInfo* method = klass->methods[i];
            if (method != nullptr && method->slot == slot)
            {
                result = method;
                break;
            }
        }
    }

    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

RtResultVoid runtime_type_handle_instantiate_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                     const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto qcall_type_handle = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto type_handles = interp::EvalStackOp::get_param<void**>(params, 2);
    int32_t type_handle_count = interp::EvalStackOp::get_param<int32_t>(params, 3);
    auto ret_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType**>(params, 4);
    if (ret_type == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionRuntimeType*, instantiated_type,
                                            instantiate_runtime_type(qcall_type_handle, native_handle, type_handles, type_handle_count));
    *ret_type = instantiated_type;
    RET_VOID_OK();
}

RtResultVoid runtime_type_handle_make_array_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                    const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto qcall_type_handle = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    int32_t rank = interp::EvalStackOp::get_param<int32_t>(params, 2);
    auto ret_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType**>(params, 3);
    if (ret_type == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionRuntimeType*, array_type,
                                            make_array_runtime_type(qcall_type_handle, native_handle, rank));
    *ret_type = array_type;
    RET_VOID_OK();
}

RtResultVoid runtime_type_handle_make_szarray_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                      const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto qcall_type_handle = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto ret_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType**>(params, 2);
    if (ret_type == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionRuntimeType*, array_type,
                                            make_szarray_runtime_type(qcall_type_handle, native_handle));
    *ret_type = array_type;
    RET_VOID_OK();
}

RtResultVoid runtime_type_handle_make_byref_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                    const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto qcall_type_handle = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto ret_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType**>(params, 2);
    if (ret_type == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionRuntimeType*, byref_type,
                                            make_byref_runtime_type(qcall_type_handle, native_handle));
    *ret_type = byref_type;
    RET_VOID_OK();
}

RtResultVoid runtime_type_handle_make_pointer_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                      const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto qcall_type_handle = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto ret_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType**>(params, 2);
    if (ret_type == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionRuntimeType*, pointer_type,
                                            make_pointer_runtime_type(qcall_type_handle, native_handle));
    *ret_type = pointer_type;
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

RtResultVoid module_handle_get_md_stream_version_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                         const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    auto qcall_module = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(int32_t, version, get_module_md_stream_version(qcall_module, native_handle));
    interp::EvalStackOp::set_return(ret, version);
    RET_VOID_OK();
}

RtResultVoid assembly_get_modules_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                          interp::RtStackObject*) noexcept
{
    auto qcall_assembly = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto modules_slot = interp::EvalStackOp::get_param<vm::RtArray**>(params, 4);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, modules, get_assembly_modules(qcall_assembly, native_handle));
    if (modules_slot != nullptr)
    {
        *modules_slot = modules;
    }
    RET_VOID_OK();
}

RtResultVoid assembly_get_full_name_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                            interp::RtStackObject*) noexcept
{
    auto qcall_assembly = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto ret_string = interp::EvalStackOp::get_param<vm::RtString**>(params, 2);
    if (ret_string == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtString*, full_name, get_assembly_full_name(qcall_assembly, native_handle));
    *ret_string = full_name;
    RET_VOID_OK();
}

RtResultVoid assembly_get_image_runtime_version_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                        const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto qcall_assembly = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto ret_string = interp::EvalStackOp::get_param<vm::RtString**>(params, 2);
    if (ret_string == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtString*, runtime_version,
                                            get_assembly_image_runtime_version(qcall_assembly, native_handle));
    *ret_string = runtime_version;
    RET_VOID_OK();
}

RtResultVoid assembly_get_entry_point_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                              interp::RtStackObject*) noexcept
{
    auto qcall_assembly = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto ret_method = interp::EvalStackOp::get_param<vm::RtObject**>(params, 2);
    if (ret_method == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionMethod*, entry_point,
                                            get_assembly_entry_point(qcall_assembly, native_handle));
    *ret_method = reinterpret_cast<vm::RtObject*>(entry_point);
    RET_VOID_OK();
}

RtResultVoid assembly_get_manifest_resource_names_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                          const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto qcall_assembly = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto ret_resource_names = interp::EvalStackOp::get_param<vm::RtArray**>(params, 2);
    if (ret_resource_names == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, resource_names,
                                            get_assembly_manifest_resource_names(qcall_assembly, native_handle));
    *ret_resource_names = resource_names;
    RET_VOID_OK();
}

RtResultVoid assembly_get_simple_name_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                              interp::RtStackObject*) noexcept
{
    auto qcall_assembly = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto ret_string = interp::EvalStackOp::get_param<vm::RtString**>(params, 2);
    if (ret_string == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtString*, simple_name, get_assembly_simple_name(qcall_assembly, native_handle));
    *ret_string = simple_name;
    RET_VOID_OK();
}

RtResultVoid assembly_get_locale_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                         interp::RtStackObject*) noexcept
{
    auto qcall_assembly = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto ret_string = interp::EvalStackOp::get_param<vm::RtString**>(params, 2);
    if (ret_string == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtString*, locale, get_assembly_locale(qcall_assembly, native_handle));
    *ret_string = locale;
    RET_VOID_OK();
}

RtResultVoid assembly_get_public_key_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                             interp::RtStackObject*) noexcept
{
    auto qcall_assembly = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto ret_public_key = interp::EvalStackOp::get_param<vm::RtArray**>(params, 2);
    if (ret_public_key == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, public_key, get_assembly_public_key(qcall_assembly, native_handle));
    *ret_public_key = public_key;
    RET_VOID_OK();
}

RtResultVoid assembly_get_version_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                          interp::RtStackObject*) noexcept
{
    auto qcall_assembly = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto major = interp::EvalStackOp::get_param<int32_t*>(params, 2);
    auto minor = interp::EvalStackOp::get_param<int32_t*>(params, 3);
    auto build = interp::EvalStackOp::get_param<int32_t*>(params, 4);
    auto revision = interp::EvalStackOp::get_param<int32_t*>(params, 5);
    if (major == nullptr || minor == nullptr || build == nullptr || revision == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtAssemblyName*, assembly_name,
                                            get_qcall_assembly_name(qcall_assembly, native_handle));
    *major = assembly_name->version_major;
    *minor = assembly_name->version_minor;
    *build = assembly_name->version_build;
    *revision = assembly_name->version_revision;
    RET_VOID_OK();
}

RtResultVoid assembly_get_flags_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                        interp::RtStackObject* ret) noexcept
{
    auto qcall_assembly = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtAssemblyName*, assembly_name,
                                            get_qcall_assembly_name(qcall_assembly, native_handle));
    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(assembly_name->flags));
    RET_VOID_OK();
}

RtResultVoid assembly_get_hash_algorithm_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                 const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    auto qcall_assembly = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtAssemblyName*, assembly_name,
                                            get_qcall_assembly_name(qcall_assembly, native_handle));
    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(assembly_name->hash_algorithm));
    RET_VOID_OK();
}

RtResultVoid assembly_get_code_base_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                            interp::RtStackObject* ret) noexcept
{
    auto ret_string = interp::EvalStackOp::get_param<vm::RtString**>(params, 2);
    if (ret_string != nullptr)
    {
        *ret_string = nullptr;
    }

    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(0));
    RET_VOID_OK();
}

RtResultVoid assembly_get_referenced_assemblies_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                        const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto qcall_assembly = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto referenced_assemblies_slot = interp::EvalStackOp::get_param<vm::RtArray**>(params, 2);
    if (referenced_assemblies_slot == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, referenced_assemblies,
                                            get_assembly_referenced_assemblies(qcall_assembly, native_handle));
    *referenced_assemblies_slot = referenced_assemblies;
    RET_VOID_OK();
}

RtResultVoid assembly_internal_load_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                            interp::RtStackObject*) noexcept
{
    auto name_parts = interp::EvalStackOp::get_param<NativeAssemblyNameParts*>(params, 0);
    (void)interp::EvalStackOp::get_param<vm::RtObject**>(params, 1);
    (void)interp::EvalStackOp::get_param<void*>(params, 2);
    bool throw_on_file_not_found = interp::EvalStackOp::get_param<int32_t>(params, 3) != 0;
    (void)interp::EvalStackOp::get_param<vm::RtObject**>(params, 4);
    auto ret_assembly = interp::EvalStackOp::get_param<vm::RtObject**>(params, 5);
    if (ret_assembly == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionAssembly*, assembly,
                                            load_runtime_assembly(name_parts, throw_on_file_not_found));
    *ret_assembly = reinterpret_cast<vm::RtObject*>(assembly);
    RET_VOID_OK();
}

RtResultVoid runtime_type_handle_get_fields_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                                    interp::RtStackObject* ret) noexcept
{
    auto method_table = interp::EvalStackOp::get_param<void*>(params, 0);
    auto data_ptr = interp::EvalStackOp::get_param<void**>(params, 1);
    auto used_count = interp::EvalStackOp::get_param<int32_t*>(params, 2);
    RtIntPtrSpan data{data_ptr, used_count != nullptr ? *used_count : 0};

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(int32_t, result, get_runtime_type_fields(method_table, data, used_count));
    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

RtResultVoid runtime_type_handle_get_interfaces_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                        const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto method_table = interp::EvalStackOp::get_param<void*>(params, 0);
    auto result_slot = interp::EvalStackOp::get_param<vm::RtArray**>(params, 1);
    if (result_slot == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtArray*, interfaces, get_runtime_type_interfaces(method_table));
    *result_slot = interfaces;
    RET_VOID_OK();
}

RtResultVoid runtime_type_handle_register_collectible_type_dependency_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                                              const interp::RtStackObject*, interp::RtStackObject*) noexcept
{
    // LeanCLR does not model collectible AssemblyLoadContext / LoaderAllocator yet.
    // The .NET QCall is therefore a no-op for the current minimal net10 profile.
    RET_VOID_OK();
}

RtResultVoid module_handle_get_token_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                             interp::RtStackObject* ret) noexcept
{
    auto qcall_module = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(int32_t, token, get_module_token(qcall_module, native_handle));
    interp::EvalStackOp::set_return(ret, token);
    RET_VOID_OK();
}

RtResultVoid module_handle_get_module_type_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                   const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto qcall_module = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto ret_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType**>(params, 2);
    if (ret_type == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionRuntimeType*, module_type,
                                            get_module_runtime_type(qcall_module, native_handle));
    *ret_type = module_type;
    RET_VOID_OK();
}

RtResultVoid runtime_module_get_name_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                             interp::RtStackObject*) noexcept
{
    auto qcall_module = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto ret_string = interp::EvalStackOp::get_param<vm::RtString**>(params, 2);
    if (ret_string == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtString*, name, get_runtime_module_name(qcall_module, native_handle));
    *ret_string = name;
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
                                            vm::Reflection::get_assembly_from_qcall_assembly(qcall_assembly, native_handle));
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
                                            vm::Reflection::get_assembly_from_qcall_assembly(qcall_assembly, native_handle));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionType*, type,
                                            get_runtime_assembly_type_core_ignore_case(assembly, type_name, nested_type_names,
                                                                                       nested_type_names_length));
    *ret_type = type;
    RET_VOID_OK();
}

RtResultVoid metadata_import_enum_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                          interp::RtStackObject*) noexcept
{
    auto metadata_import_arg = interp::EvalStackOp::get_param<const void*>(params, 0);
    int32_t token_type = interp::EvalStackOp::get_param<int32_t>(params, 1);
    int32_t parent_token = interp::EvalStackOp::get_param<int32_t>(params, 2);
    auto count = interp::EvalStackOp::get_param<int32_t*>(params, 3);
    auto result_buffer = interp::EvalStackOp::get_param<int32_t*>(params, 4);
    auto large_result = interp::EvalStackOp::get_param<vm::RtArray**>(params, 5);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtModuleDef*, metadata_import,
                                            vm::Reflection::get_module_from_handle_arg(metadata_import_arg));
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

RtResultVoid module_handle_resolve_type_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                                interp::RtStackObject*) noexcept
{
    auto qcall_module = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    int32_t type_token = interp::EvalStackOp::get_param<int32_t>(params, 2);
    auto type_inst_args = interp::EvalStackOp::get_param<void**>(params, 3);
    int32_t type_inst_count = interp::EvalStackOp::get_param<int32_t>(params, 4);
    auto method_inst_args = interp::EvalStackOp::get_param<void**>(params, 5);
    int32_t method_inst_count = interp::EvalStackOp::get_param<int32_t>(params, 6);
    auto ret_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType**>(params, 7);
    if (ret_type == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionRuntimeType*, type,
                                            resolve_module_type(qcall_module, native_handle, type_token, type_inst_args, type_inst_count,
                                                                method_inst_args, method_inst_count));
    *ret_type = type;
    RET_VOID_OK();
}

RtResultVoid module_handle_resolve_field_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                                 interp::RtStackObject*) noexcept
{
    auto qcall_module = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    int32_t field_token = interp::EvalStackOp::get_param<int32_t>(params, 2);
    auto type_inst_args = interp::EvalStackOp::get_param<void**>(params, 3);
    int32_t type_inst_count = interp::EvalStackOp::get_param<int32_t>(params, 4);
    auto method_inst_args = interp::EvalStackOp::get_param<void**>(params, 5);
    int32_t method_inst_count = interp::EvalStackOp::get_param<int32_t>(params, 6);
    auto ret_field = interp::EvalStackOp::get_param<vm::RtObject**>(params, 7);
    if (ret_field == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtFieldInfo*, field,
                                            resolve_module_field(qcall_module, native_handle, field_token, type_inst_args, type_inst_count,
                                                                 method_inst_args, method_inst_count));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, stub, vm::Reflection::create_runtime_field_info_stub(field));
    *ret_field = stub;
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
                                            vm::Reflection::get_type_sig_from_qcall_type_handle(method_table, method_table));
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
    auto field_arg = interp::EvalStackOp::get_param<const void*>(params, 3);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtFieldInfo*, field,
                                            vm::Reflection::get_field_info_from_handle_arg(field_arg));
    auto method_arg = interp::EvalStackOp::get_param<const void*>(params, 4);
    const metadata::RtMethodInfo* method = nullptr;
    if (method_arg != nullptr)
    {
        UNWRAP_OR_RET_ERR_ON_FAIL(method, vm::Reflection::get_method_info_from_handle_arg(method_arg));
    }

    vm::RtSignature* signature = signature_slot != nullptr ? *signature_slot : nullptr;
    RET_ERR_ON_FAIL(initialize_signature_from_metadata(signature, raw_sig, raw_sig_size, field, method));
    RET_VOID_OK();
}

RtResultVoid signature_get_custom_modifiers_at_offset_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                              const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    (void)interp::EvalStackOp::get_param<void*>(params, 0);
    (void)interp::EvalStackOp::get_param<int32_t>(params, 1);
    (void)interp::EvalStackOp::get_param<int32_t>(params, 2);
    auto result_slot = interp::EvalStackOp::get_param<vm::RtArray**>(params, 3);
    if (result_slot == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(
        vm::RtArray*, result,
        LEANCLR_NEW_EMPTY_SZARRAY_BY_ELE_KLASS_INTERNAL(vm::Class::get_corlib_types().cls_systemtype,
                                                        "Signature_GetCustomModifiersAtOffset"));
    *result_slot = result;
    RET_VOID_OK();
}

RtResultVoid runtime_method_handle_get_method_body_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                           const interp::RtStackObject* params, interp::RtStackObject*) noexcept
{
    auto method_arg = interp::EvalStackOp::get_param<const void*>(params, 0);
    (void)interp::EvalStackOp::get_param<void*>(params, 1);
    (void)interp::EvalStackOp::get_param<void*>(params, 2);
    auto result = interp::EvalStackOp::get_param<vm::RtReflectionMethodBody**>(params, 3);

    if (result == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtMethodInfo*, method,
                                            vm::Reflection::get_method_info_from_handle_arg(method_arg));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionMethodBody*, body, vm::Method::create_reflection_method_body(method));
    *result = body;
    RET_VOID_OK();
}

RtResultVoid runtime_method_handle_is_ca_visible_from_decorated_type_invoker(metadata::RtManagedMethodPointer,
                                                                             const metadata::RtMethodInfo*,
                                                                             const interp::RtStackObject* params,
                                                                             interp::RtStackObject* ret) noexcept
{
    auto attr_qcall_type_handle = interp::EvalStackOp::get_param<void*>(params, 0);
    auto attr_native_handle = interp::EvalStackOp::get_param<void*>(params, 1);
    auto attr_ctor_arg = interp::EvalStackOp::get_param<const void*>(params, 2);
    auto source_qcall_type_handle = interp::EvalStackOp::get_param<void*>(params, 3);
    auto source_native_handle = interp::EvalStackOp::get_param<void*>(params, 4);
    auto source_qcall_module = interp::EvalStackOp::get_param<void*>(params, 5);
    auto source_native_module = interp::EvalStackOp::get_param<void*>(params, 6);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtMethodInfo*, attr_ctor,
                                            vm::Reflection::get_method_info_from_handle_arg(attr_ctor_arg));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, visible,
                                            is_ca_visible_from_decorated_type(attr_qcall_type_handle, attr_native_handle, attr_ctor,
                                                                             source_qcall_type_handle, source_native_handle,
                                                                             source_qcall_module, source_native_module));
    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(visible));
    RET_VOID_OK();
}

RtResultVoid custom_attribute_create_custom_attribute_instance_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*,
                                                                       const interp::RtStackObject* params,
                                                                       interp::RtStackObject*) noexcept
{
    auto qcall_module = interp::EvalStackOp::get_param<void*>(params, 0);
    auto native_module = interp::EvalStackOp::get_param<void*>(params, 1);
    (void)interp::EvalStackOp::get_param<vm::RtObject**>(params, 2);
    auto ctor_slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 3);
    auto blob_start = interp::EvalStackOp::get_param<void**>(params, 4);
    auto blob_end = interp::EvalStackOp::get_param<void*>(params, 5);
    auto named_arg_count = interp::EvalStackOp::get_param<int32_t*>(params, 6);
    auto instance_slot = interp::EvalStackOp::get_param<vm::RtObject**>(params, 7);

    if (ctor_slot == nullptr || *ctor_slot == nullptr || instance_slot == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtModuleDef*, module,
                                            vm::Reflection::get_module_from_qcall_module(qcall_module, native_module));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, instance,
                                            create_custom_attribute_instance(module, *ctor_slot, blob_start, blob_end, named_arg_count));
    *instance_slot = instance;
    RET_VOID_OK();
}

RtResultVoid materialize_runtime_method_handle_invoke_args(const metadata::RtMethodInfo* method, void** args,
                                                           utils::Vector<vm::RtObject*>& invoke_args,
                                                           RtObjectSlotRootGuard& roots) noexcept
{
    assert(method != nullptr);

    size_t parameter_count = static_cast<size_t>(method->parameter_count);
    if (parameter_count == 0)
    {
        RET_VOID_OK();
    }
    if (args == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    invoke_args.resize(parameter_count);
    for (size_t i = 0; i < parameter_count; ++i)
    {
        const metadata::RtTypeSig* parameter_type_sig = method->parameters[i];
        if (parameter_type_sig == nullptr)
        {
            RET_ERR(RtErr::ArgumentNull);
        }
        if (parameter_type_sig->by_ref)
        {
            RET_ERR(RtErr::NotSupported);
        }

        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, parameter_class, vm::Class::get_class_from_typesig(parameter_type_sig));
        RET_ERR_ON_FAIL(vm::Class::initialize_all(parameter_class));

        void* arg = args[i];
        if (vm::Class::is_value_type(parameter_class))
        {
            if (arg == nullptr)
            {
                RET_ERR(RtErr::ArgumentNull);
            }

            DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, boxed_arg,
                                                    LEANCLR_BOX_OBJECT_INTERNAL(parameter_class, arg,
                                                                               "RuntimeMethodHandle_InvokeMethod"));
            invoke_args[i] = boxed_arg;
        }
        else
        {
            invoke_args[i] = arg != nullptr ? *reinterpret_cast<vm::RtObject**>(arg) : nullptr;
        }
        roots.register_slot(&invoke_args[i]);
    }

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
    RtObjectSlotRootGuard roots;
    utils::Vector<vm::RtObject*> invoke_args;
    RET_ERR_ON_FAIL(materialize_runtime_method_handle_invoke_args(method, args, invoke_args, roots));

    vm::RtObject** invoke_args_data = invoke_args.size() > 0 ? invoke_args.data() : nullptr;
    vm::RtObject* target = target_slot != nullptr ? *target_slot : nullptr;
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtObject*, result,
                                            vm::Runtime::invoke_object_arguments_with_run_cctor(
                                                method, is_constructor ? nullptr : target, invoke_args_data,
                                                static_cast<int32_t>(method->parameter_count)));

    *result_slot = result;
    RET_VOID_OK();
}

} // namespace

void register_coreclr_qcall_pinvokes() noexcept
{
    vm::PInvokes::register_pinvoke("System.Threading.Thread::GetCurrentThread(System.Runtime.CompilerServices.ObjectHandleOnStack)", nullptr,
                                   get_current_thread_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::GetCurrentThread", nullptr, get_current_thread_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::Initialize(System.Runtime.CompilerServices.ObjectHandleOnStack)", nullptr,
                                   thread_initialize_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::Initialize", nullptr, thread_initialize_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_Initialize(System.Runtime.CompilerServices.ObjectHandleOnStack)", nullptr,
                                   thread_initialize_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_Initialize", nullptr, thread_initialize_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::GetIsBackground(System.Threading.ThreadHandle)", nullptr,
                                   thread_get_is_background_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::GetIsBackground", nullptr, thread_get_is_background_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_GetIsBackground(System.Threading.ThreadHandle)", nullptr, thread_get_is_background_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_GetIsBackground", nullptr, thread_get_is_background_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::SetIsBackground(System.Threading.ThreadHandle,Interop/BOOL)", nullptr,
                                   thread_set_is_background_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::SetIsBackground", nullptr, thread_set_is_background_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_SetIsBackground(System.Threading.ThreadHandle,Interop/BOOL)", nullptr,
                                   thread_set_is_background_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_SetIsBackground", nullptr, thread_set_is_background_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::GetThreadState(System.Threading.ThreadHandle)", nullptr,
                                   thread_get_thread_state_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::GetThreadState", nullptr, thread_get_thread_state_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_GetThreadState(System.Threading.ThreadHandle)", nullptr, thread_get_thread_state_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_GetThreadState", nullptr, thread_get_thread_state_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::SetPriority(System.Runtime.CompilerServices.ObjectHandleOnStack,System.Int32)", nullptr,
                                   thread_set_priority_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::SetPriority", nullptr, thread_set_priority_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_SetPriority(System.Runtime.CompilerServices.ObjectHandleOnStack,System.Int32)", nullptr,
                                   thread_set_priority_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_SetPriority", nullptr, thread_set_priority_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Threading.Thread::StartInternal(System.Threading.ThreadHandle,System.Int32,System.Int32,Interop/BOOL,System.Char*)", nullptr,
        thread_start_internal_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::StartInternal", nullptr, thread_start_internal_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_Start(System.Threading.ThreadHandle,System.Int32,System.Int32,Interop/BOOL,System.Char*)", nullptr,
                                   thread_start_internal_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_Start", nullptr, thread_start_internal_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::InformThreadNameChange(System.Threading.ThreadHandle,System.String,System.Int32)", nullptr,
                                   thread_inform_thread_name_change_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::InformThreadNameChange(System.Threading.ThreadHandle,System.Char*,System.Int32)", nullptr,
                                   thread_inform_thread_name_change_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::InformThreadNameChange", nullptr, thread_inform_thread_name_change_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Threading.Thread::<InformThreadNameChange>g____PInvoke|32_0(System.Threading.ThreadHandle,System.UInt16*,System.Int32)", nullptr,
        thread_inform_thread_name_change_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::<InformThreadNameChange>g____PInvoke|32_0", nullptr,
                                   thread_inform_thread_name_change_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_InformThreadNameChange(System.Threading.ThreadHandle,System.String,System.Int32)", nullptr,
                                   thread_inform_thread_name_change_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_InformThreadNameChange(System.Threading.ThreadHandle,System.Char*,System.Int32)", nullptr,
                                   thread_inform_thread_name_change_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_InformThreadNameChange", nullptr, thread_inform_thread_name_change_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::SleepInternal(System.Int32)", nullptr, thread_sleep_internal_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::SleepInternal", nullptr, thread_sleep_internal_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_Sleep(System.Int32)", nullptr, thread_sleep_internal_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_Sleep", nullptr, thread_sleep_internal_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::SpinWaitInternal(System.Int32)", nullptr, thread_spin_wait_internal_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::SpinWaitInternal", nullptr, thread_spin_wait_internal_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_SpinWait(System.Int32)", nullptr, thread_spin_wait_internal_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_SpinWait", nullptr, thread_spin_wait_internal_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::YieldInternal()", nullptr, thread_yield_internal_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::YieldInternal", nullptr, thread_yield_internal_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_YieldThread()", nullptr, thread_yield_internal_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_YieldThread", nullptr, thread_yield_internal_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::GetCurrentOSThreadId()", nullptr, thread_get_current_os_thread_id_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::GetCurrentOSThreadId", nullptr, thread_get_current_os_thread_id_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_GetCurrentOSThreadId()", nullptr, thread_get_current_os_thread_id_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_GetCurrentOSThreadId", nullptr, thread_get_current_os_thread_id_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::Interrupt(System.Threading.ThreadHandle)", nullptr, thread_interrupt_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::Interrupt", nullptr, thread_interrupt_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_Interrupt(System.Threading.ThreadHandle)", nullptr, thread_interrupt_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_Interrupt", nullptr, thread_interrupt_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::Join(System.Runtime.CompilerServices.ObjectHandleOnStack,System.Int32)", nullptr,
                                   thread_join_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::Join", nullptr, thread_join_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_Join(System.Runtime.CompilerServices.ObjectHandleOnStack,System.Int32)", nullptr, thread_join_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_Join", nullptr, thread_join_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::PollGCInternal()", nullptr, thread_poll_gc_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::PollGCInternal", nullptr, thread_poll_gc_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_PollGC()", nullptr, thread_poll_gc_invoker);
    vm::PInvokes::register_pinvoke("ThreadNative_PollGC", nullptr, thread_poll_gc_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.Debugger::IsManagedDebuggerAttached()", nullptr, is_managed_debugger_attached_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.Debugger::IsManagedDebuggerAttached", nullptr, is_managed_debugger_attached_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.Debugger::IsLoggingInternal()", nullptr, is_debugger_logging_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.Debugger::IsLoggingInternal", nullptr, is_debugger_logging_invoker);
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
    vm::PInvokes::register_pinvoke("Interop/Kernel32::QueryPerformanceFrequency(System.Int64*)", nullptr,
                                   kernel32_query_performance_frequency_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::QueryPerformanceFrequency", nullptr, kernel32_query_performance_frequency_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::QueryPerformanceFrequency(System.Int64*)", nullptr,
                                   kernel32_query_performance_frequency_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::QueryPerformanceFrequency", nullptr, kernel32_query_performance_frequency_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::QueryPerformanceFrequency(System.Int64*)", nullptr,
                                   kernel32_query_performance_frequency_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::QueryPerformanceFrequency", nullptr, kernel32_query_performance_frequency_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::QueryPerformanceCounter(System.Int64*)", nullptr,
                                   kernel32_query_performance_counter_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::QueryPerformanceCounter", nullptr, kernel32_query_performance_counter_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::QueryPerformanceCounter(System.Int64*)", nullptr, kernel32_query_performance_counter_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::QueryPerformanceCounter", nullptr, kernel32_query_performance_counter_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::QueryPerformanceCounter(System.Int64*)", nullptr,
                                   kernel32_query_performance_counter_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::QueryPerformanceCounter", nullptr, kernel32_query_performance_counter_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::GetConsoleCP()", nullptr, kernel32_get_console_cp_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::GetConsoleCP", nullptr, kernel32_get_console_cp_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::GetConsoleCP()", nullptr, kernel32_get_console_cp_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::GetConsoleCP", nullptr, kernel32_get_console_cp_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::GetConsoleCP()", nullptr, kernel32_get_console_cp_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::GetConsoleCP", nullptr, kernel32_get_console_cp_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::GetConsoleOutputCP()", nullptr, kernel32_get_console_output_cp_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::GetConsoleOutputCP", nullptr, kernel32_get_console_output_cp_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::GetConsoleOutputCP()", nullptr, kernel32_get_console_output_cp_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::GetConsoleOutputCP", nullptr, kernel32_get_console_output_cp_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::GetConsoleOutputCP()", nullptr, kernel32_get_console_output_cp_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::GetConsoleOutputCP", nullptr, kernel32_get_console_output_cp_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::GetStdHandle(System.Int32)", nullptr, kernel32_get_std_handle_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::GetStdHandle", nullptr, kernel32_get_std_handle_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::GetStdHandle(System.Int32)", nullptr, kernel32_get_std_handle_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::GetStdHandle", nullptr, kernel32_get_std_handle_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::GetStdHandle(System.Int32)", nullptr, kernel32_get_std_handle_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::GetStdHandle", nullptr, kernel32_get_std_handle_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::<WriteFile>g____PInvoke|58_0(System.IntPtr,System.Byte*,System.Int32,System.Int32*,System.IntPtr)",
                                   nullptr, kernel32_write_file_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::<WriteFile>g____PInvoke|58_0", nullptr, kernel32_write_file_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::<WriteFile>g____PInvoke|58_0(System.IntPtr,System.Byte*,System.Int32,System.Int32*,System.IntPtr)",
                                   nullptr, kernel32_write_file_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::<WriteFile>g____PInvoke|58_0", nullptr, kernel32_write_file_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::<WriteFile>g____PInvoke|58_0(System.IntPtr,System.Byte*,System.Int32,System.Int32*,System.IntPtr)",
                                   nullptr, kernel32_write_file_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::<WriteFile>g____PInvoke|58_0", nullptr, kernel32_write_file_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::<GetFileType>g____PInvoke|37_0(System.IntPtr)", nullptr,
                                   kernel32_get_file_type_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::<GetFileType>g____PInvoke|37_0", nullptr, kernel32_get_file_type_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::<GetFileType>g____PInvoke|37_0(System.IntPtr)", nullptr, kernel32_get_file_type_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::<GetFileType>g____PInvoke|37_0", nullptr, kernel32_get_file_type_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::<GetFileType>g____PInvoke|37_0(System.IntPtr)", nullptr, kernel32_get_file_type_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::<GetFileType>g____PInvoke|37_0", nullptr, kernel32_get_file_type_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::<GetFileType>g____PInvoke|140_0(System.IntPtr)", nullptr,
                                   kernel32_get_file_type_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::<GetFileType>g____PInvoke|140_0", nullptr, kernel32_get_file_type_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::<GetFileType>g____PInvoke|140_0(System.IntPtr)", nullptr, kernel32_get_file_type_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::<GetFileType>g____PInvoke|140_0", nullptr, kernel32_get_file_type_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::<GetFileType>g____PInvoke|140_0(System.IntPtr)", nullptr, kernel32_get_file_type_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::<GetFileType>g____PInvoke|140_0", nullptr, kernel32_get_file_type_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::InitializeCriticalSection(Interop/Kernel32/CRITICAL_SECTION*)", nullptr,
                                   kernel32_initialize_critical_section_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::InitializeCriticalSection", nullptr, kernel32_initialize_critical_section_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::InitializeCriticalSection(Interop/Kernel32/CRITICAL_SECTION*)", nullptr,
                                   kernel32_initialize_critical_section_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::InitializeCriticalSection", nullptr, kernel32_initialize_critical_section_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::InitializeCriticalSection(Interop/Kernel32/CRITICAL_SECTION*)", nullptr,
                                   kernel32_initialize_critical_section_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::InitializeCriticalSection", nullptr, kernel32_initialize_critical_section_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::DeleteCriticalSection(Interop/Kernel32/CRITICAL_SECTION*)", nullptr,
                                   kernel32_delete_critical_section_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::DeleteCriticalSection", nullptr, kernel32_delete_critical_section_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::DeleteCriticalSection(Interop/Kernel32/CRITICAL_SECTION*)", nullptr,
                                   kernel32_delete_critical_section_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::DeleteCriticalSection", nullptr, kernel32_delete_critical_section_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::DeleteCriticalSection(Interop/Kernel32/CRITICAL_SECTION*)", nullptr,
                                   kernel32_delete_critical_section_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::DeleteCriticalSection", nullptr, kernel32_delete_critical_section_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::EnterCriticalSection(Interop/Kernel32/CRITICAL_SECTION*)", nullptr,
                                   kernel32_enter_critical_section_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::EnterCriticalSection", nullptr, kernel32_enter_critical_section_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::EnterCriticalSection(Interop/Kernel32/CRITICAL_SECTION*)", nullptr,
                                   kernel32_enter_critical_section_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::EnterCriticalSection", nullptr, kernel32_enter_critical_section_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::EnterCriticalSection(Interop/Kernel32/CRITICAL_SECTION*)", nullptr,
                                   kernel32_enter_critical_section_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::EnterCriticalSection", nullptr, kernel32_enter_critical_section_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::LeaveCriticalSection(Interop/Kernel32/CRITICAL_SECTION*)", nullptr,
                                   kernel32_leave_critical_section_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::LeaveCriticalSection", nullptr, kernel32_leave_critical_section_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::LeaveCriticalSection(Interop/Kernel32/CRITICAL_SECTION*)", nullptr,
                                   kernel32_leave_critical_section_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::LeaveCriticalSection", nullptr, kernel32_leave_critical_section_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::LeaveCriticalSection(Interop/Kernel32/CRITICAL_SECTION*)", nullptr,
                                   kernel32_leave_critical_section_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::LeaveCriticalSection", nullptr, kernel32_leave_critical_section_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::InitializeConditionVariable(Interop/Kernel32/CONDITION_VARIABLE*)", nullptr,
                                   kernel32_initialize_condition_variable_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::InitializeConditionVariable", nullptr,
                                   kernel32_initialize_condition_variable_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::InitializeConditionVariable(Interop/Kernel32/CONDITION_VARIABLE*)", nullptr,
                                   kernel32_initialize_condition_variable_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::InitializeConditionVariable", nullptr, kernel32_initialize_condition_variable_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::InitializeConditionVariable(Interop/Kernel32/CONDITION_VARIABLE*)", nullptr,
                                   kernel32_initialize_condition_variable_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::InitializeConditionVariable", nullptr, kernel32_initialize_condition_variable_invoker);
    vm::PInvokes::register_pinvoke(
        "Interop/Kernel32::<SleepConditionVariableCS>g____PInvoke|57_0(Interop/Kernel32/CONDITION_VARIABLE*,Interop/Kernel32/CRITICAL_SECTION*,System.Int32)",
        nullptr, kernel32_sleep_condition_variable_cs_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::<SleepConditionVariableCS>g____PInvoke|57_0", nullptr,
                                   kernel32_sleep_condition_variable_cs_invoker);
    vm::PInvokes::register_pinvoke(
        "Kernel32::<SleepConditionVariableCS>g____PInvoke|57_0(Interop/Kernel32/CONDITION_VARIABLE*,Interop/Kernel32/CRITICAL_SECTION*,System.Int32)",
        nullptr, kernel32_sleep_condition_variable_cs_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::<SleepConditionVariableCS>g____PInvoke|57_0", nullptr,
                                   kernel32_sleep_condition_variable_cs_invoker);
    vm::PInvokes::register_pinvoke(
        ".Kernel32::<SleepConditionVariableCS>g____PInvoke|57_0(Interop/Kernel32/CONDITION_VARIABLE*,Interop/Kernel32/CRITICAL_SECTION*,System.Int32)",
        nullptr, kernel32_sleep_condition_variable_cs_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::<SleepConditionVariableCS>g____PInvoke|57_0", nullptr,
                                   kernel32_sleep_condition_variable_cs_invoker);
    vm::PInvokes::register_pinvoke(
        "Interop/Kernel32::SleepConditionVariableCS(Interop/Kernel32/CONDITION_VARIABLE*,Interop/Kernel32/CRITICAL_SECTION*,System.Int32)",
        nullptr, kernel32_sleep_condition_variable_cs_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::SleepConditionVariableCS", nullptr, kernel32_sleep_condition_variable_cs_invoker);
    vm::PInvokes::register_pinvoke(
        "Kernel32::SleepConditionVariableCS(Interop/Kernel32/CONDITION_VARIABLE*,Interop/Kernel32/CRITICAL_SECTION*,System.Int32)",
        nullptr, kernel32_sleep_condition_variable_cs_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::SleepConditionVariableCS", nullptr, kernel32_sleep_condition_variable_cs_invoker);
    vm::PInvokes::register_pinvoke(
        ".Kernel32::SleepConditionVariableCS(Interop/Kernel32/CONDITION_VARIABLE*,Interop/Kernel32/CRITICAL_SECTION*,System.Int32)",
        nullptr, kernel32_sleep_condition_variable_cs_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::SleepConditionVariableCS", nullptr, kernel32_sleep_condition_variable_cs_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::WakeConditionVariable(Interop/Kernel32/CONDITION_VARIABLE*)", nullptr,
                                   kernel32_wake_condition_variable_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::WakeConditionVariable", nullptr, kernel32_wake_condition_variable_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::WakeConditionVariable(Interop/Kernel32/CONDITION_VARIABLE*)", nullptr,
                                   kernel32_wake_condition_variable_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::WakeConditionVariable", nullptr, kernel32_wake_condition_variable_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::WakeConditionVariable(Interop/Kernel32/CONDITION_VARIABLE*)", nullptr,
                                   kernel32_wake_condition_variable_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::WakeConditionVariable", nullptr, kernel32_wake_condition_variable_invoker);
    vm::PInvokes::register_pinvoke(
        "Interop/Kernel32::<CreateIoCompletionPort>g____PInvoke|49_0(System.IntPtr,System.IntPtr,System.UIntPtr,System.Int32)", nullptr,
        kernel32_create_io_completion_port_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::<CreateIoCompletionPort>g____PInvoke|49_0", nullptr,
                                   kernel32_create_io_completion_port_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::<CreateIoCompletionPort>g____PInvoke|49_0(System.IntPtr,System.IntPtr,System.UIntPtr,System.Int32)",
                                   nullptr, kernel32_create_io_completion_port_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::<CreateIoCompletionPort>g____PInvoke|49_0", nullptr,
                                   kernel32_create_io_completion_port_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::<CreateIoCompletionPort>g____PInvoke|49_0(System.IntPtr,System.IntPtr,System.UIntPtr,System.Int32)",
                                   nullptr, kernel32_create_io_completion_port_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::<CreateIoCompletionPort>g____PInvoke|49_0", nullptr,
                                   kernel32_create_io_completion_port_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::CreateIoCompletionPort(System.IntPtr,System.IntPtr,System.UIntPtr,System.Int32)", nullptr,
                                   kernel32_create_io_completion_port_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::CreateIoCompletionPort", nullptr, kernel32_create_io_completion_port_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::CreateIoCompletionPort(System.IntPtr,System.IntPtr,System.UIntPtr,System.Int32)", nullptr,
                                   kernel32_create_io_completion_port_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::CreateIoCompletionPort", nullptr, kernel32_create_io_completion_port_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::CreateIoCompletionPort(System.IntPtr,System.IntPtr,System.UIntPtr,System.Int32)", nullptr,
                                   kernel32_create_io_completion_port_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::CreateIoCompletionPort", nullptr, kernel32_create_io_completion_port_invoker);
    vm::PInvokes::register_pinvoke(
        "Interop/Kernel32::<PostQueuedCompletionStatus>g____PInvoke|50_0(System.IntPtr,System.UInt32,System.UIntPtr,System.IntPtr)", nullptr,
        kernel32_post_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::<PostQueuedCompletionStatus>g____PInvoke|50_0", nullptr,
                                   kernel32_post_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::<PostQueuedCompletionStatus>g____PInvoke|50_0(System.IntPtr,System.UInt32,System.UIntPtr,System.IntPtr)",
                                   nullptr, kernel32_post_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::<PostQueuedCompletionStatus>g____PInvoke|50_0", nullptr,
                                   kernel32_post_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::<PostQueuedCompletionStatus>g____PInvoke|50_0(System.IntPtr,System.UInt32,System.UIntPtr,System.IntPtr)",
                                   nullptr, kernel32_post_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::<PostQueuedCompletionStatus>g____PInvoke|50_0", nullptr,
                                   kernel32_post_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::PostQueuedCompletionStatus(System.IntPtr,System.UInt32,System.UIntPtr,System.IntPtr)", nullptr,
                                   kernel32_post_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::PostQueuedCompletionStatus", nullptr, kernel32_post_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::PostQueuedCompletionStatus(System.IntPtr,System.UInt32,System.UIntPtr,System.IntPtr)", nullptr,
                                   kernel32_post_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::PostQueuedCompletionStatus", nullptr, kernel32_post_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::PostQueuedCompletionStatus(System.IntPtr,System.UInt32,System.UIntPtr,System.IntPtr)", nullptr,
                                   kernel32_post_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::PostQueuedCompletionStatus", nullptr, kernel32_post_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke(
        "Interop/Kernel32::<GetQueuedCompletionStatus>g____PInvoke|51_0(System.IntPtr,System.UInt32*,System.UIntPtr*,System.IntPtr*,System.Int32)",
        nullptr, kernel32_get_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::<GetQueuedCompletionStatus>g____PInvoke|51_0", nullptr,
                                   kernel32_get_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke(
        "Kernel32::<GetQueuedCompletionStatus>g____PInvoke|51_0(System.IntPtr,System.UInt32*,System.UIntPtr*,System.IntPtr*,System.Int32)",
        nullptr, kernel32_get_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::<GetQueuedCompletionStatus>g____PInvoke|51_0", nullptr,
                                   kernel32_get_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke(
        ".Kernel32::<GetQueuedCompletionStatus>g____PInvoke|51_0(System.IntPtr,System.UInt32*,System.UIntPtr*,System.IntPtr*,System.Int32)",
        nullptr, kernel32_get_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::<GetQueuedCompletionStatus>g____PInvoke|51_0", nullptr,
                                   kernel32_get_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::GetQueuedCompletionStatus(System.IntPtr,System.UInt32*,System.UIntPtr*,System.IntPtr*,System.Int32)",
                                   nullptr, kernel32_get_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::GetQueuedCompletionStatus", nullptr, kernel32_get_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::GetQueuedCompletionStatus(System.IntPtr,System.UInt32*,System.UIntPtr*,System.IntPtr*,System.Int32)",
                                   nullptr, kernel32_get_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::GetQueuedCompletionStatus", nullptr, kernel32_get_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::GetQueuedCompletionStatus(System.IntPtr,System.UInt32*,System.UIntPtr*,System.IntPtr*,System.Int32)",
                                   nullptr, kernel32_get_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::GetQueuedCompletionStatus", nullptr, kernel32_get_queued_completion_status_invoker);
    vm::PInvokes::register_pinvoke(
        "Interop/Kernel32::<GetQueuedCompletionStatusEx>g____PInvoke|52_0(System.IntPtr,Interop/Kernel32/OVERLAPPED_ENTRY*,System.Int32,System.Int32*,System.Int32,System.Int32)",
        nullptr, kernel32_get_queued_completion_status_ex_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::<GetQueuedCompletionStatusEx>g____PInvoke|52_0", nullptr,
                                   kernel32_get_queued_completion_status_ex_invoker);
    vm::PInvokes::register_pinvoke(
        "Kernel32::<GetQueuedCompletionStatusEx>g____PInvoke|52_0(System.IntPtr,Interop/Kernel32/OVERLAPPED_ENTRY*,System.Int32,System.Int32*,System.Int32,System.Int32)",
        nullptr, kernel32_get_queued_completion_status_ex_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::<GetQueuedCompletionStatusEx>g____PInvoke|52_0", nullptr,
                                   kernel32_get_queued_completion_status_ex_invoker);
    vm::PInvokes::register_pinvoke(
        ".Kernel32::<GetQueuedCompletionStatusEx>g____PInvoke|52_0(System.IntPtr,Interop/Kernel32/OVERLAPPED_ENTRY*,System.Int32,System.Int32*,System.Int32,System.Int32)",
        nullptr, kernel32_get_queued_completion_status_ex_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::<GetQueuedCompletionStatusEx>g____PInvoke|52_0", nullptr,
                                   kernel32_get_queued_completion_status_ex_invoker);
    vm::PInvokes::register_pinvoke(
        "Interop/Kernel32::GetQueuedCompletionStatusEx(System.IntPtr,Interop/Kernel32/OVERLAPPED_ENTRY*,System.Int32,System.Int32*,System.Int32,System.Int32)",
        nullptr, kernel32_get_queued_completion_status_ex_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::GetQueuedCompletionStatusEx", nullptr,
                                   kernel32_get_queued_completion_status_ex_invoker);
    vm::PInvokes::register_pinvoke(
        "Kernel32::GetQueuedCompletionStatusEx(System.IntPtr,Interop/Kernel32/OVERLAPPED_ENTRY*,System.Int32,System.Int32*,System.Int32,System.Int32)",
        nullptr, kernel32_get_queued_completion_status_ex_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::GetQueuedCompletionStatusEx", nullptr,
                                   kernel32_get_queued_completion_status_ex_invoker);
    vm::PInvokes::register_pinvoke(
        ".Kernel32::GetQueuedCompletionStatusEx(System.IntPtr,Interop/Kernel32/OVERLAPPED_ENTRY*,System.Int32,System.Int32*,System.Int32,System.Int32)",
        nullptr, kernel32_get_queued_completion_status_ex_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::GetQueuedCompletionStatusEx", nullptr,
                                   kernel32_get_queued_completion_status_ex_invoker);
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
    vm::PInvokes::register_pinvoke(
        "Interop/Kernel32::<LCMapStringEx>g____PInvoke|27_0(System.UInt16*,System.UInt32,System.Char*,System.Int32,System.Void*,System.Int32,System.Void*,System.Void*,System.IntPtr)",
        nullptr, kernel32_lc_map_string_ex_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::<LCMapStringEx>g____PInvoke|27_0", nullptr,
                                   kernel32_lc_map_string_ex_invoker);
    vm::PInvokes::register_pinvoke(
        "Kernel32::<LCMapStringEx>g____PInvoke|27_0(System.UInt16*,System.UInt32,System.Char*,System.Int32,System.Void*,System.Int32,System.Void*,System.Void*,System.IntPtr)",
        nullptr, kernel32_lc_map_string_ex_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::<LCMapStringEx>g____PInvoke|27_0", nullptr,
                                   kernel32_lc_map_string_ex_invoker);
    vm::PInvokes::register_pinvoke(
        ".Kernel32::<LCMapStringEx>g____PInvoke|27_0(System.UInt16*,System.UInt32,System.Char*,System.Int32,System.Void*,System.Int32,System.Void*,System.Void*,System.IntPtr)",
        nullptr, kernel32_lc_map_string_ex_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::<LCMapStringEx>g____PInvoke|27_0", nullptr,
                                   kernel32_lc_map_string_ex_invoker);
    vm::PInvokes::register_pinvoke(
        "Interop/Kernel32::LCMapStringEx(System.UInt16*,System.UInt32,System.Char*,System.Int32,System.Void*,System.Int32,System.Void*,System.Void*,System.IntPtr)",
        nullptr, kernel32_lc_map_string_ex_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::LCMapStringEx", nullptr, kernel32_lc_map_string_ex_invoker);
    vm::PInvokes::register_pinvoke(
        "Kernel32::LCMapStringEx(System.UInt16*,System.UInt32,System.Char*,System.Int32,System.Void*,System.Int32,System.Void*,System.Void*,System.IntPtr)",
        nullptr, kernel32_lc_map_string_ex_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::LCMapStringEx", nullptr, kernel32_lc_map_string_ex_invoker);
    vm::PInvokes::register_pinvoke(
        "Interop/Kernel32::FindNLSStringEx(System.Char*,System.UInt32,System.Char*,System.Int32,System.Char*,System.Int32,System.Int32*,System.Void*,System.Void*,System.IntPtr)",
        nullptr, kernel32_find_nls_string_ex_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::FindNLSStringEx", nullptr, kernel32_find_nls_string_ex_invoker);
    vm::PInvokes::register_pinvoke(
        "Kernel32::FindNLSStringEx(System.Char*,System.UInt32,System.Char*,System.Int32,System.Char*,System.Int32,System.Int32*,System.Void*,System.Void*,System.IntPtr)",
        nullptr, kernel32_find_nls_string_ex_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::FindNLSStringEx", nullptr, kernel32_find_nls_string_ex_invoker);
    vm::PInvokes::register_pinvoke(
        ".Kernel32::FindNLSStringEx(System.Char*,System.UInt32,System.Char*,System.Int32,System.Char*,System.Int32,System.Int32*,System.Void*,System.Void*,System.IntPtr)",
        nullptr, kernel32_find_nls_string_ex_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::FindNLSStringEx", nullptr, kernel32_find_nls_string_ex_invoker);
    vm::PInvokes::register_pinvoke(
        "Interop/Kernel32::FindStringOrdinal(System.UInt32,System.Char*,System.Int32,System.Char*,System.Int32,Interop/BOOL)",
        nullptr, kernel32_find_string_ordinal_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::FindStringOrdinal", nullptr, kernel32_find_string_ordinal_invoker);
    vm::PInvokes::register_pinvoke(
        "Kernel32::FindStringOrdinal(System.UInt32,System.Char*,System.Int32,System.Char*,System.Int32,Interop/BOOL)",
        nullptr, kernel32_find_string_ordinal_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::FindStringOrdinal", nullptr, kernel32_find_string_ordinal_invoker);
    vm::PInvokes::register_pinvoke(
        ".Kernel32::FindStringOrdinal(System.UInt32,System.Char*,System.Int32,System.Char*,System.Int32,Interop/BOOL)",
        nullptr, kernel32_find_string_ordinal_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::FindStringOrdinal", nullptr, kernel32_find_string_ordinal_invoker);
    vm::PInvokes::register_pinvoke(
        "Interop/Kernel32::CompareStringEx(System.Char*,System.UInt32,System.Char*,System.Int32,System.Char*,System.Int32,System.Void*,System.Void*,System.IntPtr)",
        nullptr, kernel32_compare_string_ex_invoker);
    vm::PInvokes::register_pinvoke("Interop/Kernel32::CompareStringEx", nullptr, kernel32_compare_string_ex_invoker);
    vm::PInvokes::register_pinvoke(
        "Kernel32::CompareStringEx(System.Char*,System.UInt32,System.Char*,System.Int32,System.Char*,System.Int32,System.Void*,System.Void*,System.IntPtr)",
        nullptr, kernel32_compare_string_ex_invoker);
    vm::PInvokes::register_pinvoke("Kernel32::CompareStringEx", nullptr, kernel32_compare_string_ex_invoker);
    vm::PInvokes::register_pinvoke(
        ".Kernel32::CompareStringEx(System.Char*,System.UInt32,System.Char*,System.Int32,System.Char*,System.Int32,System.Void*,System.Void*,System.IntPtr)",
        nullptr, kernel32_compare_string_ex_invoker);
    vm::PInvokes::register_pinvoke(".Kernel32::CompareStringEx", nullptr, kernel32_compare_string_ex_invoker);
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
    vm::PInvokes::register_pinvoke(
        "Interop/Advapi32::EventRegister(System.Guid*,delegate* unmanaged[Unmanaged]<System.Guid*,System.Int32,System.Byte,System.Int64,System.Int64,Interop/Advapi32/EVENT_FILTER_DESCRIPTOR*,System.Void*,System.Void>,System.Void*,System.Int64*)",
        nullptr, advapi32_event_register_invoker);
    vm::PInvokes::register_pinvoke("Advapi32::EventRegister", nullptr, advapi32_event_register_invoker);
    vm::PInvokes::register_pinvoke(
        ".Advapi32::EventRegister(System.Guid*,delegate* unmanaged[Unmanaged]<System.Guid*,System.Int32,System.Byte,System.Int64,System.Int64,Interop/Advapi32/EVENT_FILTER_DESCRIPTOR*,System.Void*,System.Void>,System.Void*,System.Int64*)",
        nullptr, advapi32_event_register_invoker);
    vm::PInvokes::register_pinvoke("Interop/Advapi32::EventUnregister", nullptr, advapi32_event_unregister_invoker);
    vm::PInvokes::register_pinvoke("Interop/Advapi32::EventUnregister(System.Int64)", nullptr,
                                   advapi32_event_unregister_invoker);
    vm::PInvokes::register_pinvoke("Advapi32::EventUnregister", nullptr, advapi32_event_unregister_invoker);
    vm::PInvokes::register_pinvoke(".Advapi32::EventUnregister(System.Int64)", nullptr,
                                   advapi32_event_unregister_invoker);
    vm::PInvokes::register_pinvoke("Interop/Advapi32::EventWriteTransfer", nullptr, advapi32_event_write_transfer_invoker);
    vm::PInvokes::register_pinvoke("Advapi32::EventWriteTransfer", nullptr, advapi32_event_write_transfer_invoker);
    vm::PInvokes::register_pinvoke("Interop/Advapi32::EventActivityIdControl", nullptr, advapi32_event_activity_id_control_invoker);
    vm::PInvokes::register_pinvoke("Advapi32::EventActivityIdControl", nullptr, advapi32_event_activity_id_control_invoker);
    vm::PInvokes::register_pinvoke("Interop/Advapi32::EventSetInformation", nullptr, advapi32_event_set_information_invoker);
    vm::PInvokes::register_pinvoke(
        "Interop/Advapi32::EventSetInformation(System.Int64,Interop/Advapi32/EVENT_INFO_CLASS,System.Void*,System.UInt32)",
        nullptr, advapi32_event_set_information_invoker);
    vm::PInvokes::register_pinvoke("Advapi32::EventSetInformation", nullptr, advapi32_event_set_information_invoker);
    vm::PInvokes::register_pinvoke(
        ".Advapi32::EventSetInformation(System.Int64,Interop/Advapi32/EVENT_INFO_CLASS,System.Void*,System.UInt32)",
        nullptr, advapi32_event_set_information_invoker);
    vm::PInvokes::register_pinvoke("System.Environment::GetProcessorCount()", nullptr, environment_get_processor_count_invoker);
    vm::PInvokes::register_pinvoke("System.Environment::GetProcessorCount", nullptr, environment_get_processor_count_invoker);
    vm::PInvokes::register_pinvoke("System.GC::<_Collect>g____PInvoke|8_0(System.Int32,System.Int32,System.Byte)", nullptr,
                                   gc_collect_invoker);
    vm::PInvokes::register_pinvoke("System.GC::<_Collect>g____PInvoke|8_0", nullptr, gc_collect_invoker);
    vm::PInvokes::register_pinvoke("System.GC::_Collect", nullptr, gc_collect_invoker);
    vm::PInvokes::register_pinvoke("System.String::Intern(System.Runtime.CompilerServices.StringHandleOnStack)", nullptr,
                                   string_intern_invoker);
    vm::PInvokes::register_pinvoke("System.String::Intern", nullptr, string_intern_invoker);
    vm::PInvokes::register_pinvoke("System.String::IsInterned(System.Runtime.CompilerServices.StringHandleOnStack)", nullptr,
                                   string_is_interned_invoker);
    vm::PInvokes::register_pinvoke("System.String::IsInterned", nullptr, string_is_interned_invoker);
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
    vm::PInvokes::register_pinvoke(
        "System.Runtime.CompilerServices.RuntimeHelpers::AllocateUninitializedClone(System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, runtime_helpers_allocate_uninitialized_clone_invoker);
    vm::PInvokes::register_pinvoke("ObjectNative_AllocateUninitializedClone", nullptr,
                                   runtime_helpers_allocate_uninitialized_clone_invoker);
    vm::PInvokes::register_pinvoke("Buffer_MemMove", nullptr, buffer_memmove_invoker);
    vm::PInvokes::register_pinvoke("System.Buffer::MemmoveInternal(System.Byte*,System.Byte*,System.UIntPtr)", nullptr, buffer_memmove_invoker);
    vm::PInvokes::register_pinvoke("Buffer_Clear", nullptr, buffer_clear_invoker);
    vm::PInvokes::register_pinvoke("System.Buffer::ZeroMemoryInternal(System.Void*,System.UIntPtr)", nullptr, buffer_clear_invoker);
    vm::PInvokes::register_pinvoke("MethodBase_GetCurrentMethod", nullptr, method_base_get_current_method_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.MethodBase::GetCurrentMethod(System.Runtime.CompilerServices.StackCrawlMarkHandle)", nullptr,
                                   method_base_get_current_method_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.MethodBase::GetCurrentMethod", nullptr, method_base_get_current_method_invoker);
    vm::PInvokes::register_pinvoke("AssemblyNative_GetExecutingAssembly", nullptr, assembly_get_executing_assembly_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.Assembly::GetExecutingAssemblyNative(System.Runtime.CompilerServices.StackCrawlMarkHandle,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, assembly_get_executing_assembly_invoker);
    vm::PInvokes::register_pinvoke("AssemblyNative_GetEntryAssembly", nullptr, assembly_get_entry_assembly_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.Assembly::GetEntryAssemblyNative(System.Runtime.CompilerServices.ObjectHandleOnStack)", nullptr,
        assembly_get_entry_assembly_invoker);
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
    vm::PInvokes::register_pinvoke(
        "System.Enum::GetEnumValuesAndNames(System.Runtime.CompilerServices.QCallTypeHandle,System.Runtime.CompilerServices.ObjectHandleOnStack,System.Runtime.CompilerServices.ObjectHandleOnStack,Interop/BOOL)",
        nullptr, enum_get_values_and_names_invoker);
    vm::PInvokes::register_pinvoke("System.Enum::GetEnumValuesAndNames", nullptr, enum_get_values_and_names_invoker);
    vm::PInvokes::register_pinvoke("MethodTable_CanCompareBitsOrUseFastGetHashCode", nullptr,
                                   method_table_can_compare_bits_or_use_fast_get_hash_code_invoker);
    vm::PInvokes::register_pinvoke(
        "System.ValueType::<CanCompareBitsOrUseFastGetHashCodeHelper>g____PInvoke|2_0(System.Runtime.CompilerServices.MethodTable*)",
        nullptr, method_table_can_compare_bits_or_use_fast_get_hash_code_invoker);
    vm::PInvokes::register_pinvoke("System.ValueType::<CanCompareBitsOrUseFastGetHashCodeHelper>g____PInvoke|2_0", nullptr,
                                   method_table_can_compare_bits_or_use_fast_get_hash_code_invoker);
    vm::PInvokes::register_pinvoke(
        "System.MdUtf8String::<EqualsCaseInsensitive>g____PInvoke|0_0(System.Void*,System.Void*,System.Int32)", nullptr,
        md_utf8_string_equals_case_insensitive_invoker);
    vm::PInvokes::register_pinvoke("System.MdUtf8String::<EqualsCaseInsensitive>g____PInvoke|0_0", nullptr,
                                   md_utf8_string_equals_case_insensitive_invoker);
    vm::PInvokes::register_pinvoke("MdUtf8String_EqualsCaseInsensitive", nullptr, md_utf8_string_equals_case_insensitive_invoker);
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
    vm::PInvokes::register_pinvoke("System.Runtime.CompilerServices.TypeHandle::GetCorElementType(System.Void*)", nullptr,
                                   get_cor_element_type_invoker);
    vm::PInvokes::register_pinvoke("System.Runtime.CompilerServices.TypeHandle::GetCorElementType", nullptr, get_cor_element_type_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeFieldHandle::<GetRVAFieldInfo>g____PInvoke|24_0(System.RuntimeFieldHandleInternal,System.Void**,System.UInt32*)",
        nullptr, get_rva_field_info_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeFieldHandle::<GetRVAFieldInfo>g____PInvoke|24_0", nullptr,
                                   get_rva_field_info_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeFieldHandle::GetValueDirect(System.IntPtr,System.Void*,System.Runtime.CompilerServices.QCallTypeHandle,System.Runtime.CompilerServices.QCallTypeHandle,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, runtime_field_handle_get_value_direct_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeFieldHandle::GetValueDirect", nullptr,
                                   runtime_field_handle_get_value_direct_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeFieldHandle::SetValueDirect(System.IntPtr,System.Void*,System.Runtime.CompilerServices.ObjectHandleOnStack,System.Runtime.CompilerServices.QCallTypeHandle,System.Runtime.CompilerServices.QCallTypeHandle)",
        nullptr, runtime_field_handle_set_value_direct_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeFieldHandle::SetValueDirect", nullptr,
                                   runtime_field_handle_set_value_direct_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeFieldHandle::<SetValue>g____PInvoke|34_0(System.IntPtr,System.Runtime.CompilerServices.ObjectHandleOnStack,System.Runtime.CompilerServices.ObjectHandleOnStack,System.Runtime.CompilerServices.QCallTypeHandle,System.Runtime.CompilerServices.QCallTypeHandle,System.Int32*)",
        nullptr, runtime_field_handle_set_value_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeFieldHandle::<SetValue>g____PInvoke|34_0", nullptr,
                                   runtime_field_handle_set_value_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeTypeHandle::GetDeclaringTypeHandle(System.IntPtr)", nullptr,
                                   get_declaring_type_handle_invoker);
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
    vm::PInvokes::register_pinvoke(
        "System.RuntimeTypeHandle::GetInstantiation(System.Runtime.CompilerServices.QCallTypeHandle,System.Runtime.CompilerServices.ObjectHandleOnStack,Interop/BOOL)",
        nullptr, runtime_type_handle_get_instantiation_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeTypeHandle::GetInstantiation", nullptr, runtime_type_handle_get_instantiation_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeTypeHandle::Instantiate(System.Runtime.CompilerServices.QCallTypeHandle,System.IntPtr*,System.Int32,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, runtime_type_handle_instantiate_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeTypeHandle::Instantiate", nullptr, runtime_type_handle_instantiate_invoker);
    vm::PInvokes::register_pinvoke("RuntimeTypeHandle_Instantiate", nullptr, runtime_type_handle_instantiate_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeTypeHandle::MakeArray(System.Runtime.CompilerServices.QCallTypeHandle,System.Int32,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, runtime_type_handle_make_array_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeTypeHandle::MakeArray", nullptr, runtime_type_handle_make_array_invoker);
    vm::PInvokes::register_pinvoke("RuntimeTypeHandle_MakeArray", nullptr, runtime_type_handle_make_array_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeTypeHandle::MakeSZArray(System.Runtime.CompilerServices.QCallTypeHandle,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, runtime_type_handle_make_szarray_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeTypeHandle::MakeSZArray", nullptr, runtime_type_handle_make_szarray_invoker);
    vm::PInvokes::register_pinvoke("RuntimeTypeHandle_MakeSZArray", nullptr, runtime_type_handle_make_szarray_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeTypeHandle::MakeByRef(System.Runtime.CompilerServices.QCallTypeHandle,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, runtime_type_handle_make_byref_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeTypeHandle::MakeByRef", nullptr, runtime_type_handle_make_byref_invoker);
    vm::PInvokes::register_pinvoke("RuntimeTypeHandle_MakeByRef", nullptr, runtime_type_handle_make_byref_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeTypeHandle::MakePointer(System.Runtime.CompilerServices.QCallTypeHandle,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, runtime_type_handle_make_pointer_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeTypeHandle::MakePointer", nullptr, runtime_type_handle_make_pointer_invoker);
    vm::PInvokes::register_pinvoke("RuntimeTypeHandle_MakePointer", nullptr, runtime_type_handle_make_pointer_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.RuntimeModule::GetTypes(System.Runtime.CompilerServices.QCallModule,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, get_module_types_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.RuntimeModule::GetTypes", nullptr, get_module_types_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.RuntimeModule::GetFullyQualifiedName(System.Runtime.CompilerServices.QCallModule,System.Runtime.CompilerServices.StringHandleOnStack)",
        nullptr, runtime_module_get_name_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.RuntimeModule::GetFullyQualifiedName", nullptr, runtime_module_get_name_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.RuntimeModule::GetScopeName(System.Runtime.CompilerServices.QCallModule,System.Runtime.CompilerServices.StringHandleOnStack)",
        nullptr, runtime_module_get_name_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.RuntimeModule::GetScopeName", nullptr, runtime_module_get_name_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeTypeHandle::GetFields(System.Runtime.CompilerServices.MethodTable*,System.Span`1<System.IntPtr>,System.Int32&)",
        nullptr, runtime_type_handle_get_fields_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeTypeHandle::<GetFields>g____PInvoke|67_0(System.Runtime.CompilerServices.MethodTable*,System.IntPtr*,System.Int32*)",
        nullptr, runtime_type_handle_get_fields_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeTypeHandle::<GetFields>g____PInvoke|67_0", nullptr,
                                   runtime_type_handle_get_fields_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeTypeHandle::GetFields", nullptr, runtime_type_handle_get_fields_invoker);
    vm::PInvokes::register_pinvoke("RuntimeTypeHandle_GetFields", nullptr, runtime_type_handle_get_fields_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeTypeHandle::GetInterfaces(System.Runtime.CompilerServices.MethodTable*,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, runtime_type_handle_get_interfaces_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeTypeHandle::GetInterfaces", nullptr, runtime_type_handle_get_interfaces_invoker);
    vm::PInvokes::register_pinvoke("RuntimeTypeHandle_GetInterfaces", nullptr, runtime_type_handle_get_interfaces_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeTypeHandle::RegisterCollectibleTypeDependency(System.Runtime.CompilerServices.QCallTypeHandle,System.Runtime.CompilerServices.QCallAssembly)",
        nullptr, runtime_type_handle_register_collectible_type_dependency_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeTypeHandle::RegisterCollectibleTypeDependency", nullptr,
                                   runtime_type_handle_register_collectible_type_dependency_invoker);
    vm::PInvokes::register_pinvoke("RuntimeTypeHandle_RegisterCollectibleTypeDependency", nullptr,
                                   runtime_type_handle_register_collectible_type_dependency_invoker);
    vm::PInvokes::register_pinvoke("System.ModuleHandle::GetToken(System.Runtime.CompilerServices.QCallModule)", nullptr,
                                   module_handle_get_token_invoker);
    vm::PInvokes::register_pinvoke("System.ModuleHandle::GetToken", nullptr, module_handle_get_token_invoker);
    vm::PInvokes::register_pinvoke("System.ModuleHandle::GetMDStreamVersion(System.Runtime.CompilerServices.QCallModule)", nullptr,
                                   module_handle_get_md_stream_version_invoker);
    vm::PInvokes::register_pinvoke("System.ModuleHandle::GetMDStreamVersion", nullptr,
                                   module_handle_get_md_stream_version_invoker);
    vm::PInvokes::register_pinvoke("ModuleHandle_GetMDStreamVersion", nullptr,
                                   module_handle_get_md_stream_version_invoker);
    vm::PInvokes::register_pinvoke("ModuleHandle_GetModuleType", nullptr, module_handle_get_module_type_invoker);
    vm::PInvokes::register_pinvoke(
        "System.ModuleHandle::GetModuleType(System.Runtime.CompilerServices.QCallModule,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, module_handle_get_module_type_invoker);
    vm::PInvokes::register_pinvoke("System.ModuleHandle::GetModuleType", nullptr, module_handle_get_module_type_invoker);
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
        "System.Reflection.RuntimeAssembly::<GetModules>g____PInvoke|89_0(System.Runtime.CompilerServices.QCallAssembly,System.Int32,System.Int32,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, assembly_get_modules_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.RuntimeAssembly::<GetModules>g____PInvoke|89_0", nullptr,
                                   assembly_get_modules_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.RuntimeAssembly::GetFullName(System.Runtime.CompilerServices.QCallAssembly,System.Runtime.CompilerServices.StringHandleOnStack)",
        nullptr, assembly_get_full_name_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.RuntimeAssembly::GetFullName", nullptr, assembly_get_full_name_invoker);
    vm::PInvokes::register_pinvoke("AssemblyNative_GetFullName", nullptr, assembly_get_full_name_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.RuntimeAssembly::GetImageRuntimeVersion(System.Runtime.CompilerServices.QCallAssembly,System.Runtime.CompilerServices.StringHandleOnStack)",
        nullptr, assembly_get_image_runtime_version_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.RuntimeAssembly::GetImageRuntimeVersion", nullptr,
                                   assembly_get_image_runtime_version_invoker);
    vm::PInvokes::register_pinvoke("AssemblyNative_GetImageRuntimeVersion", nullptr,
                                   assembly_get_image_runtime_version_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.RuntimeAssembly::GetEntryPoint(System.Runtime.CompilerServices.QCallAssembly,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, assembly_get_entry_point_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.RuntimeAssembly::GetEntryPoint", nullptr, assembly_get_entry_point_invoker);
    vm::PInvokes::register_pinvoke("AssemblyNative_GetEntryPoint", nullptr, assembly_get_entry_point_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.RuntimeAssembly::GetManifestResourceNames(System.Runtime.CompilerServices.QCallAssembly,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, assembly_get_manifest_resource_names_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.RuntimeAssembly::GetManifestResourceNames", nullptr,
                                   assembly_get_manifest_resource_names_invoker);
    vm::PInvokes::register_pinvoke("AssemblyNative_GetManifestResourceNames", nullptr,
                                   assembly_get_manifest_resource_names_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.RuntimeAssembly::<GetCodeBase>g____PInvoke|14_0(System.Runtime.CompilerServices.QCallAssembly,System.Runtime.CompilerServices.StringHandleOnStack)",
        nullptr, assembly_get_code_base_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.RuntimeAssembly::<GetCodeBase>g____PInvoke|14_0", nullptr,
                                   assembly_get_code_base_invoker);
    vm::PInvokes::register_pinvoke("AssemblyNative_GetCodeBase", nullptr, assembly_get_code_base_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.RuntimeAssembly::GetFlags(System.Runtime.CompilerServices.QCallAssembly)", nullptr,
        assembly_get_flags_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.RuntimeAssembly::GetFlags", nullptr, assembly_get_flags_invoker);
    vm::PInvokes::register_pinvoke("AssemblyNative_GetFlags", nullptr, assembly_get_flags_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.RuntimeAssembly::GetHashAlgorithm(System.Runtime.CompilerServices.QCallAssembly)", nullptr,
        assembly_get_hash_algorithm_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.RuntimeAssembly::GetHashAlgorithm", nullptr,
                                   assembly_get_hash_algorithm_invoker);
    vm::PInvokes::register_pinvoke("AssemblyNative_GetHashAlgorithm", nullptr, assembly_get_hash_algorithm_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.RuntimeAssembly::GetLocale(System.Runtime.CompilerServices.QCallAssembly,System.Runtime.CompilerServices.StringHandleOnStack)",
        nullptr, assembly_get_locale_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.RuntimeAssembly::GetLocale", nullptr, assembly_get_locale_invoker);
    vm::PInvokes::register_pinvoke("AssemblyNative_GetLocale", nullptr, assembly_get_locale_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.RuntimeAssembly::GetPublicKey(System.Runtime.CompilerServices.QCallAssembly,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, assembly_get_public_key_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.RuntimeAssembly::GetPublicKey", nullptr, assembly_get_public_key_invoker);
    vm::PInvokes::register_pinvoke("AssemblyNative_GetPublicKey", nullptr, assembly_get_public_key_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.RuntimeAssembly::GetReferencedAssemblies(System.Runtime.CompilerServices.QCallAssembly,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, assembly_get_referenced_assemblies_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.RuntimeAssembly::GetReferencedAssemblies", nullptr,
                                   assembly_get_referenced_assemblies_invoker);
    vm::PInvokes::register_pinvoke("AssemblyNative_GetReferencedAssemblies", nullptr,
                                   assembly_get_referenced_assemblies_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.RuntimeAssembly::GetSimpleName(System.Runtime.CompilerServices.QCallAssembly,System.Runtime.CompilerServices.StringHandleOnStack)",
        nullptr, assembly_get_simple_name_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.RuntimeAssembly::GetSimpleName", nullptr, assembly_get_simple_name_invoker);
    vm::PInvokes::register_pinvoke("AssemblyNative_GetSimpleName", nullptr, assembly_get_simple_name_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.RuntimeAssembly::<GetVersion>g____PInvoke|71_0(System.Runtime.CompilerServices.QCallAssembly,System.Int32*,System.Int32*,System.Int32*,System.Int32*)",
        nullptr, assembly_get_version_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.RuntimeAssembly::<GetVersion>g____PInvoke|71_0", nullptr,
                                   assembly_get_version_invoker);
    vm::PInvokes::register_pinvoke("AssemblyNative_GetVersion", nullptr, assembly_get_version_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.RuntimeAssembly::<InternalLoad>g____PInvoke|48_0(System.Reflection.NativeAssemblyNameParts*,System.Runtime.CompilerServices.ObjectHandleOnStack,System.Runtime.CompilerServices.StackCrawlMarkHandle,System.Boolean,System.Runtime.CompilerServices.ObjectHandleOnStack,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, assembly_internal_load_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.RuntimeAssembly::<InternalLoad>g____PInvoke|48_0", nullptr,
                                   assembly_internal_load_invoker);
    vm::PInvokes::register_pinvoke("AssemblyNative_InternalLoad", nullptr, assembly_internal_load_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.MetadataImport::<Enum>g____PInvoke|8_0(System.IntPtr,System.Int32,System.Int32,System.Int32*,System.Int32*,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, metadata_import_enum_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.MetadataImport::<Enum>g____PInvoke|8_0", nullptr, metadata_import_enum_invoker);
    vm::PInvokes::register_pinvoke(
        "System.ModuleHandle::ResolveMethod(System.Runtime.CompilerServices.QCallModule,System.Int32,System.IntPtr*,System.Int32,System.IntPtr*,System.Int32)",
        nullptr, module_handle_resolve_method_invoker);
    vm::PInvokes::register_pinvoke("System.ModuleHandle::ResolveMethod", nullptr, module_handle_resolve_method_invoker);
    vm::PInvokes::register_pinvoke(
        "System.ModuleHandle::ResolveType(System.Runtime.CompilerServices.QCallModule,System.Int32,System.IntPtr*,System.Int32,System.IntPtr*,System.Int32,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, module_handle_resolve_type_invoker);
    vm::PInvokes::register_pinvoke("System.ModuleHandle::ResolveType", nullptr, module_handle_resolve_type_invoker);
    vm::PInvokes::register_pinvoke(
        "System.ModuleHandle::ResolveField(System.Runtime.CompilerServices.QCallModule,System.Int32,System.IntPtr*,System.Int32,System.IntPtr*,System.Int32,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, module_handle_resolve_field_invoker);
    vm::PInvokes::register_pinvoke("System.ModuleHandle::ResolveField", nullptr, module_handle_resolve_field_invoker);
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
        "System.RuntimeTypeHandle::GetMethodAt(System.Runtime.CompilerServices.MethodTable*,System.Int32)", nullptr,
        runtime_type_handle_get_method_at_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeTypeHandle::GetMethodAt", nullptr, runtime_type_handle_get_method_at_invoker);
    vm::PInvokes::register_pinvoke("RuntimeTypeHandle_GetMethodAt", nullptr, runtime_type_handle_get_method_at_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Signature::Init(System.Runtime.CompilerServices.ObjectHandleOnStack,System.Void*,System.Int32,System.RuntimeFieldHandleInternal,System.RuntimeMethodHandleInternal)",
        nullptr, signature_init_invoker);
    vm::PInvokes::register_pinvoke("System.Signature::Init", nullptr, signature_init_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Signature::GetCustomModifiersAtOffset(System.Runtime.CompilerServices.ObjectHandleOnStack,System.Int32,Interop/BOOL,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, signature_get_custom_modifiers_at_offset_invoker);
    vm::PInvokes::register_pinvoke("System.Signature::GetCustomModifiersAtOffset", nullptr,
                                   signature_get_custom_modifiers_at_offset_invoker);
    vm::PInvokes::register_pinvoke("Signature_GetCustomModifiersAtOffset", nullptr,
                                   signature_get_custom_modifiers_at_offset_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeMethodHandle::GetMethodBody(System.RuntimeMethodHandleInternal,System.Runtime.CompilerServices.QCallTypeHandle,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, runtime_method_handle_get_method_body_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeMethodHandle::GetMethodBody", nullptr, runtime_method_handle_get_method_body_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeMethodHandle::GetMethodInstantiation(System.RuntimeMethodHandleInternal,System.Runtime.CompilerServices.ObjectHandleOnStack,Interop/BOOL)",
        nullptr, runtime_method_handle_get_method_instantiation_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeMethodHandle::GetMethodInstantiation", nullptr,
                                   runtime_method_handle_get_method_instantiation_invoker);
    vm::PInvokes::register_pinvoke("RuntimeMethodHandle_GetMethodInstantiation", nullptr,
                                   runtime_method_handle_get_method_instantiation_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeMethodHandle::GetStubIfNeededSlow(System.RuntimeMethodHandleInternal,System.Runtime.CompilerServices.QCallTypeHandle,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, runtime_method_handle_get_stub_if_needed_slow_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeMethodHandle::GetStubIfNeededSlow", nullptr,
                                   runtime_method_handle_get_stub_if_needed_slow_invoker);
    vm::PInvokes::register_pinvoke("RuntimeMethodHandle_GetStubIfNeededSlow", nullptr,
                                   runtime_method_handle_get_stub_if_needed_slow_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeMethodHandle::StripMethodInstantiation(System.RuntimeMethodHandleInternal,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, runtime_method_handle_strip_method_instantiation_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeMethodHandle::StripMethodInstantiation", nullptr,
                                   runtime_method_handle_strip_method_instantiation_invoker);
    vm::PInvokes::register_pinvoke("RuntimeMethodHandle_StripMethodInstantiation", nullptr,
                                   runtime_method_handle_strip_method_instantiation_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeMethodHandle::IsCAVisibleFromDecoratedType(System.Runtime.CompilerServices.QCallTypeHandle,System.RuntimeMethodHandleInternal,System.Runtime.CompilerServices.QCallTypeHandle,System.Runtime.CompilerServices.QCallModule)",
        nullptr, runtime_method_handle_is_ca_visible_from_decorated_type_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeMethodHandle::IsCAVisibleFromDecoratedType", nullptr,
                                   runtime_method_handle_is_ca_visible_from_decorated_type_invoker);
    vm::PInvokes::register_pinvoke("RuntimeMethodHandle_IsCAVisibleFromDecoratedType", nullptr,
                                   runtime_method_handle_is_ca_visible_from_decorated_type_invoker);
    vm::PInvokes::register_pinvoke(
        "System.Reflection.CustomAttribute::<CreateCustomAttributeInstance>g____PInvoke|30_0(System.Runtime.CompilerServices.QCallModule,System.Runtime.CompilerServices.ObjectHandleOnStack,System.Runtime.CompilerServices.ObjectHandleOnStack,System.IntPtr*,System.IntPtr,System.Int32*,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, custom_attribute_create_custom_attribute_instance_invoker);
    vm::PInvokes::register_pinvoke("System.Reflection.CustomAttribute::<CreateCustomAttributeInstance>g____PInvoke|30_0", nullptr,
                                   custom_attribute_create_custom_attribute_instance_invoker);
    vm::PInvokes::register_pinvoke("CustomAttribute_CreateCustomAttributeInstance", nullptr,
                                   custom_attribute_create_custom_attribute_instance_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeMethodHandle::InvokeMethod(System.Runtime.CompilerServices.ObjectHandleOnStack,System.Void**,System.Runtime.CompilerServices.ObjectHandleOnStack,System.Boolean,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, runtime_method_handle_invoke_method_invoker);
    vm::PInvokes::register_pinvoke(
        "System.RuntimeMethodHandle::InvokeMethod(System.Runtime.CompilerServices.ObjectHandleOnStack,System.Void**,System.Runtime.CompilerServices.ObjectHandleOnStack,Interop/BOOL,System.Runtime.CompilerServices.ObjectHandleOnStack)",
        nullptr, runtime_method_handle_invoke_method_invoker);
    vm::PInvokes::register_pinvoke("System.RuntimeMethodHandle::InvokeMethod", nullptr, runtime_method_handle_invoke_method_invoker);
    vm::PInvokes::register_pinvoke("RuntimeMethodHandle_InvokeMethod", nullptr, runtime_method_handle_invoke_method_invoker);
}

} // namespace pinvokes
} // namespace leanclr

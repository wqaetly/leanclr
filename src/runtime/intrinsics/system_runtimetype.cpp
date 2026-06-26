#include "system_runtimetype.h"

#include <cctype>
#include <cstring>

#include "interp/eval_stack_op.h"
#include "utils/string_builder.h"
#include "vm/class.h"
#include "vm/field.h"
#include "vm/reflection.h"
#include "vm/rt_string.h"

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

static vm::IntrinsicEntry s_intrinsic_entries_system_runtimetype[] = {
    {"System.RuntimeType::GetField(System.String,System.Reflection.BindingFlags)", (vm::IntrinsicFunction)&SystemRuntimeType::get_field, get_field_invoker},
};

utils::Span<vm::IntrinsicEntry> SystemRuntimeType::get_intrinsic_entries() noexcept
{
    return utils::Span<vm::IntrinsicEntry>(s_intrinsic_entries_system_runtimetype,
                                           sizeof(s_intrinsic_entries_system_runtimetype) / sizeof(vm::IntrinsicEntry));
}

} // namespace intrinsics
} // namespace leanclr

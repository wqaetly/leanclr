#include "system_runtime_compilerservices_runtimehelpers.h"

#include <climits>

#include "interp/interp_defs.h"
#include "vm/class.h"
#include "vm/field.h"

namespace leanclr
{
namespace intrinsics
{

static RtResult<const metadata::RtTypeSig*> get_single_method_generic_arg(const metadata::RtMethodInfo* method) noexcept
{
    if (method == nullptr || method->generic_method == nullptr || method->generic_method->generic_context.method_inst == nullptr)
    {
        RET_ERR(RtErr::ExecutionEngine);
    }

    const metadata::RtGenericInst* method_inst = method->generic_method->generic_context.method_inst;
    if (method_inst->generic_arg_count != 1)
    {
        RET_ERR(RtErr::ExecutionEngine);
    }

    RET_OK(method_inst->generic_args[0]);
}

static RtResult<bool> is_reference_or_contains_references_by_typesig(const metadata::RtTypeSig* type_sig) noexcept
{
    if (type_sig->by_ref)
    {
        RET_OK(false);
    }

    switch (type_sig->ele_type)
    {
    case metadata::RtElementType::Object:
    case metadata::RtElementType::String:
    case metadata::RtElementType::Class:
    case metadata::RtElementType::Array:
    case metadata::RtElementType::SZArray:
        RET_OK(true);

    case metadata::RtElementType::ValueType:
    case metadata::RtElementType::GenericInst:
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));
        if (vm::Class::is_reference_type(klass))
        {
            RET_OK(true);
        }
        RET_ERR_ON_FAIL(vm::Class::initialize_fields(klass));
        RET_OK(vm::Class::get_has_references(klass));
    }

    default:
        RET_OK(false);
    }
}

static RtResult<bool> is_bitwise_equatable_by_typesig(const metadata::RtTypeSig* type_sig) noexcept
{
    if (type_sig->by_ref)
    {
        RET_OK(false);
    }

    switch (type_sig->ele_type)
    {
    case metadata::RtElementType::Boolean:
    case metadata::RtElementType::Char:
    case metadata::RtElementType::I1:
    case metadata::RtElementType::U1:
    case metadata::RtElementType::I2:
    case metadata::RtElementType::U2:
    case metadata::RtElementType::I4:
    case metadata::RtElementType::U4:
    case metadata::RtElementType::I8:
    case metadata::RtElementType::U8:
    case metadata::RtElementType::I:
    case metadata::RtElementType::U:
        RET_OK(true);

    case metadata::RtElementType::ValueType:
    {
        DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_sig));
        RET_OK(vm::Class::is_enum_type(klass));
    }

    default:
        RET_OK(false);
    }
}

RtResult<const metadata::RtClass*> SystemRuntimeCompilerServicesRuntimeHelpers::get_method_table(vm::RtObject* obj) noexcept
{
    if (obj == nullptr)
    {
        RET_ERR(RtErr::NullReference);
    }

    RET_OK(obj->klass);
}

RtResult<metadata::RtElementType> SystemRuntimeCompilerServicesRuntimeHelpers::get_primitive_cor_element_type(
    const metadata::RtClass* method_table) noexcept
{
    if (method_table == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    if (vm::Class::is_enum_type(method_table))
    {
        RET_OK(method_table->element_class->by_val->ele_type);
    }

    RET_OK(method_table->by_val->ele_type);
}

RtResult<vm::RtReadOnlySpan<uint8_t>> SystemRuntimeCompilerServicesRuntimeHelpers::create_span(const metadata::RtMethodInfo* method,
                                                                                               const metadata::RtFieldInfo* field) noexcept
{
    if (method == nullptr || method->generic_method == nullptr || method->generic_method->generic_context.method_inst == nullptr)
    {
        RET_ERR(RtErr::ExecutionEngine);
    }

    const metadata::RtGenericInst* method_inst = method->generic_method->generic_context.method_inst;
    if (method_inst->generic_arg_count != 1)
    {
        RET_ERR(RtErr::ExecutionEngine);
    }

    if (field == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    const metadata::RtTypeSig* element_type = method_inst->generic_args[0];
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(interp::ReduceTypeAndSize, type_and_size, interp::InterpDefs::get_reduce_type_and_size_by_typesig(element_type));
    if (type_and_size.byte_size == 0)
    {
        RET_ERR(RtErr::Argument);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const uint8_t*, rva_data, vm::Field::get_field_rva_data(field));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(size_t, field_size, vm::Field::get_field_size(field));
    if ((field_size % type_and_size.byte_size) != 0)
    {
        RET_ERR(RtErr::Argument);
    }

    size_t length = field_size / type_and_size.byte_size;
    if (length > static_cast<size_t>(INT32_MAX))
    {
        RET_ERR(RtErr::ArgumentOutOfRange);
    }

    vm::RtReadOnlySpan<uint8_t> span{rva_data, static_cast<int32_t>(length)};
    RET_OK(span);
}

RtResult<bool> SystemRuntimeCompilerServicesRuntimeHelpers::is_bitwise_equatable(const metadata::RtMethodInfo* method) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, type_sig, get_single_method_generic_arg(method));
    return is_bitwise_equatable_by_typesig(type_sig);
}

RtResult<bool> SystemRuntimeCompilerServicesRuntimeHelpers::is_reference_or_contains_references(const metadata::RtMethodInfo* method) noexcept
{
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, type_sig, get_single_method_generic_arg(method));
    return is_reference_or_contains_references_by_typesig(type_sig);
}

/// @intrinsic: System.Runtime.CompilerServices.RuntimeHelpers::GetMethodTable(System.Object)
static RtResultVoid get_method_table_invoker(metadata::RtManagedMethodPointer methodPtr, const metadata::RtMethodInfo* method,
                                             const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    (void)methodPtr;
    (void)method;
    vm::RtObject* obj = interp::EvalStackOp::get_param<vm::RtObject*>(params, 0);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtClass*, method_table,
                                            SystemRuntimeCompilerServicesRuntimeHelpers::get_method_table(obj));
    interp::EvalStackOp::set_return(ret, method_table);
    RET_VOID_OK();
}

/// @intrinsic: System.Runtime.CompilerServices.MethodTable::GetPrimitiveCorElementType()
static RtResultVoid get_primitive_cor_element_type_invoker(metadata::RtManagedMethodPointer methodPtr, const metadata::RtMethodInfo* method,
                                                           const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    (void)methodPtr;
    (void)method;
    const metadata::RtClass* method_table = interp::EvalStackOp::get_param<const metadata::RtClass*>(params, 0);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtElementType, element_type,
                                            SystemRuntimeCompilerServicesRuntimeHelpers::get_primitive_cor_element_type(method_table));
    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(element_type));
    RET_VOID_OK();
}

/// @intrinsic: System.Runtime.CompilerServices.RuntimeHelpers::CreateSpan<>(System.RuntimeFieldHandle)
static RtResultVoid create_span_invoker(metadata::RtManagedMethodPointer methodPtr, const metadata::RtMethodInfo* method,
                                        const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    (void)methodPtr;
    const metadata::RtFieldInfo* field = interp::EvalStackOp::get_param<const metadata::RtFieldInfo*>(params, 0);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReadOnlySpan<uint8_t>, span,
                                            SystemRuntimeCompilerServicesRuntimeHelpers::create_span(method, field));
    interp::EvalStackOp::set_return(ret, span);
    RET_VOID_OK();
}

/// @intrinsic: System.Runtime.CompilerServices.RuntimeHelpers::IsBitwiseEquatable<>()
static RtResultVoid is_bitwise_equatable_invoker(metadata::RtManagedMethodPointer methodPtr, const metadata::RtMethodInfo* method,
                                                const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    (void)methodPtr;
    (void)params;

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, result,
                                            SystemRuntimeCompilerServicesRuntimeHelpers::is_bitwise_equatable(method));
    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

/// @intrinsic: System.Runtime.CompilerServices.RuntimeHelpers::IsReferenceOrContainsReferences<>()
static RtResultVoid is_reference_or_contains_references_invoker(metadata::RtManagedMethodPointer methodPtr, const metadata::RtMethodInfo* method,
                                                                const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    (void)methodPtr;
    (void)params;

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, result,
                                            SystemRuntimeCompilerServicesRuntimeHelpers::is_reference_or_contains_references(method));
    interp::EvalStackOp::set_return(ret, result);
    RET_VOID_OK();
}

static vm::IntrinsicEntry s_intrinsic_entries_system_runtime_compilerservices_runtimehelpers[] = {
    {"System.Runtime.CompilerServices.RuntimeHelpers::GetMethodTable(System.Object)",
     (vm::IntrinsicFunction)&SystemRuntimeCompilerServicesRuntimeHelpers::get_method_table, get_method_table_invoker},
    {"System.Runtime.CompilerServices.MethodTable::GetPrimitiveCorElementType()",
     (vm::IntrinsicFunction)&SystemRuntimeCompilerServicesRuntimeHelpers::get_primitive_cor_element_type, get_primitive_cor_element_type_invoker},
    {"System.Runtime.CompilerServices.RuntimeHelpers::CreateSpan<>(System.RuntimeFieldHandle)",
     (vm::IntrinsicFunction)&SystemRuntimeCompilerServicesRuntimeHelpers::create_span, create_span_invoker},
    {"System.Runtime.CompilerServices.RuntimeHelpers::IsBitwiseEquatable<>()",
     (vm::IntrinsicFunction)&SystemRuntimeCompilerServicesRuntimeHelpers::is_bitwise_equatable, is_bitwise_equatable_invoker},
    {"System.Runtime.CompilerServices.RuntimeHelpers::IsReferenceOrContainsReferences<>()",
     (vm::IntrinsicFunction)&SystemRuntimeCompilerServicesRuntimeHelpers::is_reference_or_contains_references,
     is_reference_or_contains_references_invoker},
};

utils::Span<vm::IntrinsicEntry> SystemRuntimeCompilerServicesRuntimeHelpers::get_intrinsic_entries() noexcept
{
    constexpr size_t entry_count =
        sizeof(s_intrinsic_entries_system_runtime_compilerservices_runtimehelpers) / sizeof(s_intrinsic_entries_system_runtime_compilerservices_runtimehelpers[0]);
    return utils::Span<vm::IntrinsicEntry>(s_intrinsic_entries_system_runtime_compilerservices_runtimehelpers, entry_count);
}

} // namespace intrinsics
} // namespace leanclr

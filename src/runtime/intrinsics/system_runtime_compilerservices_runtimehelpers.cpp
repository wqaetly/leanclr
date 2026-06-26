#include "system_runtime_compilerservices_runtimehelpers.h"

#include <climits>

#include "interp/interp_defs.h"
#include "vm/field.h"

namespace leanclr
{
namespace intrinsics
{

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

static vm::IntrinsicEntry s_intrinsic_entries_system_runtime_compilerservices_runtimehelpers[] = {
    {"System.Runtime.CompilerServices.RuntimeHelpers::CreateSpan<>(System.RuntimeFieldHandle)",
     (vm::IntrinsicFunction)&SystemRuntimeCompilerServicesRuntimeHelpers::create_span, create_span_invoker},
};

utils::Span<vm::IntrinsicEntry> SystemRuntimeCompilerServicesRuntimeHelpers::get_intrinsic_entries() noexcept
{
    constexpr size_t entry_count =
        sizeof(s_intrinsic_entries_system_runtime_compilerservices_runtimehelpers) / sizeof(s_intrinsic_entries_system_runtime_compilerservices_runtimehelpers[0]);
    return utils::Span<vm::IntrinsicEntry>(s_intrinsic_entries_system_runtime_compilerservices_runtimehelpers, entry_count);
}

} // namespace intrinsics
} // namespace leanclr

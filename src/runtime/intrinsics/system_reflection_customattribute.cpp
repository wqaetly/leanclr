#include "intrinsics/system_reflection_customattribute.h"

#include "interp/eval_stack_op.h"
#include "vm/class.h"
#include "vm/customattribute.h"
#include "vm/reflection.h"

namespace leanclr
{
namespace intrinsics
{

RtResult<bool> SystemReflectionCustomAttribute::is_defined(vm::RtReflectionMethod* method, vm::RtReflectionRuntimeType* attribute_type,
                                                           bool inherit) noexcept
{
    (void)inherit;
    if (method == nullptr || attribute_type == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, attr_klass,
                                            vm::Class::get_class_from_typesig(attribute_type->reflection_type.type_handle));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtMethodInfo*, method_info,
                                            vm::Reflection::get_method_info_from_reflection_object(method));
    return vm::CustomAttribute::has_customattribute_on_method(method_info, attr_klass);
}

/// @intrinsic: System.Reflection.CustomAttribute::IsDefined(System.Reflection.RuntimeMethodInfo,System.RuntimeType,System.Boolean)
static RtResultVoid is_defined_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                       interp::RtStackObject* ret) noexcept
{
    auto method = interp::EvalStackOp::get_param<vm::RtReflectionMethod*>(params, 0);
    auto attribute_type = interp::EvalStackOp::get_param<vm::RtReflectionRuntimeType*>(params, 1);
    bool inherit = interp::EvalStackOp::get_param<bool>(params, 2);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(bool, result, SystemReflectionCustomAttribute::is_defined(method, attribute_type, inherit));
    interp::EvalStackOp::set_return(ret, static_cast<int32_t>(result));
    RET_VOID_OK();
}

static vm::IntrinsicEntry s_intrinsic_entries_system_reflection_customattribute[] = {
    {"System.Reflection.CustomAttribute::IsDefined(System.Reflection.RuntimeMethodInfo,System.RuntimeType,System.Boolean)",
     (vm::IntrinsicFunction)&SystemReflectionCustomAttribute::is_defined, is_defined_invoker},
};

utils::Span<vm::IntrinsicEntry> SystemReflectionCustomAttribute::get_intrinsic_entries() noexcept
{
    return utils::Span<vm::IntrinsicEntry>(s_intrinsic_entries_system_reflection_customattribute,
                                           sizeof(s_intrinsic_entries_system_reflection_customattribute) / sizeof(vm::IntrinsicEntry));
}

} // namespace intrinsics
} // namespace leanclr

#include "system_type.h"

#include "vm/class.h"
#include "vm/reflection.h"

namespace leanclr
{
namespace intrinsics
{

RtResult<vm::RtReflectionRuntimeType*> SystemType::get_type_from_handle(const metadata::RtTypeSig* type_handle) noexcept
{
    if (type_handle == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(metadata::RtClass*, klass, vm::Class::get_class_from_typesig(type_handle));
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionType*, ref_type, vm::Reflection::get_klass_reflection_object(klass));
    RET_OK(reinterpret_cast<vm::RtReflectionRuntimeType*>(ref_type));
}

/// @intrinsic: System.Type::GetTypeFromHandle(System.RuntimeTypeHandle)
static RtResultVoid get_type_from_handle_invoker(metadata::RtManagedMethodPointer methodPtr, const metadata::RtMethodInfo* method,
                                                 const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    (void)methodPtr;
    (void)method;
    const metadata::RtTypeSig* type_handle = interp::EvalStackOp::get_param<const metadata::RtTypeSig*>(params, 0);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReflectionRuntimeType*, runtime_type, SystemType::get_type_from_handle(type_handle));
    interp::EvalStackOp::set_return(ret, runtime_type);
    RET_VOID_OK();
}

static vm::IntrinsicEntry s_intrinsic_entries_system_type[] = {
    {"System.Type::GetTypeFromHandle(System.RuntimeTypeHandle)", (vm::IntrinsicFunction)&SystemType::get_type_from_handle,
     get_type_from_handle_invoker},
};

utils::Span<vm::IntrinsicEntry> SystemType::get_intrinsic_entries() noexcept
{
    constexpr size_t entry_count = sizeof(s_intrinsic_entries_system_type) / sizeof(s_intrinsic_entries_system_type[0]);
    return utils::Span<vm::IntrinsicEntry>(s_intrinsic_entries_system_type, entry_count);
}

} // namespace intrinsics
} // namespace leanclr

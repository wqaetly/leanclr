#include "mono_runtimeclasshandle.h"

#include "icall_base.h"
#include "vm/class.h"
#include "vm/reflection.h"

namespace leanclr
{
namespace icalls
{

RtResult<const metadata::RtTypeSig*> MonoRuntimeClassHandle::get_type_from_class(metadata::RtClass* klass) noexcept
{
    RET_OK(vm::Class::get_by_val_type_sig(klass));
}

/// @icall: Mono.RuntimeClassHandle::GetTypeFromClass
static RtResultVoid get_type_from_class_invoker(metadata::RtManagedMethodPointer methodPtr, const metadata::RtMethodInfo* method,
                                                const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    auto klass_handle = EvalStackOp::get_param<const void*>(params, 0);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const metadata::RtTypeSig*, type_sig,
                                            vm::Reflection::get_type_sig_from_net10_type_handle(klass_handle));
    EvalStackOp::set_return(ret, type_sig);
    RET_VOID_OK();
}

utils::Span<vm::InternalCallEntry> MonoRuntimeClassHandle::get_internal_call_entries() noexcept
{
    static vm::InternalCallEntry s_entries[] = {
        {"Mono.RuntimeClassHandle::GetTypeFromClass", (vm::InternalCallFunction)&MonoRuntimeClassHandle::get_type_from_class, get_type_from_class_invoker},
    };
    return utils::Span<vm::InternalCallEntry>(s_entries, sizeof(s_entries) / sizeof(s_entries[0]));
}

} // namespace icalls
} // namespace leanclr

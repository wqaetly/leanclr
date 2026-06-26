#include "coreclr_qcall.h"

#include "interp/eval_stack_op.h"
#include "vm/gchandle.h"
#include "vm/pinvoke.h"
#include "vm/rt_thread.h"

namespace leanclr
{
namespace pinvokes
{
namespace
{

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

RtResultVoid get_type_handle_gc_handle_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                               interp::RtStackObject* ret) noexcept
{
    (void)interp::EvalStackOp::get_param<void*>(params, 0);
    (void)interp::EvalStackOp::get_param<void*>(params, 1);
    int32_t handle_type = interp::EvalStackOp::get_param<int32_t>(params, 2);
    void* handle = vm::GCHandle::get_target_handle(nullptr, nullptr, handle_type);
    interp::EvalStackOp::set_return(ret, handle);
    RET_VOID_OK();
}

RtResultVoid free_type_handle_gc_handle_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                                interp::RtStackObject* ret) noexcept
{
    (void)interp::EvalStackOp::get_param<void*>(params, 0);
    (void)interp::EvalStackOp::get_param<void*>(params, 1);
    void* handle = interp::EvalStackOp::get_param<void*>(params, 2);
    vm::GCHandle::free_handle(handle);
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

} // namespace

void register_coreclr_qcall_pinvokes() noexcept
{
    vm::PInvokes::register_pinvoke("System.Threading.Thread::GetCurrentThread(System.Runtime.CompilerServices.ObjectHandleOnStack)", nullptr,
                                   get_current_thread_invoker);
    vm::PInvokes::register_pinvoke("System.Threading.Thread::GetCurrentThread", nullptr, get_current_thread_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.Debugger::IsManagedDebuggerAttached()", nullptr, is_managed_debugger_attached_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.Debugger::IsManagedDebuggerAttached", nullptr, is_managed_debugger_attached_invoker);
    vm::PInvokes::register_pinvoke("System.Diagnostics.Debugger::<LogInternal>g____PInvoke|10_0", nullptr, debugger_log_invoker);
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
}

} // namespace pinvokes
} // namespace leanclr

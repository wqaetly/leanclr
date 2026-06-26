#include <cstring>

#include "system_span.h"
#include "system_readonlyspan.h"
#include "interp/interp_defs.h"
#include "interp/eval_stack_op.h"

namespace leanclr
{
namespace intrinsics
{

// ========== Implementation Functions ==========

RtResult<const uint8_t*> SystemSpan::get_item(const vm::RtReadOnlySpan<uint8_t>& span, int32_t index, size_t ele_size) noexcept
{
    if ((uint32_t)index >= (uint32_t)span.length)
    {
        RET_ERR(RtErr::IndexOutOfRange);
    }
    RET_OK(span.pointer + (static_cast<size_t>(index) * ele_size));
}

RtResult<vm::RtReadOnlySpan<uint8_t>> SystemSpan::newobj_pointer_length(void* pointer, int32_t length) noexcept
{
    if (length < 0)
    {
        RET_ERR(RtErr::ArgumentOutOfRange);
    }

    vm::RtReadOnlySpan<uint8_t> span{reinterpret_cast<const uint8_t*>(pointer), length};
    RET_OK(span);
}

RtResult<int32_t> SystemSpan::index_of_null_byte(const uint8_t* pointer) noexcept
{
    if (pointer == nullptr)
    {
        RET_ERR(RtErr::ArgumentNull);
    }

    RET_OK(static_cast<int32_t>(std::strlen(reinterpret_cast<const char*>(pointer))));
}

// ========== Invoker Functions ==========

/// @intrinsic: System.Span`1::get_Item
static RtResultVoid get_item_invoker(metadata::RtManagedMethodPointer methodPtr, const metadata::RtMethodInfo* method, const interp::RtStackObject* params,
                                     interp::RtStackObject* ret) noexcept
{
    (void)methodPtr;
    const vm::RtReadOnlySpan<uint8_t>& span = *interp::EvalStackOp::get_param<const vm::RtReadOnlySpan<uint8_t>*>(params, 0);
    int32_t index = interp::EvalStackOp::get_param<int32_t>(params, 1);

    const metadata::RtClass* klass = method->parent;
    const metadata::RtGenericClass* generic_class = klass->by_val->data.generic_class;
    const metadata::RtTypeSig* ele_type = *generic_class->class_inst->generic_args;
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(interp::ReduceTypeAndSize, type_and_size, interp::InterpDefs::get_reduce_type_and_size_by_typesig(ele_type));
    size_t ele_size = type_and_size.byte_size;

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(const uint8_t*, item_ptr, SystemSpan::get_item(span, index, ele_size));
    interp::EvalStackOp::set_return(ret, item_ptr);
    RET_VOID_OK();
}

/// @newobj: System.Span`1::.ctor(System.Void*,System.Int32)
/// @newobj: System.ReadOnlySpan`1::.ctor(System.Void*,System.Int32)
static RtResultVoid newobj_pointer_length_invoker(metadata::RtManagedMethodPointer methodPtr, const metadata::RtMethodInfo* method,
                                                  const interp::RtStackObject* params, interp::RtStackObject* ret) noexcept
{
    (void)methodPtr;
    (void)method;
    void* pointer = interp::EvalStackOp::get_param<void*>(params, 0);
    int32_t length = interp::EvalStackOp::get_param<int32_t>(params, 1);

    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(vm::RtReadOnlySpan<uint8_t>, span, SystemSpan::newobj_pointer_length(pointer, length));
    interp::EvalStackOp::set_return(ret, span);
    RET_VOID_OK();
}

static RtResultVoid index_of_null_byte_invoker(metadata::RtManagedMethodPointer, const metadata::RtMethodInfo*, const interp::RtStackObject* params,
                                               interp::RtStackObject* ret) noexcept
{
    auto pointer = interp::EvalStackOp::get_param<const uint8_t*>(params, 0);
    DECLARING_AND_UNWRAP_OR_RET_ERR_ON_FAIL(int32_t, length, SystemSpan::index_of_null_byte(pointer));
    interp::EvalStackOp::set_return(ret, length);
    RET_VOID_OK();
}

// ========== Intrinsic Entries ==========

static vm::IntrinsicEntry s_intrinsic_entries_system_span[] = {
    {"System.Span`1::get_Item", (vm::IntrinsicFunction)&SystemSpan::get_item, get_item_invoker},
    {"System.ReadOnlySpan`1::get_Item", (vm::IntrinsicFunction)&SystemSpan::get_item, get_item_invoker},
    {"System.SpanHelpers::IndexOfNullByte(System.Byte*)", (vm::IntrinsicFunction)&SystemSpan::index_of_null_byte,
     index_of_null_byte_invoker},
    {"System.SpanHelpers::IndexOfNullByte", (vm::IntrinsicFunction)&SystemSpan::index_of_null_byte, index_of_null_byte_invoker},
};

static vm::NewobjIntrinsicEntry s_newobj_intrinsic_entries_system_span[] = {
    {"System.Span`1::.ctor(System.Void*,System.Int32)", newobj_pointer_length_invoker},
    {"System.ReadOnlySpan`1::.ctor(System.Void*,System.Int32)", newobj_pointer_length_invoker},
};

utils::Span<vm::IntrinsicEntry> SystemSpan::get_intrinsic_entries() noexcept
{
    constexpr size_t entry_count = sizeof(s_intrinsic_entries_system_span) / sizeof(s_intrinsic_entries_system_span[0]);
    return utils::Span<vm::IntrinsicEntry>(s_intrinsic_entries_system_span, entry_count);
}

utils::Span<vm::NewobjIntrinsicEntry> SystemSpan::get_newobj_intrinsic_entries() noexcept
{
    constexpr size_t entry_count = sizeof(s_newobj_intrinsic_entries_system_span) / sizeof(s_newobj_intrinsic_entries_system_span[0]);
    return utils::Span<vm::NewobjIntrinsicEntry>(s_newobj_intrinsic_entries_system_span, entry_count);
}

} // namespace intrinsics
} // namespace leanclr

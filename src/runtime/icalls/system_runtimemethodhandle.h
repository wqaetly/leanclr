#pragma once

#include "icall_base.h"

namespace leanclr
{
namespace icalls
{

class SystemRuntimeMethodHandle
{
  public:
    static utils::Span<vm::InternalCallEntry> get_internal_call_entries() noexcept;

    static RtResult<intptr_t> get_function_pointer(intptr_t method) noexcept;
    static RtResult<int32_t> get_method_def(const metadata::RtMethodInfo* method) noexcept;
};

} // namespace icalls
} // namespace leanclr

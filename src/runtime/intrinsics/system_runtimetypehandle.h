#pragma once

#include "vm/intrinsics.h"

namespace leanclr
{
namespace intrinsics
{
class SystemRuntimeTypeHandle
{
  public:
    static utils::Span<vm::IntrinsicEntry> get_intrinsic_entries() noexcept;

    static RtResult<vm::RtReflectionRuntimeType*> get_runtime_type(const metadata::RtClass* method_table) noexcept;
    static RtResult<vm::RtReflectionRuntimeType*> get_runtime_type_from_handle(void* runtime_type_handle) noexcept;
    static RtResult<bool> can_cast_to(vm::RtReflectionRuntimeType* source_type, vm::RtReflectionRuntimeType* target_type) noexcept;
};
} // namespace intrinsics
} // namespace leanclr

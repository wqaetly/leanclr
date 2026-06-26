#pragma once

#include "vm/intrinsics.h"

namespace leanclr
{
namespace intrinsics
{
class SystemRuntimeType
{
  public:
    static RtResult<vm::RtReflectionField*> get_field(vm::RtReflectionRuntimeType* runtime_type, vm::RtString* name, int32_t binding_flags) noexcept;
    static utils::Span<vm::IntrinsicEntry> get_intrinsic_entries() noexcept;
};
} // namespace intrinsics
} // namespace leanclr

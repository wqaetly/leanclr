#pragma once

#include "vm/intrinsics.h"

namespace leanclr
{
namespace intrinsics
{

class SystemRuntimeCompilerServicesUnsafe
{
  public:
    static RtResult<void*> as_pointer(void* location) noexcept;

    static utils::Span<vm::IntrinsicEntry> get_intrinsic_entries() noexcept;
};

} // namespace intrinsics
} // namespace leanclr

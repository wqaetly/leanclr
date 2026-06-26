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
    static RtResult<void*> as(void* source) noexcept;
    static RtResultVoid bit_cast(const metadata::RtMethodInfo* method, const interp::RtStackObject* params,
                                 interp::RtStackObject* ret) noexcept;

    static utils::Span<vm::IntrinsicEntry> get_intrinsic_entries() noexcept;
};

} // namespace intrinsics
} // namespace leanclr

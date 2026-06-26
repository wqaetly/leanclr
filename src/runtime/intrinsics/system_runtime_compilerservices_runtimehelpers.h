#pragma once

#include "vm/intrinsics.h"

namespace leanclr
{
namespace intrinsics
{
class SystemRuntimeCompilerServicesRuntimeHelpers
{
  public:
    static utils::Span<vm::IntrinsicEntry> get_intrinsic_entries() noexcept;

    static RtResult<vm::RtReadOnlySpan<uint8_t>> create_span(const metadata::RtMethodInfo* method, const metadata::RtFieldInfo* field) noexcept;
};
} // namespace intrinsics
} // namespace leanclr

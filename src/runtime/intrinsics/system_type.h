#pragma once

#include "vm/intrinsics.h"

namespace leanclr
{
namespace intrinsics
{
class SystemType
{
  public:
    static utils::Span<vm::IntrinsicEntry> get_intrinsic_entries() noexcept;

    static RtResult<vm::RtReflectionRuntimeType*> get_type_from_handle(const metadata::RtTypeSig* type_handle) noexcept;
    static RtResult<bool> get_is_value_type(vm::RtReflectionRuntimeType* runtime_type) noexcept;
};
} // namespace intrinsics
} // namespace leanclr

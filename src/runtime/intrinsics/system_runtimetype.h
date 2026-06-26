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
    static RtResult<vm::RtArray*> get_custom_attributes(vm::RtReflectionRuntimeType* runtime_type, vm::RtReflectionRuntimeType* attribute_type,
                                                        bool inherit) noexcept;
    static RtResult<vm::RtReflectionRuntimeType*> get_parent_type(vm::RtReflectionRuntimeType* runtime_type) noexcept;
    static RtResult<bool> get_is_actual_interface(vm::RtReflectionRuntimeType* runtime_type) noexcept;
    static utils::Span<vm::IntrinsicEntry> get_intrinsic_entries() noexcept;
};
} // namespace intrinsics
} // namespace leanclr

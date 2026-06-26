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
    static RtResult<vm::RtArray*> get_methods(vm::RtReflectionRuntimeType* runtime_type, int32_t binding_flags) noexcept;
    static RtResult<vm::RtArray*> get_custom_attributes(vm::RtReflectionRuntimeType* runtime_type, vm::RtReflectionRuntimeType* attribute_type,
                                                        bool inherit) noexcept;
    static RtResult<vm::RtReflectionRuntimeType*> get_parent_type(vm::RtReflectionRuntimeType* runtime_type) noexcept;
    static RtResult<bool> get_is_actual_interface(vm::RtReflectionRuntimeType* runtime_type) noexcept;
    static RtResult<bool> get_is_generic_type(vm::RtReflectionRuntimeType* runtime_type) noexcept;
    static RtResult<bool> get_is_generic_type_definition(vm::RtReflectionRuntimeType* runtime_type) noexcept;
    static RtResult<vm::RtObject*> create_instance(vm::RtReflectionRuntimeType* runtime_type) noexcept;
    static RtResultVoid call_default_struct_constructor(vm::RtReflectionRuntimeType* runtime_type, void* data) noexcept;
    static utils::Span<vm::IntrinsicEntry> get_intrinsic_entries() noexcept;
};
} // namespace intrinsics
} // namespace leanclr

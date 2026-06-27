#pragma once

#include "rt_managed_types.h"

namespace leanclr
{
namespace vm
{
class Reflection
{
  public:
    static RtResult<RtReflectionType*> get_type_reflection_object(const metadata::RtTypeSig* type_sig);
    static RtResult<RtReflectionType*> get_klass_reflection_object(const metadata::RtClass* klass);
    static RtResult<const metadata::RtTypeSig*> get_net10_type_handle(const metadata::RtTypeSig* type_sig);
    static RtResult<const metadata::RtTypeSig*> get_type_sig_from_net10_type_handle(const void* type_handle);
    static RtResult<const metadata::RtClass*> get_class_from_net10_method_table(const void* method_table);
    static RtResult<RtReflectionMethod*> get_method_reflection_object(const metadata::RtMethodInfo* method, const metadata::RtClass* reflection_at_klass);
    static RtResult<const metadata::RtMethodInfo*> get_method_info_from_reflection_object(RtReflectionMethod* method_obj);
    static RtResult<const metadata::RtClass*> get_reflection_method_klass(RtReflectionMethod* method_obj);
    static RtResult<RtArray*> get_param_objects(const metadata::RtMethodInfo* method, const metadata::RtClass* reflection_at_klass);
    static RtResult<RtReflectionField*> get_field_reflection_object(const metadata::RtFieldInfo* field, const metadata::RtClass* reflection_at_klass);
    static RtResult<const metadata::RtFieldInfo*> get_field_info_from_reflection_object(RtReflectionField* field_obj);
    static RtResult<const metadata::RtClass*> get_reflection_field_klass(RtReflectionField* field_obj);
    static RtResult<RtReflectionProperty*> get_property_reflection_object(const metadata::RtPropertyInfo* prop, const metadata::RtClass* reflection_at_klass);
    static RtResult<RtReflectionEventInfo*> get_event_reflection_object(metadata::RtEventInfo* event_info, const metadata::RtClass* reflection_at_klass);
    static RtResult<RtReflectionAssembly*> get_assembly_reflection_object(metadata::RtAssembly* assembly);
    static RtResult<metadata::RtMonoAssemblyName*> get_assembly_name_object(metadata::RtAssembly* ass);
    static RtResult<RtReflectionModule*> get_module_reflection_object(metadata::RtModuleDef* mod);
    static RtResult<RtObject*> invoke_method(const metadata::RtMethodInfo* method, RtObject* obj, RtArray* params, RtObject** out_ex);
};
} // namespace vm
} // namespace leanclr

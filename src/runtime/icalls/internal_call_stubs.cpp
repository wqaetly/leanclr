#include "internal_call_stubs.h"

#include "system_array.h"
#include "system_object.h"
#include "system_reflection_runtimemethodinfo.h"
#include "system_runtime_compilerservices_runtimehelpers.h"
#include "system_diagnostics_stopwatch.h"
#include "system_runtimetype.h"
#include "system_runtimetypehandle.h"
#include "icalls/system_string.h"
#include "system_globalization_cultureinfo.h"
#include "system_globalization_culturedata.h"
#include "system_globalization_calendardata.h"
#include "system_globalization_compareinfo.h"
#include "system_threading_interlocked.h"
#include "system_threading_thread.h"
#include "system_type.h"
#include "system_valuetype.h"
#include "system_environment.h"
#include "system_runtime_interopservices_gchandle.h"
#include "system_runtime_interopservices_marshal.h"
#include "system_runtime_interopservices_runtimeinformation.h"
#include "system_runtime_dependenthandle.h"
#include "system_runtime_runtimeimports.h"
#include "system_runtimefieldhandle.h"
#include "system_runtimemethodhandle.h"
#include "system_signature.h"
#include "system_enum.h"
#include "system_buffer.h"
#include "system_reflection_fieldinfo.h"
#include "system_reflection_runtimeconstructorinfo.h"
#include "system_reflection_runtimefieldinfo.h"
#include "system_reflection_runtimepropertyinfo.h"
#include "system_reflection_assembly.h"
#include "system_reflection_methodbase.h"
#include "system_reflection_runtimeassembly.h"
#include "system_reflection_assemblyname.h"
#include "system_reflection_customattributedata.h"
#include "system_reflection_eventinfo.h"
#include "system_reflection_runtimeeventinfo.h"
#include "system_reflection_runtimeparameterinfo.h"
#include "system_threading_monitor.h"
#include "system_threading_timer.h"
#include "system_threading_nativeeventcalls.h"
#include "system_threading_osspecificsynchronizationcontext.h"
#include "system_threading_volatile.h"
#include "system_appdomain.h"
#include "system_delegate.h"
#include "system_argiterator.h"
#include "system_diagnostics_debugger.h"
#include "system_diagnostics_stackframe.h"
#include "system_diagnostics_stacktrace.h"
#include "system_exception.h"
#include "system_reflection_runtimemodule.h"
#include "system_currentsystemtimezone.h"
#include "system_text_encodinghelper.h"
#include "system_security_cryptography_rngcryptoserviceprovider.h"
#include "system_security_securitymanager.h"
#include "system_threading_internalthread.h"
#include "system_threading_threadpool.h"
#include "system_typedreference.h"
#include "system_gc.h"
#include "system_datetime.h"
#include "system_math.h"
#include "system_mathf.h"
#include "system_io_path.h"
#include "system_console_windowsconsole.h"
#include "system_consoleriver.h"
#include "system_windowsconsoledriver.h"
#include "interop.h"
#include "leanclr_profile.h"

namespace leanclr
{
namespace icalls
{

template <typename T>
static void Append(utils::Vector<T>& entries, const utils::Span<T>& sub_entries) noexcept
{
    entries.push_range(sub_entries.begin(), sub_entries.size());
}

void InternalCallStubs::get_internal_call_entries(utils::Vector<vm::InternalCallEntry>& entries) noexcept
{
    entries.reserve(1000);
    Append(entries, SystemArray::get_internal_call_entries());
    Append(entries, SystemObject::get_internal_call_entries());
    Append(entries, SystemRuntimeCompilerServicesRuntimeHelpers::get_internal_call_entries());
    Append(entries, SystemRuntimeType::get_internal_call_entries());
    Append(entries, SystemRuntimeTypeHandle::get_net10_internal_call_entries());
    Append(entries, SystemString::get_internal_call_entries());
    Append(entries, SystemGlobalizationCultureInfo::get_internal_call_entries());
    Append(entries, SystemGlobalizationCultureData::get_internal_call_entries());
    Append(entries, SystemGlobalizationCalendarData::get_internal_call_entries());
    Append(entries, SystemGlobalizationCompareInfo::get_internal_call_entries());
    Append(entries, SystemEnvironment::get_internal_call_entries());
    Append(entries, SystemThreadingInterlocked::get_internal_call_entries());
    Append(entries, SystemThreadingThread::get_net10_internal_call_entries());
    Append(entries, SystemType::get_internal_call_entries());
    Append(entries, SystemValueType::get_internal_call_entries());
    Append(entries, SystemTypedReference::get_internal_call_entries());
    Append(entries, SystemRuntimeInteropServicesMarshal::get_internal_call_entries());
    Append(entries, SystemRuntimeInteropServicesGCHandle::get_internal_call_entries());
    Append(entries, SystemRuntimeInteropServicesRuntimeInformation::get_internal_call_entries());
    Append(entries, SystemRuntimeDependentHandle::get_internal_call_entries());
    Append(entries, SystemRuntimeRuntimeImports::get_internal_call_entries());
    Append(entries, SystemRuntimeFieldHandle::get_net10_internal_call_entries());
    Append(entries, SystemRuntimeMethodHandle::get_net10_internal_call_entries());
    Append(entries, SystemSignature::get_internal_call_entries());
    Append(entries, Interop::get_internal_call_entries());
    Append(entries, SystemEnum::get_internal_call_entries());
    Append(entries, SystemBuffer::get_internal_call_entries());
    Append(entries, SystemReflectionRuntimeMethodInfo::get_net10_internal_call_entries());
    Append(entries, SystemReflectionRuntimeFieldInfo::get_net10_internal_call_entries());
    Append(entries, SystemReflectionMethodBase::get_internal_call_entries());
    Append(entries, SystemReflectionRuntimeAssembly::get_net10_internal_call_entries());
    Append(entries, SystemThreadingMonitor::get_internal_call_entries());
    Append(entries, SystemThreadingVolatile::get_internal_call_entries());
    Append(entries, SystemDelegate::get_internal_call_entries());
    Append(entries, SystemException::get_net10_internal_call_entries());
    Append(entries, SystemReflectionRuntimeModule::get_net10_internal_call_entries());
    Append(entries, SystemGC::get_internal_call_entries());
    Append(entries, SystemDateTime::get_internal_call_entries());
    Append(entries, SystemMath::get_internal_call_entries());
    Append(entries, SystemMathF::get_internal_call_entries());
    Append(entries, SystemIOPath::get_internal_call_entries());
    Append(entries, SystemTextEncodingHelper::get_internal_call_entries());
    Append(entries, LeanCLRProfile::get_internal_call_entries());
}

void InternalCallStubs::get_newobj_internal_call_entries(utils::Vector<vm::NewobjInternalCallEntry>& entries) noexcept
{
    entries.reserve(200);
    // append all newobj internal call entries here
    Append(entries, SystemString::get_newobj_internal_call_entries());
    Append(entries, SystemReflectionRuntimeMethodInfo::get_newobj_internal_call_entries());
    Append(entries, SystemReflectionRuntimeFieldInfo::get_newobj_internal_call_entries());
    Append(entries, SystemReflectionRuntimePropertyInfo::get_newobj_internal_call_entries());
}

} // namespace icalls
} // namespace leanclr

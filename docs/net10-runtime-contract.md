# LeanCLR .NET 10 Runtime Contract Plan

状态：第三版执行基准（2026-06-29）

本文档是 `coreclr-net10` / `minimal-net10` 的 contract-first 改造清单。它回答两个问题：

1. .NET 10 `System.Private.CoreLib` 期望 runtime 暴露什么形状。
2. LeanCLR 应该如何把这些形状映射到已有 VM，而不是继续复用 Mono-era 适配或重写整套 CLR。

## 目标

当前阶段的目标是让 LeanCLR 稳定运行项目自己的、受控的纯逻辑 `net10.0` DLL。实现方式不是按旧 managed / Mono 测试失败点继续补丁，而是先重建 .NET 10 主路径的 runtime contract / model，再用测试和真实 workload 验收。

第一阶段完成标准：

- 每个 net10 runtime 入口都能说明来源：CoreLib 调用链、runtime pack extern、smoke 入口或真实 workload。
- 每个 façade 都有明确的 LeanCLR 内部映射：`RtClass`、`RtMethodInfo`、`RtFieldInfo`、`RtModuleDef`、metadata cache、interpreter、GC 或 host bridge。
- `coreclr-net10` 不隐式命中 Mono-era 入口；旧 Mono 适配保留在 `mono45` / legacy profile。
- 白名单外 API 输出明确诊断或 `NotSupported`，不以宽松签名匹配继续前进。

## 当前执行批次

本轮不再把旧用例失败点当作设计入口，而是按 `.NET 10` CoreLib 真正会读取的 runtime façade 逐层收敛。当前 P0 已经从 `RuntimeType` 身份推进到 `RuntimeFieldHandle`、`RuntimeModule`、`ValueType`、delegate 相关 `MethodTable*` façade、multicast delegate allocation 和 legacy `RunAll` 基线；P1 已经覆盖 `CustomAttributeData` metadata-only、NKG/Odin 轻量 workload、单线程 threading / file I/O façade、RuntimeHelpers / Span / RVA 最小语义、AssemblyLoadContext / Reflection.Emit 受限 façade，以及 handle/reflection consolidation。P2 mock host 已覆盖 ABI skeleton、opaque handle registry、主线程 dispatcher 和 event/callback bridge；P3 已启动托管 wrapper contract 和 engine binding minimal adapter，下一步转向 value marshal / property-call 的最小引擎交互面。

已验证切片：

| 批次 | 已落地 contract | 证据 | 后续要求 |
| --- | --- | --- | --- |
| P0.1 RuntimeType 身份与 Type equality | `typeof(T)`、`Object.GetType()`、`Type.GetTypeFromHandle()`、`Signature.Init` / `MethodInfo.ReturnType` 对同一 `RtClass` 走同一套 net10 façade；解释器 `bool` 返回写满栈槽，避免高位脏值影响 `Type::op_Inequality` | `ManagedNet10.LegacyTests.Program::RunLegacyDiscoverySmoke` 通过 | 保留为第一道 reflection discovery gate |
| P0.2 RuntimeModule / RuntimeFieldHandle | `RuntimeFieldHandleInternal` 可从 direct field desc、栈槽、boxed handle、`RtFieldInfo` 与 runtime field info stub 解码；`GetApproxDeclaringMethodTable` 返回 net10 MethodTable façade | `ManagedNet10.LegacyTests.Program::RunCorlibReflectionRuntimeModule` 通过 | 将所有 field handle 入口纳入同一 decode helper，继续禁止宽松签名误命中 |
| P0.3 ValueType MethodTable façade | `ValueType` QCall/PInvoke 入口不再把 `System.Runtime.CompilerServices.MethodTable*` 当 raw `RtClass*`；边界处统一解析到 LeanCLR `RtClass` | `RunCorlibValueTypeEqualsStructValueTypes`、`RunCorlibValueTypeGetHashCodeStructIsStable` 通过 | 所有 `MethodTable*` native 边界必须复用同一解析规则 |
| P0.4 Delegate MethodTable façade | `System.Delegate::GetInvokeMethod(MethodTable*)` 和 `GetMulticastInvoke(MethodTable*)` 已改为解析 net10 MethodTable façade | `ManagedNet10.LegacyTests.Program::RunRuntimeDelegateDynamicInvoke` 通过 | 所有 `MethodTable*` native 边界必须复用同一解析规则 |
| P0.5 Delegate allocation / multicast | `MulticastDelegate.NewMulticastDelegate` 触发的 `RuntimeTypeHandle.InternalAllocNoChecks_FastPath(MethodTable*)` 解析 net10 MethodTable façade，而不是 raw `RtClass*` | `ManagedNet10.LegacyTests.Program::RunRuntimeDelegateDynamicInvoke` 通过；未发现 `[delegate-dyn]` 临时日志残留 | 保留为 delegate multicast allocation gate |
| P0.6 RunAll 重新分层 | delegate 切片通过后重新跑 `ManagedNet10.LegacyTests.Program::RunAll`，把后续失败归类为 contract、façade、VM 通用语义或白名单外 API | `ManagedNet10.LegacyTests.Program::RunAll` 通过 | 保留为 P0 回归基线，不作为设计来源 |
| P1.1 CustomAttributeData metadata-only | `RuntimeCustomAttributeData` 最小路径可从 assembly / module / type / field / method / parameter / property / event target 的 metadata blob 解出 constructor arguments、property named argument 和 field named argument，不需要实例化 attribute；同时覆盖 enum、Type、int[]、Type[]、object、object[]、named enum/type/array/object 等 Odin/NKG 常见 blob 形状 | `ManagedNet10.Smoke.Program::TestCustomAttributeDataOnly` 纳入默认 smoke；`interp-smoke.ps1 -Configuration Release` 通过 | 继续接 NKG/Odin 轻量 attribute 枚举作为真实 workload gate |
| P1.2 NKG/Odin light workload | `ManagedNet10.NkgSmoke` 使用真实 NKGGameFramework、OdinSerializer 与 UniTask `net10.0` 输出目录验证轻量反射、attribute 枚举和白名单 API 边界；默认 sampler/Hosting 完整面仍在第一阶段外 | `scripts/dotnet10/api-scan.ps1` 扫描 NKG core/Odin/UniTask：unsupported `0`；`scripts/dotnet10/nkg-smoke.ps1` 通过 | 保留为真实 workload gate；后续失败按 contract、façade、VM 通用语义或白名单外 API 归类 |
| P1.3 Thread / Monitor / WaitHandle bounded semantics | 单线程 profile 下补齐 `CurrentManagedThreadId`、Monitor fast path / wait PInvoke、ThreadPool sizing / queue dispatch、ManualResetEvent / WaitAny 最小路径；复杂并行调度仍不承诺 | `ManagedNet10.LegacyTests.Program::RunCorlibThreading` 通过；`ManagedNet10.LegacyTests.Program::RunCorlibMonitor` 通过 | 保留为 threading regression gate；后续 Task/dispatcher 只接可控 continuation |
| P1.4 System.IO / Kernel32 platform façade | .NET 10 `FileStream` / `File` / `Path` 触发的 Kernel32 generated PInvoke 统一落到 LeanCLR cross-platform `platform::Kernel32` / `os::File` / `os::Path`，非 Windows 走 POSIX fallback 并写回 Win32-style last error | `ManagedNet10.LegacyTests.Program::RunCorlibIO` 通过 | 保留为 file I/O regression gate；完整 watcher、ACL、reparse point、overlapped I/O 后置 |
| P1.5 RuntimeHelpers / Span / RVA | `RuntimeHelpers.RunClassConstructor` / `RunModuleConstructor`、stack check、`CompileMethod` / `PrepareMethod`、RVA `InitializeArray` / `CreateSpan`、inline array span helper 和 bitwise/reference checks 统一映射到 LeanCLR class/module/interpreter metadata；method preparation 在解释 profile 中是 no-op façade | `ManagedNet10.LegacyTests.Program::RunCorlibRuntimeHelpers`、`ManagedNet10.Smoke.Program::TestRuntimeHelpers`、`ManagedNet10.Smoke.Program::TestSpan`、`ManagedNet10.LegacyTests.Program::RunNet10SpanBinaryPrimitives` 通过 | 保留为 RuntimeHelpers/Span regression gate；byref-like escape、任意 function pointer 和完整 JIT preparation 后置 |
| P1.6 AssemblyLoadContext / Reflection.Emit limited façade | `AssemblyLoadContext` 初始化、已加载程序集枚举、release bookkeeping、`RuntimeAssemblyBuilder.CreateDynamicAssembly` 和 `ModuleHandle.GetDynamicMethod` 只承诺 LeanCLR metadata / interpreter 能消费的受限动态程序集与 light lambda 路径；collectible ALC、LoaderAllocator、unload 和任意动态 IL/JIT codegen 后置 | `ManagedNet10.LegacyTests.Program::RunCorlibLightLambda` 通过 | 保留为 Reflection.Emit / light lambda regression gate；后续动态方法失败先区分 metadata façade 缺口和完整 JIT codegen 非目标 |
| P1.7 Handle / reflection consolidation | type / method / field / module / assembly handle 入口统一经 `vm::Reflection` 边界解码；`MethodTable*` 走 net10 façade registry；QCall object/slot 参数先做 GC allocated-object guard，再映射到 LeanCLR metadata；unsupported handle shape 输出明确错误，不靠宽松指针猜测前进 | `RunLegacyDiscoverySmoke`、`RunCorlibReflectionRuntimeModule`、`RunRuntimeDelegateDynamicInvoke`、`RunCorlibValueTypeEqualsStructValueTypes`、`RunCorlibValueTypeGetHashCodeStructIsStable`、`RunCorlibRuntimeHelpers`、`RunNet10SpanBinaryPrimitives`、`api-scan.ps1`、`nkg-smoke.ps1` 通过 | 保留为 reflection/handle regression gate；新增 CoreLib handle 入口必须复用同一 helper 或显式说明例外 |
| P2.1 Host Bridge ABI skeleton | 新增 `LeanClrHostBridgeFunctions` C ABI、ABI version、capability flags、状态码、错误字符串和最小 managed entry callback；mock host 只验证初始化、函数表校验和托管静态入口回调，不加载真实 Unity/Godot 或完整 runtime hosting API | `scripts/dotnet10/host-bridge-smoke.ps1 -Configuration Release -Scenario AbiSkeleton` 通过 | 保留为 host bridge ABI regression gate；后续 P2.2-P2.4 在同一脚本上扩展 scenario |
| P2.2 Opaque handle registry | 在 host function table 中定义宿主对象 handle 创建、retain/release、判活和销毁通知；mock host 维护 opaque handle table，托管侧只接收 `LeanClrHostHandle`，不暴露真实宿主对象指针 | `scripts/dotnet10/host-bridge-smoke.ps1 -Configuration Release -Scenario HandleRegistry` 通过 | 保留为 handle lifecycle regression gate；后续 dispatcher/event callback 必须复用同一 handle 失效诊断语义 |
| P2.3 Main-thread dispatcher | 在 host function table 中定义主线程投递、next-frame pump、同步调用和 reentry guard；mock host 只承诺 FIFO 队列、受控 pump、callback 结果回传和托管异常诊断，不扩展完整 ThreadPool | `scripts/dotnet10/host-bridge-smoke.ps1 -Configuration Release -Scenario Dispatcher` 通过 | 保留为 dispatcher regression gate；event/callback bridge 必须通过同一主线程队列回到托管调用边界 |
| P2.4 Event / callback bridge | 在 host function table 中定义 event subscription token、取消订阅和宿主事件触发；mock host 触发事件时先投递到主线程 dispatcher，再由 pump 进入托管回调边界，取消订阅后触发为 no-op | `scripts/dotnet10/host-bridge-smoke.ps1 -Configuration Release -Scenario EventCallback` 通过 | 保留为 event/callback regression gate；真实 UnityEvent / Godot Signal 和复杂 Variant marshal 后置 |
| P3.1 Managed host wrapper contract | 新增 `ManagedNet10.Smoke.HostBridgeWrapperSmoke`，定义托管侧 `HostObject` / `HostEventSubscription` / `HostDispatcher` 最小 wrapper contract；wrapper 只保存 opaque handle / subscription token，通过 mock bridge 把 host status 转成 `ObjectDisposedException` 或 `HostBridgeException` | `scripts/dotnet10/interp-smoke.ps1 -Configuration Release -Entry "ManagedNet10.Smoke.HostBridgeWrapperSmoke::Run"` 通过；`scripts/dotnet10/api-scan.ps1 -Configuration Release` unsupported `0`；P2 host bridge 四场景仍通过 | 保留为 managed wrapper regression gate；后续真实 Unity/Godot 接入必须先落到同一 wrapper contract，再连接具体引擎对象模型 |
| P3.2 Engine binding minimal adapter | 新增 native `EngineAdapter` mock 场景和 `ManagedNet10.Smoke.EngineBindingSmoke`，把引擎节点创建、事件订阅、事件触发、主线程 pump 和宿主销毁诊断串到 P2/P3.1 contract；托管 `EngineNode` 只持有 `HostObject`，不接触真实引擎指针 | `scripts/dotnet10/host-bridge-smoke.ps1 -Configuration Release -Scenario EngineAdapter` 通过；`scripts/dotnet10/interp-smoke.ps1 -Configuration Release -Entry "ManagedNet10.Smoke.EngineBindingSmoke::Run"` 通过；`scripts/dotnet10/api-scan.ps1 -Configuration Release` unsupported `0` | 保留为 engine binding adapter regression gate；后续属性/方法调用必须继续经过 opaque handle、dispatcher 和明确 value marshal |

下一批次：

| 批次 | 目标 | 输出 | 验收 |
| --- | --- | --- | --- |
| P3.3 Value marshal / property-call adapter | 定义 bool/int/float/string 与小 struct 的最小 value marshal，以及引擎属性 get/set / command 调用的错误转换 | native value marshal stub + managed property smoke | 不引入复杂 Variant / UnityEngine.Object 语义；失败仍走 host status + managed exception |

## 非目标

- 不重写 LeanCLR 的 metadata loader、类型系统、对象模型、解释器、GC、异常、委托和泛型底座。
- 不复制 CoreCLR 的 MethodTable、loader、GC、ThreadPool、PAL 和完整 VM 实现。
- 不承诺第一阶段完整承载 `Microsoft.NETCore.App`。
- 不再把 extern diff 归零作为近期目标；extern diff 只作为定位工具。
- 不删除 `mono45` / Unity 旧资产；先隔离 profile，等 `coreclr-net10` 跑绿后再清理。

## 输入来源

| 来源 | 用途 | 输出 |
| --- | --- | --- |
| .NET 10 `System.Private.CoreLib` 源码 | 确认托管类型、私有字段、QCall/PInvoke/Intrinsic 调用链 | contract map |
| CoreCLR VM / native entry 源码 | 理解入口语义，但不照搬实现 | LeanCLR 映射说明 |
| .NET 10 runtime pack extern 清单 | 确认签名和调用入口是否仍存在 | `coreclr-net10` catalog 差异 |
| `ManagedNet10.Smoke` | 稳定触发小能力入口 | 每个领域的最小验收 |
| 原作者 managed / Mono 测试资产 | 证明 LeanCLR runtime 能力没有退化 | 分阶段回归验收 |
| NKGGameFramework 纯逻辑 DLL | 证明真实项目结构能跑 | workload 验收 |

## Contract 领域

### 1. Type / RuntimeType

外部形状：

- `object.GetType()` 返回 CoreLib 可接受的 `System.RuntimeType` / `System.Type` 对象。
- `RuntimeType` 的 `Name`、`FullName`、`BaseType`、`IsAssignableFrom`、`IsSubclassOf` 等基础查询稳定。
- `typeof(T)`、boxed value type、array type、generic type、byref-like type 的 type handle 能被区分。

内部映射：

- `RuntimeType` façade 持有或可解析到 `metadata::RtClass*`。
- 类型名、namespace、assembly、metadata token 从 `RtClass` / `RtModuleDef` 派生。
- assignable / subclass / interface 判断优先复用 `vm::Class` 的现有语义。

验收入口：

- `Object.GetType()` on reference type / boxed value type。
- `RuntimeType.Name` / `FullName`。
- `RuntimeTypeHandle.GetBaseType` / `is_subclass_of`。

### 2. RuntimeTypeHandle

外部形状：

- `RuntimeTypeHandle` / `RuntimeTypeHandleInternal` / QCall handle 参数能在 CoreLib 调用链中往返。
- 支持 array rank、element type、generic definition、generic instantiation、metadata token、module、assembly 等查询。

内部映射：

- handle 只表示 LeanCLR 中稳定的 `RtClass` 身份，不暴露 CoreCLR MethodTable 假设。
- `QCallTypeHandle` 等 façade 应在边界处转换为 `RtClass*`，并集中校验空值、类型不匹配和 unsupported 状态。

验收入口：

- `RuntimeTypeHandle.GetArrayRank`。
- `RuntimeTypeHandle.GetElementType`。
- `RuntimeTypeHandle.GetModule` / `GetAssembly`。
- `RuntimeTypeHandle.GetGenericTypeDefinition`。

### 3. RuntimeMethodHandle / RuntimeFieldHandle

外部形状：

- 反射方法、字段和 handle 能互相转换。
- `GetFunctionPointer`、`GetValueDirect`、`SetValueDirect`、RVA field 查询等入口有明确支持或明确拒绝。

内部映射：

- method handle 映射到 `metadata::RtMethodInfo*`。
- field handle 映射到 `metadata::RtFieldInfo*`。
- function pointer 只在 LeanCLR 已有 AOT/native bridge 能表达时支持；解释路径默认不伪造不可调用指针。
- field get/set 复用现有 field offset、static data、boxed value type 和 GC write barrier 规则。
- direct metadata pointer、栈槽、boxed handle、runtime reflection object 和 stub object 必须先进入同一 decode helper，再交给具体业务入口。
- `MethodTable*` 参数必须先解析为 net10 MethodTable façade registry 中的类型身份，不允许在 native 边界直接当作 `RtClass*` 使用。

验收入口：

- 反射枚举字段/方法。
- private field 基础读写。
- RVA initializer / static readonly data。

### 3.1 Net10 handle boundary consolidation

外部形状：

- CoreLib 传入的 `RuntimeTypeHandle`、`RuntimeMethodHandleInternal`、`RuntimeFieldHandleInternal`、`QCallTypeHandle`、`QCallModule`、`QCallAssembly` 和 `MethodTable*` 都有集中解析路径。
- QCall 参数可以是 direct managed object、object handle slot 或 native handle；slot/object 形状必须先通过 GC allocated-object guard 和 runtime reflection class 校验。
- unsupported handle 形状返回 `ArgumentNull`、`BadImageFormat`、`Argument` 或明确 `NotSupported`，不静默退回 Mono-era 宽松签名或 raw pointer 猜测。

内部映射：

- `RuntimeTypeHandle` / `MethodTable*` 经 `get_net10_method_table`、`get_type_sig_from_net10_method_table`、`get_class_from_net10_method_table` 和 `get_type_sig_from_qcall_type_handle` 落到 `RtTypeSig` / `RtClass`。
- `RuntimeMethodHandleInternal` 经 `get_method_info_from_handle_arg` 解析 reflection method/constructor object、runtime method info stub、direct metadata pointer 或栈槽。
- `RuntimeFieldHandleInternal` 经 `get_field_info_from_handle_arg` 解析 reflection field object、boxed runtime field handle、direct metadata pointer、栈槽或 runtime field info stub。
- `QCallModule` / `QCallAssembly` 经 `get_module_from_qcall_module` / `get_assembly_from_qcall_assembly` 映射到 LeanCLR module / assembly registry。

验收入口：

- `ManagedNet10.LegacyTests.Program::RunLegacyDiscoverySmoke`。
- `ManagedNet10.LegacyTests.Program::RunCorlibReflectionRuntimeModule`。
- `ManagedNet10.LegacyTests.Program::RunRuntimeDelegateDynamicInvoke`。
- `ManagedNet10.LegacyTests.Program::RunCorlibValueTypeEqualsStructValueTypes`。
- `ManagedNet10.LegacyTests.Program::RunCorlibValueTypeGetHashCodeStructIsStable`。
- `ManagedNet10.LegacyTests.Program::RunCorlibRuntimeHelpers`。
- `ManagedNet10.LegacyTests.Program::RunNet10SpanBinaryPrimitives`。
- `scripts/dotnet10/api-scan.ps1` 与 `scripts/dotnet10/nkg-smoke.ps1`。

### 4. RuntimeAssembly / RuntimeModule

外部形状：

- `RuntimeAssembly.GetFullName`、manifest module、module list、module type 枚举稳定。
- `RuntimeModule.GetTypes` / token resolve 只覆盖 minimal profile 需要的查询。

内部映射：

- assembly façade 映射到 LeanCLR 加载的 assembly / module 集合。
- module façade 映射到 `RtModuleDef`。
- token resolve 必须走 metadata table，不做字符串猜测。

验收入口：

- `Assembly.FullName`。
- `Assembly.GetTypes()`。
- `RuntimeModule.InternalGetTypes`。
- `RuntimeModule.ResolveTypeToken` / `ResolveMethodToken` 的白名单子集。

### 4.1 AssemblyLoadContext / Reflection.Emit 受限 façade

外部形状：

- `AssemblyLoadContext` 的初始化入口返回稳定的非空 runtime handle，让 CoreLib 的上下文登记和默认上下文 bookkeeping 可以继续执行。
- `AssemblyLoadContext.GetLoadedAssemblies` 返回 LeanCLR 当前已加载 assembly façade 数组，支撑 `AppDomain.GetAssemblies()` 和反射枚举。
- `PrepareForAssemblyLoadContextRelease` 可被 CoreLib 调用，但第一阶段只做 release bookkeeping，不承诺 collectible ALC 或 unload。
- `RuntimeAssemblyBuilder.CreateDynamicAssembly` 可创建 LeanCLR 可持有的动态 assembly façade，支撑 expression / light lambda 生成过程中的 metadata 对象。
- `ModuleHandle.GetDynamicMethod` 可把受限 dynamic resolver 解析为 runtime method info stub，供解释 profile 的 lambda 调用路径继续执行。

内部映射：

- ALC handle 是 LeanCLR 自有稳定 token，不暴露 CoreCLR LoaderAllocator / AssemblyLoadContextNative 结构。
- 已加载程序集枚举复用 LeanCLR appdomain / module registry，不从 CoreCLR loader graph 推导。
- 动态 assembly 只映射到 metadata façade 和可解释方法入口；不构建完整 Reflection.Emit module builder、ILGenerator、JIT code buffer 或 collectible allocator。
- `GetDynamicMethod` 优先从 resolver 托管对象读取可识别 method 信息，失败时输出具体 unsupported diagnostics，而不是宽松吞掉动态 codegen 缺口。

验收入口：

- `ManagedNet10.LegacyTests.Program::RunCorlibLightLambda`。
- `CorlibLightLambdaNet10Semantics` 中的 static call lambda、`in` 参数 lambda 和 reflection invoke 语义。

### 5. CustomAttribute / RuntimeCustomAttributeData

外部形状：

- `GetCustomAttribute<T>()`、`GetCustomAttributes(Type, inherit)` 和 `CustomAttributeData` 的最小路径可运行。
- constructor arguments、named arguments、inherit lookup 有明确支持边界。

内部映射：

- 从 metadata custom attribute blob 解码；不要复用 Mono `MonoCustomAttrs` 入口作为 net10 主路径。
- attribute 实例化走现有 `newobj` / interpreter 调用。
- unsupported blob 形状输出具体 attribute type 和 owner。

验收入口：

- `ManagedNet10.Smoke` 中的自定义 attribute 查询。
- `ManagedNet10.Smoke.Program::TestCustomAttributeDataOnly` 中 assembly / module / type / field / method / parameter / property / event target 的 metadata-only constructor arguments、property named argument 和 field named argument。
- `ManagedNet10.Smoke.Program::TestCustomAttributeDataOnly` 中的 Odin/NKG 常见 blob 形状：enum、Type、int[]、Type[]、object、object[]、named enum/type/array/object。
- NKG / Odin 会触发的轻量 attribute 枚举，当前由 `scripts/dotnet10/nkg-smoke.ps1` 验收。

### 6. RuntimeHelpers / Unsafe / Span

外部形状：

- `RuntimeHelpers` 的类型初始化、array data、object identity、generic helper 有明确映射。
- `Unsafe` / `Span<T>` 的 byref、stackalloc、RVA initializer 和 ref reinterpret 路径可解释执行。
- `SufficientExecutionStack` / `TryEnsureSufficientExecutionStack` 在解释 profile 下提供可前进的栈检查结果。
- `RuntimeHelpers.CompileMethod(RuntimeMethodHandleInternal)` / `PrepareMethod(RuntimeMethodHandleInternal, IntPtr*, int)` 可被 CoreLib / reflection 路径调用；解释 profile 中它只确认入口可接受，不触发 JIT。

内部映射：

- `Span<T>` 不创建 CoreCLR 内部对象模型；它按 .NET 10 layout 在解释器栈和托管对象数据区中表达。
- `Unsafe` 只支持已有解释器和内存模型能保证的路径。
- `RuntimeHelpers.InitializeArray` / `CreateSpan` 从 `RtFieldInfo` 的 RVA blob 读取数据，使用同一 field handle decode 规则。
- `RuntimeHelpers.RunClassConstructor` / `RunModuleConstructor` 进入 LeanCLR class / module cctor 路径，不复制 CoreCLR loader lock 模型。
- inline array helper 映射到托管对象或栈上缓冲区的元素地址，不允许 byref-like 结果逃逸到 heap。
- byref-like 类型不得逃逸到 boxed object 或 heap field。

验收入口：

- Span stackalloc。
- RVA initializer。
- `Unsafe.As` / `Unsafe.Add` 的白名单子集。
- inline array first element / indexed element helper。
- `RuntimeHelpers.RunClassConstructor` / `RunModuleConstructor`。
- `RuntimeHelpers.SufficientExecutionStack` / `TryEnsureSufficientExecutionStack`。
- `RuntimeHelpers.CompileMethod` / `PrepareMethod` no-op façade。

### 7. Exception / Delegate

外部形状：

- .NET 10 CoreLib 触发的异常构造、throw/catch、stack trace 最小查询可运行。
- delegate 创建、闭包 target、static method、instance method、multicast invoke 最小路径可运行。

内部映射：

- 复用 LeanCLR 现有异常和委托实现。
- 需要补齐的是 CoreLib 对 exception/delegate façade 的字段和 helper 调用期待。

验收入口：

- throw/catch/finally。
- delegate invoke。
- simple event / callback。

### 8. Thread / Monitor / Task

外部形状：

- 单线程 profile 下，`Thread.CurrentThread`、`Environment.CurrentManagedThreadId`、`Monitor.Enter/Exit/TryEnter/IsEntered/Wait/Pulse` 和基础 `Task` continuation 有最小语义。
- `ManualResetEvent` / `WaitHandle.WaitOne` / `WaitHandle.WaitAny` 支持由 runtime 创建的事件 handle 和受控 timeout。
- `ThreadPool.GetMinThreads/GetMaxThreads/GetAvailableThreads/SetMinThreads/SetMaxThreads` 返回逻辑容量；`QueueUserWorkItem` 可触发 callback，但不承诺真实并行 worker。
- blocking wait、Timer、complex async scheduler、跨线程取消和真实 OS thread affinity 必须明确分级。

内部映射：

- 第一阶段以单线程 frame scheduler / host dispatcher 为边界。
- `Monitor.Wait` / CoreLib generated PInvoke path 映射到 `vm::Monitor::monitor_wait`，只保证当前单线程 runtime 可控场景。
- `WaitHandle` generated PInvoke path 映射到 runtime 自建 `platform::EventHandle`，不接任意宿主 OS handle。
- `ThreadPool` sizing 是逻辑状态，queue dispatch 走 LeanCLR 调用边界，不扩展为完整 work-stealing pool。
- async continuation 优先映射到 LeanCLR frame pump 或宿主主线程 dispatcher。

验收入口：

- `lock` / `Monitor.Enter` / `Monitor.Exit`。
- `Monitor.TryEnter` / `Monitor.IsEntered` / `Monitor.Wait` / `Pulse`。
- `ManualResetEvent.WaitOne(0)` 和 `WaitHandle.WaitAny(..., 0)`。
- `ThreadPool.QueueUserWorkItem`、`GetAvailableThreads`、`GetMaxThreads`。
- `Task.Yield()` 的可控 continuation。
- `System.Threading.Monitor::<Wait>g____PInvoke|24_0`、`System.Threading.WaitHandle::<WaitOneCore>g____PInvoke|0_0`、`System.Threading.WaitHandle::<WaitMultipleIgnoringSyncContext>g____PInvoke|2_0`。

### 9. Host Bridge

外部形状：

- 托管代码通过稳定 ABI 调用 Unity/Godot 宿主服务。
- 引擎对象通过 opaque handle 暴露，不让托管对象持有真实引擎指针。
- 托管 wrapper 可表现为普通 C# 对象、属性、事件和 delegate，但跨边界只传递基础值、字符串、托管对象引用、opaque handle 和明确 layout 的小 struct。
- 引擎回调托管代码必须重新进入 LeanCLR 调用边界，由 runtime 负责异常捕获、返回值 marshal 和 GC root 生命周期。

内部映射：

- 第一阶段主路径采用 host function table：宿主在 runtime 初始化时注册版本号、能力 flags 和函数表；PInvoke / internal call 只作为 façade，不直接绑定 Unity/Godot C++ 对象布局。
- handle registry 由宿主侧持有真实引擎对象，LeanCLR 只保存不透明整数 / 指针形 handle；访问时必须校验 handle 是否仍有效。
- 主线程 dispatcher 由宿主侧提供，LeanCLR 把需要引擎主线程的调用、托管回调和 `Task` continuation 投递到同一队列。
- 错误转换采用显式状态码 + 可选错误字符串；host bridge 返回失败时，LeanCLR 转成托管异常，不把宿主异常对象跨 ABI 泄露。
- 生命周期释放分为托管 wrapper 释放、GCHandle / delegate 订阅释放和宿主对象销毁三类；任意一侧销毁后 registry 必须让 handle 失效。

ABI 分组：

| 组 | 最小 contract | 第一阶段边界 |
| --- | --- | --- |
| Runtime | 初始化、关闭、设置搜索路径、加载 assembly、调用静态入口 | 不承诺完整 CoreCLR hosting API |
| Object / Handle | 创建、查询、判活、retain/release、销毁通知 | handle 不暴露真实引擎指针和布局 |
| Logging / Diagnostics | log、warning、error、托管异常栈回传 | 不要求宿主实现完整 debugger |
| Scheduler | 主线程投递、下一帧执行、同步调用保护 | 阻塞 wait 和跨线程直接访问默认不支持 |
| Event / Callback | 注册托管 delegate、取消注册、触发回调 | 订阅必须有可释放 token 或 handle |
| Value Marshal | bool/int/float/string、opaque handle、小 struct、数组 view | 复杂 Variant / UnityEngine.Object 语义后置 |

P2 mock host 节点拆解：

| 节点 | 最小产物 | 必测场景 | 非目标 |
| --- | --- | --- | --- |
| P2.1 ABI skeleton | 一个 C ABI header、mock host 初始化流程、runtime 侧注册入口 | 版本 / capability 校验、初始化失败错误字符串、托管静态入口调用成功；`host-bridge-smoke.ps1 -Scenario AbiSkeleton` 已通过 | 不复制 CoreCLR hosting API；不加载真实 Unity/Godot |
| P2.2 Opaque handle registry | 宿主侧 handle table、retain/release、destroy notification、托管 wrapper handle 字段 | 创建后查询、retain/release 平衡、宿主销毁后访问返回 ObjectDisposed / MissingReference 风格诊断；`host-bridge-smoke.ps1 -Scenario HandleRegistry` 已通过 | 不暴露真实引擎指针；不承诺跨进程 handle |
| P2.3 Main-thread dispatcher | mock 主线程队列、next-frame pump、同步 reentry guard、结果回传结构 | FIFO 投递、下一帧执行、托管异常转错误状态、阻塞 wait 明确拒绝；`host-bridge-smoke.ps1 -Scenario Dispatcher` 已通过 | 不实现完整 ThreadPool / work stealing；不允许跨线程直接访问引擎对象 |
| P2.4 Event / callback bridge | delegate subscription token、触发入口、取消注册入口、异常返回字段 | 注册后触发托管 delegate、托管异常回传、取消后不再触发、重复取消幂等；`host-bridge-smoke.ps1 -Scenario EventCallback` 已通过 | 不实现 UnityEvent / Godot Signal 完整语义；复杂 Variant marshal 后置 |

P2 验收命令使用独立脚本，避免混入 BCL smoke：

- `scripts/dotnet10/host-bridge-smoke.ps1 -Configuration Release -Scenario AbiSkeleton`（已通过）。
- `scripts/dotnet10/host-bridge-smoke.ps1 -Configuration Release -Scenario HandleRegistry`（已通过）。
- `scripts/dotnet10/host-bridge-smoke.ps1 -Configuration Release -Scenario Dispatcher`（已通过）。
- `scripts/dotnet10/host-bridge-smoke.ps1 -Configuration Release -Scenario EventCallback`（已通过）。

P3 托管 wrapper 验收命令：

- `scripts/dotnet10/interp-smoke.ps1 -Configuration Release -Entry "ManagedNet10.Smoke.HostBridgeWrapperSmoke::Run"`（已通过）。
- `scripts/dotnet10/interp-smoke.ps1 -Configuration Release -Entry "ManagedNet10.Smoke.EngineBindingSmoke::Run"`（已通过）。
- `scripts/dotnet10/host-bridge-smoke.ps1 -Configuration Release -Scenario EngineAdapter`（已通过）。
- `scripts/dotnet10/api-scan.ps1 -Configuration Release`（已通过，unsupported `0`）。

验收入口：

- mock host 调用托管入口。
- opaque handle 创建、查询、释放。
- 主线程投递和结果回传。
- 托管 delegate 注册为宿主事件回调，触发后返回成功或托管异常诊断。
- 宿主对象销毁后再次访问 wrapper，稳定输出 ObjectDisposed / MissingReference 风格诊断。
- 托管 `HostObject` / `HostEventSubscription` wrapper 的释放、重复释放、事件订阅取消和 host status 到托管异常的转换。
- 托管 `EngineNode` binding 只通过 `HostObject`、dispatcher 和 subscription token 操作 mock engine adapter。

### 10. System.IO / Platform PInvoke

外部形状：

- .NET 10 `System.IO.FileStream`、`File`、`Path` 和 `SafeFileHandle` 触发的 generated PInvoke 可在 Windows 与非 Windows profile 下链接并执行。
- `CreateFile` / `ReadFile` / `WriteFile` / `CloseHandle` / `SetFilePointer` / file attribute / file information / temp path / full path 查询有最小语义。
- `DeleteFile`、`CopyFile`、`CreateDirectory`、`MoveFile`、`RemoveDirectory`、`ReplaceFile`、`SetFileAttributes` 和 `SetFileInformationByHandle` 在可表达的平台能力内工作。
- 失败时统一写入 Win32-style last error，让 CoreLib 能走自己的异常转换路径。

内部映射：

- CoreLib generated `Kernel32` PInvoke 先进入 `platform::Kernel32` façade，再映射到 `os::File`、`os::Path`、`os::Sys` 或 POSIX fallback。
- 非 Windows 不暴露真实 Win32 handle 语义；file handle 只表示 LeanCLR / OS file descriptor 可管理的句柄。
- `GetFullPathNameW` 动态读取当前工作目录后拼接相对路径，避免固定缓冲或宿主沙盒路径假设。
- `SafeFileHandle` 只负责 runtime 文件句柄生命周期，不承诺任意 native handle、overlapped I/O 或平台 ACL。

验收入口：

- `Path.GetTempPath()`、`Path.GetFullPath()`。
- `File.Exists()`、`File.Delete()`。
- `File.OpenHandle()` 与默认 / Activator 创建的 `SafeFileHandle`。
- `FileStream` create / write / flush / read round trip。
- `ManagedNet10.LegacyTests.Program::RunCorlibIO`。

## 初始 Contract Inventory

| Contract | 来源/触发 | LeanCLR 映射 | 当前处理 |
| --- | --- | --- | --- |
| `System.Object.GetType` | 基础 CoreLib / smoke | boxed object -> `RtClass` -> `RuntimeType` façade | 已有路径，需纳入正式 façade |
| `System.Type::op_Inequality` / `bool` return slot | reflection discovery | interpreter return slot -> `0/1` full-width stack value | 已修复，`RunLegacyDiscoverySmoke` 通过 |
| `System.RuntimeTypeHandle::GetBaseType` | reflection / type query | `RtClass::base_type` 或等价查询 | 已桥接，`RunLegacyDiscoverySmoke` 通过 |
| `System.RuntimeTypeHandle::is_subclass_of` | reflection / type query | `vm::Class` assignability | 已桥接，`RunLegacyDiscoverySmoke` 通过 |
| `System.Reflection.RuntimeAssembly::GetFullName` | legacy `RunAll` blocker / .NET 10 QCall | assembly metadata -> managed string via `StringHandleOnStack` | QCall façade 已实现，smoke 通过 |
| `System.Reflection.RuntimeModule::InternalGetTypes` | reflection smoke | `RtModuleDef` type table -> `RuntimeType[]` | 已桥接，`RunCorlibReflectionRuntimeModule` 通过 |
| `System.Reflection.CustomAttributeData` minimal path | attribute smoke / Odin | metadata blob decoder -> attribute data façade | 已覆盖核心 target 与常见 blob-shape metadata-only smoke；NKG/Odin 轻量 workload gate 通过 |
| `System.RuntimeFieldHandle::GetApproxDeclaringMethodTable` | `RuntimeModule.ResolveField` / field reflection | `RtFieldInfo` -> declaring `RtClass` -> net10 MethodTable façade | 已修复，`RunCorlibReflectionRuntimeModule` 通过 |
| `System.ValueType::<CanCompareBitsOrUseFastGetHashCodeHelper>g____PInvoke|2_0` | `ValueType.Equals` / `GetHashCode` | `MethodTable*` façade -> `RtClass` | 已修复，ValueType 子入口通过 |
| `System.Delegate::GetInvokeMethod` / `GetMulticastInvoke` | delegate reflection / multicast | `MethodTable*` façade -> delegate `RtClass` -> invoke method | 已修复，`RunRuntimeDelegateDynamicInvoke` 通过 |
| `System.RuntimeTypeHandle::InternalAllocNoChecks_FastPath(System.Runtime.CompilerServices.MethodTable*)` | `MulticastDelegate.NewMulticastDelegate` | `MethodTable*` façade -> `RtClass` -> object allocation | 已修复，`RunRuntimeDelegateDynamicInvoke` 通过 |
| `System.RuntimeFieldHandle::<GetRVAFieldInfo>g____PInvoke|24_0` | Span/RVA initializer | `RtFieldInfo` RVA data | 已桥接，`RunNet10SpanBinaryPrimitives` 通过 |
| `System.Runtime.CompilerServices.RuntimeHelpers::CompileMethod` / `PrepareMethod` | reflection / dynamic method preparation | accepted no-op preparation façade in interpreter profile | 已桥接，`TestRuntimeHelpers` 通过 |
| `RuntimeHelpers.InitializeArray` / `CreateSpan` / inline array helpers | Span/RVA / inline array lowering | `RtFieldInfo` RVA blob + interpreter stack/object buffer view | 已桥接，`TestSpan` 与 `RunNet10SpanBinaryPrimitives` 通过 |
| `System.Runtime.Loader.AssemblyLoadContext::GetLoadedAssemblies` / `InitializeAssemblyLoadContext` / `PrepareForAssemblyLoadContextRelease` | CoreLib ALC bookkeeping / `AppDomain.GetAssemblies` | stable LeanCLR ALC token + loaded assembly façade registry | 已桥接，`RunCorlibLightLambda` 通过 |
| `System.Reflection.Emit.RuntimeAssemblyBuilder::CreateDynamicAssembly` | Reflection.Emit / expression light lambda setup | metadata-only dynamic assembly façade | 已桥接，`RunCorlibLightLambda` 通过 |
| `System.ModuleHandle::<GetDynamicMethod>g____PInvoke|9_0` | dynamic resolver / DynamicMethod handle lookup | restricted resolver -> runtime method info stub | 已桥接，`RunCorlibLightLambda` 通过 |
| `System.Threading.Monitor::<Wait>g____PInvoke|24_0` | Monitor wait / pulse | `vm::Monitor::monitor_wait` -> single-thread controlled wait | 已桥接，`RunCorlibMonitor` 通过 |
| `System.Threading.WaitHandle::<WaitOneCore>g____PInvoke|0_0` / `WaitMultipleIgnoringSyncContext` | ManualResetEvent / WaitAny | runtime `EventHandle` -> `Kernel32` façade wait helpers | 已桥接，`RunCorlibThreading` 通过 |
| `System.IO.FileStream` / generated `Kernel32` file PInvoke | File / Path / SafeFileHandle | `platform::Kernel32` façade -> `os::File` / `os::Path` / POSIX fallback | 已桥接，`RunCorlibIO` 通过 |

## 迁移顺序

1. 建立 `net10_runtime_contract` 代码分区或等价边界，禁止 `coreclr-net10` 隐式落回 Mono-era 名称/签名。
2. 先完成 `RuntimeType` / `MethodTable*` façade 边界，确保所有 CoreLib 传入的 `System.Runtime.CompilerServices.MethodTable*` 都先解析为 net10 façade，再映射到 LeanCLR `RtClass`。
3. 保持 delegate allocation / multicast 路径绿色：`RunRuntimeDelegateDynamicInvoke` 作为 `MethodTable*` façade 回归 gate。
4. 重写并收敛 type / method / field / module handle decode helper，把 direct pointer、stack slot、boxed handle、stub object 和 managed reflection object 都纳入同一边界校验。当前解释 profile 已归档为 P1.7 handle/reflection consolidation。
5. 重写 assembly / module façade，优先解锁 `RuntimeAssembly.GetFullName`、`Assembly.GetTypes()`、`RuntimeModule.GetTypes` 和 token resolve 白名单。
6. 重写 custom attribute 最小路径，覆盖 smoke 与 NKG/Odin 会触发的读取模式。当前 metadata-only smoke 与 NKG/Odin 轻量 workload 已通过，后续只按真实失败补齐新 blob 形状或实例化路径。
7. 固化 Span / Unsafe / RuntimeHelpers contract，确保解释路径和 AOT 路径使用同一份语义说明。当前解释 profile 已覆盖 RuntimeHelpers cctor、栈检查、CompileMethod / PrepareMethod no-op façade、RVA `InitializeArray` / `CreateSpan` 与 inline array helper。
8. 固化 AssemblyLoadContext / Reflection.Emit 受限 façade，支撑 light lambda / dynamic assembly metadata 路径，同时明确 collectible ALC、unload 与完整动态 codegen 后置。
9. 分级支持 exception / delegate / Thread / Monitor / Task；第一阶段单线程可控，复杂 ThreadPool 后置。
10. 固化 System.IO / platform PInvoke 最小 contract，让真实 workload 可做临时文件、路径解析和基础资源读取。
11. 建立 host bridge mock，证明 Unity/Godot 接入不需要扩大 BCL 支持面。

## 验收方式

每个领域提交前必须给出三类证据：

- contract evidence：文档中有调用链、签名、内部映射和 unsupported 边界。
- catalog evidence：`coreclr-net10` catalog / PInvoke / intrinsic 注册表只包含本领域需要的入口。
- runtime evidence：至少一个 `ManagedNet10.Smoke` 子入口或真实 workload 入口可以稳定触发并通过，或稳定输出预期 `NotSupported` 诊断。

当前第一道自动化 gate 已落在 `src/generator/check_runtime_api_signatures.py --profile coreclr-net10 --repo-root .`：它会用 .NET 10 runtime pack externs 校验 `coreclr-net10` catalog，并禁止 `Mono.*`、`System.IO.Mono*`、`System.Reflection.Mono*`、`System.Runtime.Remoting*`、`mscorlib` 以及 Mono-era implementation symbol/header 混入 active profile。

API 边界 gate 已落在 `scripts/dotnet10/api-scan.ps1`：它构建 `src/tools/net10apiscan`，用 `minimal-net10-whitelist.json` 扫描 `AssemblyRef`、`TypeRef` 和 `MemberRef`，并在白名单外引用出现时输出 `unsupported_api` 诊断。当前默认扫描 `ManagedNet10.Smoke`、`ManagedNet10.NkgSmoke` 与 NKG core/Odin/UniTask 三件套；2026-06-29 验收结果为 NKG core/Odin/UniTask 扫描 unsupported `0`。完整 sampler/Hosting 仍在第一阶段边界外。

NKG/Odin 真实 workload gate 使用 `scripts/dotnet10/nkg-smoke.ps1`。脚本默认查找仓库同级的 `NKGGameFramework`，构建 `samples/NKGGameFramework.Sampler`，再把带齐 `NKGGameFramework`、`OdinSerializer` 和 `UniTask` 的 `net10.0` 输出目录传给 `ManagedNet10.NkgSmoke`。2026-06-29 已在默认路径通过；如果本机目录不同，可显式传入 `-NkgRoot`。

完整 `RunAll` 和原作者测试资产仍是最终质量线，但执行顺序后置。测试失败时先归类：contract 缺口、façade 映射缺口、LeanCLR VM 通用语义缺口、或白名单外 API。只有前两类进入本计划的 contract/model 重写循环。

# LeanCLR .NET 10 Runtime Contract Plan

状态：第二版执行基准（2026-06-28）

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

本轮不再把旧用例失败点当作设计入口，而是按 `.NET 10` CoreLib 真正会读取的 runtime façade 逐层收敛。当前 P0 已经从 `RuntimeType` 身份推进到 `RuntimeFieldHandle`、`RuntimeModule`、`ValueType`、delegate 相关 `MethodTable*` façade、multicast delegate allocation 和 legacy `RunAll` 基线；下一步进入 P1 的 handle/reflection consolidation 与 CustomAttribute 最小路径。

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

下一批次：

| 批次 | 目标 | 输出 | 验收 |
| --- | --- | --- | --- |
| P1 Handle / reflection consolidation | 收敛 method / field / type / module handle 的 decode helper、GC allocated-object guard 和 NotSupported 诊断 | 统一的 net10 handle boundary | Reflection、Span/RVA、ValueType、delegate 子集全部保持绿色 |
| P1 Assembly / Module / CustomAttribute 扩展 | assembly/module façade 与 attribute blob decoder 成为正式 net10 路径 | `RuntimeAssembly`、`RuntimeModule`、真实 workload attribute 枚举 contract | reflection smoke、NKG/Odin 轻量 attribute 路径 |

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

验收入口：

- 反射枚举字段/方法。
- private field 基础读写。
- RVA initializer / static readonly data。

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
- NKG / Odin 会触发的轻量 attribute 枚举。

### 6. RuntimeHelpers / Unsafe / Span

外部形状：

- `RuntimeHelpers` 的类型初始化、array data、object identity、generic helper 有明确映射。
- `Unsafe` / `Span<T>` 的 byref、stackalloc、RVA initializer 和 ref reinterpret 路径可解释执行。

内部映射：

- `Span<T>` 不创建 CoreCLR 内部对象模型；它按 .NET 10 layout 在解释器栈和托管对象数据区中表达。
- `Unsafe` 只支持已有解释器和内存模型能保证的路径。
- byref-like 类型不得逃逸到 boxed object 或 heap field。

验收入口：

- Span stackalloc。
- RVA initializer。
- `Unsafe.As` / `Unsafe.Add` 的白名单子集。

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

- 单线程 profile 下，`Thread.CurrentThread`、`Monitor.Enter/Exit/TryEnter/IsEntered` 和基础 `Task` continuation 有最小语义。
- blocking wait、ThreadPool、Timer、complex async scheduler 必须明确分级。

内部映射：

- 第一阶段以单线程 frame scheduler / host dispatcher 为边界。
- `Monitor.Wait` / CoreLib generated PInvoke path 若无法安全支持，应给出明确 `NotSupported`，不能静默挂起。
- async continuation 优先映射到 LeanCLR frame pump 或宿主主线程 dispatcher。

验收入口：

- `lock` / `Monitor.Enter` / `Monitor.Exit`。
- `Task.Yield()` 的可控 continuation。
- `System.Threading.Monitor::<Wait>g____PInvoke|24_0` 当前作为阻塞点记录。

### 9. Host Bridge

外部形状：

- 托管代码通过稳定 ABI 调用 Unity/Godot 宿主服务。
- 引擎对象通过 opaque handle 暴露，不让托管对象持有真实引擎指针。

内部映射：

- host function table / PInvoke / internal call 三者只能选定一种主路径后 profile 化。
- handle registry、主线程 dispatcher、错误转换和生命周期释放由宿主侧负责。

验收入口：

- mock host 调用托管入口。
- opaque handle 创建、查询、释放。
- 主线程投递和结果回传。

## 初始 Contract Inventory

| Contract | 来源/触发 | LeanCLR 映射 | 当前处理 |
| --- | --- | --- | --- |
| `System.Object.GetType` | 基础 CoreLib / smoke | boxed object -> `RtClass` -> `RuntimeType` façade | 已有路径，需纳入正式 façade |
| `System.Type::op_Inequality` / `bool` return slot | reflection discovery | interpreter return slot -> `0/1` full-width stack value | 已修复，`RunLegacyDiscoverySmoke` 通过 |
| `System.RuntimeTypeHandle::GetBaseType` | reflection / type query | `RtClass::base_type` 或等价查询 | 重写并验收 |
| `System.RuntimeTypeHandle::is_subclass_of` | reflection / type query | `vm::Class` assignability | 重写并验收 |
| `System.Reflection.RuntimeAssembly::GetFullName` | legacy `RunAll` blocker / .NET 10 QCall | assembly metadata -> managed string via `StringHandleOnStack` | QCall façade 已实现，smoke 通过 |
| `System.Reflection.RuntimeModule::InternalGetTypes` | reflection smoke | `RtModuleDef` type table -> `RuntimeType[]` | P0/P1 |
| `System.Reflection.CustomAttributeData` minimal path | attribute smoke / Odin | metadata blob decoder -> attribute data façade | 已覆盖核心 target 与常见 blob-shape metadata-only smoke；继续接真实 workload |
| `System.RuntimeFieldHandle::GetApproxDeclaringMethodTable` | `RuntimeModule.ResolveField` / field reflection | `RtFieldInfo` -> declaring `RtClass` -> net10 MethodTable façade | 已修复，`RunCorlibReflectionRuntimeModule` 通过 |
| `System.ValueType::<CanCompareBitsOrUseFastGetHashCodeHelper>g____PInvoke|2_0` | `ValueType.Equals` / `GetHashCode` | `MethodTable*` façade -> `RtClass` | 已修复，ValueType 子入口通过 |
| `System.Delegate::GetInvokeMethod` / `GetMulticastInvoke` | delegate reflection / multicast | `MethodTable*` façade -> delegate `RtClass` -> invoke method | 部分完成，仍需修复 multicast allocation |
| `System.RuntimeTypeHandle::InternalAllocNoChecks_FastPath(System.Runtime.CompilerServices.MethodTable*)` | `MulticastDelegate.NewMulticastDelegate` | `MethodTable*` façade -> `RtClass` -> object allocation | 已修复，`RunRuntimeDelegateDynamicInvoke` 通过 |
| `System.RuntimeFieldHandle::<GetRVAFieldInfo>g____PInvoke|24_0` | Span/RVA initializer | `RtFieldInfo` RVA data | 已有修复需归档到 contract |
| `System.Threading.Monitor::<Wait>g____PInvoke|24_0` | full smoke blocker | single-thread wait policy | P0 诊断或受限实现 |

## 迁移顺序

1. 建立 `net10_runtime_contract` 代码分区或等价边界，禁止 `coreclr-net10` 隐式落回 Mono-era 名称/签名。
2. 先完成 `RuntimeType` / `MethodTable*` façade 边界，确保所有 CoreLib 传入的 `System.Runtime.CompilerServices.MethodTable*` 都先解析为 net10 façade，再映射到 LeanCLR `RtClass`。
3. 保持 delegate allocation / multicast 路径绿色：`RunRuntimeDelegateDynamicInvoke` 作为 `MethodTable*` façade 回归 gate。
4. 重写并收敛 type / method / field / module handle decode helper，把 direct pointer、stack slot、boxed handle、stub object 和 managed reflection object 都纳入同一边界校验。
5. 重写 assembly / module façade，优先解锁 `RuntimeAssembly.GetFullName`、`Assembly.GetTypes()`、`RuntimeModule.GetTypes` 和 token resolve 白名单。
6. 重写 custom attribute 最小路径，覆盖 smoke 与 NKG/Odin 会触发的读取模式。
7. 固化 Span / Unsafe / RuntimeHelpers contract，确保解释路径和 AOT 路径使用同一份语义说明。
8. 分级支持 exception / delegate / Thread / Monitor / Task；第一阶段单线程可控，复杂 ThreadPool 后置。
9. 建立 host bridge mock，证明 Unity/Godot 接入不需要扩大 BCL 支持面。

## 验收方式

每个领域提交前必须给出三类证据：

- contract evidence：文档中有调用链、签名、内部映射和 unsupported 边界。
- catalog evidence：`coreclr-net10` catalog / PInvoke / intrinsic 注册表只包含本领域需要的入口。
- runtime evidence：至少一个 `ManagedNet10.Smoke` 子入口或真实 workload 入口可以稳定触发并通过，或稳定输出预期 `NotSupported` 诊断。

当前第一道自动化 gate 已落在 `src/generator/check_runtime_api_signatures.py --profile coreclr-net10 --repo-root .`：它会用 .NET 10 runtime pack externs 校验 `coreclr-net10` catalog，并禁止 `Mono.*`、`System.IO.Mono*`、`System.Reflection.Mono*`、`System.Runtime.Remoting*`、`mscorlib` 以及 Mono-era implementation symbol/header 混入 active profile。

完整 `RunAll` 和原作者测试资产仍是最终质量线，但执行顺序后置。测试失败时先归类：contract 缺口、façade 映射缺口、LeanCLR VM 通用语义缺口、或白名单外 API。只有前两类进入本计划的 contract/model 重写循环。

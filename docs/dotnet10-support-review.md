# LeanCLR .NET 10 支持审查

审查日期：2026-06-26

审查对象：`coreclr` 分支，HEAD `33e9194`（`doc: update core and standard edition status`）

## 结论摘要

LeanCLR 当前已经能在本机 .NET 10 SDK 下构建 `net8.0` 的 LeanAOT 与工具项目，但这不等于已经支持 .NET 10 BCL。

要真正支持 .NET 10，主线工作不是简单把所有 `TargetFramework` 从 `net8.0` 改成 `net10.0`，而是建立 CoreCLR/.NET 10 BCL 输入、内部调用覆盖、测试工程和 CI 门禁。当前仓库主要仍围绕 Mono 4.5 BCL 与 `mscorlib` 模型组织，运行时和 AOT 工具里有多处对 `mscorlib`、`System`、`System.Core`、Mono icall 名称和旧式 `.NET Framework 4.8` 测试项目的假设。

优先级最高的事情：

1. 明确定义 `.NET 10 支持` 的范围：仅运行用户 `net10.0` 程序集，还是承载 `System.Private.CoreLib` / `Microsoft.NETCore.App` 的 .NET 10 BCL 子集。
2. 为 CoreCLR/.NET 10 增加独立 BCL profile，不要覆盖现有 Mono/Unity profile。
3. 生成并对比 .NET 10 BCL 的 internal call / intrinsic / PInvoke 需求清单。
4. 新增 `net10.0` 托管测试资产，保留现有 Mono 4.5 测试资产。
5. 恢复 CI，并在 CI 中同时跑 SDK 工具链、旧 Mono 测试和新增 .NET 10 测试。

## 官方基线

本次审查按以下官方信息作为外部基线：

- .NET 10 是当前 LTS 线，`.NET 10.0.9` 为本机已安装 runtime patch。
- SDK 风格项目可使用 `net10.0` 作为目标框架名字（TFM）。
- .NET SDK 支持向下构建旧 TFM，但目标框架、引用程序集、runtime pack、workload 与 CI 环境仍要明确固定。

参考：

- [.NET 10 版本概览](https://learn.microsoft.com/en-us/dotnet/core/whats-new/dotnet-10/overview)
- [.NET target frameworks](https://learn.microsoft.com/en-us/dotnet/standard/frameworks)
- [.NET support policy](https://dotnet.microsoft.com/en-us/platform/support/policy/dotnet-core)
- [.NET 10 compatibility changes](https://learn.microsoft.com/en-us/dotnet/core/compatibility/10.0)

## 当前项目结构

仓库大致分成这些区域：

| 区域 | 现状 | .NET 10 影响 |
| --- | --- | --- |
| `src/runtime` | C++11 LeanCLR runtime，CMake 构建 | 运行时是否支持 .NET 10 BCL 的核心区域 |
| `src/leanaot` | SDK 风格 `net8.0`，LeanAOT 代码生成工具 | 可在 .NET 10 SDK 下构建，但 runtime API catalog 仍偏 Mono/Unity BCL |
| `src/tools/exportextern` | SDK 风格 `net8.0`，依赖 `dnlib` | 可用于导出 .NET 10 BCL 入口清单 |
| `src/tools/pgo2aot` | SDK 风格 `net8.0`，带 `RollForward=LatestMajor` | 工具可在 .NET 10 runtime 上 roll forward |
| `src/libraries/mono-4.5` | 内置 Mono 4.5 BCL 及 targets | 当前测试和部分 runtime 假设的主要 BCL 来源 |
| `src/libraries/LeanCLR` | 旧式 `.NET Framework v4.8`，引用 `mono-4.5` | profile 库仍绑定 `mscorlib` 模型 |
| `src/tests/managed` | 旧式 `.NET Framework v4.8` 测试，很多项目 `NoStandardLib` | 主要验证 Mono 4.5 BCL，不验证 .NET 10 BCL |
| `.github/workflows/ci.yml` | workflow 只有手动触发且 job `if: false` | CI 当前没有实际保护作用 |

## 目标框架与依赖

SDK 风格项目：

| 项目 | 当前 TFM | 备注 |
| --- | --- | --- |
| `src/leanaot/LeanAOT/LeanAOT.csproj` | `net8.0` | `RollForward=LatestMajor` |
| `src/leanaot/LeanAOT.Core/LeanAOT.Core.csproj` | `net8.0` | 依赖 `dnlib 4.5.0`、`NLog 6.0.7` |
| `src/leanaot/LeanAOT.ToCpp/LeanAOT.ToCpp.csproj` | `net8.0` | 引用 Core 和 GenerationPlan |
| `src/leanaot/LeanAOT.GenerationPlan/LeanAOT.GenerationPlan.csproj` | `net8.0` | 引用 Core |
| `src/tools/exportextern/ExportExtern.csproj` | `net8.0` | 依赖 `dnlib 4.5.0` |
| `src/tools/pgo2aot/Pgo2Aot.csproj` | `net8.0` | `RollForward=LatestMajor` |

旧式项目：

- `src/libraries/LeanCLR/LeanCLR.csproj`：`.NET Framework v4.8`，`NoStandardLib=true`，引用 `src/libraries/mono-4.5`。
- `src/tests/managed/*/*.csproj`：大多为 `.NET Framework v4.8`，其中 `CoreTests`、`CorlibTests`、`GcTests`、`ILTests`、`RefNetstandard` 等直接绑定 `mono-4.5`。
- `ILTests` 依赖 `ILAsm 9.4.0` NuGet 包。

依赖审查结果：

- `dotnet list src/leanaot/LeanAOT.sln package --outdated`：仅 `NLog 6.0.7 -> 6.1.3` 有更新。
- `dotnet list src/leanaot/LeanAOT.sln package --vulnerable`：无漏洞包。
- `ExportExtern` 和 `Pgo2Aot` 当前无更新包。
- `managed.sln` 的 `dotnet list package` 被旧式项目阻断，CLI 报这些项目不适用于该命令。

## 本机验证记录

本机环境：

- OS：Windows `10.0.26200`
- .NET SDK：`10.0.301`
- MSBuild：`18.6.4`
- Runtime：`Microsoft.NETCore.App 10.0.9`
- `global.json`：不存在
- CMake：不在 PATH

命令结果：

| 命令 | 结果 | 说明 |
| --- | --- | --- |
| `dotnet --info` | 成功 | 只有 .NET 10 SDK/runtime |
| `dotnet build src\leanaot\LeanAOT.sln -c Debug` | 成功 | 0 warning / 0 error，输出到 `out/dotnet/*/Debug/net8.0` |
| `dotnet build src\tools\exportextern\ExportExtern.csproj -c Debug` | 成功 | 1 个 nullable warning：`Program.cs(83,32) CS8600` |
| `dotnet build src\tools\pgo2aot\Pgo2Aot.csproj -c Debug` | 成功 | 0 warning / 0 error |
| `dotnet build src\tests\managed\managed.sln -c Debug` | 失败 | `MSB3644`，缺少 `.NETFramework,Version=v4.8` reference assemblies |
| `cmake --version` | 失败 | `cmake` 不在 PATH，原生 runtime/test runner 无法构建 |

这些结果说明：

- SDK 工具链目前可用 .NET 10 SDK 构建现有 `net8.0` 项目。
- 旧式托管测试需要 .NET Framework 4.8 Developer Pack 或测试工程现代化。
- 原生构建和测试 runner 需要安装/定位 CMake。

## 关键风险

### 1. BCL 模型仍偏 `mscorlib`

当前运行时把 corlib 名字固定为 `mscorlib`：

- `src/runtime/const_strs.h`：`STR_CORLIB_NAME = "mscorlib"`
- `src/runtime/metadata/module_def.cpp`：用 `moduleDef->is_corlib()` 记录全局 corlib module
- `src/leanaot/LeanAOT.Core/MetaUtil.cs`：`IsCorlibOrSystemOrSystemCore` 只识别 `mscorlib`、`System`、`System.Core`
- `src/leanaot/LeanAOT.ToCpp/RuntimeApiCatalog.cs`：`_coreLibModules = { "mscorlib", "System", "System.Core", "LeanCLR" }`

.NET 10 的核心库体系以 `System.Private.CoreLib` / `System.Runtime` / `Microsoft.NETCore.App` 为中心。若不抽象 BCL profile，AOT catalog 和 runtime corlib 判断会错过大量 .NET 10 类型和内部方法。

建议：

- 增加 `BclProfile` 或类似概念：`mono45`、`unity`、`coreclr-net10`。
- `STR_CORLIB_NAME` 不应全局固定为 `mscorlib`；至少要允许 `System.Private.CoreLib`。
- `RuntimeApiCatalog` 的 core module 集合应来自 profile 配置，而不是硬编码。

### 2. Runtime API 表需要按 .NET 10 重建

当前 LeanAOT runtime API JSON：

| 文件 | 条目数 |
| --- | ---: |
| `src/leanaot/LeanAOT/icalls.json` | 550 |
| `src/leanaot/LeanAOT/intrinsics.json` | 45 |
| `src/leanaot/LeanAOT/icalls_newobj.json` | 8 |
| `src/leanaot/LeanAOT/intrinsics_newobj.json` | 1 |
| `src/leanaot/LeanAOT/pinvokes.json` | 1 |

C++ icall 覆盖集中在：

- `system_threading_thread.cpp`：52 个
- `interop.cpp`：51 个
- `system_runtime_interopservices_marshal.cpp`：42 个
- `system_math.cpp`：26 个
- `system_environment.cpp`：23 个
- `system_appdomain.cpp`：23 个
- `system_runtimetype.cpp`：23 个
- `system_threading_interlocked.cpp`：22 个

这些名称混合了 Mono、CoreCLR PAL/Interop、Unity 兼容需求。支持 .NET 10 时需要从 .NET 10 runtime assemblies 实际导出：

- `InternalCall`
- runtime implemented methods
- P/Invoke / `LibraryImport` / `DllImport`
- intrinsic 候选
- `System.Private.CoreLib` 与其他 runtime assemblies 的引用关系

建议产物：

- `artifacts/dotnet10-externs/*.txt`：从 .NET 10 BCL 导出的需求清单。
- `src/leanaot/LeanAOT/runtime-apis/coreclr-net10/*.json`：独立于现有 Mono/Unity 的 API catalog。
- `scripts/generator/check_runtime_api_signatures.*` 支持传入 profile 和多组 externs。

### 3. 现有托管测试不能证明 .NET 10 BCL

当前测试项目主要通过旧式 `.NET Framework v4.8` 项目引用 `src/libraries/mono-4.5`。这对 Mono 4.5 兼容测试是合理的，但无法覆盖：

- `net10.0` 编译输出。
- `System.Private.CoreLib` 元数据。
- .NET 10 BCL 内部方法签名。
- C# 14 / .NET 10 SDK 可能产生的现代 IL 和属性形态。

建议：

- 新增 `src/tests/managed-net10/managed-net10.sln`，使用 SDK 风格 `net10.0`。
- 保留 `src/tests/managed` 作为 Mono profile 测试，不要直接替换。
- 抽出共享测试基础设施，避免 `Common` 被 `.NET Framework v4.8` 锁死。
- 新增 `CoversIcall`/coverage 报告对 `coreclr-net10` profile 独立统计。

### 4. CI 当前不可用

`.github/workflows/ci.yml`：

- push/pull_request 触发被注释。
- job 顶层 `if: false`。
- 只安装 `cmake g++`，没有固定 .NET SDK，也没有 .NET Framework reference assemblies。

建议：

- 先恢复手动 workflow 的实际执行，移除 `if: false`。
- 增加 Windows job：覆盖 `.NET Framework 4.8` 旧测试和 Visual Studio/CMake x64。
- 增加 Linux job：覆盖 CMake runtime/basic tester。
- 增加 .NET 10 SDK 固定安装：`actions/setup-dotnet` 使用 `10.0.x`。
- 后续再打开 PR/push 触发。

### 5. AOT 代码生成器仍有现代 IL 缺口

静态扫描发现 LeanAOT 仍有一些直接抛出的不支持路径：

- `arglist`、`endfilter`、`leave`、`endfinally` 未实现。
- `jmp`、部分 prefix instruction 未支持。
- 静态 RVA field、const field address、字符串常量 field 等路径会抛 `NotSupportedException`。
- P/Invoke marshal 支持面有限，部分 `NativeType`、calling convention、array/string builder 场景不支持。
- `System.Span<T>` 相关 intrinsic 当前只有很小覆盖。

这些缺口不一定全部阻断 .NET 10，但必须通过 .NET 10 BCL 和用户程序集的实际 AOT smoke test 来排序。

### 6. 线程和同步语义需要明确支持边界

README 声明 Standard 仍是单线程，多线程规划中。与此同时 runtime 已有大量 `System.Threading.*` icall stub 和测试覆盖。对于 .NET 10 BCL，线程、ThreadPool、Timer、Monitor、WaitHandle、async 相关路径更容易被间接触发。

建议：

- 文档层明确 `.NET 10 profile` 是否支持多线程。
- 对不支持的 threading API 建立可诊断失败策略，而不是静默 stub。
- 将单线程可运行的 BCL 子集与多线程 API 支持分开验收。

## 已确认的架构结论与改造边界

### 1. `coreclr` 分支定位

`coreclr` 分支的目标应是引擎无关的 CoreCLR/.NET 10 BCL profile，不是 Unity 专用版本。Unity 支持应继续位于 `unity` 分支或 `leanclr-unity` 插件侧；Godot 支持也应以宿主插件/bridge 的方式接入。`coreclr` 主线不应继续把 Unity/Godot API 当作运行时内建依赖。

因此，NKGGameFramework 的推荐接入模型是：

```text
Unity/Godot host
  -> load LeanCLR native runtime
  -> register engine bridge APIs
  -> load NKGGameFramework and game assemblies
  -> drive RuntimeContext.Update every frame
```

从引擎侧看，LeanCLR 是一个 native CLR-like runtime；从 NKG 侧看，Unity/Godot 是外部 service provider。

### 2. LeanCLR VM 核心应复用

本次迁移不应理解为重写整个 LeanCLR。以下能力属于 LeanCLR 自身 VM 核心，应尽量复用并只做针对 .NET 10 的兼容补强：

- 元数据解析。
- 类型系统。
- IR 解释器。
- LeanAOT IL -> C++ 转译框架。
- GC。
- 异常处理。
- 委托。
- 泛型与泛型共享。
- 基础反射框架。

`unity` / `mono` / `coreclr` 分支的主要差异不是这些 VM 核心能力，而是 BCL 来源、runtime API catalog、internal call/intrinsic 覆盖、平台宿主集成和测试资产。

### 3. 需要大改的是 BCL 与 runtime contract 层

支持 .NET 10 的工作本质上是让 LeanCLR 成为一套 `.NET 10-compatible runtime`，而不是重新实现完整 .NET 10。可以复用 .NET 10 的托管 BCL assemblies，但 LeanCLR 必须实现这些 assemblies 期待的 runtime contract。

重点改造项：

- `System.Private.CoreLib` / `System.Runtime` / `Microsoft.NETCore.App` 的 BCL profile。
- corlib 名称和 core module 识别，从固定 `mscorlib` 改为 profile 化。
- assembly resolver，支持用户程序集、NuGet 依赖、runtime pack 与 adapter assembly。
- runtime API catalog，按 profile 拆分 `mono45`、`unity`、`coreclr-net10`。
- `.NET 10` BCL 的 internal call、intrinsic、P/Invoke、runtime implemented method 覆盖。
- runtime handle、reflection、custom attribute、generic instantiation、Activator 等 CoreCLR BCL 常用服务。
- async/await、UniTask、timer、continuation、cancellation 的单线程 frame scheduler 或宿主调度器映射。
- Odin 等反射型序列化库所需的 private member access 和 attribute/metadata 能力。
- AOT 对现代 C# / .NET 10 输出 IL、metadata 和 attribute 形态的覆盖。
- `net10.0` 托管测试资产和 `coreclr-net10` coverage 报告。

### 4. 当前 `coreclr` 分支状态判断

当前分支不能说没有任何 CoreCLR 方向工作：README 已明确 `coreclr` 分支目标，runtime 和 AOT 中也存在部分 CoreCLR 语义兼容点，SDK 工具链可在 .NET 10 SDK 下构建。

但以“能承载 `.NET 10` BCL 和 NKGGameFramework”为标准，当前还没有完成可验证的 .NET Core BCL 适配。主要证据：

- runtime 仍固定 `STR_CORLIB_NAME = "mscorlib"`。
- LeanAOT `RuntimeApiCatalog` 仍只把 `mscorlib`、`System`、`System.Core`、`LeanCLR` 视为 core library。
- `src/libraries/LeanCLR` 仍是 `.NET Framework v4.8` 旧式项目，引用 `mono-4.5/mscorlib.dll`。
- 现有托管测试主要是 `.NET Framework v4.8` + `mono-4.5`，不能证明 `System.Private.CoreLib` 或 `net10.0` 用户程序集可运行。
- 源码中尚未形成 `System.Private.CoreLib` profile、.NET 10 extern diff、net10 smoke test 和独立 coverage 门禁。

## 建议实施计划

### 阶段 0：定义支持范围

输出一页设计决策：

- 支持目标是 `net10.0` 用户程序集、.NET 10 BCL 子集，还是完整 CoreCLR BCL。
- 是否支持单线程-only。
- 是否支持 AOT、解释执行，或两者。
- 支持平台优先级：Windows x64、Linux x64、wasm、移动端。
- 明确 `coreclr` 分支是引擎无关 runtime 主线，Unity/Godot 通过宿主插件和 bridge 接入。
- 明确 VM 核心复用边界，禁止把 .NET 10 支持误拆成重写 metadata/type system/interpreter/GC。

### 阶段 1：构建与 CI 基线

任务：

- 增加 `global.json` 或 CI 固定 SDK，避免开发机 SDK 漂移。
- 安装/定位 CMake，确保 `scripts/runtime/build.*` 和 `scripts/test/basic-tester/build.*` 可运行。
- 恢复 GitHub Actions，不再 `if: false`。
- Windows CI 安装 .NET Framework 4.8 Developer Pack 或调整旧测试项目以不依赖系统 reference assemblies。
- 把 `dotnet build src/leanaot/LeanAOT.sln`、`ExportExtern`、`Pgo2Aot` 纳入 CI。

### 阶段 2：.NET 10 BCL 输入与差异报告

任务：

- 新增脚本定位本机/CI 的 .NET 10 reference assemblies 和 runtime assemblies。
- 用 `ExportExtern` 或增强版工具导出 `System.Private.CoreLib` 等 assembly 的 external/runtime API 需求。
- 将导出结果与现有 550 个 icall 和 45 个 intrinsic 做 diff。
- 生成 `missing / signature changed / extra / renamed` 四类报告。

### 阶段 3：BCL profile 化

任务：

- 将 runtime API JSON 从单套文件拆成 profile 目录。
- `RuntimeApiCatalog.LoadFromDirectory` 支持按 profile 加载。
- `MetaUtil.IsCorlibOrSystemOrSystemCore` 和 `_coreLibModules` 配置化。
- runtime 的 corlib 名称识别支持 `System.Private.CoreLib`。
- 保留 `mono45` 和 `unity` 现有行为。

### 阶段 4：新增 .NET 10 测试资产

任务：

- 新增 SDK 风格 `net10.0` smoke tests。
- 覆盖基础类型、泛型、异常、反射、数组、delegate、string、span、threading subset、P/Invoke subset。
- 增加 `coreclr-net10` 的 icall coverage 标注。
- AOT smoke test 至少跑一个最小 `net10.0` 程序集。

### 阶段 5：实现高优先级 runtime API

任务：

- 按 diff 报告优先补齐 `System.Private.CoreLib` 直接启动路径。
- 优先模块：string、array、object、runtime handles、reflection、marshal、interop、threading 基础、environment、math。
- 对暂不支持 API 输出明确 NotSupported/NotImplemented 诊断。

### 阶段 6：AOT 与现代 IL 兼容

任务：

- 用 .NET 10 SDK 编译 fixture，统计实际出现的 IL opcode、metadata table、custom attributes。
- 针对现代 C# 输出补齐 AOT codegen 缺口。
- 对 `LibraryImport`、`UnmanagedCallersOnly`、function pointer、byref-like、InlineArray 等建立测试。

### 阶段 7：发布与文档

任务：

- README 的 `.NET 10 BCL` 状态从“开发中”细化为 profile 能力矩阵。
- 文档站补充 `.NET 10` 构建、BCL 输入、支持 API、限制和测试命令。
- 版本发布前给出已验证 SDK/runtime patch。

## 全切 .NET 10 后的历史资产清理

如果项目决策是完全切到 CoreCLR/.NET 10 BCL，不再维护 Mono/Unity/mono-4.5 profile，那么可以把大量历史兼容资产纳入清理范围。这个清理应发生在 `coreclr-net10` profile 和 `net10.0` 测试资产跑绿之后，而不是作为第一步执行；否则会丢掉当前唯一稳定的 Mono profile 回归基线。

可清理候选：

- `src/libraries/mono-4.5` BCL 资产。
- `src/libraries/mono-4.5` 下的旧 MSBuild targets。
- `src/libraries/LeanCLR` 里绑定 `mono-4.5/mscorlib` 的旧式 `.NET Framework v4.8` profile 项目。
- `src/tests/managed` 中仅服务 Mono profile 的旧式 `.NET Framework v4.8` 测试项目，或将其迁移/替换为 SDK 风格 `net10.0` 测试资产。
- `scripts/test/*`、`scripts/test/aot-tester/*`、示例脚本中复制或加载 `src/libraries/mono-4.5` 的逻辑。
- runtime 中固定 `mscorlib` 的识别逻辑，以及与 `System.Private.CoreLib` 不兼容的 corlib 判断。
- LeanAOT 中 `RuntimeApiCatalog`、`MetaUtil.IsCorlibOrSystemOrSystemCore` 等只识别 `mscorlib`、`System`、`System.Core` 的硬编码。
- 明确只服务 Mono BCL 的 internal call 入口，例如 `Mono.*`、`System.IO.MonoIO`、旧 `ConsoleDriver` 兼容层等。
- README、脚本文档和测试文档中关于 mono 分支、mono-4.5 BCL 的过时说明。

建议清理顺序：

1. 给当前 Mono profile 打 tag 或保留维护分支，确保历史行为可追溯。
2. 增加 `coreclr-net10` profile，并让最小启动、基础 BCL、AOT smoke test 跑绿。
3. 建立 `.NET 10` icall/intrinsic coverage 报告，确认被删入口没有被新 profile 依赖。
4. 迁移或替换旧托管测试，移除 `.NET Framework v4.8` reference assemblies 依赖。
5. 删除 `mono-4.5` BCL 资产和相关脚本分支。
6. 清理 `mscorlib` / `Mono.*` 命名遗留和文档描述。

如果仍要维护 Unity 或 Mono profile，则不要删除这些资产；应改为 profile 化隔离，例如 `profiles/mono45`、`profiles/unity`、`profiles/coreclr-net10`，让构建、测试和 runtime API catalog 按 profile 选择。

## NKGGameFramework 接入 LeanCLR 的改造范围

`E:\Study\wqaetly\NKGGameFramework` 当前主包是 SDK 风格 `net10.0`，并直接依赖 `UniTask 2.5.11` 与 `OdinSerializerNetCore`。Unity/Godot adapter 项目目前主要是接口契约：`IUnityGameLoopDriver` / `IGodotGameLoopDriver`、asset service、scene service 等，没有把引擎程序集反向引入核心包。这个结构适合 LeanCLR 分阶段接入：先跑引擎无关核心，再通过宿主桥接访问 Unity/Godot API。

### 推荐运行边界

不建议第一阶段让 LeanCLR 直接加载 `UnityEngine.dll` 或 Godot 的完整托管 API，并在 LeanCLR 内部任意调用引擎对象。更稳妥的边界是：

1. LeanCLR 只加载 `NKGGameFramework`、业务程序集、`UniTask`、`OdinSerializerNetCore` 和必要的 `.NET 10` BCL 子集。
2. Unity/Godot 进程作为宿主，负责创建 LeanCLR runtime、装载程序集、驱动每帧 `RuntimeContext.Update`。
3. 引擎对象不直接跨 LeanCLR 边界传递；使用 `int`、`long`、`IntPtr` 或自定义 handle 表示资源、节点、场景、GameObject、Control 等对象。
4. 引擎 API 通过 LeanCLR profile 的 internal call、P/Invoke、host function table 或统一 bridge ABI 暴露。
5. 所有必须主线程调用的 Unity/Godot API 都通过宿主主线程队列调度，LeanCLR 侧只发请求并接收结果。

### LeanCLR 必做改造

为跑 NKGGameFramework 核心，LeanCLR 至少需要：

- `coreclr-net10` BCL profile：支持 `System.Private.CoreLib`、`System.Runtime`、`Microsoft.NETCore.App` 相关 runtime assemblies，而不是继续假设 `mscorlib`。
- 程序集解析器：能从 NKG 输出目录、NuGet 缓存、`externals/odin-serializer` 输出、`.NET 10` runtime pack 中解析依赖程序集。
- 现代 C# / .NET metadata 支持：record、record struct、`init`、`required`、collection expression、nullable attribute、custom attribute、泛型约束和接口默认方法等都要能被加载、AOT 和反射识别。
- 基础反射能力：`Activator`、字段/属性/方法枚举、attribute 查询、泛型类型构造、private field 访问、`MetadataToken` 等。Odin 和调试链路都会用到这些能力。
- UniTask/async 支持：async state machine、struct awaiter、continuation 调度、cancellation、timer/next-frame 等能力需要落到 LeanCLR 的单线程 frame scheduler 或宿主调度器上。
- Odin 序列化支持：如果完全支持 Odin runtime reflection 成本过高，需要为 LeanCLR 增加预生成 serializer 或限制序列化策略的 profile。
- AOT codegen 覆盖：NKG 代码大量使用泛型集合、record/value type、delegate、interface dispatch、异常、`TimeSpan`、`Dictionary`/`List`/`HashSet` 等，需要以实际 NKG 程序集作为 smoke test 排缺口。

### Unity/Godot Bridge 改造

为调用 Unity/Godot API，LeanCLR 需要新增一个引擎宿主 bridge 层，而不是把引擎 API 当普通 .NET 10 BCL 处理：

- 统一 bridge ABI：例如 `lc_host_call(functionId, argsBuffer)` 或按模块拆分的 internal call 表。
- 对象 handle registry：宿主侧维护 handle 到 Unity `Object` / Godot `Object`、Node、Resource 的映射，并处理生命周期失效。
- 主线程 dispatcher：资源加载、场景切换、节点树修改、UI 操作、音频播放等请求必须回到引擎主线程执行。
- Frame pump：Unity `Update/LateUpdate/FixedUpdate` 或 Godot `_Process/_PhysicsProcess` 驱动 LeanCLR 的 `RuntimeContext.Update`，并把 `GameFrameTime` 传入。
- 资源与场景服务：优先实现 NKG 已抽象的 `IAssetService`、`ISceneService`、audio、UI、config、localization、MVVM binding，而不是暴露整套引擎对象模型。
- 异步结果回传：Unity/Godot 的异步加载结果应回填到 LeanCLR 侧 UniTask completion source 或等价 continuation。
- 错误与诊断：bridge 调用需要把引擎异常、缺失 handle、线程错误转换成 LeanCLR 可诊断异常。

### 暂缓纳入的部分

`NKGGameFramework.Hosting` / Web Debug 依赖 `System.Net`、socket、HTTP/SSE、`System.Threading.Channels`、压缩、`System.Text.Json` 和更多反射 API。它适合放到后续阶段；第一阶段不应把 Hosting 作为 LeanCLR 支持 NKG 的验收标准。

### 建议验收顺序

1. LeanCLR 能运行最小 `net10.0` Hello/Smoke 程序。
2. LeanCLR 能加载并执行 `NKGGameFramework` 核心最小样例，不包含 Hosting、Unity、Godot。
3. `RuntimeContext.Update`、事件、ECS、Timer、基础 GameplayTag/Skill/Buff 路径跑通。
4. UniTask 的 completed/result/canceled、timer、next-frame、WhenAll/WhenAny 跑通。
5. Odin 对 NKG 常见组件、Buff、Skill、BehaviorTree 数据结构序列化/反序列化跑通，或明确切到预生成 serializer profile。
6. Unity bridge 实现 asset/scene/game-loop 三个最小服务，并用 opaque handle 调用真实 Unity API。
7. Godot bridge 按同样 ABI 实现 process、resource、scene/node 服务。
8. 最后再评估是否把 Hosting/Web Debug 搬进 LeanCLR，或改成宿主进程提供调试传输。

## 不建议的做法

- 不建议直接把所有旧式 `.NET Framework v4.8` 测试项目改成 `net10.0`。它们承担的是 Mono profile 回归测试。
- 不建议只依赖 `RollForward=LatestMajor` 宣称支持 .NET 10。那只说明工具能在新 runtime 上跑，不说明 LeanCLR runtime 能承载 .NET 10 BCL。
- 不建议把 .NET 10 icall 覆盖直接合进现有 `icalls.json`。应先 profile 化，否则会破坏 Mono/Unity 分支的行为边界。
- 不建议在 `coreclr-net10` 测试跑绿前直接删除 `mono-4.5`；这会让迁移缺少对照基线。
- 不建议在 CI 继续关闭的状态下推进支持声明。

## 近期任务清单

1. 定稿架构边界：`coreclr` 是引擎无关 `.NET 10` runtime 主线；Unity/Godot 只作为宿主插件和 bridge 接入。
2. 定稿复用边界：保留 LeanCLR VM 核心，集中改造 BCL profile、runtime contract、AOT 兼容、测试和 bridge。
3. 恢复 CI 基线：移除 `.github/workflows/ci.yml` 的 `if: false`，固定 .NET 10 SDK，补齐 CMake。
4. 决定是否加入 `global.json`，建议 pin 到当前验证过的 `10.0.301` 或 CI 统一的 `10.0.x`。
5. 安装 .NET Framework 4.8 Developer Pack 或改造旧测试引用，解决 `managed.sln` 的 `MSB3644`。
6. 给 `ExportExtern` 增加一组 .NET 10 BCL 导出脚本。
7. 输出 `coreclr-net10` 的 extern diff 报告。
8. 将 `RuntimeApiCatalog` 和 corlib/module 判断 profile 化，拆出 `mono45`、`unity`、`coreclr-net10`。
9. 增加 `System.Private.CoreLib` / `.NET 10` runtime pack 的 assembly resolver。
10. 新增最小 `net10.0` 测试解决方案。
11. 新增 NKGGameFramework 核心 smoke test，不包含 Hosting、Unity、Godot。
12. 根据 diff 优先补齐启动路径 icall。
13. 为 AOT modern IL 增加 smoke test。
14. 设计 Unity/Godot host bridge ABI、opaque handle registry 和主线程 dispatcher。
15. 更新 README/文档站能力矩阵。
16. 若确认全切 `.NET 10`，按“历史资产清理”小节逐步删除或归档 mono-4.5 资产、旧式测试和 `mscorlib`/`Mono.*` 遗留。

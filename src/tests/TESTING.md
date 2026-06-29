# LeanCLR 测试工程

本地快速开始：

```bat
powershell -ExecutionPolicy Bypass -File scripts\dotnet10\interp-smoke.ps1 -Configuration Release
powershell -ExecutionPolicy Bypass -File scripts\dotnet10\interp-smoke.ps1 -Configuration Release -AssemblyName ManagedNet10.LegacyTests -Entry "ManagedNet10.LegacyTests.Program::RunAll"
powershell -ExecutionPolicy Bypass -File scripts\dotnet10\legacy-managed-audit.ps1
powershell -ExecutionPolicy Bypass -File scripts\dotnet10\interp-smoke-matrix.ps1 -Configuration Release
powershell -ExecutionPolicy Bypass -File scripts\dotnet10\api-scan.ps1 -Configuration Release
powershell -ExecutionPolicy Bypass -File scripts\dotnet10\nkg-smoke.ps1 -Configuration Release
powershell -ExecutionPolicy Bypass -File scripts\dotnet10\aot-smoke.ps1 -Configuration Release -NativeRun
python src\generator\check_runtime_api_signatures.py --profile coreclr-net10 --repo-root .
```

Host bridge 验收使用 `scripts\dotnet10\host-bridge-smoke.ps1 -Configuration Release -Scenario <scenario>`，当前七个场景是 `AbiSkeleton`、`HandleRegistry`、`Dispatcher`、`EventCallback`、`EngineAdapter`、`ValueMarshal` 和 `Diagnostics`。本地完整 sweep 可用：

```powershell
$scenarios = @('AbiSkeleton','HandleRegistry','Dispatcher','EventCallback','EngineAdapter','ValueMarshal','Diagnostics')
foreach ($s in $scenarios) {
  powershell -ExecutionPolicy Bypass -File scripts\dotnet10\host-bridge-smoke.ps1 -Configuration Release -Scenario $s
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
```

当前测试入口以 `scripts\dotnet10` 为准。旧 Mono `scripts\test`、`basic-tester` 和 `managed.sln` 已随 Mono profile 清理删除；`managed/` 目录仍保留被 `managed-net10/ManagedNet10.LegacyTests` 链接的源码素材。虚调用/虚表覆盖保留在 `src\tests\managed\SharedTests\Instructions\Funcs\TC_callvir_interp.cs`，不得通过整文件注释跳过。

原仓库测试资产口径：`src\tests` 下除 `src\tests\managed-net10` 外的旧测试源码，当前由 `.NET 10` runner/profile 验证；其中 `src\tests\managed` 通过 `ManagedNet10.LegacyTests` 链接原源码。`legacy-managed-audit.ps1` 会展开旧 managed 链接并扫描未链接源码里的 `[UnitTest]`、`[AotMethod]`、`Assert`、旧 runner `Main` 和 public static helper。2026-06-29 全量验收结果为：374 个旧 managed 文件已链接，仅原始 `SharedTests/Instructions/Arithmetic/TC_sub.cs` 作为 active-ish 未链接文件保留，由 `managed-net10` 本地替代版覆盖，20 个 `[UnitTest]` 方法匹配。

上述通过表示 LeanCLR 自有 `src\tests` 原仓库测试资产已在当前 `.NET 10` runner/profile 下跑通；不表示完整 `Microsoft.NETCore.App`、真实 Unity/Godot SDK binding、完整 debugger、动态 native codegen 或多线程 runtime 已完成。

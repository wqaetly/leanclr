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

Host bridge 验收使用 `scripts\dotnet10\host-bridge-smoke.ps1 -Configuration Release -Scenario <scenario>`，当前七个场景是 `AbiSkeleton`、`HandleRegistry`、`Dispatcher`、`EventCallback`、`EngineAdapter`、`ValueMarshal` 和 `Diagnostics`。

当前测试入口以 `scripts\dotnet10` 为准。旧 Mono `scripts\test`、`basic-tester` 和 `managed.sln` 已随 Mono profile 清理删除；`managed/` 目录仍保留被 `managed-net10/ManagedNet10.LegacyTests` 链接的源码素材。虚调用/虚表覆盖保留在 `src\tests\managed\SharedTests\Instructions\Funcs\TC_callvir_interp.cs`，不得通过整文件注释跳过。

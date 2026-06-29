# LeanCLR 测试工程

本地快速开始：

```bat
powershell -ExecutionPolicy Bypass -File scripts\dotnet10\interp-smoke.ps1 -Configuration Release
powershell -ExecutionPolicy Bypass -File scripts\dotnet10\interp-smoke.ps1 -Configuration Release -AssemblyName ManagedNet10.LegacyTests -Entry "ManagedNet10.LegacyTests.Program::RunAll"
powershell -ExecutionPolicy Bypass -File scripts\dotnet10\api-scan.ps1 -Configuration Release
python src\generator\check_runtime_api_signatures.py --profile coreclr-net10 --repo-root .
```

当前测试入口以 `scripts\dotnet10` 为准。旧 Mono `scripts\test`、`basic-tester` 和 `managed.sln` 已随 Mono profile 清理删除；`managed/` 目录仍保留被 `managed-net10/ManagedNet10.LegacyTests` 链接的源码素材。

# Scripts

Repository-level build, test, and developer scripts. Current validation is centered on the `.NET 10` smoke and host bridge gates under `scripts/dotnet10`.

All build and intermediate outputs go under `out/` at the repository root (override with `LEANCLR_OUT_ROOT`). Source trees under `src/` stay clean.

```text
out/
├── cmake/<module>/<Config>-<Arch>/   # CMake build trees and native binaries
└── dotnet/<ProjectName>/<Config>/    # .NET assemblies
```

Clean all outputs: `scripts\dev\clean-out.bat` (Windows) or `./scripts/dev/clean-out.sh` (Unix).

## Quick Start

| Task | Windows |
| --- | --- |
| Default interpreter smoke | `powershell -ExecutionPolicy Bypass -File scripts\dotnet10\interp-smoke.ps1 -Configuration Release` |
| Legacy net10 regression | `powershell -ExecutionPolicy Bypass -File scripts\dotnet10\interp-smoke.ps1 -Configuration Release -AssemblyName ManagedNet10.LegacyTests -Entry "ManagedNet10.LegacyTests.Program::RunAll"` |
| Legacy managed asset audit | `powershell -ExecutionPolicy Bypass -File scripts\dotnet10\legacy-managed-audit.ps1` |
| Interpreter smoke matrix | `powershell -ExecutionPolicy Bypass -File scripts\dotnet10\interp-smoke-matrix.ps1 -Configuration Release` |
| API whitelist scan | `powershell -ExecutionPolicy Bypass -File scripts\dotnet10\api-scan.ps1 -Configuration Release` |
| NKG/Odin workload smoke | `powershell -ExecutionPolicy Bypass -File scripts\dotnet10\nkg-smoke.ps1 -Configuration Release` |
| NKG/Odin full AOT smoke | `powershell -ExecutionPolicy Bypass -File scripts\dotnet10\nkg-aot-smoke.ps1 -Configuration Release` |
| Host bridge smoke | Run `powershell -ExecutionPolicy Bypass -File scripts\dotnet10\host-bridge-smoke.ps1 -Configuration Release -Scenario <scenario>` for `AbiSkeleton`, `HandleRegistry`, `Dispatcher`, `EventCallback`, `EngineAdapter`, `ValueMarshal`, and `Diagnostics` |
| Runtime API signature gate | `python src\generator\check_runtime_api_signatures.py --profile coreclr-net10 --repo-root .` |
| LeanAOT net10 native smoke | `powershell -ExecutionPolicy Bypass -File scripts\dotnet10\aot-smoke.ps1 -Configuration Release -NativeRun` |
| Build runtime | `scripts\build.bat runtime Release` |
| Build LeanAOT | `scripts\build.bat leanaot Release` |
| Format runtime C++ | `scripts\dev\format-cpp-files.bat` |

## Layout

```text
scripts/
├── build.sh / build.bat     # Orchestration entry point
├── ci.sh                    # CI entry
├── dotnet10/                # .NET 10 smoke, API scan, NKG and host bridge gates
├── runtime/                 # leanclr runtime library
├── leanaot/                 # LeanAOT tooling
├── generator/               # Opcode and runtime API checks
├── dev/                     # clean-out, format-cpp-files
└── lib/                     # repo-root, out-dir-init, cmake-dir
```

The old `scripts/test` Mono/mono-4.5 entry points were removed with the Mono profile cleanup. Use the `scripts/dotnet10` gates for current validation.

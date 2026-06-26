param(
    [string]$RuntimeDir,
    [string]$Configuration = "Release",
    [string]$NativeBuildDir,
    [string]$CMakeGenerator,
    [string]$CMakeArchitecture,
    [string]$Entry = "ManagedNet10.Smoke.Program::TestPairArithmetic",
    [string]$AssemblyName = "ManagedNet10.Smoke",
    [switch]$BuildOnly
)

$ErrorActionPreference = "Stop"

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath $($Arguments -join ' ') failed with exit code $LASTEXITCODE"
    }
}

function Get-DotNet10RuntimeDir {
    $runtime = dotnet --list-runtimes |
        ForEach-Object {
            if ($_ -match '^Microsoft\.NETCore\.App\s+(?<version>10\.[^\s]+)\s+\[(?<path>.+)\]$') {
                [pscustomobject]@{
                    Version = [version]$Matches.version
                    Path = [System.IO.Path]::Combine($Matches.path, $Matches.version)
                }
            }
        } |
        Sort-Object Version -Descending |
        Select-Object -First 1

    if ($null -eq $runtime) {
        throw "Microsoft.NETCore.App 10.x runtime was not found. Install .NET 10 or pass -RuntimeDir."
    }

    return $runtime.Path
}

$repoRoot = (Resolve-Path ([System.IO.Path]::Combine($PSScriptRoot, "..", ".."))).Path

if ([string]::IsNullOrWhiteSpace($RuntimeDir)) {
    $RuntimeDir = Get-DotNet10RuntimeDir
}

$RuntimeDir = (Resolve-Path $RuntimeDir).Path

Invoke-Checked dotnet build ([System.IO.Path]::Combine($repoRoot, "src", "tests", "managed-net10", "managed-net10.sln")) -c $Configuration

if ([string]::IsNullOrWhiteSpace($NativeBuildDir)) {
    $NativeBuildDir = [System.IO.Path]::Combine($repoRoot, "out", "cmake", "tools", "leanrun", "$Configuration-x64")
}
$NativeBuildDir = [System.IO.Path]::GetFullPath($NativeBuildDir)

$leanrunSourceDir = [System.IO.Path]::Combine($repoRoot, "src", "tools", "leanrun")
$configureArgs = @(
    "-S",
    $leanrunSourceDir,
    "-B",
    $NativeBuildDir
)

$isWindowsHost = $env:OS -eq "Windows_NT"
if ($isWindowsHost) {
    if ([string]::IsNullOrWhiteSpace($CMakeGenerator)) {
        $CMakeGenerator = "Visual Studio 17 2022"
    }
    if ([string]::IsNullOrWhiteSpace($CMakeArchitecture)) {
        $CMakeArchitecture = "x64"
    }
    if (-not [string]::IsNullOrWhiteSpace($CMakeGenerator)) {
        $configureArgs += @("-G", $CMakeGenerator)
    }
    if (-not [string]::IsNullOrWhiteSpace($CMakeArchitecture)) {
        $configureArgs += @("-A", $CMakeArchitecture)
    }
}
else {
    $configureArgs += "-DCMAKE_BUILD_TYPE=$Configuration"
}

Invoke-Checked -FilePath cmake -Arguments $configureArgs
Invoke-Checked -FilePath cmake -Arguments @("--build", $NativeBuildDir, "--config", $Configuration, "--target", "leanrun", "--parallel")

$exeName = if ($isWindowsHost) { "leanrun.exe" } else { "leanrun" }
$nativeRunner = [System.IO.Path]::Combine($NativeBuildDir, "bin", $Configuration, $exeName)
if (-not (Test-Path $nativeRunner)) {
    $nativeRunner = [System.IO.Path]::Combine($NativeBuildDir, "bin", $exeName)
}
if (-not (Test-Path $nativeRunner)) {
    throw "leanrun output not found under $NativeBuildDir"
}

Write-Host "Built .NET 10 interpreter smoke runner at $nativeRunner"

if ($BuildOnly) {
    return
}

$smokeDir = [System.IO.Path]::Combine($repoRoot, "out", "dotnet", "ManagedNet10.Smoke", $Configuration, "net10.0")
if (-not (Test-Path ([System.IO.Path]::Combine($smokeDir, "$AssemblyName.dll")))) {
    throw "$AssemblyName output not found: $smokeDir"
}

$runArgs = @("-l", $smokeDir, "-l", $RuntimeDir)
if (-not [string]::IsNullOrWhiteSpace($Entry)) {
    $runArgs += @("-e", $Entry)
}
$runArgs += $AssemblyName

Invoke-Checked -FilePath $nativeRunner -Arguments $runArgs

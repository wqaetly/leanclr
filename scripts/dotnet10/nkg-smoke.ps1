param(
    [string]$NkgRoot,
    [string]$Configuration = "Release",
    [string]$RuntimeDir,
    [string]$NativeBuildDir,
    [string]$CMakeGenerator,
    [string]$CMakeArchitecture,
    [string]$Entry = "ManagedNet10.NkgSmoke.Program::RunCoreWorkloadSurfaceSmoke",
    [switch]$SkipNkgBuild
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

$repoRoot = (Resolve-Path ([System.IO.Path]::Combine($PSScriptRoot, "..", ".."))).Path

if ([string]::IsNullOrWhiteSpace($NkgRoot)) {
    $candidateRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($repoRoot, "..", "NKGGameFramework"))
    if (Test-Path $candidateRoot) {
        $NkgRoot = $candidateRoot
    }
}

if ([string]::IsNullOrWhiteSpace($NkgRoot) -or -not (Test-Path $NkgRoot)) {
    throw "NKGGameFramework root was not found. Pass -NkgRoot, for example C:\study\wqaetly\new\NKGGameFramework."
}

$NkgRoot = (Resolve-Path $NkgRoot).Path
$samplerProject = [System.IO.Path]::Combine($NkgRoot, "samples", "NKGGameFramework.Sampler", "NKGGameFramework.Sampler.csproj")
if (-not (Test-Path $samplerProject)) {
    throw "NKG sampler project was not found: $samplerProject"
}

if (-not $SkipNkgBuild) {
    Invoke-Checked dotnet build $samplerProject -c $Configuration
}

$samplerOutputDir = [System.IO.Path]::Combine($NkgRoot, "samples", "NKGGameFramework.Sampler", "bin", $Configuration, "net10.0")
if (-not (Test-Path ([System.IO.Path]::Combine($samplerOutputDir, "NKGGameFramework.dll")))) {
    throw "NKGGameFramework output was not found: $samplerOutputDir"
}
if (-not (Test-Path ([System.IO.Path]::Combine($samplerOutputDir, "UniTask.dll")))) {
    throw "UniTask dependency output was not found: $samplerOutputDir"
}
if (-not (Test-Path ([System.IO.Path]::Combine($samplerOutputDir, "OdinSerializer.dll")))) {
    throw "OdinSerializer dependency output was not found: $samplerOutputDir"
}

$interpSmokeScript = [System.IO.Path]::Combine($PSScriptRoot, "interp-smoke.ps1")
$args = @(
    "-Configuration",
    $Configuration,
    "-AssemblyName",
    "ManagedNet10.NkgSmoke",
    "-Entry",
    $Entry,
    "-AdditionalAssemblyDir",
    $samplerOutputDir
)

if (-not [string]::IsNullOrWhiteSpace($RuntimeDir)) {
    $args += @("-RuntimeDir", $RuntimeDir)
}
if (-not [string]::IsNullOrWhiteSpace($NativeBuildDir)) {
    $args += @("-NativeBuildDir", $NativeBuildDir)
}
if (-not [string]::IsNullOrWhiteSpace($CMakeGenerator)) {
    $args += @("-CMakeGenerator", $CMakeGenerator)
}
if (-not [string]::IsNullOrWhiteSpace($CMakeArchitecture)) {
    $args += @("-CMakeArchitecture", $CMakeArchitecture)
}

$interpArgs = @("-ExecutionPolicy", "Bypass", "-File", $interpSmokeScript) + $args
Invoke-Checked -FilePath powershell -Arguments $interpArgs

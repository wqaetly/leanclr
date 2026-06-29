param(
    [string]$RuntimeDir,
    [string]$Configuration = "Release",
    [string]$NativeBuildDir,
    [string]$CMakeGenerator,
    [string]$CMakeArchitecture,
    [string]$AssemblyName = "ManagedNet10.Smoke",
    [string]$SourceFile,
    [string[]]$Entries = @(),
    [string[]]$ExcludeEntries = @(),
    [string[]]$AdditionalAssemblyDir = @(),
    [switch]$ListOnly
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

function Get-SmokeEntries {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $pattern = '^\s*private\s+static\s+(?:async\s+)?(?:void|Task)\s+(?<name>Test[A-Za-z0-9_]+)\s*\(\s*\)'
    $matches = Select-String -Path $Path -Pattern $pattern
    $names = foreach ($match in $matches) {
        $match.Matches[0].Groups["name"].Value
    }

    $names | Sort-Object -Unique
}

$repoRoot = (Resolve-Path ([System.IO.Path]::Combine($PSScriptRoot, "..", ".."))).Path
$interpSmokeScript = [System.IO.Path]::Combine($PSScriptRoot, "interp-smoke.ps1")

if ([string]::IsNullOrWhiteSpace($SourceFile)) {
    $SourceFile = [System.IO.Path]::Combine($repoRoot, "src", "tests", "managed-net10", $AssemblyName, "Program.cs")
}
$SourceFile = (Resolve-Path $SourceFile).Path

if ($Entries.Count -eq 0) {
    $Entries = @(Get-SmokeEntries -Path $SourceFile)
}
if ($ExcludeEntries.Count -ne 0) {
    $excluded = @{}
    foreach ($entry in $ExcludeEntries) {
        if (-not [string]::IsNullOrWhiteSpace($entry)) {
            $excluded[$entry] = $true
        }
    }
    $Entries = @($Entries | Where-Object { -not $excluded.ContainsKey($_) })
}
if ($Entries.Count -eq 0) {
    throw "No smoke entries were found in $SourceFile"
}

if ($ListOnly) {
    $Entries
    return
}

if ([string]::IsNullOrWhiteSpace($RuntimeDir)) {
    $RuntimeDir = Get-DotNet10RuntimeDir
}
$RuntimeDir = (Resolve-Path $RuntimeDir).Path

$buildArgs = @(
    "-ExecutionPolicy",
    "Bypass",
    "-File",
    $interpSmokeScript,
    "-Configuration",
    $Configuration,
    "-AssemblyName",
    $AssemblyName,
    "-RuntimeDir",
    $RuntimeDir,
    "-BuildOnly"
)

if (-not [string]::IsNullOrWhiteSpace($NativeBuildDir)) {
    $buildArgs += @("-NativeBuildDir", $NativeBuildDir)
}
if (-not [string]::IsNullOrWhiteSpace($CMakeGenerator)) {
    $buildArgs += @("-CMakeGenerator", $CMakeGenerator)
}
if (-not [string]::IsNullOrWhiteSpace($CMakeArchitecture)) {
    $buildArgs += @("-CMakeArchitecture", $CMakeArchitecture)
}

Invoke-Checked powershell @buildArgs

if ([string]::IsNullOrWhiteSpace($NativeBuildDir)) {
    $NativeBuildDir = [System.IO.Path]::Combine($repoRoot, "out", "cmake", "tools", "leanrun", "$Configuration-x64")
}
$NativeBuildDir = [System.IO.Path]::GetFullPath($NativeBuildDir)

$exeName = if ($env:OS -eq "Windows_NT") { "leanrun.exe" } else { "leanrun" }
$nativeRunner = [System.IO.Path]::Combine($NativeBuildDir, "bin", $Configuration, $exeName)
if (-not (Test-Path $nativeRunner)) {
    $nativeRunner = [System.IO.Path]::Combine($NativeBuildDir, "bin", $exeName)
}
if (-not (Test-Path $nativeRunner)) {
    throw "leanrun output not found under $NativeBuildDir"
}

$assemblyDir = [System.IO.Path]::Combine($repoRoot, "out", "dotnet", $AssemblyName, $Configuration, "net10.0")
if (-not (Test-Path ([System.IO.Path]::Combine($assemblyDir, "$AssemblyName.dll")))) {
    throw "$AssemblyName output not found: $assemblyDir"
}

for ($i = 0; $i -lt $Entries.Count; $i++) {
    $entryName = $Entries[$i]
    $entry = "$AssemblyName.Program::$entryName"
    Write-Host ("[{0}/{1}] {2}" -f ($i + 1), $Entries.Count, $entry)

    $runArgs = @("-l", $assemblyDir)
    foreach ($dir in $AdditionalAssemblyDir) {
        if ([string]::IsNullOrWhiteSpace($dir)) {
            continue
        }
        $resolvedDir = (Resolve-Path $dir).Path
        $runArgs += @("-l", $resolvedDir)
    }
    $runArgs += @("-l", $RuntimeDir, "-e", $entry, $AssemblyName)

    Invoke-Checked -FilePath $nativeRunner -Arguments $runArgs
}

Write-Host "ok! $($Entries.Count) smoke entries passed."

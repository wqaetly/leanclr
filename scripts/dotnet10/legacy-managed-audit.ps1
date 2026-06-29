param(
    [string]$ProjectPath
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path ([System.IO.Path]::Combine($PSScriptRoot, "..", ".."))).Path
if ([string]::IsNullOrWhiteSpace($ProjectPath)) {
    $ProjectPath = [System.IO.Path]::Combine(
        $repoRoot,
        "src",
        "tests",
        "managed-net10",
        "ManagedNet10.LegacyTests",
        "ManagedNet10.LegacyTests.csproj")
}

$ProjectPath = (Resolve-Path $ProjectPath).Path
$projectDir = Split-Path $ProjectPath
$managedRoot = (Resolve-Path ([System.IO.Path]::Combine($repoRoot, "src", "tests", "managed"))).Path

[xml]$project = Get-Content $ProjectPath
$linked = @{}
foreach ($compile in $project.Project.ItemGroup.Compile) {
    $include = [string]$compile.Include
    if (-not $include.StartsWith("..\..\managed\", [System.StringComparison]::OrdinalIgnoreCase)) {
        continue
    }

    $pattern = Join-Path $projectDir $include
    foreach ($resolved in Resolve-Path -Path $pattern -ErrorAction SilentlyContinue) {
        if ($resolved.ProviderPath.EndsWith(".cs", [System.StringComparison]::OrdinalIgnoreCase)) {
            $linked[$resolved.ProviderPath.ToLowerInvariant()] = $true
        }
    }
}

function Get-RelativeManagedPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $relative = $Path.Substring($managedRoot.Length + 1)
    return $relative.Replace("/", "\")
}

function Get-ActiveMarkers {
    param([Parameter(Mandatory = $true)][string]$Path)

    $lines = Get-Content -LiteralPath $Path
    $markers = @()
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $trimmed = $lines[$i].TrimStart()
        if ($trimmed.StartsWith("//", [System.StringComparison]::Ordinal)) {
            continue
        }

        $kind = $null
        if ($trimmed -match "\[UnitTest\]") {
            $kind = "UnitTest"
        } elseif ($trimmed -match "Assert\.|Assert\(") {
            $kind = "Assert"
        } elseif ($trimmed -match "public\s+static\s+void\s+Main\s*\(") {
            $kind = "Main"
        } elseif ($trimmed -match "public\s+static\s+void\s+[A-Za-z0-9_]+\s*\(") {
            $kind = "PublicStatic"
        }

        if ($null -ne $kind) {
            $markers += [pscustomobject]@{
                Line = $i + 1
                Kind = $kind
                Text = $trimmed
            }
        }
    }

    return $markers
}

$allowedKindsByPath = @{
    "AotTests\App.cs" = @("Main")
    "SharedTests\Instructions\Arithmetic\TC_sub.cs" = @("UnitTest", "Assert", "PublicStatic")
    "SharedTests\Fixtures\CallFromNatives.cs" = @("PublicStatic")
    "SharedTests\Fixtures\FullGenericClass.cs" = @("PublicStatic")
    "SharedTests\Fixtures\FunctionPointers.cs" = @("PublicStatic")
    "SharedTests\Fixtures\Test.cs" = @("PublicStatic")
    "SharedTests\Fixtures\TypeThreadStaticFields.cs" = @("PublicStatic")
    "SharedTests\Fixtures\TypeThreadStaticFields2.cs" = @("PublicStatic")
    "SharedTests\Fixtures\VirtualGenericMethod.cs" = @("PublicStatic")
}

$activeUnlinked = @()
$violations = @()
foreach ($file in Get-ChildItem $managedRoot -Recurse -Filter *.cs) {
    if ($linked.ContainsKey($file.FullName.ToLowerInvariant())) {
        continue
    }

    $markers = @(Get-ActiveMarkers -Path $file.FullName)
    if ($markers.Count -eq 0) {
        continue
    }

    $relativePath = Get-RelativeManagedPath -Path $file.FullName
    $activeUnlinked += [pscustomobject]@{
        Path = $relativePath
        Markers = ($markers | ForEach-Object { "$($_.Line):$($_.Kind)" }) -join ", "
    }

    if (-not $allowedKindsByPath.ContainsKey($relativePath)) {
        $violations += [pscustomobject]@{
            Path = $relativePath
            Reason = "unclassified active markers"
            Markers = ($markers | ForEach-Object { "$($_.Line):$($_.Kind):$($_.Text)" }) -join " | "
        }
        continue
    }

    $allowedKinds = $allowedKindsByPath[$relativePath]
    foreach ($marker in $markers) {
        if ($allowedKinds -notcontains $marker.Kind) {
            $violations += [pscustomobject]@{
                Path = $relativePath
                Reason = "disallowed marker kind '$($marker.Kind)'"
                Markers = "$($marker.Line):$($marker.Kind):$($marker.Text)"
            }
        }
    }
}

Write-Host "Linked old managed files: $($linked.Count)"
Write-Host "Unlinked active-ish files: $($activeUnlinked.Count)"
if ($activeUnlinked.Count -gt 0) {
    $activeUnlinked | Sort-Object Path | Format-Table -AutoSize
}

if ($violations.Count -gt 0) {
    Write-Host ""
    Write-Host "Legacy managed audit violations:"
    $violations | Sort-Object Path | Format-Table -AutoSize -Wrap
    throw "Legacy managed audit failed."
}

Write-Host "Legacy managed audit passed."

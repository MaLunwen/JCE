# cook-project.ps1
# Mirror source_assets -> cooked_assets for a JCE project before build.
# Reads jce_project.json (schema v2 fields) with graceful fallback to
# the v1 convention "assets" -> "resources/_cooked".
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File cook-project.ps1
#       -ProjectDir <abs path>
#       -Manifest   <abs path to jce_project.json>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $ProjectDir,
    [Parameter(Mandatory = $true)] [string] $Manifest
)

$ErrorActionPreference = 'Stop'

$src = 'assets'
$dst = 'resources/_cooked'

if (Test-Path -LiteralPath $Manifest) {
    try {
        $json = Get-Content -Raw -LiteralPath $Manifest | ConvertFrom-Json
        if ($json.PSObject.Properties.Name -contains 'source_assets' -and $json.source_assets) {
            $src = [string]$json.source_assets
        }
        if ($json.PSObject.Properties.Name -contains 'cooked_assets' -and $json.cooked_assets) {
            $dst = [string]$json.cooked_assets
        }
    } catch {
        Write-Host "[cook] WARN: failed to parse manifest ($($_.Exception.Message)) — using defaults"
    }
} else {
    Write-Host "[cook] no manifest at $Manifest — using defaults"
}

$srcAbs = Join-Path $ProjectDir $src
$dstAbs = Join-Path $ProjectDir $dst

if (-not (Test-Path -LiteralPath $srcAbs)) {
    Write-Host "[cook] source_assets dir not present ($srcAbs) — skipping"
    exit 0
}

Write-Host "[cook] $src -> $dst"

if (-not (Test-Path -LiteralPath $dstAbs)) {
    New-Item -ItemType Directory -Path $dstAbs -Force | Out-Null
}

# /E recurse incl empty, /I assume dir, /Y overwrite, /D mtime-newer only, /Q quiet.
$xcopyArgs = @($srcAbs, $dstAbs, '/E', '/I', '/Y', '/D', '/Q')
& xcopy @xcopyArgs | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "[cook] xcopy failed with exit $LASTEXITCODE"
    exit 2
}

exit 0

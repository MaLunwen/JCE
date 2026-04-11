param(
    [string]$BuildRoot = (Join-Path $PSScriptRoot "..\..\build"),
    [string]$EditorExePath = "",
    [int]$FrameCount = 600,
    [int]$TimeoutSeconds = 40
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-EditorExecutable {
    param(
        [string]$Root,
        [string]$ExplicitPath
    )

    if ($ExplicitPath -and (Test-Path -LiteralPath $ExplicitPath)) {
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }

    if (-not (Test-Path -LiteralPath $Root)) {
        throw "Build root does not exist: $Root"
    }

    $allExes = Get-ChildItem -Path $Root -Recurse -File -Filter *.exe
    $exeCandidates = @(
        @(
            foreach ($exe in $allExes) {
                if ($exe.Name -like "*JCE_Editor*" -or $exe.Name -like "*Editor*") {
                    $exe
                }
            }
        ) | Sort-Object LastWriteTime -Descending
    )

    if ($exeCandidates.Count -eq 0) {
        throw "Could not locate editor executable under build root: $Root"
    }

    return $exeCandidates[0].FullName
}

function Get-Median {
    param([double[]]$Values)

    if (-not $Values -or $Values.Count -eq 0) {
        return [double]::NaN
    }

    $sorted = $Values | Sort-Object
    $count = $sorted.Count
    if ($count % 2 -eq 1) {
        return [double]$sorted[[int]($count / 2)]
    }

    $left = [double]$sorted[($count / 2) - 1]
    $right = [double]$sorted[$count / 2]
    return ($left + $right) / 2.0
}

function Get-Percentile {
    param(
        [double[]]$Values,
        [double]$Percentile
    )

    if (-not $Values -or $Values.Count -eq 0) {
        return [double]::NaN
    }

    $sorted = $Values | Sort-Object
    $rank = [Math]::Ceiling($Percentile * $sorted.Count)
    if ($rank -lt 1) { $rank = 1 }
    if ($rank -gt $sorted.Count) { $rank = $sorted.Count }
    return [double]$sorted[$rank - 1]
}

if ($FrameCount -lt 30) {
    throw "FrameCount must be at least 30 for a meaningful baseline."
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$metricsDir = Join-Path $repoRoot "docs\contracts\metrics"
New-Item -Path $metricsDir -ItemType Directory -Force | Out-Null

$editorExe = Resolve-EditorExecutable -Root $BuildRoot -ExplicitPath $EditorExePath
Write-Host "Using editor executable: $editorExe"

$tempLog = Join-Path ([System.IO.Path]::GetTempPath()) ("jce_kpi_frame_{0}.csv" -f $PID)
if (Test-Path -LiteralPath $tempLog) {
    Remove-Item -LiteralPath $tempLog -Force
}

$env:JCE_KPI_FRAME_LOG = $tempLog
$env:JCE_KPI_FRAME_COUNT = "$FrameCount"

$proc = Start-Process -FilePath $editorExe -PassThru
try {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)

    while ((Get-Date) -lt $deadline) {
        if (Test-Path -LiteralPath $tempLog) {
            $lineCount = @(Get-Content -LiteralPath $tempLog -ErrorAction SilentlyContinue).Count
            if ($lineCount -ge ($FrameCount + 1)) {
                break
            }
        }

        if ($proc.HasExited) {
            break
        }

        Start-Sleep -Milliseconds 100
    }
}
finally {
    if (-not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force
    }
}

if (-not (Test-Path -LiteralPath $tempLog)) {
    throw "Frame KPI log was not produced."
}

$rows = @(Import-Csv -LiteralPath $tempLog)
if ($rows.Count -eq 0) {
    throw "Frame KPI log is empty."
}

$values = New-Object System.Collections.Generic.List[double]
foreach ($row in $rows) {
    $values.Add([double]$row.frame_ms)
}

$outCsv = Join-Path $metricsDir "frame_time_samples.csv"
Copy-Item -LiteralPath $tempLog -Destination $outCsv -Force

$median = Get-Median -Values $values.ToArray()
$p95 = Get-Percentile -Values $values.ToArray() -Percentile 0.95
$avg = ($values | Measure-Object -Average).Average

Write-Host ""
Write-Host "Frame KPI summary"
Write-Host ("Average frame_ms: {0:N3}" -f $avg)
Write-Host ("Median frame_ms:  {0:N3}" -f $median)
Write-Host ("P95 frame_ms:     {0:N3}" -f $p95)
Write-Host ("Samples:          {0}" -f $values.Count)
Write-Host ("CSV:              {0}" -f $outCsv)

Remove-Item Env:JCE_KPI_FRAME_LOG -ErrorAction SilentlyContinue
Remove-Item Env:JCE_KPI_FRAME_COUNT -ErrorAction SilentlyContinue
if (Test-Path -LiteralPath $tempLog) {
    try {
        Remove-Item -LiteralPath $tempLog -Force -ErrorAction Stop
    }
    catch {
        # Best-effort cleanup only.
    }
}

param(
    [string]$BuildRoot = (Join-Path $PSScriptRoot "..\..\build"),
    [string]$EditorExePath = "",
    [int]$Runs = 5,
    [int]$TimeoutSeconds = 25
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

    $exeCandidates = @(
        Get-ChildItem -Path $Root -Recurse -File -Filter *.exe |
            Where-Object { $_.Name -like "*JCE_Editor*" -or $_.Name -like "*Editor*" } |
            Sort-Object LastWriteTime -Descending
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

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$metricsDir = Join-Path $repoRoot "docs\contracts\metrics"
New-Item -Path $metricsDir -ItemType Directory -Force | Out-Null

$editorExe = Resolve-EditorExecutable -Root $BuildRoot -ExplicitPath $EditorExePath
Write-Host "Using editor executable: $editorExe"

if ($Runs -lt 1) {
    throw "Runs must be >= 1"
}

$samples = New-Object System.Collections.Generic.List[double]
$rows = New-Object System.Collections.Generic.List[string]
$rows.Add("run,startup_ms")

for ($run = 1; $run -le $Runs; $run++) {
    $tempLog = Join-Path ([System.IO.Path]::GetTempPath()) ("jce_kpi_startup_{0}_{1}.csv" -f $PID, $run)
    if (Test-Path -LiteralPath $tempLog) {
        Remove-Item -LiteralPath $tempLog -Force
    }

    $env:JCE_KPI_STARTUP_LOG = $tempLog
    $startupValue = $null

    $proc = Start-Process -FilePath $editorExe -PassThru
    try {
        $deadline = (Get-Date).AddSeconds($TimeoutSeconds)

        while ((Get-Date) -lt $deadline) {
            if (Test-Path -LiteralPath $tempLog) {
                $line = Get-Content -LiteralPath $tempLog -TotalCount 1 -ErrorAction SilentlyContinue
                $matchResult = [regex]::Match($line, '^startup_ms,([0-9]+(?:\.[0-9]+)?)$')
                if ($matchResult.Success) {
                    $startupValue = [double]$matchResult.Groups[1].Value
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

    if ($null -eq $startupValue) {
        throw "Run $run failed: startup KPI not captured within timeout."
    }

    $samples.Add($startupValue)
    $startupCsv = $startupValue.ToString("F3", [System.Globalization.CultureInfo]::InvariantCulture)
    $rows.Add("$run,$startupCsv")
    Write-Host ("Run {0}: startup_ms={1:N3}" -f $run, $startupValue)

    if (Test-Path -LiteralPath $tempLog) {
        Remove-Item -LiteralPath $tempLog -Force
    }
}

Remove-Item Env:JCE_KPI_STARTUP_LOG -ErrorAction SilentlyContinue

$outCsv = Join-Path $metricsDir "startup_samples.csv"
Set-Content -Path $outCsv -Value $rows

$median = Get-Median -Values $samples.ToArray()
$p95 = Get-Percentile -Values $samples.ToArray() -Percentile 0.95

Write-Host ""
Write-Host "Startup KPI summary"
Write-Host ("Median startup_ms: {0:N3}" -f $median)
Write-Host ("P95 startup_ms:    {0:N3}" -f $p95)
Write-Host ("Samples:           {0}" -f $samples.Count)
Write-Host ("CSV:               {0}" -f $outCsv)

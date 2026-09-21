param(
    [Parameter(Mandatory)][string]$BaselineDirectory,
    [Parameter(Mandatory)][string]$OptimizedDirectory,
    [string]$OutputDirectory = $PSScriptRoot,
    [long]$AffinityMask = 1
)
$ErrorActionPreference = 'Stop'
$baseline = (Resolve-Path -LiteralPath $BaselineDirectory).Path
$optimized = (Resolve-Path -LiteralPath $OptimizedDirectory).Path
$output = (Resolve-Path -LiteralPath $OutputDirectory).Path
$process = [System.Diagnostics.Process]::GetCurrentProcess()
$previousAffinity = $process.ProcessorAffinity
try {
    # Child benchmark processes inherit affinity. Restore this shell afterwards.
    $process.ProcessorAffinity = [IntPtr]$AffinityMask
    foreach ($name in @('random_create', 'access_paths')) {
        $prefix = if ($name -eq 'access_paths') { 'paged_access' } else { $name }
        for ($round = 0; $round -lt 6; ++$round) {
            $versions = if ($round % 2 -eq 0) { @('baseline', 'optimized') } else { @('optimized', 'baseline') }
            foreach ($version in $versions) {
                $directory = if ($version -eq 'baseline') { $baseline } else { $optimized }
                $executable = Join-Path $directory "ekit_${name}_bench.exe"
                if ($name -eq 'access_paths') {
                    # Give each storage backend a fresh process/heap state.
                    $lines = @(& $executable $round sparse)
                    if ($LASTEXITCODE -ne 0) { throw "Sparse benchmark failed: round $round" }
                    $denseLines = @(& $executable $round dense)
                    if ($LASTEXITCODE -ne 0) { throw "Dense benchmark failed: round $round" }
                    $lines += $denseLines | Select-Object -Skip 1
                } else {
                    $lines = @(& $executable $round)
                    if ($LASTEXITCODE -ne 0) { throw "Benchmark failed: $executable, round $round" }
                }
                $file = Join-Path $output "${prefix}_${version}.csv"
                if ($round -eq 0) { $lines | Set-Content -Encoding utf8 -LiteralPath $file }
                else { $lines | Select-Object -Skip 1 | Add-Content -Encoding utf8 -LiteralPath $file }
            }
        }
    }
    $coverage = @()
    for ($round = 0; $round -lt 6; ++$round) {
        $versions = if ($round % 2 -eq 0) { @('baseline', 'optimized') } else { @('optimized', 'baseline') }
        foreach ($version in $versions) {
            $directory = if ($version -eq 'baseline') { $baseline } else { $optimized }
            $coverage += "${version}_run=$round"
            $coverage += & (Join-Path $directory 'ekit_sparse_query_bench.exe')
            if ($LASTEXITCODE -ne 0) { throw "Coverage benchmark failed: $version, round $round" }
        }
    }
    $coverage | Set-Content -Encoding utf8 -LiteralPath (Join-Path $output 'paged_coverage.txt')
} finally {
    $process.ProcessorAffinity = $previousAffinity
}

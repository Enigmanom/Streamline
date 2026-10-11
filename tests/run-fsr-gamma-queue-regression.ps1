param(
    [Parameter(Mandatory)][string]$SdkDirectory,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\_artifacts\tests\fsr-gamma-queue')
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$SdkDirectory = (Resolve-Path -LiteralPath $SdkDirectory).Path
foreach ($name in @('sl.interposer.dll', 'sl.common.dll', 'sl.fsr_g.dll',
    'cs_fidelityfx_framegeneration_dx12.dll', 'amd_fidelityfx_framegeneration_dx12.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $SdkDirectory $name))) {
        throw "Missing SDK runtime: $name"
    }
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'Run from a Visual Studio developer shell.'
}
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$exe = Join-Path $OutputDirectory 'fsr-gamma-queue-regression.exe'
& cl.exe /nologo /std:c++20 /EHsc /W4 /WX /MD /DNOMINMAX "/I$root\include" `
    (Join-Path $PSScriptRoot 'fsr-gamma-queue-regression.cpp') d3d12.lib dxgi.lib user32.lib `
    "/Fo:$OutputDirectory\\" "/Fe:$exe"
if ($LASTEXITCODE -ne 0) { throw 'Failed to compile the GPU regression.' }
$cases = @(
    @{Name='fsr4-gamma-loading'; Algorithm='fsr4'; Color='gamma'; Milliseconds='166.000015'; First='4726'}
)
Push-Location $OutputDirectory
try {
    foreach ($case in $cases) {
        $log = Join-Path $OutputDirectory ($case.Name + '.log')
        & $exe $SdkDirectory $case.Algorithm $case.Color $case.Milliseconds $case.First 96 *> $log
        $caseExit = $LASTEXITCODE
        Write-Output "$($case.Name): exit=$caseExit; log=$log"
        if ($caseExit -ne 0) { throw "GPU regression failed: $($case.Name)" }
        if (-not (Select-String -LiteralPath $log -SimpleMatch 'PASS: 96 generated-frame submissions')) {
            throw "GPU regression produced no completion result: $($case.Name)"
        }
    }
} finally { Pop-Location }

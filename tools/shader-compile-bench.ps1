<#
.SYNOPSIS
    Measure shader compile times: offline fxc bench or CommunityShaders.log parse.

.DESCRIPTION
    Two modes, one CSV shape (shader,stage,ms,defines) so in-game recompiles and
    offline benches are directly comparable:

    1) Bench mode (-Shader + -PermutationsFile): compiles each permutation with
       fxc at RELEASE PARITY (/O3, no debug flags) -- matching the game's runtime
       compile for users -- and records wall-time per permutation. This is the
       baseline that perf claims for compile-time refactors must diff against;
       the validation configs' D3DCOMPILE_DEBUG/SKIP_OPTIMIZATION flags make
       compiles artificially fast and must not be used for timing.

    2) Log mode (-FromLog): parses [ShaderTiming] lines emitted by the runtime
       into the same CSV, so any in-game recompile doubles as a measurement run.

.PARAMETER Shader
    Path to the .hlsl file (bench mode).

.PARAMETER PermutationsFile
    One space-separated define set per line (tools/gen-verify-perms.py output).

.PARAMETER FromLog
    Path to a CommunityShaders.log containing [ShaderTiming] lines (log mode).

.PARAMETER OutCsv
    CSV output path. Default: shader-bench.csv (bench) / <log>.timing.csv (log).

.PARAMETER Profile
    fxc target profile (bench mode). Default: auto (cs_5_0 for *CS.hlsl, else
    ps_5_0; pass vs_5_0 explicitly for VSHADER permutation lists).

.PARAMETER Top
    How many slowest entries to print in the summary. Default 15.

.EXAMPLE
    pwsh tools/shader-compile-bench.ps1 -Shader package/Shaders/Lighting.hlsl `
        -PermutationsFile build/verify-perms/Lighting.PSHADER.full.txt

.EXAMPLE
    pwsh tools/shader-compile-bench.ps1 -FromLog "$env:USERPROFILE\Documents\My Games\Skyrim Special Edition\SKSE\CommunityShaders.log"
#>
[CmdletBinding(DefaultParameterSetName = "Bench")]
param(
    [Parameter(ParameterSetName = "Bench", Mandatory = $true)]
    [string]$Shader,
    [Parameter(ParameterSetName = "Bench", Mandatory = $true)]
    [string]$PermutationsFile,
    [Parameter(ParameterSetName = "Log", Mandatory = $true)]
    [string]$FromLog,
    [string]$OutCsv,
    [string]$IncludeDir = "package/Shaders",
    [string]$Entry = "main",
    [string]$Profile,
    [string]$Fxc,
    [int]$Top = 15
)

$ErrorActionPreference = "Stop"

function Write-Summary([object[]]$rows, [string]$csv) {
    $rows | Export-Csv -NoTypeInformation -Encoding UTF8 $csv
    $total = ($rows | Measure-Object ms -Sum).Sum
    Write-Host ("rows={0}  total={1:N0}s  csv={2}" -f $rows.Count, ($total / 1000), $csv)
    Write-Host "-- by shader:stage --"
    $rows | Group-Object { "$($_.shader):$($_.stage)" } | ForEach-Object {
        [pscustomobject]@{ key = $_.Name; n = $_.Count; totalS = [math]::Round((($_.Group | Measure-Object ms -Sum).Sum) / 1000, 1) }
    } | Sort-Object totalS -Descending | Format-Table -AutoSize | Out-String | Write-Host
    Write-Host "-- slowest $Top --"
    $rows | Sort-Object ms -Descending | Select-Object -First $Top |
        ForEach-Object { Write-Host ("{0,9:N1}s  {1}:{2}  {3}" -f ($_.ms / 1000), $_.shader, $_.stage, $_.defines.Substring(0, [Math]::Min(110, $_.defines.Length))) }
}

if ($PSCmdlet.ParameterSetName -eq "Log") {
    if (-not (Test-Path $FromLog)) { throw "Log '$FromLog' not found." }
    if (-not $OutCsv) { $OutCsv = "$FromLog.timing.csv" }
    $pat = [regex]'\[ShaderTiming\] (\d+)ms \|.*?\| (\S+?):(\S+?):(.*)$'
    $rows = foreach ($line in [IO.File]::ReadLines($FromLog)) {
        $m = $pat.Match($line)
        if ($m.Success) {
            [pscustomobject]@{
                shader  = $m.Groups[2].Value
                stage   = $m.Groups[3].Value
                ms      = [int]$m.Groups[1].Value
                defines = $m.Groups[4].Value.Trim()
            }
        }
    }
    if (-not $rows) { throw "No [ShaderTiming] lines found in '$FromLog' (debug logging must be enabled)." }
    Write-Summary $rows $OutCsv
    exit 0
}

# ---- bench mode ----
function Resolve-Fxc {
    if ($Fxc -and (Test-Path $Fxc)) { return $Fxc }
    $cmd = Get-Command fxc.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $roots = @("${env:ProgramFiles(x86)}\Windows Kits\10\bin", "${env:ProgramFiles}\Windows Kits\10\bin")
    $found = foreach ($r in $roots) {
        if (Test-Path $r) {
            Get-ChildItem -Path $r -Recurse -Filter fxc.exe -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match "x64" }
        }
    }
    $pick = $found | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $pick) { throw "fxc.exe not found. Install the Windows 10/11 SDK or pass -Fxc." }
    return $pick.FullName
}

$fxcPath = Resolve-Fxc
if (-not (Test-Path $Shader)) { throw "Shader '$Shader' not found." }
if (-not (Test-Path $PermutationsFile)) { throw "Permutations file '$PermutationsFile' not found." }
if (-not $Profile) { $Profile = if ($Shader -match 'CS\.hlsl$') { "cs_5_0" } else { "ps_5_0" } }
if (-not $OutCsv) { $OutCsv = "shader-bench.csv" }

$shaderName = [IO.Path]::GetFileNameWithoutExtension($Shader)
$stage = switch -Wildcard ($Profile) { "cs_*" { "Compute" } "vs_*" { "Vertex" } default { "Pixel" } }
$perms = @(Get-Content $PermutationsFile | ForEach-Object { $_.Trim() } |
        Where-Object { $_ -and -not $_.StartsWith('#') })
if (-not $perms) { throw "No permutations in '$PermutationsFile'." }

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("shaderbench_" + [Guid]::NewGuid().ToString("N") + ".cso")
Write-Host "Bench: $Shader  profile=$Profile  perms=$($perms.Count)  flags=/O3 (release parity)"
$rows = New-Object System.Collections.Generic.List[object]
$i = 0
try {
    foreach ($perm in $perms) {
        $i++
        $defArgs = @()
        foreach ($d in ($perm -split '\s+' | Where-Object { $_ })) {
            $defArgs += "/D"
            $defArgs += $(if ($d -like '*=*') { $d } else { "$d=1" })
        }
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $null = & $fxcPath /nologo /O3 /T $Profile /E $Entry @defArgs /I $IncludeDir $Shader /Fo $tmp 2>&1
        $sw.Stop()
        $ok = ($LASTEXITCODE -eq 0)
        $rows.Add([pscustomobject]@{
                shader  = $shaderName
                stage   = $stage
                ms      = [int]$sw.ElapsedMilliseconds
                defines = $perm + $(if (-not $ok) { " [COMPILE-ERROR]" })
            })
        Write-Host ("[{0}/{1}] {2,8:N1}s {3} {4}" -f $i, $perms.Count, ($sw.ElapsedMilliseconds / 1000), $(if ($ok) { "ok " } else { "ERR" }), $perm.Substring(0, [Math]::Min(90, $perm.Length)))
    }
}
finally {
    Remove-Item $tmp -ErrorAction SilentlyContinue
}
Write-Summary $rows.ToArray() $OutCsv

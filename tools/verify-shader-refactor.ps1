<#
.SYNOPSIS
    Prove an HLSL refactor is behavior-preserving by comparing compiled bytecode.

.DESCRIPTION
    Compiles a shader from a base git revision and from the current working tree
    across a set of preprocessor permutations, then compares the resulting DXBC.
    The base ref's entire include tree (-IncludeDir) is materialized via git archive,
    so the base compiles against base-ref headers and the working tree against working
    headers -- a refactor that also edits a shared .hlsli is therefore compared correctly.

    Tier 1 (this script): identical SHA-256 of the compiled .cso == provably identical
    GPU program. fxc emits no timestamps without /Zi, so same source -> same bytes.

    Tier 2 (this script, on mismatch): dumps /Fc assembly for both revisions and lists
    the differing lines (base/work markers), so a legitimate-but-non-identical refactor
    (e.g. register reorder) can be eyeballed.

    A refactor that is Tier-1 IDENTICAL on the swept permutations needs no further proof.
    Note: the default sweep (VR x HDR_OUTPUT) is strong evidence, not the full build
    matrix from shader-validation.yaml. Pass -PermutationsFile for the real matrix.

    Batch + bisect workflow (the efficient cadence for many changes): apply a whole
    batch of commits, run this once with -FailFast. On DIFFERS it prints the failing
    permutation; use `git bisect run` with that single permutation (-Permutations
    "<failing>") as the probe -- seconds per bisect step -- then one final full-list
    run on the clean batch. Base-side compile results are cached per base SHA under
    build/verify-cache/, so repeat runs only pay for the work side.

.PARAMETER Shader
    Path to the .hlsl file (repo-relative or absolute).

.PARAMETER BaseRef
    Git ref to treat as "before". Default: merge-base of HEAD and origin/dev.

.PARAMETER IncludeDir
    Shader include root(s) passed to fxc /I. Default: package/Shaders plus every
    features/*/Shaders dir, mirroring the merged tree the game compiles against
    (Lighting and friends include feature .hlsli files).

.PARAMETER Permutations
    Optional explicit permutation list; each entry is a space-separated define set,
    e.g. -Permutations "PSHADER","PSHADER VR". Overrides the auto sweep.

.PARAMETER PermutationsFile
    File with one space-separated define set per line ('#' comments allowed), as
    produced by tools/gen-verify-perms.py. Overrides -Permutations and the auto
    sweep. Use this for the big shaders -- the default sweep is far too weak there.

.PARAMETER PreprocessOnly
    Tier-0 gate: compare fxc /P preprocessed output (with #line directives and
    blank lines stripped) instead of compiled DXBC. Identical preprocessed text
    proves identical bytecode at every optimization level, and runs in milliseconds
    per permutation. Only valid for refactors that don't change the preprocessed
    text (pure cut-paste moves); a guard-tightening change must use the DXBC compare.

.PARAMETER Jobs
    Parallel fxc workers. Default 0 = auto (logical cores - 2, min 1). fxc is
    CPU-bound and independent per permutation, so this scales nearly linearly.

.PARAMETER FailFast
    Stop at the first DIFFERS/error and print the failing permutation in a
    re-invokable form (the bisect probe). Without it, all permutations run and
    the first few mismatches get detail diffs.

.PARAMETER NoCache
    Skip the base-side result cache (build/verify-cache/). The cache is keyed by
    base SHA + shader + mode + profile + entry, so it is safe by construction;
    use this only when debugging the tool itself.

.PARAMETER Entry
    Shader entry point. Default: main.

.PARAMETER Profile
    fxc target profile. Default: auto (cs_5_0 for *CS.hlsl, else ps_5_0).

.EXAMPLE
    pwsh tools/verify-shader-refactor.ps1 package/Shaders/ISTemporalAA.hlsl

.EXAMPLE
    pwsh tools/verify-shader-refactor.ps1 package/Shaders/Lighting.hlsl `
        -PermutationsFile build/verify-perms/Lighting.PSHADER.full.txt -FailFast
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Shader,
    [string]$BaseRef,
    [string[]]$IncludeDir,
    [string[]]$Permutations,
    [string]$PermutationsFile,
    [switch]$PreprocessOnly,
    [int]$Jobs = 0,
    [switch]$FailFast,
    [switch]$NoCache,
    [string]$Entry = "main",
    [string]$Profile,
    [string]$Fxc
)

# Continue (not Stop): native git calls write warnings to stderr that would otherwise
# abort the run; control flow keys off explicit $LASTEXITCODE checks and `throw`.
$ErrorActionPreference = "Continue"

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

# Self-contained worker (runspaces do not inherit function scope). Compiles or
# preprocesses one permutation and returns its content hash.
$workerScript = {
    param($fxc, $mode, $profile, $entry, $defs, $incDirs, $src, $outFile)
    $defArgs = @()
    foreach ($d in ($defs -split '\s+' | Where-Object { $_ })) {
        $defArgs += "/D"
        $defArgs += $(if ($d -like '*=*') { $d } else { "$d=1" })
    }
    $incArgs = @()
    foreach ($i in $incDirs) { $incArgs += "/I"; $incArgs += $i }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        if ($mode -eq 'P') {
            $out = & $fxc /nologo /P $outFile @defArgs @incArgs $src 2>&1
            if ($LASTEXITCODE -ne 0) { return @{ Ok = $false; Err = (($out | Out-String)) } }
            # Strip #line directives, blank lines, trailing whitespace: none can affect
            # tokenization (fxc embeds no file/line info without /Zi; /P retokenizes).
            $sb = New-Object System.Text.StringBuilder
            foreach ($line in [System.IO.File]::ReadLines($outFile)) {
                $t = $line.TrimEnd()
                if ($t -and $t -notmatch '^\s*#line') { [void]$sb.AppendLine($t) }
            }
            $bytes = [System.Text.Encoding]::UTF8.GetBytes($sb.ToString())
            return @{ Ok = $true; Hash = (-join ($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString("x2") })) }
        } else {
            $out = & $fxc /nologo /T $profile /E $entry @defArgs @incArgs $src /Fo $outFile 2>&1
            if ($LASTEXITCODE -ne 0) { return @{ Ok = $false; Err = (($out | Out-String)) } }
            $fs = [System.IO.File]::OpenRead($outFile)
            try { $hash = -join ($sha.ComputeHash($fs) | ForEach-Object { $_.ToString("x2") }) }
            finally { $fs.Close() }
            return @{ Ok = $true; Hash = $hash }
        }
    }
    finally {
        Remove-Item $outFile -ErrorAction SilentlyContinue
        $sha.Dispose()
    }
}

# Resolve repo root so the script works from any cwd.
$repoRoot = (git rev-parse --show-toplevel 2>$null)
if (-not $repoRoot) { throw "Not inside a git repository." }
Push-Location $repoRoot
$work = $null
$pool = $null
try {
    $fxcPath = Resolve-Fxc

    # Normalize the shader path to repo-relative (forward slashes) for git.
    # (Path.GetRelativePath is unavailable in Windows PowerShell 5.1 / .NET Framework.)
    $shaderFull = (Resolve-Path $Shader).Path
    $rootFull = (Resolve-Path $repoRoot).Path
    if (-not $shaderFull.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Shader '$Shader' resolves outside the repo root '$rootFull'."
    }
    $relPath = $shaderFull.Substring($rootFull.Length).TrimStart('\', '/').Replace('\', '/')

    if (-not $BaseRef) {
        $BaseRef = (git merge-base HEAD origin/dev 2>$null)
        if (-not $BaseRef) { $BaseRef = "HEAD" }
    }
    $baseSha = (git rev-parse $BaseRef 2>$null)
    if (-not $baseSha) { throw "Cannot resolve base ref '$BaseRef'." }

    # The game compiles against the MERGED tree (package/Shaders + every feature's
    # Shaders dir deployed together), so feature includes like
    # "WaterEffects/WaterCaustics.hlsli" need each features/*/Shaders as a root.
    if (-not $IncludeDir) {
        $IncludeDir = @("package/Shaders") + @(Get-ChildItem -Directory "features" -ErrorAction SilentlyContinue |
                ForEach-Object { "features/$($_.Name)/Shaders" } | Where-Object { Test-Path $_ })
    }
    $IncludeDir = @($IncludeDir | ForEach-Object { $_.Replace('\', '/').TrimEnd('/') })

    if (-not $Profile) {
        $Profile = if ($relPath -match 'CS\.hlsl$') { "cs_5_0" } else { "ps_5_0" }
    }
    $stageDefine = switch -Wildcard ($Profile) {
        "cs_*" { "CSHADER" }
        "vs_*" { "VSHADER" }
        default { "PSHADER" }
    }

    if ($PermutationsFile) {
        if (-not (Test-Path $PermutationsFile)) { throw "Permutations file '$PermutationsFile' not found." }
        $Permutations = @(Get-Content $PermutationsFile | ForEach-Object { $_.Trim() } |
                Where-Object { $_ -and -not $_.StartsWith('#') })
        if ($Permutations.Count -eq 0) { throw "Permutations file '$PermutationsFile' contains no define sets." }
    }
    if (-not $Permutations -or $Permutations.Count -eq 0) {
        $Permutations = @(
            "$stageDefine",
            "$stageDefine VR",
            "$stageDefine HDR_OUTPUT",
            "$stageDefine VR HDR_OUTPUT"
        )
    }

    if ($Jobs -le 0) { $Jobs = [Math]::Max(1, [Environment]::ProcessorCount - 2) }
    $mode = if ($PreprocessOnly) { 'P' } else { 'O' }

    # Base-side cache: the base ref is immutable, so its per-permutation hashes are
    # reusable across runs -- repeat runs only pay for the work side.
    $cache = @{}
    $cacheFile = $null
    if (-not $NoCache) {
        $cacheDir = Join-Path $repoRoot "build/verify-cache"
        New-Item -ItemType Directory -Force $cacheDir | Out-Null
        $stem = [IO.Path]::GetFileNameWithoutExtension($relPath)
        $cacheFile = Join-Path $cacheDir ("{0}.{1}.{2}.{3}.{4}.json" -f $stem, $mode, $Profile, $Entry, $baseSha.Substring(0, 12))
        if (Test-Path $cacheFile) {
            $json = Get-Content $cacheFile -Raw | ConvertFrom-Json
            foreach ($p in $json.PSObject.Properties) { $cache[$p.Name] = $p.Value }
        }
    }

    $work = Join-Path ([IO.Path]::GetTempPath()) ("shaderverify_" + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force $work | Out-Null

    # Materialize the base revision's FULL include tree lazily (skipped entirely when
    # every base hash is cached). git archive -> tar via a file, never a PS pipeline,
    # which would corrupt the binary tar.
    $script:baseFile = $null
    $script:baseInclude = $null
    function Ensure-BaseTree {
        if ($script:baseFile) { return }
        $baseRoot = Join-Path $work "base"
        New-Item -ItemType Directory -Force $baseRoot | Out-Null
        $tar = Join-Path $work "base.tar"
        git archive --format=tar -o $tar $baseSha -- @IncludeDir $relPath 2>$null
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path $tar)) {
            throw "git archive failed for '$BaseRef' (paths: $($IncludeDir -join ', '), $relPath)."
        }
        tar -xf $tar -C $baseRoot
        if ($LASTEXITCODE -ne 0) { throw "Failed to extract base archive." }
        $script:baseFile = Join-Path $baseRoot $relPath
        $script:baseInclude = @($IncludeDir | ForEach-Object { Join-Path $baseRoot $_ })
        if (-not (Test-Path $script:baseFile)) { throw "'$relPath' not found at '$BaseRef'." }
    }

    $uncached = @($Permutations | Where-Object { -not $cache.ContainsKey($_) })
    if ($uncached.Count -gt 0) { Ensure-BaseTree }

    function DefineArgs([string]$defs) {
        $defArgs = @()
        foreach ($d in ($defs -split '\s+' | Where-Object { $_ })) {
            $defArgs += "/D"
            $defArgs += $(if ($d -like '*=*') { $d } else { "$d=1" })
        }
        return $defArgs
    }
    function IncludeArgs([string[]]$incDirs) {
        $incArgs = @()
        foreach ($d in $incDirs) { $incArgs += "/I"; $incArgs += $d }
        return $incArgs
    }

    # Sequential detail helpers, used only on mismatch (the parallel pass trades
    # line-level detail for speed; detail is recomputed for the few failures).
    function Compile([string]$src, [string[]]$incDirs, [string]$defs, [string]$outFile, [switch]$Asm) {
        $defArgs = DefineArgs $defs
        $incArgs = IncludeArgs $incDirs
        $fmt = if ($Asm) { "/Fc" } else { "/Fo" }
        $out = & $fxcPath /nologo /T $Profile /E $Entry @defArgs @incArgs $src $fmt $outFile 2>&1
        return @{ Code = $LASTEXITCODE; Out = $out }
    }
    function PreprocessLines([string]$src, [string[]]$incDirs, [string]$defs, [string]$outFile) {
        $defArgs = DefineArgs $defs
        $incArgs = IncludeArgs $incDirs
        $out = & $fxcPath /nologo /P $outFile @defArgs @incArgs $src 2>&1
        if ($LASTEXITCODE -ne 0) { return @{ Code = $LASTEXITCODE; Out = $out } }
        $lines = @(Get-Content $outFile | ForEach-Object { $_.TrimEnd() } |
                Where-Object { $_ -and $_ -notmatch '^\s*#line' })
        return @{ Code = 0; Lines = $lines }
    }

    function Show-Detail([string]$perm) {
        Ensure-BaseTree
        if ($PreprocessOnly) {
            $rb = PreprocessLines $script:baseFile $script:baseInclude $perm (Join-Path $work "dbase.i")
            $rw = PreprocessLines $shaderFull $IncludeDir $perm (Join-Path $work "dwork.i")
            if ($rb.Code -ne 0 -or $rw.Code -ne 0) { return }
            $d = Compare-Object $rb.Lines $rw.Lines
        } else {
            $baseAsm = Join-Path $work "dbase.asm"; $workAsm = Join-Path $work "dwork.asm"
            Compile $script:baseFile $script:baseInclude $perm $baseAsm -Asm | Out-Null
            Compile $shaderFull $IncludeDir $perm $workAsm -Asm | Out-Null
            if (-not (Test-Path $baseAsm) -or -not (Test-Path $workAsm)) { return }
            $d = Compare-Object (Get-Content $baseAsm) (Get-Content $workAsm)
        }
        if ($d) {
            $d | Select-Object -First 30 | ForEach-Object {
                $mark = if ($_.SideIndicator -eq '=>') { 'work' } else { 'base' }
                Write-Host ("    [{0}] {1}" -f $mark, $_.InputObject)
            }
            if (@($d).Count -gt 30) { Write-Host ("    ... (+{0} more lines)" -f (@($d).Count - 30)) }
        }
    }

    Write-Host "Shader   : $relPath"
    Write-Host "Base ref : $BaseRef ($($baseSha.Substring(0,12)))  cached=$($Permutations.Count - $uncached.Count)/$($Permutations.Count)"
    Write-Host "Profile  : $Profile  (entry $Entry)  mode=$(if ($PreprocessOnly) { 'preprocess (Tier 0)' } else { 'DXBC (Tier 1)' })  jobs=$Jobs"
    Write-Host "Include  : $($IncludeDir.Count) roots (package/Shaders + features/*/Shaders)"
    Write-Host ("-" * 60)

    $pool = [runspacefactory]::CreateRunspacePool(1, $Jobs)
    $pool.Open()
    $tasks = New-Object System.Collections.Generic.List[object]
    $idx = 0
    foreach ($perm in $Permutations) {
        $pair = @{ Perm = $perm }
        if (-not $cache.ContainsKey($perm)) {
            $ps = [powershell]::Create()
            $ps.RunspacePool = $pool
            [void]$ps.AddScript($workerScript)
            [void]$ps.AddArgument($fxcPath); [void]$ps.AddArgument($mode); [void]$ps.AddArgument($Profile); [void]$ps.AddArgument($Entry)
            [void]$ps.AddArgument($perm); [void]$ps.AddArgument($script:baseInclude); [void]$ps.AddArgument($script:baseFile)
            [void]$ps.AddArgument((Join-Path $work "b$idx.tmp"))
            $pair.BasePs = $ps
            $pair.BaseHandle = $ps.BeginInvoke()
        }
        $ps2 = [powershell]::Create()
        $ps2.RunspacePool = $pool
        [void]$ps2.AddScript($workerScript)
        [void]$ps2.AddArgument($fxcPath); [void]$ps2.AddArgument($mode); [void]$ps2.AddArgument($Profile); [void]$ps2.AddArgument($Entry)
        [void]$ps2.AddArgument($perm); [void]$ps2.AddArgument($IncludeDir); [void]$ps2.AddArgument($shaderFull)
        [void]$ps2.AddArgument((Join-Path $work "w$idx.tmp"))
        $pair.WorkPs = $ps2
        $pair.WorkHandle = $ps2.BeginInvoke()
        $tasks.Add($pair)
        $idx++
    }

    $allIdentical = $true
    $anyError = $false
    $detailShown = 0
    $stopped = $false
    $newBaseHashes = $false

    foreach ($t in $tasks) {
        if ($stopped) {
            # FailFast already tripped: cancel outstanding compiles.
            if ($t.BasePs) { $t.BasePs.Stop(); $t.BasePs.Dispose() }
            $t.WorkPs.Stop(); $t.WorkPs.Dispose()
            continue
        }
        $perm = $t.Perm
        $baseHash = $null
        $baseErr = $null
        if ($t.BasePs) {
            $rb = $t.BasePs.EndInvoke($t.BaseHandle)[0]
            $t.BasePs.Dispose()
            if ($rb.Ok) { $baseHash = $rb.Hash; $cache[$perm] = $rb.Hash; $newBaseHashes = $true }
            else { $baseErr = $rb.Err }
        } else {
            $baseHash = $cache[$perm]
        }
        $rw = $t.WorkPs.EndInvoke($t.WorkHandle)[0]
        $t.WorkPs.Dispose()

        if ($baseErr -or -not $rw.Ok) {
            $anyError = $true
            $which = if ($baseErr) { "BASE" } else { "WORK" }
            Write-Host "[$perm] $(if ($PreprocessOnly) { 'PREPROCESS' } else { 'COMPILE' })-ERROR ($which)" -ForegroundColor Red
            (($(if ($baseErr) { $baseErr } else { $rw.Err }) -split "`r?`n") | Where-Object { $_ -match 'error|warning' } | Select-Object -First 6) |
                ForEach-Object { Write-Host "    $_" }
            if ($FailFast) { $script:failedPerm = $perm; $stopped = $true }
            continue
        }

        if ($baseHash -eq $rw.Hash) {
            Write-Host "[$perm] IDENTICAL" -ForegroundColor Green
        } else {
            $allIdentical = $false
            Write-Host "[$perm] DIFFERS  base=$($baseHash.Substring(0,12)) work=$($rw.Hash.Substring(0,12))" -ForegroundColor Yellow
            if ($detailShown -lt 3) { Show-Detail $perm; $detailShown++ }
            if ($FailFast) { $script:failedPerm = $perm; $stopped = $true }
        }
    }

    if ($cacheFile -and $newBaseHashes) {
        $cache | ConvertTo-Json -Compress | Out-File -Encoding utf8 $cacheFile
    }

    Write-Host ("-" * 60)
    if ($script:failedPerm -and $FailFast) {
        Write-Host "FAILFAST: stopped at first failure. Bisect probe:" -ForegroundColor Yellow
        Write-Host ("  -Permutations `"{0}`"" -f $script:failedPerm)
    }
    if ($anyError) { Write-Host "RESULT: compile error" -ForegroundColor Red; $exit = 1 }
    elseif ($allIdentical) { Write-Host "RESULT: behavior-preserving (all permutations identical)" -ForegroundColor Green; $exit = 0 }
    else { Write-Host "RESULT: bytecode differs - inspect diff above" -ForegroundColor Yellow; $exit = 2 }

    exit $exit
}
finally {
    if ($pool) { $pool.Close(); $pool.Dispose() }
    # Runs on normal exit and on throw, so the temp dir never leaks.
    if ($work -and (Test-Path $work)) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
    Pop-Location
}

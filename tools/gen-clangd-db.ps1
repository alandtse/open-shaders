#requires -Version 5.1
<#
.SYNOPSIS
  Generate a clangd compile_commands.json for this checkout.

.DESCRIPTION
  The shipped CMake presets use the Visual Studio generator, which does NOT emit
  compile_commands.json (only Ninja/Makefile generators do). Without a real
  compile DB, clangd parses every TU with the hand-maintained fallback flags in
  .clangd and floods the editor with false "unknown include / undefined symbol"
  diagnostics. This script stands up a throwaway Ninja configure (no build) purely
  to emit an accurate DB, then drops it at the repo root where clangd
  auto-discovers it.

  It reuses the vcpkg packages already installed by the -ReuseVcpkgFrom build dir
  (default build/ALL), so it does not re-run a vcpkg install. Configure that
  preset once before running this.

  MAX_PATH note: long checkout paths blow MAX_PATH for the FidelityFX/vcpkg
  permutation headers, so some setups build under a `subst` drive. If a subst
  drive maps to this checkout, the script builds under it and rewrites the
  emitted DB paths back to the real checkout path (clangd opens files via the
  real path, so the DB must match). With no subst drive it builds in-place.

  Re-run after any change that alters the include graph or defines (new source
  file, new feature dir, CMake option flip).

.EXAMPLE
  pwsh tools/gen-clangd-db.ps1
#>
[CmdletBinding()]
param(
    # Build dir whose installed vcpkg packages are reused (so this does not
    # re-run a full vcpkg install). Configure this preset first.
    [string]$ReuseVcpkgFrom = 'build/ALL'
)
$ErrorActionPreference = 'Stop'

$root = (& git -C $PSScriptRoot rev-parse --show-toplevel).Trim()
if (-not $root) { throw 'Not inside a git checkout' }
$rootFwd = ($root -replace '\\', '/').TrimEnd('/')

# Build under a subst drive if one maps to this checkout (MAX_PATH workaround);
# otherwise build in-place at the repo root.
$drive = $null
foreach ($line in (subst)) {
    # `subst` prints "Z:\: => C:\path"; tolerate the trailing colon and an
    # optional backslash so the mapped-drive lookup actually matches.
    if ($line -match '^([A-Z]):\\?:?\s*=>\s*(.+)$') {
        if ($matches[2].TrimEnd('\') -ieq $root.TrimEnd('\')) { $drive = "$($matches[1]):"; break }
    }
}
$base = if ($drive) { $drive } else { $rootFwd }
Write-Host "Checkout : $root"
Write-Host "Build at : $base$(if ($drive) { '  (subst drive; paths rewritten back)' })"

# Discover the MSVC dev environment. Ninja needs cl on PATH; the VS generator
# sets this up implicitly, Ninja does not. -prerelease covers Insiders installs
# and is harmless on stable-only machines.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found at $vswhere" }
$vsRoot = (& $vswhere -latest -prerelease -property installationPath | Select-Object -First 1).Trim()
if (-not $vsRoot) { throw 'vswhere found no Visual Studio install (need the Desktop development with C++ workload).' }
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found under $vsRoot" }
cmd /c "`"$vcvars`" && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) { throw 'cl not on PATH after vcvars import' }

# Read the vcpkg toolchain + triplet from the existing cache so we match the real
# build instead of hardcoding paths.
$cache = Join-Path $root "$ReuseVcpkgFrom/CMakeCache.txt"
if (-not (Test-Path $cache)) { throw "No CMakeCache at $cache - configure the $ReuseVcpkgFrom preset first" }
$cacheText = Get-Content $cache -Raw
$toolchain = ([regex]'CMAKE_TOOLCHAIN_FILE(?::\w+)?=(.+)').Match($cacheText).Groups[1].Value.Trim()
$triplet   = ([regex]'VCPKG_TARGET_TRIPLET(?::\w+)?=(.+)').Match($cacheText).Groups[1].Value.Trim()
if (-not $toolchain) { throw "Could not read CMAKE_TOOLCHAIN_FILE from $cache" }
if (-not $triplet)   { $triplet = 'x64-windows-static-md' }

$genDir   = "$base/build/clangd-gen"
$vcpkgDir = "$base/$ReuseVcpkgFrom/vcpkg_installed"

# Configure-only (no build) under Ninja to emit the DB. /machine:x64 satisfies
# FidelityFX-SDK's platform check, which keys off the VS -A arch that Ninja
# doesn't set. Flags mirror the ALL preset (all three runtimes).
$cmakeArgs = @(
    '-S', "$base/", '-B', $genDir, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release',
    '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
    '-DCMAKE_EXE_LINKER_FLAGS=/machine:x64',
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
    "-DVCPKG_OVERLAY_PORTS=$base/cmake/ports/",
    "-DVCPKG_TARGET_TRIPLET=$triplet",
    "-DVCPKG_INSTALLED_DIR=$vcpkgDir",
    '-DVCPKG_MANIFEST_INSTALL=OFF',
    '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL',
    '-DSKSE_SUPPORT_XBYAK=ON',
    '-DENABLE_SKYRIM_AE=ON', '-DENABLE_SKYRIM_SE=ON', '-DENABLE_SKYRIM_VR=ON',
    '-DAUTO_PLUGIN_DEPLOYMENT=OFF',
    '-DCOMMONLIB_PREBUILT_MULTICONFIG=ON',
    '-DBUILD_CPP_TESTS=OFF'
)
Write-Host 'Configuring Ninja DB (no build)...'
# Let cmake's diagnostics reach the console and check its native exit code —
# $ErrorActionPreference='Stop' does not trip on a non-zero exe exit, so a
# silent configure failure would otherwise surface only as the generic
# "did not produce compile_commands.json" below.
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed (exit code $LASTEXITCODE)." }
$dbSrc = Join-Path $root 'build/clangd-gen/compile_commands.json'
if (-not (Test-Path $dbSrc)) { throw 'Configure did not produce compile_commands.json' }

# Drop the DB at repo root. When built under a subst drive, rewrite that drive
# root back to the real checkout path so clangd's file lookups match.
$text = [System.IO.File]::ReadAllText($dbSrc)
if ($drive) { $text = $text.Replace($drive, $rootFwd) }
$dest = Join-Path $root 'compile_commands.json'
[System.IO.File]::WriteAllText($dest, $text, (New-Object System.Text.UTF8Encoding($false)))
$count = ([regex]'"file":').Matches($text).Count
Write-Host "Wrote $dest ($count entries). Restart clangd to pick it up."

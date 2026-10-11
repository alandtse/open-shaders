<#
.SYNOPSIS
Loads a built CommunityShaders.dll into a stand-in host repeatedly, without launching the game.

.DESCRIPTION
A DLL whose static initialisers fault is reported by the loader as error 1114. On VR those
initialisers parse the Address Library CSV, so a toolchain or ABI mistake there can fail a fraction of
loads. This builds a small host (version resource 1.4.15.0 for VR or 1.6.1170.0 for SE), loads the DLL
in a fresh process -Runs times, and exits non-zero if any load fails.

Run from an x64 Native Tools prompt (cl.exe and rc.exe on PATH). -GameRoot must hold the Address
Library for the runtime (Data\SKSE\Plugins\version-1-4-15-0.csv for VR, versionlib-*.bin for SE).

.EXAMPLE
pwsh tools/dll-load-smoke/run.ps1 -Dll build\ALL-ClangCL\CommunityShaders.dll -Runtime VR -GameRoot 'E:\SteamLibrary\steamapps\common\SkyrimVR'
#>
param(
	[Parameter(Mandatory)][string]$Dll,
	[Parameter(Mandatory)][string]$GameRoot,
	[ValidateSet('VR', 'SE')][string]$Runtime = 'VR',
	[int]$Runs = 30,
	[int]$PadMB = 80
)

$ErrorActionPreference = 'Stop'
foreach ($tool in 'cl.exe', 'rc.exe') {
	if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool not found; run from an x64 Native Tools prompt." }
}
$Dll = (Resolve-Path $Dll).Path
$GameRoot = (Resolve-Path $GameRoot).Path
$version = if ($Runtime -eq 'VR') { '1.4.15.0' } else { '1.6.1170.0' }

$work = Join-Path ([IO.Path]::GetTempPath()) "dll-load-smoke-$Runtime"
New-Item -ItemType Directory -Force $work | Out-Null
$commas = $version -replace '\.', ','
@"
1 VERSIONINFO
FILEVERSION $commas
PRODUCTVERSION $commas
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904b0"
    BEGIN
      VALUE "FileVersion", "$version"
      VALUE "ProductVersion", "$version"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
"@ | Set-Content (Join-Path $work 'host.rc') -Encoding ascii

Push-Location $work
try {
	& rc.exe /nologo host.rc | Out-Null
	$pad = '0x{0:X}' -f ($PadMB * 1MB)
	& cl.exe /nologo /EHsc /DPAD_BYTES=$pad /Fe:host.exe "$PSScriptRoot\host.cpp" host.res /link /SUBSYSTEM:CONSOLE /SECTION:.pad,ERW | Out-Null
	if (-not (Test-Path host.exe)) { throw 'host build failed' }

	$loaded = 0
	for ($i = 1; $i -le $Runs; ++$i) {
		$out = Join-Path $work "run$i.txt"
		$p = Start-Process (Join-Path $work 'host.exe') -ArgumentList "`"$Dll`"", "`"$GameRoot`"" -RedirectStandardOutput $out -PassThru -WindowStyle Hidden
		if (-not $p.WaitForExit(60000)) { $p.Kill() }
		if ((Get-Content $out -ErrorAction SilentlyContinue) -match '^LOADED') { ++$loaded }
	}
	$failed = $Runs - $loaded
	"{0}: loaded {1}/{2}, failed {3}" -f $Runtime, $loaded, $Runs, $failed
	if ($failed -gt 0) { exit 1 }
}
finally {
	Pop-Location
}

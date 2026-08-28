[CmdletBinding()]
param(
    [ValidateSet('Auto', 'Cpp', 'Shaders', 'Full')]
    [string]$Mode = 'Auto',
    [switch]$Reconfigure,
    [switch]$LaunchMO2
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = $PSScriptRoot
$buildDirectory = Join-Path $repositoryRoot 'build\OpenShaders-MO2-DevFast'
$presetFile = Join-Path $repositoryRoot 'CMakeUserPresets.json'

function Stop-Build([string]$Message) {
    throw "Build/deploy stopped: $Message"
}

function Invoke-CMake([string]$Arguments, [string]$Description) {
    Write-Host "`n== $Description ==" -ForegroundColor Cyan

    $vsWhere = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe')
    ) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1

    $vcVars = $null
    if (!(Get-Command cl.exe -ErrorAction SilentlyContinue) -and $vsWhere) {
        $vsInstall = & $vsWhere -latest -prerelease -products * -property installationPath | Select-Object -First 1
        if ($vsInstall) {
            $vsInstall = $vsInstall.Trim()
            $vcVars = Join-Path $vsInstall 'VC\Auxiliary\Build\vcvars64.bat'
        }
    }

    if (!(Get-Command cl.exe -ErrorAction SilentlyContinue) -and (!$vcVars -or !(Test-Path -LiteralPath $vcVars))) {
        Stop-Build 'MSVC was not found. Install Visual Studio C++ tools or run from a VS x64 developer prompt.'
    }

    Push-Location $repositoryRoot
    try {
        if ($vcVars) {
            $command = 'call "{0}" >nul 2>&1 && cmake {1}' -f $vcVars, $Arguments
            & cmd.exe /d /s /c $command
        } else {
            & cmd.exe /d /s /c "cmake $Arguments"
        }
        $exitCode = $LASTEXITCODE
    } finally {
        Pop-Location
    }

    if ($exitCode -ne 0) {
        Stop-Build "$Description failed with exit code $exitCode."
    }
}

function Get-BranchModName([string]$Branch) {
    $words = $Branch -split '[-_]' | Where-Object { $_ }
    $displayName = ($words | ForEach-Object {
        if ($_.Length -eq 1) { $_.ToUpperInvariant() }
        else { $_.Substring(0, 1).ToUpperInvariant() + $_.Substring(1).ToLowerInvariant() }
    }) -join ' '
    return "Open Shaders - $displayName Dev"
}

function Close-GameBeforeDeploy {
    $gameProcessNames = @('SkyrimSE', 'SkyrimVR', 'skse64_loader', 'sksevr_loader')
    $runningGames = @(Get-Process -Name $gameProcessNames -ErrorAction SilentlyContinue | Sort-Object Id -Unique)
    if ($runningGames.Count -eq 0) {
        return
    }

    Write-Warning 'Build finished. Closing Skyrim before deployment.'
    foreach ($gameProcess in $runningGames) {
        try {
            if ($gameProcess.MainWindowHandle -ne 0) {
                $gameProcess.CloseMainWindow() | Out-Null
            }
        } catch {
        }
    }

    $deadline = (Get-Date).AddSeconds(5)
    do {
        Start-Sleep -Milliseconds 250
        $runningGames = @(Get-Process -Name $gameProcessNames -ErrorAction SilentlyContinue | Sort-Object Id -Unique)
    } while ($runningGames.Count -gt 0 -and (Get-Date) -lt $deadline)

    foreach ($gameProcess in $runningGames) {
        try {
            Stop-Process -Id $gameProcess.Id -Force
        } catch {
        }
    }

    $deadline = (Get-Date).AddSeconds(5)
    do {
        Start-Sleep -Milliseconds 250
        $runningGames = @(Get-Process -Name $gameProcessNames -ErrorAction SilentlyContinue | Sort-Object Id -Unique)
    } while ($runningGames.Count -gt 0 -and (Get-Date) -lt $deadline)

    if ($runningGames.Count -gt 0) {
        Stop-Build 'Skyrim is still running after the close attempt; deployment was not started.'
    }
}

if (!(Test-Path -LiteralPath $presetFile)) {
    Stop-Build 'CMakeUserPresets.json is missing. Restore the local OpenShaders-MO2 preset first.'
}

$branch = (& git -C $repositoryRoot branch --show-current).Trim()
if (!$branch) {
    Stop-Build 'The checkout is detached or the current branch could not be determined.'
}

$expectedModName = Get-BranchModName $branch
$presets = Get-Content -LiteralPath $presetFile -Raw | ConvertFrom-Json
$mo2Preset = $presets.configurePresets | Where-Object { $_.name -eq 'OpenShaders-MO2' } | Select-Object -First 1
if (!$mo2Preset -or !$mo2Preset.environment.CommunityShadersOutputDir) {
    Stop-Build 'The OpenShaders-MO2 preset has no CommunityShadersOutputDir deployment list.'
}

$modRoots = @($mo2Preset.environment.CommunityShadersOutputDir -split ';' | Where-Object { $_ })
if ($modRoots.Count -ne 2) {
    Stop-Build "Expected exactly two branch-specific MO2 destinations, found $($modRoots.Count)."
}

foreach ($modRoot in $modRoots) {
    if (!(Test-Path -LiteralPath $modRoot)) {
        Stop-Build "Deployment directory does not exist: $modRoot"
    }

    $modName = Split-Path -Leaf $modRoot.TrimEnd('\', '/')
    if ($modName -ne $expectedModName) {
        Stop-Build "Preset points at '$modName' while branch '$branch' requires '$expectedModName'."
    }
}

$cacheFile = Join-Path $buildDirectory 'CMakeCache.txt'
$presetStamp = Join-Path $buildDirectory '.OpenShaders-MO2.preset.sha256'
$presetHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $presetFile).Hash
$needsConfigure = $Reconfigure -or !(Test-Path -LiteralPath $cacheFile) -or !(Test-Path -LiteralPath $presetStamp)
if (!$needsConfigure) {
    $needsConfigure = (Get-Content -LiteralPath $presetStamp -Raw).Trim() -ne $presetHash
}

if ($needsConfigure) {
    Invoke-CMake '-S . --preset OpenShaders-MO2' 'Configure OpenShaders-MO2'
    New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
    Set-Content -LiteralPath $presetStamp -Value $presetHash -NoNewline
}

function Deploy-Plugin {
    $sourceFiles = @('CommunityShaders.dll', 'CommunityShaders.pdb') | ForEach-Object {
        Join-Path $buildDirectory $_
    }

    foreach ($sourceFile in $sourceFiles) {
        if (!(Test-Path -LiteralPath $sourceFile)) {
            Stop-Build "Expected build output is missing: $sourceFile"
        }
    }

    foreach ($modRoot in $modRoots) {
        $pluginDirectory = Join-Path $modRoot 'SKSE\Plugins'
        if (!(Test-Path -LiteralPath $pluginDirectory)) {
            Stop-Build "Plugin directory does not exist: $pluginDirectory"
        }

        foreach ($sourceFile in $sourceFiles) {
            Copy-Item -LiteralPath $sourceFile -Destination (Join-Path $pluginDirectory (Split-Path -Leaf $sourceFile)) -Force
        }
    }

    foreach ($sourceFile in $sourceFiles) {
        $sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $sourceFile).Hash
        foreach ($modRoot in $modRoots) {
            $deployedFile = Join-Path (Join-Path $modRoot 'SKSE\Plugins') (Split-Path -Leaf $sourceFile)
            $deployedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $deployedFile).Hash
            if ($deployedHash -ne $sourceHash) {
                Stop-Build "Hash mismatch after deployment: $deployedFile"
            }
        }
    }

    Write-Host 'DLL/PDB deployment verified in both branch mods.' -ForegroundColor Green
}

switch ($Mode) {
    'Auto' {
        Invoke-CMake '--build --preset OpenShaders-MO2 --parallel' 'Incremental C++ build'
        Close-GameBeforeDeploy
        Deploy-Plugin
        Invoke-CMake "--build `"$buildDirectory`" --target COPY_SHADERS --parallel" 'Incremental shader deployment'
    }
    'Cpp' {
        Invoke-CMake '--build --preset OpenShaders-MO2 --parallel' 'Incremental C++ build'
        Close-GameBeforeDeploy
        Deploy-Plugin
    }
    'Shaders' {
        Close-GameBeforeDeploy
        Invoke-CMake "--build `"$buildDirectory`" --target COPY_SHADERS --parallel" 'Incremental shader deployment'
    }
    'Full' {
        Close-GameBeforeDeploy
        Invoke-CMake '--build --preset OpenShaders-MO2-Full --parallel' 'Full build and deployment'
        Deploy-Plugin
    }
}

if ($LaunchMO2) {
    $instanceRoot = Split-Path -Parent (Split-Path -Parent $modRoots[0])
    $mo2Executable = Join-Path $instanceRoot 'ModOrganizer.exe'
    if (!(Test-Path -LiteralPath $mo2Executable)) {
        Write-Warning "ModOrganizer.exe was not found at $mo2Executable; deployment is complete."
    } else {
        Start-Process -FilePath $mo2Executable
    }
}

Write-Host "`nFinished: $Mode build/deploy for branch '$branch'." -ForegroundColor Green

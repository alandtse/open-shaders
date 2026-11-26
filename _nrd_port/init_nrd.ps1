$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dest = Join-Path $root 'extern/NRD'
$pin = '66e616c29e28fdb777e5304b9dc35ccb184fa473'
function GitChecked { & git @args; if ($LASTEXITCODE -ne 0) { throw "git failed: $args" } }
if (-not (Test-Path -LiteralPath $dest)) {
    GitChecked clone --no-checkout https://github.com/NVIDIA-RTX/NRD.git $dest
} elseif (-not (Test-Path -LiteralPath (Join-Path $dest '.git'))) {
    throw 'extern/NRD exists but is not a Git checkout. Move it aside before continuing.'
} else {
    $dirty = & git -C $dest status --porcelain
    if ($LASTEXITCODE -ne 0 -or $dirty) { throw 'NRD checkout is not clean.' }
}
GitChecked -C $dest fetch origin $pin
GitChecked -C $dest checkout --detach $pin
GitChecked -C $dest submodule update --init --recursive
$head = & git -C $dest rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $head -ne $pin) { throw 'Unexpected NRD revision.' }
Write-Host "NRD ready: $head"

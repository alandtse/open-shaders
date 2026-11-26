$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'manifest.json') -Raw | ConvertFrom-Json
foreach ($rel in $manifest.removed) {
    $p = Join-Path $root $rel
    if (Test-Path -LiteralPath $p -PathType Leaf) {
        $expected = $manifest.base_sha256.PSObject.Properties[$rel].Value
        $actual = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $expected) { throw "Refusing to remove changed file: $rel" }
    }
}
foreach ($rel in $manifest.removed) {
    $p = Join-Path $root $rel
    if (Test-Path -LiteralPath $p -PathType Leaf) { Remove-Item -LiteralPath $p; Write-Host "Removed $rel" }
}

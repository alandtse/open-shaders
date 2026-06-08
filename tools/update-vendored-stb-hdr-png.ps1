#requires -Version 5.1
<#
.SYNOPSIS
  Refresh the vendored stb_image_write_hdr_png.h from upstream ReShade at a pinned ref.

.DESCRIPTION
  Downloads deps/stb_image/stb_image_write_hdr_png.h from crosire/reshade at -Ref,
  overwrites the vendored copy, updates the pinned commit in PROVENANCE.md, and prints
  the diff. The header is kept byte-identical to upstream (no local edits), so an update
  is a clean drop-in; review the diff, rebuild, and re-run the HDR-screenshot smoke.

.EXAMPLE
  pwsh tools/update-vendored-stb-hdr-png.ps1 -Ref main
  pwsh tools/update-vendored-stb-hdr-png.ps1 -Ref 34e108a6b4828073271773a35fdb2f3bfa31af2a
#>
param(
    [Parameter(Mandatory = $true)][string]$Ref
)
$ErrorActionPreference = 'Stop'

$repo = 'crosire/reshade'
$path = 'deps/stb_image/stb_image_write_hdr_png.h'
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'extern/stb_image_write_hdr_png/stb_image_write_hdr_png.h'
$prov = Join-Path $root 'extern/stb_image_write_hdr_png/PROVENANCE.md'

# Resolve the ref to a concrete commit SHA for a stable pin.
$sha = (gh api "repos/$repo/commits/$Ref" --jq '.sha').Trim()
if (-not $sha) { throw "Could not resolve ref '$Ref' in $repo" }

Write-Host "Fetching $path @ $sha ..."
# Write the decoded base64 as raw bytes so the vendored file is byte-identical
# to upstream. Set-Content -Encoding utf8 would prepend a UTF-8 BOM under Windows
# PowerShell 5.1 (and could rewrite line endings), breaking the byte-for-byte
# provenance guarantee.
$b64 = (gh api "repos/$repo/contents/$($path)?ref=$sha" --jq '.content') -replace '\s', ''
if (-not $b64) { throw "Empty content returned for $path @ $sha" }
[System.IO.File]::WriteAllBytes($dest, [Convert]::FromBase64String($b64))

# Update the pinned commit line in PROVENANCE.md. Write without a BOM (same
# reason as the header above: Set-Content -Encoding utf8 prepends a UTF-8 BOM
# under Windows PowerShell 5.1).
$provText = (Get-Content $prov -Raw) -replace '(\*\*Pinned commit:\*\* `)[0-9a-f]{7,40}(`)', "`${1}$sha`${2}"
[System.IO.File]::WriteAllText($prov, $provText, (New-Object System.Text.UTF8Encoding($false)))

Write-Host "Updated vendored header and pinned commit -> $sha"
Write-Host "--- git diff (review, rebuild, re-run HDR-screenshot smoke) ---"
git -C $root diff -- $dest

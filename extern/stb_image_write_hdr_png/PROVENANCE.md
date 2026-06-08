# stb_image_write_hdr_png

16-bit HDR PNG writer (cICP/sBIT/cHRM/iCCP), stb-style single header. Extends
`stb_image_write.h` (provided by vcpkg `stb`) — define `STB_IMAGE_WRITE_IMPLEMENTATION`
in exactly one TU that includes `stb_image_write.h` then this header.

- **Upstream:** ReShade — https://github.com/crosire/reshade
- **Path:** `deps/stb_image/stb_image_write_hdr_png.h`
- **Pinned commit:** `34e108a6b4828073271773a35fdb2f3bfa31af2a`
- **License:** Public Domain (Unlicense)

## Vendoring policy

`stb_image_write_hdr_png.h` is vendored **verbatim** (byte-identical to upstream). Do
**not** edit it — all integration lives in our own glue (`src/...` impl TU + the
ScreenshotFeature encode path). Keeping it pristine means absorbing an upstream
improvement is a clean drop-in (no patch to re-apply).

If a local change ever proves unavoidable, keep it as a separate `.patch` re-applied by
the update script (`git apply --3way`), never inline edits — but prefer doing the work in
our glue instead.

## Updating

```
pwsh tools/update-vendored-stb-hdr-png.ps1 -Ref <reshade-commit-or-branch>
```

Re-downloads the header at the given ref, updates the pinned commit above, and reports the
diff for review. Replaces this file in place; rebuild and re-run the HDR-screenshot smoke.

# taa-renderdoc-ab.py — Tier-3 runtime A/B check for TAA shader refactors.
#
# Purpose: prove a behavior-preserving claim that bytecode-identity CANNOT (Standard B,
# where fxc legitimately emits different-but-equivalent code). It swaps the ISTemporalAA
# pixel shader on a SINGLE captured frame and diffs the output render target. Because the
# inputs (history t1, velocity t2, depth t3, mask t4, alpha t5, cbuffer b2) are frozen from
# that one frame, A (shipping shader) and B (candidate) run on byte-identical inputs — so a
# near-zero output diff means equivalent behavior on a real frame, free of the cross-launch
# and temporal-warmup noise that a two-launch screenshot A/B suffers.
#
# HOW TO RUN: this is executed *inside RenderDoc's embedded Python* via the renderdoc MCP
# `Eval` tool (the global `ctx` HandlerContext must be in scope). It is NOT a standalone
# script. See docs/development/taa-runtime-ab.md for the full operator runbook.
#
# Candidate B (and a baseline A') are supplied as pre-compiled DXBC built with fxc using the
# SAME defines/permutation as the captured build (e.g. PSHADER VR [HDR_OUTPUT]) and with
# /I package/Shaders so includes resolve — identical to tools/verify-shader-refactor.ps1.
# Feeding DXBC (ShaderEncoding.DXBC) avoids RenderDoc's HLSL include/define handling.

import renderdoc as rd
import struct

# eid -> (origPS ResourceId, replacement ResourceId). Persists across Eval calls so restore()
# uses the real objects instead of round-tripping through strings.
_replacements = {}


def _walk(actions):
    for a in actions:
        yield a
        for c in _walk(a.children):
            yield c


def _desc_res(d):
    for attr in ("resource", "resourceId"):
        if hasattr(d, attr):
            return getattr(d, attr)
    return rd.ResourceId.Null


def _as_resid(x):
    # GetShader returns a ResourceId on current RenderDoc; guard older (id, reflection) tuples.
    return x[0] if isinstance(x, tuple) else x


def taa_candidates():
    """List draws that look like ISTemporalAA: 2 colour outputs (Color+Feedback) and
    >=5 pixel-stage SRVs (t0..t5). Operator confirms the eventId before swapping."""
    def work(ctrl):
        sdfile = ctrl.GetStructuredFile()
        res = []
        for a in _walk(ctrl.GetRootActions()):
            if not (a.flags & rd.ActionFlags.Drawcall):
                continue
            ctrl.SetFrameEvent(a.eventId, True)
            ps = ctrl.GetPipelineState()
            outs = [o for o in ps.GetOutputTargets() if _desc_res(o) != rd.ResourceId.Null]
            srvs = ps.GetReadOnlyResources(rd.ShaderStage.Pixel)
            nsrv = sum(1 for s in srvs
                       if _desc_res(s.descriptor if hasattr(s, "descriptor") else s) != rd.ResourceId.Null)
            if len(outs) == 2 and nsrv >= 5:
                res.append({"eventId": a.eventId, "name": a.GetName(sdfile),
                            "outputs": len(outs), "srvs": nsrv})
        return res
    return ctx.replay(work)


def _tex_desc(ctrl, rid):
    for t in ctrl.GetTextures():
        if t.resourceId == rid:
            return t
    return None


def grab_rt(eid, target_index=0):
    """Return (resourceId_str, raw_bytes, (width, height, format_name)) for an output RT."""
    def work(ctrl):
        ctrl.SetFrameEvent(eid, True)
        rid = _desc_res(ctrl.GetPipelineState().GetOutputTargets()[target_index])
        data = bytes(ctrl.GetTextureData(rid, rd.Subresource(0, 0, 0)))
        td = _tex_desc(ctrl, rid)
        meta = (td.width, td.height, str(td.format.Name())) if td else (0, 0, "?")
        return (str(rid), data, meta)
    return ctx.replay(work)


def replace_ps_with_dxbc(eid, dxbc_path, entry="main"):
    """Build a replacement PS from a pre-compiled DXBC file and bind it at the event.
    Returns {ok, errors}. On success the (orig, new) ResourceIds are stored for restore(eid)."""
    with open(dxbc_path, "rb") as f:
        blob = f.read()

    def work(ctrl):
        ctrl.SetFrameEvent(eid, True)
        orig = _as_resid(ctrl.GetPipelineState().GetShader(rd.ShaderStage.Pixel))
        newid, errs = ctrl.BuildTargetShader(entry, rd.ShaderEncoding.DXBC, blob,
                                             rd.ShaderCompileFlags(), rd.ShaderStage.Pixel)
        ok = newid != rd.ResourceId.Null
        if ok:
            ctrl.ReplaceResource(orig, newid)
            ctrl.SetFrameEvent(eid, True)  # re-replay with replacement bound
        return ok, orig, newid, str(errs)

    ok, orig, newid, errs = ctx.replay(work)
    if ok:
        _replacements[eid] = (orig, newid)
    return {"ok": ok, "errors": errs}


def restore(eid):
    """Undo a replacement made at eid, using the stored ResourceIds."""
    pair = _replacements.pop(eid, None)
    if not pair:
        return False
    orig, newid = pair

    def work(ctrl):
        ctrl.RemoveReplacement(orig)
        try:
            ctrl.FreeTargetResource(newid)
        except Exception:
            pass
        ctrl.SetFrameEvent(eid, True)
        return True
    return ctx.replay(work)


def _diff(a, b, meta):
    """Byte-identity fast path + a SAMPLED float-magnitude estimate (no numpy in RenderDoc,
    and full-res TAA RTs are tens of MB — never materialize the whole thing)."""
    out = {"size_a": len(a), "size_b": len(b), "format": meta[2], "dims": [meta[0], meta[1]]}
    if a == b:  # C-level compare — instant
        out["verdict"] = "IDENTICAL"
        out["bytes_differing"] = 0
        return out
    if len(a) != len(b):
        out["verdict"] = "DIFFERS"
        out["note"] = "size mismatch"
        return out

    fmt = meta[2].lower()
    if "16_float" in fmt:
        code, esz = "<e", 2
    elif "32_float" in fmt:
        code, esz = "<f", 4
    else:
        code, esz = None, 1  # 8-bit UNORM fallback

    n = len(a)
    total = n // esz
    stride = max(1, total // 100000)  # cap ~100k samples across the whole image
    maxabs = 0.0
    sse = 0.0
    cnt = 0
    ndiff = 0
    for i in range(0, total, stride):
        off = i * esz
        if code:
            x = struct.unpack_from(code, a, off)[0]
            y = struct.unpack_from(code, b, off)[0]
            dlt = abs(x - y)
        else:
            dlt = abs(a[off] - b[off]) / 255.0
        if dlt > 0:
            ndiff += 1
        if dlt > maxabs:
            maxabs = dlt
        sse += dlt
        cnt += 1

    out["sampled_elems"] = cnt
    out["sample_frac_differing"] = (ndiff / cnt) if cnt else 0.0
    out["max_abs"] = maxabs
    out["mean_abs"] = (sse / cnt) if cnt else 0.0
    # Tiny residue from FP reassociation is OK; a structured artifact is not.
    out["verdict"] = "EQUIVALENT" if maxabs < 1e-3 else "DIFFERS"
    return out


def ab(eid, candidate_dxbc, baseline_dxbc=None, entry="main"):
    """Full A/B on one event.
    - Captures the live output (A_real).
    - If baseline_dxbc given: swap it in and confirm it matches A_real (validates defines/
      permutation + the DXBC-replace path) before trusting the candidate result.
    - Swaps candidate_dxbc and diffs against A_real. Restores after each swap.
    A failed build is reported as BUILD-FAILED — never a false EQUIVALENT (which would happen
    if we diffed with the original shader still bound)."""
    rid, a_real, meta = grab_rt(eid)
    report = {"eventId": eid, "rt": rid, "rt_meta": {"dims": [meta[0], meta[1]], "format": meta[2]}}

    if baseline_dxbc:
        r = replace_ps_with_dxbc(eid, baseline_dxbc, entry)
        if not r["ok"]:
            report["baseline_vs_live"] = {"verdict": "BUILD-FAILED", "errors": r["errors"]}
        else:
            _, a_prime, _ = grab_rt(eid)
            report["baseline_vs_live"] = _diff(a_real, a_prime, meta)
            restore(eid)

    r = replace_ps_with_dxbc(eid, candidate_dxbc, entry)
    if not r["ok"]:
        report["candidate_vs_live"] = {"verdict": "BUILD-FAILED", "errors": r["errors"]}
        return report
    _, b, _ = grab_rt(eid)
    report["candidate_vs_live"] = _diff(a_real, b, meta)
    restore(eid)
    return report

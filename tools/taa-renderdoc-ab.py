# taa-renderdoc-ab.py — Tier-3 runtime A/B check for TAA shader refactors.
#
# Purpose: prove a behavior-preserving claim that bytecode-identity CANNOT (Standard B,
# where fxc legitimately emits different-but-equivalent code). It swaps the ISTemporalAA
# pixel shader on a SINGLE captured frame and diffs the output render target. Because the
# inputs (history t1, velocity t2, depth t3, mask t4, alpha t5, cbuffer b2) are frozen from
# that one frame, A (shipping shader) and B (candidate) run on byte-identical inputs — so a
# near-zero output diff means equivalent behavior on a real frame, free of the cross-launch
# and temporal-warmup noise that a two-launch screenshot A/B suffers from.
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


def _walk(actions):
    for a in actions:
        yield a
        for c in _walk(a.children):
            yield c


def _desc_res(d):
    # Descriptor.resource across RenderDoc versions.
    for attr in ("resource", "resourceId"):
        if hasattr(d, attr):
            return getattr(d, attr)
    return rd.ResourceId.Null


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
            nsrv = sum(1 for s in srvs if _desc_res(s.descriptor if hasattr(s, "descriptor") else s) != rd.ResourceId.Null)
            if len(outs) == 2 and nsrv >= 5:
                res.append({
                    "eventId": a.eventId,
                    "name": a.GetName(sdfile),
                    "outputs": len(outs),
                    "srvs": nsrv,
                })
        return res
    return ctx.replay(work)


def _tex_desc(ctrl, rid):
    for t in ctrl.GetTextures():
        if t.resourceId == rid:
            return t
    return None


def grab_rt(eid, target_index=0):
    """Return (resourceId, raw_bytes, (width,height,format_name)) for an output RT at an event."""
    def work(ctrl):
        ctrl.SetFrameEvent(eid, True)
        ps = ctrl.GetPipelineState()
        outs = ps.GetOutputTargets()
        rid = _desc_res(outs[target_index])
        sub = rd.Subresource(0, 0, 0)
        data = bytes(ctrl.GetTextureData(rid, sub))
        td = _tex_desc(ctrl, rid)
        meta = (td.width, td.height, str(td.format.Name())) if td else (0, 0, "?")
        return (str(rid), data, meta)
    return ctx.replay(work)


def replace_ps_with_dxbc(eid, dxbc_path, entry="main"):
    """Build a replacement PS from a pre-compiled DXBC file and bind it at the event.
    Returns (origPSId, newShaderId, errors). Keep origPSId to restore()."""
    with open(dxbc_path, "rb") as f:
        blob = f.read()

    def work(ctrl):
        ctrl.SetFrameEvent(eid, True)
        ps = ctrl.GetPipelineState()
        orig = ps.GetShader(rd.ShaderStage.Pixel)
        flags = rd.ShaderCompileFlags()
        newid, errs = ctrl.BuildTargetShader(entry, rd.ShaderEncoding.DXBC, blob, flags, rd.ShaderStage.Pixel)
        if newid != rd.ResourceId.Null:
            ctrl.ReplaceResource(orig, newid)
            ctrl.SetFrameEvent(eid, True)  # re-replay with replacement bound
        return (str(orig), str(newid), str(errs))
    return ctx.replay(work)


def restore(orig_ps_id_str):
    """Undo a replacement. Pass the origPSId string returned by replace_ps_with_dxbc.
    (ResourceId can't round-trip from str, so we re-resolve by matching the current PS.)"""
    def work(ctrl):
        # RemoveReplacement needs the original ResourceId; re-resolve from the live shader list.
        for rid in ctrl.GetResources():
            if str(rid.resourceId) == orig_ps_id_str:
                ctrl.RemoveReplacement(rid.resourceId)
                return True
        return False
    return ctx.replay(work)


def _diff(a, b, meta):
    """Byte diff (primary gate) plus float magnitude when the RT is a known float format."""
    n = min(len(a), len(b))
    nbytes = sum(1 for i in range(n) if a[i] != b[i]) + abs(len(a) - len(b))
    out = {"size_a": len(a), "size_b": len(b), "bytes_differing": nbytes,
           "format": meta[2], "dims": [meta[0], meta[1]]}
    if nbytes == 0:
        out["verdict"] = "IDENTICAL"
        return out
    fmt = meta[2].lower()
    maxabs = meanabs = None
    try:
        if "16_float" in fmt or "16f" in fmt:  # RGBA16F etc.
            va = struct.unpack("<%de" % (n // 2), a[:n // 2 * 2])
            vb = struct.unpack("<%de" % (n // 2), b[:n // 2 * 2])
            d = [abs(x - y) for x, y in zip(va, vb)]
            maxabs, meanabs = max(d), sum(d) / len(d)
        elif "32_float" in fmt or "32f" in fmt:
            va = struct.unpack("<%df" % (n // 4), a[:n // 4 * 4])
            vb = struct.unpack("<%df" % (n // 4), b[:n // 4 * 4])
            d = [abs(x - y) for x, y in zip(va, vb)]
            maxabs, meanabs = max(d), sum(d) / len(d)
        else:  # 8-bit UNORM fallback
            d = [abs(a[i] - b[i]) / 255.0 for i in range(n)]
            maxabs, meanabs = max(d), sum(d) / len(d)
    except Exception as e:
        out["decode_error"] = str(e)
    out["max_abs"] = maxabs
    out["mean_abs"] = meanabs
    # Tolerance: tiny residue from FP reassociation is OK; structured artifact is not.
    out["verdict"] = "EQUIVALENT" if (maxabs is not None and maxabs < 1e-3) else "DIFFERS"
    return out


def ab(eid, candidate_dxbc, baseline_dxbc=None, entry="main"):
    """Full A/B on one event.
    - Captures the live output (A_real).
    - If baseline_dxbc given: swap it in and confirm it matches A_real (validates defines/
      permutation + the DXBC-replace path) before trusting the candidate result.
    - Swaps candidate_dxbc and diffs against A_real. Restores afterward.
    Returns a dict report."""
    rid, a_real, meta = grab_rt(eid)
    report = {"eventId": eid, "rt": rid, "rt_meta": {"dims": [meta[0], meta[1]], "format": meta[2]}}

    if baseline_dxbc:
        orig, newid, errs = replace_ps_with_dxbc(eid, baseline_dxbc, entry)
        if newid == "ResourceId::0" or "0" == newid:
            report["baseline_build_error"] = errs
        _, a_prime, _ = grab_rt(eid)
        report["baseline_vs_live"] = _diff(a_real, a_prime, meta)
        restore(orig)

    orig, newid, errs = replace_ps_with_dxbc(eid, candidate_dxbc, entry)
    report["candidate_build_errors"] = errs if errs and errs != "" else None
    _, b, _ = grab_rt(eid)
    report["candidate_vs_live"] = _diff(a_real, b, meta)
    restore(orig)
    return report

"""Helpers for the Nexus changelog post in .github/workflows/nexus-upload.yaml.

The Nexus "save documentation" endpoint that BUTR.NexusUploader (`unex
changelog`) posts to *appends* the supplied text to the existing changelog
rather than replacing it. Re-running the upload for a version already on Nexus
therefore appends that version's notes a second time — the "doubled up" entries
reported in issue #198.

nexus_versions() returns the MAIN-category file versions on a mod (the single
shared Nexus file-list query used by the dry-run check and the upload
summary). strip_audit() drops the internal Feature Version Audit block from
the release body before it is posted.
"""

from __future__ import annotations

import json
import re
import urllib.error
import urllib.request

# Trailing "# Feature Version Audit" block appended to the release body is
# internal bookkeeping, not user-facing changelog content. GitHub's release
# body API returns CRLF line endings, and the surrounding blank-line run can
# be more than one line long, so each newline must be wrapped as a whole
# (?:\r?\n) repeated unit -- a bare \r?\n+ only repeats the \n, which can't
# bridge multiple full \r\n pairs.
_AUDIT_RE = re.compile(r"(?:\r?\n)+---(?:\r?\n)+# Feature Version Audit\b.*", re.DOTALL)


def strip_audit(body: str) -> str:
    """Drop the internal Feature Version Audit section from a release body."""
    return _AUDIT_RE.sub("", body or "").strip()


def nexus_versions(
    api_key: str,
    game_id: str,
    mod_id: str,
    user_agent: str = "open-shaders-ci/1.0",
    timeout: int = 15,
) -> list[str] | None:
    """Return the mod's MAIN-category file versions (first-seen order), or None.

    None means the query could not be completed (missing key/mod id, network or
    auth error) — callers treat that as "don't suppress / status unknown".

    MAIN-only on purpose: this workflow also uploads prebuilt shader caches as
    OPTIONAL files, so matching every file would let an OPTIONAL file sharing the
    release version read as the MAIN file already being on Nexus.
    """
    if not api_key or not mod_id:
        return None
    url = f"https://api.nexusmods.com/v1/games/{game_id}/mods/{mod_id}/files.json"
    req = urllib.request.Request(
        url,
        headers={
            "apikey": api_key,
            "User-Agent": user_agent,
            "Accept": "application/json",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            files = json.load(resp).get("files", [])
    except (urllib.error.URLError, TimeoutError, ValueError, OSError):
        return None
    # Strictly MAIN-only (no fall back to all files): for a mod with no MAIN
    # file yet, an OPTIONAL upload sharing the version must not read as present.
    check = [f for f in files if f.get("category_name") == "MAIN"]
    seen: list[str] = []
    for f in check:
        v = f.get("version", "")
        if v and v not in seen:
            seen.append(v)
    return seen

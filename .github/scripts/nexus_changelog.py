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


# Nexus's changelog field renders neither Markdown nor BBCode, so a release
# body must be reduced to plain text before posting there (see markdown_to_plain_text).
_MD_HEADING_RE = re.compile(r"^#{1,6}[ \t]+", re.MULTILINE)
# semantic-release wraps each PR/commit link in its own parens, e.g. "([#678](url))" --
# the (?(1)...) conditional consumes that enclosing "(...)" with the link so it isn't
# doubled up with the "label (url)" parens the substitution below adds.
_MD_LINK_RE = re.compile(r"(\()?\[([^\]]*)\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)(?(1)\))")
_MD_IMAGE_RE = re.compile(r"!\[([^\]]*)\]\([^)]*\)")
_MD_ASTERISK_EMPHASIS_RE = re.compile(r"(\*{1,3})(\S(?:.*?\S)?)\1")
# GFM only lets "_" open/close emphasis outside a word (unlike "*"), so
# "foo_bar_baz" must stay literal while "__init__" between spaces converts.
_MD_UNDERSCORE_EMPHASIS_RE = re.compile(r"(?<!\w)(_{1,3})(\S(?:.*?\S)?)\1(?!\w)")
_MD_STRIKETHROUGH_RE = re.compile(r"~~(.*?)~~")
_MD_INLINE_CODE_RE = re.compile(r"`([^`]*)`")
_MD_CODE_FENCE_RE = re.compile(r"^[ \t]*```[^\n]*\n(.*?)^[ \t]*```[ \t]*$", re.MULTILINE | re.DOTALL)
_MD_BLOCKQUOTE_RE = re.compile(r"^>[ \t]?", re.MULTILINE)
_MD_BULLET_RE = re.compile(r"^(\s*)[*+-][ \t]+", re.MULTILINE)
_MD_HR_RE = re.compile(r"^[ \t]*(?:-{3,}|\*{3,}|_{3,})[ \t]*$", re.MULTILINE)
# A link label matching the id already in its URL (issue "#678", commit hash) is
# pure repetition, so it's dropped in favor of the bare URL below.
_ISSUE_LABEL_RE = re.compile(r"^#(\d+)$")
_HEX_LABEL_RE = re.compile(r"^[0-9a-fA-F]{6,40}$")


def _link_label_is_redundant_with_url(label: str, url: str) -> bool:
    issue_match = _ISSUE_LABEL_RE.match(label)
    if issue_match:
        return url.rstrip("/").endswith("/" + issue_match.group(1))
    if _HEX_LABEL_RE.match(label):
        return label.lower() in url.lower()
    return False


def _replace_link(match: re.Match[str]) -> str:
    label, url = match.group(2), match.group(3)
    if not label or _link_label_is_redundant_with_url(label, url):
        return url
    return f"{label} ({url})"


def markdown_to_plain_text(body: str) -> str:
    """Reduce a GitHub-Markdown release body to plain text for Nexus.

    Strips formatting markers (headings, bold/italic, blockquotes, horizontal
    rules) and unwraps links/images to "label (url)" -- or the bare URL when
    the label is empty or just repeats an id already in the URL (issue/PR
    number, commit hash) -- while preserving line breaks and list structure
    so the changelog stays readable without any rendering. Fenced and inline
    code content is restored verbatim, untouched by any of those passes.
    """
    text = body or ""

    # Code content must not be exposed to the heading/HR/emphasis passes below
    # (e.g. a fenced "# define FEATURE" or "---" line, or an inline
    # `snake_case_name`), so it's swapped for placeholders and restored as-is
    # at the end.
    fenced_blocks: list[str] = []

    def _stash_fence(match: re.Match[str]) -> str:
        fenced_blocks.append(match.group(1))
        return f"\x00FENCE{len(fenced_blocks) - 1}\x00"

    text = _MD_CODE_FENCE_RE.sub(_stash_fence, text)

    inline_code: list[str] = []

    def _stash_inline_code(match: re.Match[str]) -> str:
        inline_code.append(match.group(1))
        return f"\x00CODE{len(inline_code) - 1}\x00"

    text = _MD_INLINE_CODE_RE.sub(_stash_inline_code, text)

    text = _MD_HR_RE.sub("", text)
    text = _MD_IMAGE_RE.sub(lambda m: m.group(1), text)
    text = _MD_LINK_RE.sub(_replace_link, text)
    text = _MD_HEADING_RE.sub("", text)
    text = _MD_BLOCKQUOTE_RE.sub("", text)
    text = _MD_BULLET_RE.sub(lambda m: f"{m.group(1)}- ", text)
    text = _MD_STRIKETHROUGH_RE.sub(lambda m: m.group(1), text)
    # Bold/italic markers can nest (e.g. "**_text_**"), so repeat until stable.
    previous = None
    while previous != text:
        previous = text
        text = _MD_ASTERISK_EMPHASIS_RE.sub(lambda m: m.group(2), text)
        text = _MD_UNDERSCORE_EMPHASIS_RE.sub(lambda m: m.group(2), text)

    for i, code in enumerate(inline_code):
        text = text.replace(f"\x00CODE{i}\x00", code)
    for i, block in enumerate(fenced_blocks):
        text = text.replace(f"\x00FENCE{i}\x00", block)

    text = re.sub(r"[ \t]+\n", "\n", text)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text.strip()


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

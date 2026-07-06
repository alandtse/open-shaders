'use strict';

const fs = require('fs');

// semantic-release generateNotes plugin: appends the Feature Metadata
// Summary that release-semantic.yaml's "Apply feature version bumps" step
// writes to AUDIT_FILE (via `feature_version_audit.py --apply-bumps
// --output`) to the release notes @semantic-release/release-notes-generator
// already produced. Multiple generateNotes plugins concatenate their output
// in plugins-array order (semantic-release core behavior), so this must be
// listed after release-notes-generator in .releaserc.js.
//
// This replaces release-build.yaml's old post-publish approach, which
// re-ran the audit script against the already-cut, immutable release tag
// and spliced the result into the published body via text surgery (find
// "# Feature Version Audit", strip to the preceding separator, append
// fresh). That meant a tool fix landing after a tag was cut could never
// affect that release's audit table, and re-running the release build
// re-derived the same stale content. Generating it here, in the same job
// and ref as the version bumps it reports on, removes that class of bug.
const AUDIT_FILE = 'feature-audit-notes.md';

module.exports = {
  generateNotes: async () => {
    try {
      const content = fs.readFileSync(AUDIT_FILE, 'utf8').trim();
      return content ? `\n\n---\n\n${content}` : '';
    } catch (err) {
      if (err.code === 'ENOENT') {
        // skip_feature_bumps was set, or the step failed independently of
        // this plugin; don't block the release over a missing audit.
        return '';
      }
      throw err;
    }
  },
};

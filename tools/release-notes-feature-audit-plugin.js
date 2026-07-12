'use strict';

const fs = require('fs');

// Appends AUDIT_FILE's content to the release notes; must be listed after
// release-notes-generator since generateNotes outputs concatenate in order.
const AUDIT_FILE = 'feature-audit-notes.md';

module.exports = {
  generateNotes: async () => {
    try {
      const content = fs.readFileSync(AUDIT_FILE, 'utf8').trim();
      return content ? `\n\n---\n\n${content}` : '';
    } catch (err) {
      // skip_feature_bumps was set; don't block the release over this.
      if (err.code === 'ENOENT') return '';
      throw err;
    }
  },
};

'use strict';

const { execFileSync } = require('child_process');

// Field/record separators that won't appear in real commit text. Not NUL
// (\x00): that can't be passed as a process argument at all -- the OS spawn
// call rejects it since C strings are NUL-terminated.
const FIELD_SEP = '\x1f'; // ASCII unit separator
const RECORD_SEP = '\x1e'; // ASCII record separator

// semantic-release's getCommits() has no --first-parent option, so a merge's
// non-first-parent history can corrupt the changelog/version-bump; recomputed
// here instead of via @semantic-release/commit-analyzer or
// release-notes-generator, which aren't resolvable from this checkout in CI.

function getFirstParentCommits(cwd, from, to) {
  const range = from ? `${from}..${to}` : to;
  const format = `${RECORD_SEP}%H${FIELD_SEP}%h${FIELD_SEP}%B${FIELD_SEP}%ci`;
  const raw = execFileSync('git', ['log', '--first-parent', range, `--format=${format}`], {
    cwd,
    maxBuffer: 1024 * 1024 * 256,
    encoding: 'utf8',
  });
  return raw
    .split(RECORD_SEP)
    .filter((record) => record.length > 0)
    .map((record) => {
      const [hash, shortHash, message, committerDateStr] = record.split(FIELD_SEP);
      return {
        hash,
        shortHash,
        message: (message || '').trim(),
        committerDate: new Date(committerDateStr),
      };
    });
}

function getFirstParentContextCommits(context) {
  const { cwd, lastRelease, nextRelease, commits, logger } = context;
  const from = lastRelease && lastRelease.gitHead;
  const to = (nextRelease && nextRelease.gitHead) || 'HEAD';
  const firstParentCommits = getFirstParentCommits(cwd, from, to);
  if (firstParentCommits.length !== commits.length) {
    logger.log(
      '[first-parent] %d commits via full history, %d via --first-parent; using --first-parent (a prior merge landed non-first-parent ancestry, e.g. an upstream history rewrite)',
      commits.length,
      firstParentCommits.length
    );
  }
  return firstParentCommits;
}

// Angular-preset-compatible header parse: `type(scope)!: subject`, with the
// rest of the raw message as body/footer.
const HEADER_RE = /^(\w+)(?:\(([^)]*)\))?(!)?:\s*(.*)$/;
const BREAKING_FOOTER_RE = /^BREAKING[ -]CHANGE:\s*([\s\S]*)/m;

function parseCommit(commit) {
  const lines = commit.message.split('\n');
  const header = lines[0] || '';
  const match = HEADER_RE.exec(header);
  const breakingFooterMatch = BREAKING_FOOTER_RE.exec(commit.message);
  return {
    ...commit,
    type: match ? match[1] : null,
    scope: match ? match[2] || null : null,
    breaking: Boolean((match && match[3]) || breakingFooterMatch),
    breakingBody: breakingFooterMatch ? breakingFooterMatch[1].trim() : null,
    subject: match ? match[4] : header,
  };
}

const RELEASE_ORDER = ['patch', 'minor', 'major'];

// Only the two matcher shapes .releaserc.js actually passes: { breaking, release }
// and { type, release }.
function matchCustomRule(rules, commit) {
  if (!rules) return undefined;
  for (const rule of rules) {
    if (rule.breaking !== undefined && Boolean(rule.breaking) === commit.breaking) return rule.release;
    if (rule.type !== undefined && rule.type === commit.type) return rule.release;
  }
  return undefined;
}

function defaultReleaseType(commit) {
  if (commit.breaking) return 'major';
  if (commit.type === 'feat') return 'minor';
  if (commit.type === 'fix' || commit.type === 'perf') return 'patch';
  return null;
}

function analyzeCommits(pluginConfig, context) {
  const { logger } = context;
  const commits = getFirstParentContextCommits(context).map(parseCommit);
  let releaseType = null;
  for (const commit of commits) {
    const ruleMatch = matchCustomRule(pluginConfig && pluginConfig.releaseRules, commit);
    const commitReleaseType = ruleMatch !== undefined ? ruleMatch : defaultReleaseType(commit);
    if (commitReleaseType) {
      logger.log('The release type for commit %s is %s', commit.shortHash, commitReleaseType);
      if (RELEASE_ORDER.indexOf(commitReleaseType) > RELEASE_ORDER.indexOf(releaseType)) {
        releaseType = commitReleaseType;
      }
    }
  }
  logger.log('Analysis of %d first-parent commits complete: %s release', commits.length, releaseType || 'no');
  return releaseType;
}

const NOTE_SECTIONS = [
  { type: 'feat', title: 'Features' },
  { type: 'fix', title: 'Bug Fixes' },
  { type: 'perf', title: 'Performance Improvements' },
  { type: 'revert', title: 'Reverts' },
];

function repoInfoFromUrl(repositoryUrl) {
  const cleaned = (repositoryUrl || '').replace(/\.git$/i, '');
  const match = /github\.com[/:]([^/]+)\/(.+)$/.exec(cleaned);
  return match ? { owner: match[1], repo: match[2] } : null;
}

function formatCommitLine(commit, repoInfo) {
  const scopePrefix = commit.scope ? `**${commit.scope}:** ` : '';
  let subject = commit.subject;
  const links = [];
  if (repoInfo) {
    const issueMatch = /\(#(\d+)\)\s*$/.exec(subject);
    if (issueMatch) {
      subject = subject.slice(0, issueMatch.index).trimEnd();
      links.push(`[#${issueMatch[1]}](https://github.com/${repoInfo.owner}/${repoInfo.repo}/issues/${issueMatch[1]})`);
    }
    links.push(`[${commit.shortHash}](https://github.com/${repoInfo.owner}/${repoInfo.repo}/commit/${commit.hash})`);
  } else {
    links.push(commit.shortHash);
  }
  return `* ${scopePrefix}${subject} (${links.join(') (')})`;
}

function generateNotes(pluginConfig, context) {
  const { lastRelease, nextRelease, options } = context;
  const commits = getFirstParentContextCommits(context).map(parseCommit);
  const repoInfo = repoInfoFromUrl(options && options.repositoryUrl);

  const parts = [];
  const previousTag = lastRelease && (lastRelease.gitTag || lastRelease.gitHead);
  const currentTag = nextRelease && (nextRelease.gitTag || nextRelease.gitHead);
  const date = new Date().toISOString().slice(0, 10);
  const versionHeading = `[${nextRelease.version}]`;
  const versionLink =
    repoInfo && previousTag && currentTag
      ? `[${nextRelease.version}](https://github.com/${repoInfo.owner}/${repoInfo.repo}/compare/${previousTag}...${currentTag})`
      : versionHeading;
  parts.push(`# ${versionLink} (${date})`);

  const breaking = commits.filter((c) => c.breaking && c.breakingBody);
  if (breaking.length > 0) {
    parts.push(
      `### ⚠ BREAKING CHANGES\n\n${breaking.map((c) => `* ${c.breakingBody}`).join('\n\n')}`
    );
  }

  for (const section of NOTE_SECTIONS) {
    const sectionCommits = commits.filter((c) => c.type === section.type);
    if (sectionCommits.length === 0) continue;
    parts.push(`### ${section.title}\n\n${sectionCommits.map((c) => formatCommitLine(c, repoInfo)).join('\n')}`);
  }

  return parts.join('\n\n');
}

module.exports = {
  analyzeCommits: async (pluginConfig, context) => analyzeCommits(pluginConfig, context),
  generateNotes: async (pluginConfig, context) => generateNotes(pluginConfig, context),
};

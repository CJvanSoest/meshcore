# Releases

The fixed format for every release of MeshCore for Tanmatsu, so release entries
and notes always read the same way. This is the single source for how a release
looks; [CHANGELOG.md](CHANGELOG.md) is where the entries live. Follow this
exactly rather than inventing per release wording.

## Versioning

Semantic Versioning: `MAJOR.MINOR.PATCH`, tagged `vMAJOR.MINOR.PATCH`.

- **MAJOR** — a change that breaks interop or forces user action: a wire format
  change, an NVS key migration that drops old data, a required radio firmware
  version, or a removed feature.
- **MINOR** — a new feature or a meaningful behaviour change that stays
  backward compatible.
- **PATCH** — bug fixes, robustness, docs, and internal work with no user
  visible feature change.

When in doubt between two levels, pick the higher one. The version is read at
runtime from the build description (the git tag), not from a define in code.

## The CHANGELOG entry (the canonical format)

Every release is one section in `CHANGELOG.md`, newest first, in the Keep a
Changelog shape. Work in progress accumulates under `## [Unreleased]`; cutting a
release renames that heading to the version and date.

```markdown
## [X.Y.Z] - YYYY-MM-DD

### Added
- One feature per bullet, phrased as what the user gains.

### Changed
- One change per bullet, phrased as what is now different.

### Fixed
- One fix per bullet: the symptom, then the cause in a clause.

### Removed
- What is gone and what replaces it (or why it is gone).
```

Section order is fixed: **Added, Changed, Deprecated, Removed, Fixed, Security.**
Omit a section that has no entries for the release. Never reorder them and never
invent new section names.

## How an entry is written

Consistency is in the phrasing, not just the structure. Every bullet follows the
same rules:

- **English, plain, present tense for the result.** "Adds a slippy map view",
  "Fixes the duty cycle race", not past tense narration or marketing copy.
- **User facing first.** Lead with what the user sees or gains; put the
  mechanism in a trailing clause only when it helps. A reader scanning the
  release should understand the value without reading the code.
- **One change per bullet.** Bold the feature name or the subsystem at the start
  when it helps scanning: `**Map style profiles** — ...`.
- **Name the cause for fixes.** A Fixed bullet states the symptom, then the
  cause: "Direct adverts were silently rejected because the signer produced
  wrong points for the RFC 8032 vector; ...".
- **No prose hyphens between words.** Write "host tested", "wire format", "user
  facing", not the hyphenated forms. Keep hyphens only in identifiers, file
  names, CLI flags and brand names (`ESP-IDF`, `tanmatsu-lora`, `--volumes-from`).
- **No AI attribution** anywhere in an entry, a tag message, or a release note.
- **Reference a PR or issue** in parentheses when one exists: `(PR #14)`,
  `(GitHub issue #1)`.

Keep bullets short. If a change needs a paragraph to explain, the paragraph
belongs in `docs/`, and the bullet links to it.

## Cutting a release

1. Move the accumulated `## [Unreleased]` content to a new
   `## [X.Y.Z] - YYYY-MM-DD` heading, then leave a fresh empty `## [Unreleased]`
   above it.
2. Make sure the green gate passes on the release commit (host tests, the four
   lints, and `make build DEVICE=tanmatsu`). A release is never cut from red.
3. Tag the commit `vX.Y.Z`.
4. The GitHub or Gitea release uses the version as the title and the CHANGELOG
   section body verbatim as the notes. The release notes are exactly the
   CHANGELOG entry, never a separately worded summary, so the two never drift.

## Release note skeleton (copy this)

```
Title:  vX.Y.Z

<the CHANGELOG [X.Y.Z] section, verbatim>
```

That is the whole format. A release note is the CHANGELOG entry, nothing more
and nothing differently worded.

## See also

- [CHANGELOG.md](CHANGELOG.md) — the entries themselves
- [Blueprint.md](Blueprint.md) — how to program in this project
- [CONTRIBUTING.md](CONTRIBUTING.md) — the contributor checklist

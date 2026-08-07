#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 CJ van Soest
# SPDX-License-Identifier: MIT
#
# Enforce the mechanical half of docs/guides/Releases.md against docs/CHANGELOG.md:
# heading shape, allowed section names, fixed section order and descending
# versions. Whether a release is MAJOR or MINOR is a judgement call and stays
# with the author.
#
# Exit 0 when the changelog is well formed, 1 on any violation.

set -u
cd "$(dirname "$0")/../.." || exit 2

python3 - "docs/CHANGELOG.md" <<'PY'
import re, sys

path = sys.argv[1]
text = open(path).read()
ORDER = ["Added", "Changed", "Deprecated", "Removed", "Fixed", "Security"]
fail = []

blocks = re.split(r"^## ", text, flags=re.M)[1:]
if not blocks:
    fail.append("no '## ' release sections found")

seen, versions = set(), []
for block in blocks:
    head = block.split("\n", 1)[0].strip()
    if head == "[Unreleased]":
        ver = None
    else:
        m = re.fullmatch(r"\[(\d+\.\d+\.\d+)\] - (\d{4}-\d{2}-\d{2})", head)
        if not m:
            fail.append(f"heading is not '[X.Y.Z] - YYYY-MM-DD': '## {head}'")
            continue
        ver = m.group(1)
        if ver in seen:
            fail.append(f"[{ver}] appears more than once")
        seen.add(ver)
        versions.append(ver)

    names = re.findall(r"^### (.+)$", block, flags=re.M)
    for n in names:
        if n not in ORDER:
            fail.append(f"[{ver or 'Unreleased'}]: unknown section '### {n}'")
    known = [n for n in names if n in ORDER]
    if known != sorted(known, key=ORDER.index):
        fail.append(f"[{ver or 'Unreleased'}]: sections out of order: {', '.join(known)}")
    if len(set(known)) != len(known):
        fail.append(f"[{ver or 'Unreleased'}]: duplicate section heading")

as_tuple = lambda v: tuple(int(p) for p in v.split("."))
for a, b in zip(versions, versions[1:]):
    if as_tuple(a) > as_tuple(b):
        continue
    fail.append(f"[{a}] is listed above [{b}] but does not sort above it")

if fail:
    for f in fail:
        print(f"changelog: {f}")
    print(">> VIOLATION: docs/CHANGELOG.md does not follow docs/guides/Releases.md")
    sys.exit(1)
print(f"changelog: OK ({len(versions)} releases, sections ordered)")
PY

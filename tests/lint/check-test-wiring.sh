#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 CJ van Soest
# SPDX-License-Identifier: MIT
# SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>
#
# Catch orphan host tests: a tests/test_*.c that is never named as a target in
# tests/Makefile compiles for nobody and silently never runs in CI, so a
# regression it was meant to guard slips through green. This lint fails when any
# tracked test source is missing from the Makefile. Run from anywhere:
#   tests/lint/check-test-wiring.sh
# Exit 0 when every test is wired, 1 on any orphan (prints the offenders).

set -u
cd "$(dirname "$0")/../.." || exit 2

fail=0
for f in $(git ls-files 'tests/test_*.c'); do
    base=$(basename "$f" .c)
    # A wired test appears as the "<name>:" rule head in tests/Makefile.
    grep -qE "^${base}:" tests/Makefile || {
        echo ">> ORPHAN: $f has no target in tests/Makefile"
        fail=1
    }
done

if [ "$fail" -eq 0 ]; then
    echo "test-wiring: OK (every tests/test_*.c has a Makefile target)"
fi
exit "$fail"

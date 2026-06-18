#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 CJ van Soest
# SPDX-License-Identifier: MIT
#
# Static-analysis gate (the "ruff for C"). Runs cppcheck over FIRST-PARTY
# sources only and fails on any warning / performance / portability finding.
#
# Deliberately NOT linted: components/vendor (third-party, kept verbatim) and
# the upstream protocol mirror under components/mc_proto/meshcore and
# components/mc_proto/companion-radio-protocol (tracked upstream, see
# ARCHITECTURE.md "Wire-boundary discipline").
#
# Severity choice: warning + performance + portability catch real issues; the
# "style" category is left off so the gate flags defects, not bikeshedding.
#
# Run locally:  tests/check-cppcheck.sh   (needs: cppcheck)

set -u
cd "$(dirname "$0")/.." || exit 2

if ! command -v cppcheck >/dev/null 2>&1; then
    echo "cppcheck not installed (apt-get install -y cppcheck)"
    exit 2
fi

cppcheck \
    --enable=warning,performance,portability \
    --error-exitcode=1 \
    --inline-suppr \
    --std=c11 -q \
    --suppress=missingInclude \
    --suppress=missingIncludeSystem \
    --suppress=unknownMacro \
    -I main -I components/mc_proto \
    main/*.c \
    components/mc_proto/region_limits.c \
    components/mc_proto/gps_parser.c
rc=$?

[ "$rc" -eq 0 ] && echo "cppcheck: clean (first-party)"
exit "$rc"

#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 CJ van Soest
# SPDX-License-Identifier: MIT
# SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>
#
# Structural hygiene checks that keep the component layout from rotting back
# into a fat main/ or a cluttered root. Complements check-arch-rules.sh (which
# guards include direction); this one guards file placement. Documented in
# docs/Architecture.md "Structure rules". Run from anywhere:
#   tests/lint/check-structure.sh
# Exit 0 when clean, 1 on any violation.

set -u
cd "$(dirname "$0")/../.." || exit 2

fail=0
violation() { echo ">> VIOLATION: $1"; fail=1; }

# 1. main/ stays a thin entry point — only main.c plus the build files. This is
#    the anti-regression guard: new first-party code belongs in a component, not
#    back in main/.
extra=$(ls main/ 2>/dev/null | grep -vxE 'main\.c|CMakeLists\.txt|idf_component\.yml' || true)
if [ -n "$extra" ]; then
    violation "main/ must contain only main.c + build files; found:"
    echo "$extra" | sed 's/^/     /'
fi

# 2. Every component under components/ registers itself with a CMakeLists.txt.
for d in components/*/; do
    [ -f "${d}CMakeLists.txt" ] || violation "component ${d} has no CMakeLists.txt"
done

# 3. First-party C source carries an SPDX header. Vendored libraries and the
#    upstream-mirrored companion parser keep their own upstream notices, so they
#    are exempt.
exempt='^components/vendor/|^components/mc_proto/companion-radio-protocol/'
while IFS= read -r f; do
    case "$f" in
        components/*|main/*|tests/*) ;;
        *) continue ;;
    esac
    echo "$f" | grep -qE "$exempt" && continue
    grep -qE 'SPDX-License-Identifier' "$f" || violation "missing SPDX header: $f"
done < <(git ls-files '*.c' '*.h')

# 4. The repository root stays on a known allowlist — keep new clutter out of the
#    top level. Docs go in docs/, scripts in scripts/, source in components/.
allow='AUTHORS|CLAUDE.md|CMakeLists.txt|LICENSE|Makefile|README.md|esp_idf_project_configuration.json|.clang-format|.clangd|.gitignore|.ci-build.sh'
allow_dir='assets|components|docs|main|partition_tables|scripts|sdkconfigs|tests|.claude|.git|.github|.vscode.template'
while IFS= read -r top; do
    echo "$top" | grep -qxE "$allow|$allow_dir" || violation "unexpected root entry: $top (put it under docs/, scripts/, or a component)"
done < <(git ls-files | sed 's#/.*##' | sort -u)

if [ "$fail" -eq 0 ]; then
    echo "structure: OK (main/ thin, components registered, SPDX present, root clean)"
fi
exit "$fail"

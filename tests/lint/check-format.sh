#!/usr/bin/env bash
# clang-format gate. Canonical version: clang-format 18.1.8 — the tree was
# normalized with it and the output is not stable across major versions, so a
# different version may report false diffs. CI pins 18.1.8; locally, override
# the binary with CLANG_FORMAT=/path/to/clang-format if your default differs.
#
# Vendored drops (components/vendor) and the upstream protocol mirror
# (components/mc_proto/meshcore) are excluded: they are not ours to reformat.
set -euo pipefail
cd "$(dirname "$0")/../.."

cf="${CLANG_FORMAT:-clang-format}"
if ! command -v "$cf" >/dev/null 2>&1; then
    echo "format: clang-format not found (canonical version: 18.1.8)"
    exit 1
fi

ver=$("$cf" --version | grep -oE '[0-9]+(\.[0-9]+)+' | head -1)
major=${ver%%.*}
if [ "$major" != "18" ]; then
    echo "format: warning — clang-format $ver found, tree was normalized with 18.1.8; results may differ"
fi

mapfile -t files < <(git ls-files -- components main tests |
    grep -E '\.(c|h)$' |
    grep -vE '^components/vendor/|^components/mc_proto/meshcore/')

fail=0
for f in "${files[@]}"; do
    if ! "$cf" --dry-run --Werror "$f" >/dev/null 2>&1; then
        echo "format: needs clang-format -i: $f"
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "format: run 'clang-format -i' (v18.1.8) on the files above"
    exit 1
fi
echo "format: OK (${#files[@]} files, clang-format $ver)"

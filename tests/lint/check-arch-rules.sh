#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 CJ van Soest
# SPDX-License-Identifier: MIT
#
# Enforce the forbidden-include rules from docs/Architecture.md. These were always
# meant to migrate from a manual grep into CI ("grep-checkable, intended to
# migrate into CI when the workflow grows a lint job"). This is that job.
#
# Exit 0 when the layering holds, 1 on any violation (prints the offending
# lines). Run from anywhere:  tests/lint/check-arch-rules.sh

set -u
cd "$(dirname "$0")/../.." || exit 2

fail=0
check() {
    local desc="$1"; shift
    if grep -rEn "$@" 2>/dev/null; then
        echo ">> VIOLATION: $desc"
        fail=1
    fi
}

# 1. UI must not speak the wire protocol. Whole component, not render_*.c: most
#    of the UI moved into lvgl_*.c and a file glob silently stops covering it.
check "mc_ui includes meshcore/" \
    '^#include "meshcore/' components/mc_ui/

# 2. The protocol mirror stays pure (no UI, BSP or L1 data headers).
#    meshcore/ now lives in the mc_proto component.
check "meshcore/ includes UI/BSP/L1 headers" \
    '^#include "(lvgl|lv_|bsp/|chat|nodes|channels|contacts|settings_nvs|render)' \
    components/mc_proto/meshcore/

# 3. Data and protocol layers do not drive UI. Whole components, so a new file
#    under any of them is covered without editing this list.
check "L0-L3 includes render.h, input.h or lvgl_ui.h" \
    '^#include "(render|input|lvgl_ui|lvgl_port)\.h"' \
    components/mc_proto/ components/mc_radio/ components/mc_domain/ \
    components/mc_io/ components/mc_crypto/ components/mc_rx/

if [ "$fail" -eq 0 ]; then
    echo "arch-rules: OK (no forbidden includes)"
fi
exit "$fail"

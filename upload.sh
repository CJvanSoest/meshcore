#!/usr/bin/env bash
# Upload MeshCore to Tanmatsu via badgelink
# Removes any stale/duplicate slugs before uploading.

set -e

SLUG="nl.cj.meshcore"
TITLE="MeshCore"
VERSION="1"
BIN="build/application.bin"
BADGELINK_DIR="/Users/cjvs/stack/Projects/Tanmatsu/wifi-analyzer/badgelink/tools"
BADGELINK="$BADGELINK_DIR/venv/bin/python $BADGELINK_DIR/badgelink.py"

# Build first if binary is missing
if [ ! -f "$BIN" ]; then
    echo "Binary not found — building first..."
    source /Users/cjvs/esp/esp-idf/export.sh 2>/dev/null
    idf.py build
fi

echo "=== AppFS contents before upload ==="
$BADGELINK appfs list

# Remove any entries that look like old versions of this app
OLD_SLUGS=(
    "nl.cj.meshcore-settings"
    "nl.cj.meshcore"
)
for old in "${OLD_SLUGS[@]}"; do
    if $BADGELINK appfs list 2>/dev/null | grep -q "^$old "; then
        echo "Removing old entry: $old"
        $BADGELINK appfs delete "$old"
    fi
done

echo "=== Uploading $SLUG ($TITLE) ==="
$BADGELINK appfs upload "$SLUG" "$TITLE" "$VERSION" "$BIN"

echo "=== AppFS contents after upload ==="
$BADGELINK appfs list

echo "Done!"

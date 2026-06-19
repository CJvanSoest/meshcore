#!/usr/bin/env bash
# Upload MeshCore to Tanmatsu via badgelink.
#
# Set BADGELINK_DIR to your local badgelink/tools path before running:
#   export BADGELINK_DIR=/path/to/badgelink/tools
#   ./scripts/upload.sh

set -e

SLUG="nl.cj.meshcore"
TITLE="MeshCore"
VERSION="1"
BIN="build/tanmatsu/application.bin"

if [ -z "$BADGELINK_DIR" ]; then
    echo "Error: BADGELINK_DIR is not set."
    echo "Usage: BADGELINK_DIR=/path/to/badgelink/tools ./scripts/upload.sh"
    exit 1
fi

BADGELINK="$BADGELINK_DIR/badgelink.sh"
if [ ! -x "$BADGELINK" ]; then
    echo "Error: badgelink.sh not found or not executable at $BADGELINK"
    exit 1
fi

if [ ! -f "$BIN" ]; then
    echo "Binary not found at $BIN — build the project first."
    exit 1
fi

echo "=== Uploading $SLUG ($TITLE) ==="
"$BADGELINK" appfs upload "$SLUG" "$TITLE" "$VERSION" "$BIN"
echo "Done!"

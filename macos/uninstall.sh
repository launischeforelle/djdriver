#!/bin/sh
# Remove the LaunchAgent and the djdriver binary.
set -eu

LABEL=com.github.djdriver
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"

launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
rm -f "$PLIST"
sudo rm -f /usr/local/bin/djdriver
echo "djdriver removed."

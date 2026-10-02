#!/bin/sh
# Build djdriver and install it as a LaunchAgent that starts at login.
set -eu

cd "$(dirname "$0")/.."

PREFIX=/usr/local
LABEL=com.github.djdriver
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/djdriver.log"

if ! command -v brew >/dev/null 2>&1; then
	echo "Homebrew is required: https://brew.sh" >&2
	exit 1
fi
if ! xcode-select -p >/dev/null 2>&1; then
	echo "Installing the Xcode command line tools, run this script again afterwards."
	xcode-select --install
	exit 1
fi

brew list libusb >/dev/null 2>&1 || brew install libusb
brew list pkg-config >/dev/null 2>&1 || brew list pkgconf >/dev/null 2>&1 || brew install pkg-config

make clean >/dev/null
make
make test

echo "Installing $PREFIX/bin/djdriver (needs your password)"
sudo make install PREFIX="$PREFIX"

mkdir -p "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"
cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>$LABEL</string>
	<key>ProgramArguments</key>
	<array>
		<string>$PREFIX/bin/djdriver</string>
		<string>run</string>
	</array>
	<key>RunAtLoad</key>
	<true/>
	<key>KeepAlive</key>
	<true/>
	<key>ProcessType</key>
	<string>Interactive</string>
	<key>StandardOutPath</key>
	<string>$LOG</string>
	<key>StandardErrorPath</key>
	<string>$LOG</string>
</dict>
</plist>
EOF

launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$PLIST"

echo
echo "Done. djdriver now runs in the background and starts at every login."
echo "Log file: $LOG"

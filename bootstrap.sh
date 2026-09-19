#!/usr/bin/env bash
#
# bootstrap.sh - one-line installer for CannonBall-SE.
#
# Intended to be run straight from the web:
#
#   curl -fsSL https://raw.githubusercontent.com/J1mbo/cannonball-se/master/bootstrap.sh | bash
#
# It installs git if it is missing, clones (or updates) the repository into
# ~/cannonball-se, then hands over to install.sh for the interactive build.
# install.sh asks its own questions; run it directly if you have already
# cloned the repository.

set -euo pipefail

REPO_URL="https://github.com/J1mbo/cannonball-se.git"
TARGET="${CANNONBALL_DIR:-$HOME/cannonball-se}"

echo "CannonBall-SE bootstrap"
echo "  Repository: $REPO_URL"
echo "  Location:   $TARGET"
echo

# Run as the normal user; install.sh calls sudo itself where it needs root.
# Running the whole thing as root would clone into root's home and set the
# appliance service up for the wrong user.
if [ "$(id -u)" -eq 0 ]; then
  echo "Please run this as your normal user, not root - it uses sudo where needed." >&2
  exit 1
fi

# When this script is piped from curl our own stdin is the pipe, already at
# end of file, so install.sh's prompts must be read from the terminal.
if [ -e /dev/tty ]; then
  PROMPT_INPUT=/dev/tty
else
  PROMPT_INPUT=/dev/stdin
fi

# 1. Ensure git is available (Raspberry Pi OS Lite may not ship it).
if ! command -v git >/dev/null 2>&1; then
  echo "--> Installing git..."
  sudo apt-get update -y
  sudo apt-get install -y git
fi

# 2. Clone the repository, or update an existing checkout in place.
if [ -d "$TARGET/.git" ]; then
  echo "--> Existing checkout found; updating..."
  git -C "$TARGET" pull --ff-only
elif [ -e "$TARGET" ]; then
  echo "Error: $TARGET already exists but is not a CannonBall-SE checkout." >&2
  echo "Move it aside, or set CANNONBALL_DIR to another path, then re-run." >&2
  exit 1
else
  echo "--> Cloning..."
  git clone "$REPO_URL" "$TARGET"
fi

# 3. Hand over to the interactive installer, with prompts on the terminal.
cd "$TARGET"
echo "--> Starting install.sh..."
exec bash install.sh <"$PROMPT_INPUT"

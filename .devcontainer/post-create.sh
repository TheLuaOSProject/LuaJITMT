#!/usr/bin/env bash
set -euo pipefail

sudo install -d -o "$(id -u)" -g "$(id -g)" /darling /darling/prefix
chmod 700 /darling/prefix
ensure-osxcross-clone "${OSXCROSS_DIR:-${PWD}/.devcontainer/osxcross}"

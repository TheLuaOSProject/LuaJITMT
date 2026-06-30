#!/usr/bin/env bash
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive

apt_get_update_if_needed() {
    if [ ! -d "/var/lib/apt/lists" ] || [ "$(ls /var/lib/apt/lists/ | wc -l)" = "0" ]; then
        apt-get update
    fi
}

check_packages() {
    if ! dpkg -s "$@" >/dev/null 2>&1; then
        apt_get_update_if_needed
        apt-get install -y --no-install-recommends "$@"
    fi
}

check_packages \
    binutils-mingw-w64-ucrt64 \
    binutils-mingw-w64-x86-64 \
    gcc-mingw-w64-ucrt64 \
    gcc-mingw-w64-x86-64-posix \
    g++-mingw-w64-ucrt64 \
    g++-mingw-w64-x86-64-posix \
    mingw-w64 \
    wine \
    wine64

x86_64-w64-mingw32-gcc-posix -dumpmachine
x86_64-w64-mingw32ucrt-gcc -dumpmachine
wine --version

apt-get clean -y
rm -rf /var/lib/apt/lists/*

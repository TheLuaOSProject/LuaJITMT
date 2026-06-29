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

release="${RELEASE:-v0.1.20260608}"
debs_date="${DEBSDATE:-20260608}"
debs_sha256="${DEBSSHA256:-27469ef3932da2e91dd7fb34b70e3628a3e54b7af9fb5480051f44af35eca1fd}"

check_packages \
    ca-certificates \
    curl \
    fuse3 \
    libfuse2t64 \
    unzip \
    xdg-user-dirs

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

zip_path="${work_dir}/darling-debs.zip"
curl -L --fail --retry 3 \
    -o "${zip_path}" \
    "https://github.com/darlinghq/darling/releases/download/${release}/debs_${debs_date}.zip"
printf '%s  %s\n' "${debs_sha256}" "${zip_path}" | sha256sum -c -
unzip -q "${zip_path}" -d "${work_dir}/debs"

package_path() {
    local name="$1"
    find "${work_dir}/debs" -type f -name "${name}_*.deb" -print -quit
}

apt-get install -y --no-install-recommends \
    "$(package_path darling-core)" \
    "$(package_path darling-system)" \
    "$(package_path darling-cli-gui-common)" \
    "$(package_path darling-cli-python2-common)" \
    "$(package_path darling-cli)"

darling --version || true

apt-get clean -y
rm -rf /var/lib/apt/lists/*

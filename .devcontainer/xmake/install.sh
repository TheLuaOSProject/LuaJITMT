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

normalize_version() {
    local version="$1"

    if [ "${version}" = "latest" ]; then
        git ls-remote --tags https://github.com/xmake-io/xmake.git 'refs/tags/v*' \
            | awk -F/ '{print $3}' \
            | grep -E '^v[0-9]+[.][0-9]+[.][0-9]+$' \
            | sort -V \
            | tail -n 1
    elif [ "${version#v}" = "${version}" ]; then
        printf 'v%s\n' "${version}"
    else
        printf '%s\n' "${version}"
    fi
}

xmake_version_matches() {
    local expected="$1"

    env XMAKE_COLORTERM=nocolor xmake --version 2>/dev/null \
        | grep -q "xmake ${expected}"
}

run_as_install_user() {
    local cmd="$1"

    if [ "$(id -un)" = "${install_user}" ]; then
        env HOME="${install_home}" TMPDIR=/tmp XMAKE_COLORTERM=nocolor PATH=/usr/local/bin:/usr/bin:/bin \
            bash -lc "${cmd}"
    else
        su "${install_user}" -c "env HOME='${install_home}' TMPDIR=/tmp XMAKE_COLORTERM=nocolor PATH=/usr/local/bin:/usr/bin:/bin ${cmd}"
    fi
}

check_packages \
    build-essential \
    ca-certificates \
    curl \
    git \
    libreadline-dev \
    make \
    pkg-config \
    p7zip-full

install_user="${_REMOTE_USER:-${USERNAME:-vscode}}"
if ! id -u "${install_user}" >/dev/null 2>&1; then
    echo "User '${install_user}' does not exist" >&2
    exit 1
fi
install_home="$(getent passwd "${install_user}" | cut -d: -f6)"

xmake_version="$(normalize_version "${VERSION:-latest}")"
if [ -z "${xmake_version}" ]; then
    echo "Unable to determine xmake release version" >&2
    exit 1
fi

xmake_bin="/usr/local/bin/xmake"
if ! xmake_version_matches "${xmake_version}"; then
    build_dir="$(mktemp -d)"
    trap 'rm -rf "${build_dir}"' EXIT

    git clone \
        --depth=1 \
        --branch "${xmake_version}" \
        --recurse-submodules \
        --shallow-submodules \
        https://github.com/xmake-io/xmake.git \
        "${build_dir}"

    if [ -f "${build_dir}/configure" ]; then
        (cd "${build_dir}" && ./configure)
    fi

    make -C "${build_dir}" --no-print-directory -j"$(nproc)"
    make -C "${build_dir}" --no-print-directory install PREFIX=/usr/local
fi

cat >/usr/local/bin/xrepo <<'EOF'
#!/usr/bin/env sh
exec /usr/local/bin/xmake lua private.xrepo "$@"
EOF
chmod 755 /usr/local/bin/xrepo

run_as_install_user "xmake --version"
run_as_install_user "xrepo --version"

apt-get clean -y
rm -rf /var/lib/apt/lists/*

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

install_dir="${INSTALLDIR:-/opt/osxcross}"
repo="${REPO:-https://github.com/tpoechtrager/osxcross.git}"
ref="${REF:-master}"

check_packages \
    bash \
    bison \
    bzip2 \
    ca-certificates \
    clang \
    cmake \
    cpio \
    curl \
    file \
    flex \
    git \
    libarchive-tools \
    libbz2-dev \
    liblzma-dev \
    libxml2-dev \
    libssl-dev \
    lld \
    llvm-dev \
    make \
    patch \
    python3 \
    sed \
    tar \
    xar \
    xz-utils

cat >/usr/local/bin/ensure-osxcross-clone <<'SH'
#!/usr/bin/env bash
set -euo pipefail

osxcross_dir="${1:-${OSXCROSS_DIR:-__OSXCROSS_INSTALL_DIR__}}"
repo="${OSXCROSS_REPO:-__OSXCROSS_REPO__}"
ref="${OSXCROSS_REF:-__OSXCROSS_REF__}"

if [ -d "${osxcross_dir}/.git" ]; then
    mkdir -p "${osxcross_dir}/tarballs"
    echo "osxcross already cloned at ${osxcross_dir}"
    exit 0
fi

if [ -e "${osxcross_dir}" ] && [ -n "$(find "${osxcross_dir}" -mindepth 1 -maxdepth 1 -print -quit)" ]; then
    echo "${osxcross_dir} exists but is not an empty osxcross clone" >&2
    exit 1
fi

mkdir -p "$(dirname "${osxcross_dir}")"
GIT_CONFIG_GLOBAL="${GIT_CONFIG_GLOBAL:-/dev/null}" \
    git clone --depth=1 --branch "${ref}" "${repo}" "${osxcross_dir}"
mkdir -p "${osxcross_dir}/tarballs"
echo "osxcross cloned at ${osxcross_dir}"
SH
sed -i \
    -e "s|__OSXCROSS_INSTALL_DIR__|${install_dir}|g" \
    -e "s|__OSXCROSS_REPO__|${repo}|g" \
    -e "s|__OSXCROSS_REF__|${ref}|g" \
    /usr/local/bin/ensure-osxcross-clone
chmod 755 /usr/local/bin/ensure-osxcross-clone

cat >/usr/local/bin/build-osxcross-from-xip <<'SH'
#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "usage: build-osxcross-from-xip <Xcode.xip> [osxcross-dir]" >&2
    exit 2
fi

xip_path="$(realpath "$1")"
osxcross_dir="${2:-${OSXCROSS_DIR:-__OSXCROSS_INSTALL_DIR__}}"

ensure-osxcross-clone "${osxcross_dir}" >/dev/null
cd "${osxcross_dir}"
mkdir -p tarballs
SDK_COMPRESSOR=xz GIT_CONFIG_GLOBAL="${GIT_CONFIG_GLOBAL:-/dev/null}" \
    ./tools/gen_sdk_package_pbzx.sh "${xip_path}"
find "${osxcross_dir}" -maxdepth 1 -type f -name 'MacOSX*.sdk.tar.*' \
    -exec mv -f {} "${osxcross_dir}/tarballs/" \;

if [ -z "${SDK_VERSION:-}" ]; then
    sdk_version="$(find "${osxcross_dir}/tarballs" -maxdepth 1 -type f -name 'MacOSX*.sdk.tar.*' \
        | sed -E 's|.*/MacOSX([0-9.]+)[.]sdk[.]tar[.].*|\1|' \
        | sort -V \
        | tail -n 1)"
    if [ -n "${sdk_version}" ]; then
        export SDK_VERSION="${sdk_version}"
        echo "Using SDK_VERSION=${SDK_VERSION}"
    fi
fi

UNATTENDED=1 OSX_VERSION_MIN="${OSX_VERSION_MIN:-10.13}" ./build.sh
echo "Built osxcross using SDK tarballs in ${osxcross_dir}/tarballs"
SH
sed -i "s|__OSXCROSS_INSTALL_DIR__|${install_dir}|g" /usr/local/bin/build-osxcross-from-xip
chmod 755 /usr/local/bin/build-osxcross-from-xip

echo "osxcross dependencies are installed."
echo "Run: ensure-osxcross-clone"
echo "Run: build-osxcross-from-xip /path/to/Xcode.xip"

apt-get clean -y
rm -rf /var/lib/apt/lists/*

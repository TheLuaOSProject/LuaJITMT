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

if [ ! -d "${install_dir}/.git" ]; then
    GIT_CONFIG_GLOBAL=/dev/null git clone --depth=1 --branch "${ref}" "${repo}" "${install_dir}"
else
    GIT_CONFIG_GLOBAL=/dev/null git -C "${install_dir}" fetch --depth=1 origin "${ref}"
    GIT_CONFIG_GLOBAL=/dev/null git -C "${install_dir}" checkout FETCH_HEAD
fi

mkdir -p "${install_dir}/tarballs"

cat >/usr/local/bin/build-osxcross-from-xip <<'SH'
#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "usage: build-osxcross-from-xip <Xcode.xip> [osxcross-dir]" >&2
    exit 2
fi

xip_path="$(realpath "$1")"
osxcross_dir="${2:-${OSXCROSS_DIR:-/opt/osxcross}}"

cd "${osxcross_dir}"
mkdir -p tarballs
SDK_COMPRESSOR=xz GIT_CONFIG_GLOBAL="${GIT_CONFIG_GLOBAL:-/dev/null}" \
    ./tools/gen_sdk_package_pbzx.sh "${xip_path}"
find "${osxcross_dir}" -maxdepth 1 -type f -name 'MacOSX*.sdk.tar.*' \
    -exec mv -f {} "${osxcross_dir}/tarballs/" \;
UNATTENDED=1 OSX_VERSION_MIN="${OSX_VERSION_MIN:-10.13}" ./build.sh
echo "Built osxcross using SDK tarballs in ${osxcross_dir}/tarballs"
SH
chmod 755 /usr/local/bin/build-osxcross-from-xip

echo "osxcross is installed at ${install_dir}."
echo "Run: build-osxcross-from-xip /path/to/Xcode.xip"

apt-get clean -y
rm -rf /var/lib/apt/lists/*

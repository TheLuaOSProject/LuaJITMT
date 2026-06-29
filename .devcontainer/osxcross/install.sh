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
    git clone --depth=1 --branch "${ref}" "${repo}" "${install_dir}"
else
    git -C "${install_dir}" fetch --depth=1 origin "${ref}"
    git -C "${install_dir}" checkout FETCH_HEAD
fi

mkdir -p "${install_dir}/tarballs" /usr/local/libexec

cat >/usr/local/libexec/pbzx-extract.py <<'PY'
#!/usr/bin/env python3
import lzma
import struct
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: pbzx-extract.py <Content>")

with open(sys.argv[1], "rb") as f:
    if f.read(4) != b"pbzx":
        raise SystemExit("not a pbzx payload")
    f.read(8)
    while True:
        header = f.read(16)
        if not header:
            break
        if len(header) != 16:
            raise SystemExit("truncated pbzx chunk header")
        flags, length = struct.unpack(">QQ", header)
        data = f.read(length)
        if len(data) != length:
            raise SystemExit("truncated pbzx chunk")
        if data.startswith(b"\xfd7zXZ\x00"):
            sys.stdout.buffer.write(lzma.decompress(data))
        else:
            sys.stdout.buffer.write(data)
        if (flags & (1 << 24)) == 0:
            break
PY
chmod 755 /usr/local/libexec/pbzx-extract.py

cat >/usr/local/bin/build-osxcross-from-xip <<'SH'
#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "usage: build-osxcross-from-xip <Xcode.xip> [osxcross-dir]" >&2
    exit 2
fi

xip_path="$(realpath "$1")"
osxcross_dir="${2:-${OSXCROSS_DIR:-/opt/osxcross}}"
work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

mkdir -p "${work_dir}/xip" "${work_dir}/payload" "${osxcross_dir}/tarballs"
xar -xf "${xip_path}" -C "${work_dir}/xip"
/usr/local/libexec/pbzx-extract.py "${work_dir}/xip/Content" \
    | (cd "${work_dir}/payload" && cpio -idm --quiet '*MacOSX.platform/Developer/SDKs/*')

sdk_path="$(find "${work_dir}/payload" -path '*MacOSX.platform/Developer/SDKs/MacOSX.sdk' -type d -print -quit)"
if [ -z "${sdk_path}" ]; then
    sdk_path="$(find "${work_dir}/payload" -path '*MacOSX.platform/Developer/SDKs/MacOSX*.sdk' -type d -print -quit)"
fi
if [ -z "${sdk_path}" ]; then
    echo "unable to find a macOS SDK in ${xip_path}" >&2
    exit 1
fi

sdk_version="$(
    python3 - "${sdk_path}" <<'PY'
import json
import plistlib
import pathlib
import sys

sdk = pathlib.Path(sys.argv[1])
for name in ("SDKSettings.json", "SDKSettings.plist"):
    path = sdk / name
    if not path.exists():
        continue
    with path.open("rb") as f:
        data = json.load(f) if name.endswith(".json") else plistlib.load(f)
    version = data.get("Version") or data.get("CanonicalName", "").replace("macosx", "")
    if version:
        print(version)
        raise SystemExit(0)
print("unknown")
PY
)"

tarball="${osxcross_dir}/tarballs/MacOSX${sdk_version}.sdk.tar.xz"
tar -C "$(dirname "${sdk_path}")" -cJf "${tarball}" "$(basename "${sdk_path}")"

cd "${osxcross_dir}"
UNATTENDED=1 OSX_VERSION_MIN="${OSX_VERSION_MIN:-10.13}" ./build.sh
echo "Built osxcross using ${tarball}"
SH
chmod 755 /usr/local/bin/build-osxcross-from-xip

echo "osxcross is installed at ${install_dir}."
echo "Run: build-osxcross-from-xip /path/to/Xcode.xip"

apt-get clean -y
rm -rf /var/lib/apt/lists/*

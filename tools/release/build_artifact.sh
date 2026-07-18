#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
usage: tools/release/build_artifact.sh <platform> <tag> [out-dir]

platform:
  linux-x86_64
  macos-x86_64
  windows-x86_64-ucrt

The script builds a clean release artifact, stages an install-style tree,
runs release smoke/archive checks unless LJ_RELEASE_RUN_TESTS=0, and writes
the archive plus per-artifact checksum into out-dir. Set
LJ_RELEASE_RUN_STOCK=1 to include the full stock LuaJIT test suite.
Cross-target release checks need a host LuaJIT harness runner; set
LJ_RELEASE_HOST_LUA or LUA when host luajit is not in PATH.
USAGE
  exit 2
}

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then usage; fi

platform=$1
tag=$2
out_dir=${3:-release-artifacts/${tag}}

release_tag_error() {
  local bad=$1
  if [ "$bad" = "b1.1" ]; then
    printf 'release b1.1 was renamed to b1.0.1; use b1.0.1\n' >&2
  elif [[ "$bad" =~ ^b[0-9]+[.][0-9]+$ ]]; then
    printf 'rolling release tags must be b<major>.<minor>.<patch>; got stale two-component tag %s\n' "$bad" >&2
  else
    printf 'release tag must be b<major>.<minor>.<patch>; got %s\n' "$bad" >&2
  fi
}

if [[ ! "$tag" =~ ^b[0-9]+[.][0-9]+[.][0-9]+$ ]]; then
  release_tag_error "$tag"
  exit 2
fi

script_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
root=${LJ_RELEASE_ROOT:-$script_root}
root=$(CDPATH= cd -- "$root" && pwd)
test_root=${LJ_RELEASE_TEST_ROOT:-$script_root}
test_root=$(CDPATH= cd -- "$test_root" && pwd)
jobs=${JOBS:-${MAKE_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}}
prefix=${PREFIX:-/usr/local}
commit=$(git -C "$root" rev-parse HEAD)
stage_parent=${LJ_RELEASE_STAGE_PARENT:-$(mktemp -d)}
keep_stage=${LJ_RELEASE_KEEP_STAGE:-0}
run_tests=${LJ_RELEASE_RUN_TESTS:-1}
release_host_lua_bin=

case "$platform" in
  linux-x86_64|macos-x86_64|windows-x86_64-ucrt) ;;
  *) usage ;;
esac

mkdir -p "$out_dir"
out_dir=$(CDPATH= cd -- "$out_dir" && pwd)

pkg="LuaJITMT-${tag}-${platform}"
stage="${stage_parent}/${pkg}"

cleanup() {
  if [ "$keep_stage" != "1" ]; then
    rm -rf "$stage_parent"
  else
    echo "kept release staging tree: $stage"
  fi
}
trap cleanup EXIT

rm -rf "$stage"

make_clean() {
  make -C "$root" clean
  rm -f "$root/src/.lj-test-build-signature"
}

build_make() {
  make -C "$root" -j"$jobs" "$@"
}

install_doc() {
  local docdir=$1
  local layout=$2
  mkdir -p "$docdir"
  install -m 0644 "$root/README" "$docdir/README"
  install -m 0644 "$root/COPYRIGHT" "$docdir/COPYRIGHT"
  {
    printf 'LuaJITMT %s\n' "$tag"
    printf 'commit: %s\n' "$commit"
    printf 'platform: %s\n' "$platform"
    printf 'arch: x86_64\n'
    printf 'gc64: required\n'
    printf 'gc: gc2-public-api\n'
    printf 'relver: %s\n' "$(cat "$root/src/luajit_relver.txt")"
    printf 'layout: %s\n' "$layout"
  } > "$docdir/BUILDINFO"
}

archive_stage() {
  local archive
  if ! command -v xz >/dev/null 2>&1; then
    printf 'xz is required for stable .tar.xz release artifacts\n' >&2
    exit 1
  fi
  archive="${out_dir}/${pkg}.tar.xz"
  (cd "$stage_parent" && tar -cJf "$archive" "$pkg")
  (cd "$out_dir" && checksum "$(basename "$archive")" > "$(basename "$archive").sha256")
  echo "$archive"
}

archive_zip_stage() {
  local archive="${out_dir}/${pkg}.zip"
  if ! command -v zip >/dev/null 2>&1; then
    printf 'zip is required for Windows release artifacts\n' >&2
    exit 1
  fi
  (cd "$stage_parent" && zip -qr "$archive" "$pkg")
  (cd "$out_dir" && checksum "$(basename "$archive")" > "$(basename "$archive").sha256")
  echo "$archive"
}

checksum() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$@"
  else
    shasum -a 256 "$@"
  fi
}

windows_stage_runtime() {
  local cross=$1
  local cc=$2
  local bindir=$3
  local buildinfo=$4
  local runtime_name=libwinpthread-1.dll
  local runtime_path
  local runtime_sha256

  runtime_path=$("${cross}${cc}" -print-file-name="$runtime_name")
  if [ "$runtime_path" = "$runtime_name" ] || [ ! -f "$runtime_path" ]; then
    printf '%s could not locate required runtime %s\n' \
      "${cross}${cc}" "$runtime_name" >&2
    exit 1
  fi
  runtime_path=$(CDPATH= cd -- "$(dirname -- "$runtime_path")" && pwd)/$(basename -- "$runtime_path")
  install -m 0755 "$runtime_path" "$bindir/$runtime_name"
  runtime_sha256=$(checksum "$runtime_path" | awk '{print $1}')
  {
    printf 'bundled_runtime: %s\n' "$runtime_name"
    printf 'bundled_runtime_origin: %s\n' "$runtime_path"
    printf 'bundled_runtime_sha256: %s\n' "$runtime_sha256"
  } >> "$buildinfo"
}

windows_verify_import_closure() {
  local cross=$1
  local bindir=$2
  local objdump=${cross}objdump
  local image import lower imports
  local -a images=("$bindir/luajit.exe" "$bindir/lua51.dll")

  if ! command -v "$objdump" >/dev/null 2>&1; then
    printf '%s is required to validate Windows runtime imports\n' "$objdump" >&2
    exit 1
  fi
  while IFS= read -r -d '' image; do images+=("$image"); done \
    < <(find "$bindir" -maxdepth 1 -type f -iname '*.dll' -print0)
  for image in "${images[@]}"; do
    [ -f "$image" ] || { printf 'missing Windows image: %s\n' "$image" >&2; exit 1; }
    if ! imports=$("$objdump" -p "$image" |
        sed -n 's/^[[:space:]]*DLL Name:[[:space:]]*//p'); then
      printf 'failed to inspect PE imports: %s\n' "$image" >&2
      exit 1
    fi
    while IFS= read -r import; do
      [ -n "$import" ] || continue
      lower=${import,,}
      case "$lower" in
        api-ms-win-*.dll|ext-ms-win-*.dll|advapi32.dll|bcrypt.dll|comctl32.dll|comdlg32.dll|crypt32.dll|gdi32.dll|imm32.dll|kernel32.dll|msvcrt.dll|ntdll.dll|ole32.dll|oleaut32.dll|rpcrt4.dll|secur32.dll|setupapi.dll|shell32.dll|shlwapi.dll|ucrtbase.dll|user32.dll|userenv.dll|version.dll|winmm.dll|winspool.drv|ws2_32.dll) ;;
        *)
          if [ ! -f "$bindir/$import" ] && ! find "$bindir" -maxdepth 1 -type f \
              -iname "$import" -print -quit | grep -q .; then
            printf 'unpackaged PE dependency: %s imports %s\n' \
              "$(basename -- "$image")" "$import" >&2
            exit 1
          fi
          ;;
      esac
    done <<< "$imports"
  done
}

release_host_lua() {
  if [ -n "$release_host_lua_bin" ]; then return; fi
  if [ -n "${LJ_RELEASE_HOST_LUA:-}" ]; then
    release_host_lua_bin=$LJ_RELEASE_HOST_LUA
  elif [ -n "${LUA:-}" ]; then
    release_host_lua_bin=$LUA
  elif command -v luajit >/dev/null 2>&1; then
    release_host_lua_bin=$(command -v luajit)
  elif [ "$platform" = "linux-x86_64" ] && [ -x "$root/src/luajit" ]; then
    release_host_lua_bin="$root/src/luajit"
  else
    printf 'release tests need a host LuaJIT runner; set LJ_RELEASE_HOST_LUA or LUA\n' >&2
    exit 1
  fi
}

lua_test_release() {
  release_host_lua
  LUA="$release_host_lua_bin" "$test_root/tools/ci/lua_test.sh" "$@"
}

run_release_test() {
  if [ "$run_tests" = "0" ]; then return; fi
  case "$platform" in
    linux-x86_64)
      LJ_RELEASE_PREFIX="$prefix" \
      LJ_RELEASE_REQUIRE=linux \
      LJ_RELEASE_LINUX_BIN="${stage}${prefix}/bin/luajit" \
        lua_test_release release_linux_binary
      ;;
    macos-x86_64)
      LJ_RELEASE_PREFIX="$prefix" \
      LJ_RELEASE_REQUIRE=macos \
      LJ_RELEASE_MACOS_BIN="${stage}${prefix}/bin/luajit" \
        lua_test_release release_macos_binary
      ;;
    windows-x86_64-ucrt)
      LJ_RELEASE_PREFIX="$prefix" \
      LJ_RELEASE_REQUIRE=windows \
      LJ_RELEASE_WINDOWS_BIN="${stage}${prefix}/bin/luajit.exe" \
      LJ_RELEASE_WINDOWS_RUNNER="${LJ_RELEASE_WINDOWS_RUNNER:-wine}" \
        lua_test_release release_windows_binary
      ;;
  esac
}

run_release_archive_test() {
  if [ "$run_tests" = "0" ]; then return; fi
  local archive=$1
  case "$platform" in
    linux-x86_64)
      LJ_RELEASE_PREFIX="$prefix" \
      LJ_RELEASE_REQUIRE=linux \
      LJ_RELEASE_LINUX_ARCHIVE="$archive" \
        lua_test_release release_linux_archive
      ;;
    macos-x86_64)
      LJ_RELEASE_PREFIX="$prefix" \
      LJ_RELEASE_REQUIRE=macos \
      LJ_RELEASE_MACOS_ARCHIVE="$archive" \
      LJ_RELEASE_MACOS_RUNNER="${LJ_RELEASE_MACOS_RUNNER:-}" \
        lua_test_release release_macos_archive
      ;;
    windows-x86_64-ucrt)
      LJ_RELEASE_PREFIX="$prefix" \
      LJ_RELEASE_REQUIRE=windows \
      LJ_RELEASE_WINDOWS_ARCHIVE="$archive" \
      LJ_RELEASE_WINDOWS_RUNNER="${LJ_RELEASE_WINDOWS_RUNNER:-wine}" \
        lua_test_release release_windows_archive
      ;;
  esac
}

case "$platform" in
  linux-x86_64)
    make_clean
    build_make
    make -C "$root" install DESTDIR="$stage" PREFIX="$prefix"
    install_doc "${stage}${prefix}/share/doc/luajitmt" "make install DESTDIR prefix tree"
    run_release_test
    archive=$(archive_stage)
    run_release_archive_test "$archive"
    ;;
  macos-x86_64)
    export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-13.0}
    macos_make_args=(
      TARGET_SYS=Darwin
      TARGET_FLAGS="${LJ_RELEASE_MACOS_TARGET_FLAGS:--arch x86_64}"
    )
    if [ -n "${LJ_RELEASE_MACOS_CC:-}" ]; then
      macos_make_args+=(CC="$LJ_RELEASE_MACOS_CC")
    fi
    if [ -n "${LJ_RELEASE_MACOS_HOST_CC:-${HOST_CC:-}}" ]; then
      macos_make_args+=(HOST_CC="${LJ_RELEASE_MACOS_HOST_CC:-${HOST_CC:-}}")
    fi
    if [ -n "${LJ_RELEASE_MACOS_CROSS:-}" ]; then
      macos_make_args+=(CROSS="$LJ_RELEASE_MACOS_CROSS")
    fi
    make_clean
    build_make "${macos_make_args[@]}"
    make -C "$root" install DESTDIR="$stage" PREFIX="$prefix" \
      "${macos_make_args[@]}"
    install_doc "${stage}${prefix}/share/doc/luajitmt" "make install DESTDIR prefix tree"
    printf 'macos_deployment_target: %s\n' "$MACOSX_DEPLOYMENT_TARGET" \
      >> "${stage}${prefix}/share/doc/luajitmt/BUILDINFO"
    run_release_test
    archive=$(archive_stage)
    run_release_archive_test "$archive"
    ;;
  windows-x86_64-ucrt)
    cross=${LJ_RELEASE_WINDOWS_CROSS:-x86_64-w64-mingw32ucrt-}
    cc=${LJ_RELEASE_WINDOWS_CC:-gcc}
    windows_make_args=(
      HOST_CC="${HOST_CC:-gcc}"
      CROSS="$cross"
      CC="$cc"
      TARGET_SYS=Windows
    )
    windows_install_args=(
      "${windows_make_args[@]}"
      INSTALL_DEP=src/luajit.exe
      INSTALL_TNAME=luajit.exe
      INSTALL_TSYMNAME=luajit.exe
      INSTALL_ANAME=libluajit-5.1.dll.a
      INSTALL_SONAME=lua51.dll
      'INSTALL_DYN=$(INSTALL_BIN)/lua51.dll'
      FILE_T=luajit.exe
      FILE_A=libluajit-5.1.dll.a
      FILE_SO=lua51.dll
      LDCONFIG=:
      SYMLINK=:
    )
    make_clean
    build_make "${windows_make_args[@]}"
    "$test_root/tools/ci/windows_gc2_tls_gate.sh" \
      "${cross}objdump" "$root/src/lj_gc2.o" "$root/src/lj_thr.o"
    make -C "$root" install DESTDIR="$stage" PREFIX="$prefix" \
      "${windows_install_args[@]}"
    install_doc "${stage}${prefix}/share/doc/luajitmt" "make install DESTDIR prefix tree"
    {
      printf 'toolchain: %s%s\n' "$cross" "$cc"
      printf 'runtime: UCRT\n'
    } >> "${stage}${prefix}/share/doc/luajitmt/BUILDINFO"
    windows_stage_runtime "$cross" "$cc" "${stage}${prefix}/bin" \
      "${stage}${prefix}/share/doc/luajitmt/BUILDINFO"
    if [ "$run_tests" != "0" ]; then
      gc2_cell_fixture="${stage}${prefix}/bin/lj-windows-gc2-cell.exe"
      "$test_root/tools/ci/build_windows_gc2_cell_fixture.sh" \
        "${cross}${cc}" "$root" "$test_root" "$gc2_cell_fixture"
      WINEDEBUG=${WINEDEBUG:--all} \
        "${LJ_RELEASE_WINDOWS_RUNNER:-wine}" "$gc2_cell_fixture"
      rm -f "$gc2_cell_fixture"
    fi
    windows_verify_import_closure "$cross" "${stage}${prefix}/bin"
    run_release_test
    archive=$(archive_zip_stage)
    run_release_archive_test "$archive"
    ;;
esac

#!/bin/sh
# Shared, profile-bound plumbing for focused native ARM64 contracts.
# Source this file after setting `root`; see the helpers below for the ABI.

lj_ci_native_arm64_or_skip() {
  name=$1
  if test "$(uname -s)" = Darwin && test "$(uname -m)" = arm64; then
    echo "$name native branch: macOS arm64"
    return 0
  fi
  if test "${LJ_TEST_REQUIRE_NATIVE:-0}" = 1; then
    echo "$name requires native macOS arm64 (host is $(uname -s)/$(uname -m))" >&2
    return 2
  fi
  echo "$name SKIP: requires native macOS arm64 (host is $(uname -s)/$(uname -m))"
  return 1
}

lj_ci_profile_init() {
  LJ_CI_ROOT=$1
  LJ_CI_NAME=$2
  LJ_CI_LOCK_DIR=$LJ_CI_ROOT/src/.lj-test-run.lock
  LJ_CI_LOCK_HELD=0
  LJ_CI_RESTORE_NEEDED=0
  LJ_CI_TMPDIR=
  LJ_CI_ARCHIVE=$LJ_CI_ROOT/src/libluajit.a
  LJ_CI_RUNTIME=$LJ_CI_ROOT/src/luajit
  LJ_CI_PROFILE_ID=
  LJ_CI_BUILT_ARCHIVE_SHA=
  LJ_CI_BUILT_RUNTIME_SHA=
  LJ_CI_RESTORE_TARGET_FLAGS=${LJ_TEST_RESTORE_TARGET_FLAGS:--arch arm64}
  LJ_CI_RESTORE_XCFLAGS=${LJ_TEST_RESTORE_XCFLAGS:--DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT}
  LJ_CI_MINVER=${MACOSX_DEPLOYMENT_TARGET:-13.0}
  LJ_CI_JOBS=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
  LJ_CI_CC=${CC:-$(xcrun --sdk macosx --find clang)}
  LJ_CI_TARGET_SYS=${LJ_TEST_TARGET_SYS:-Darwin}
  LJ_CI_CFLAGS=${CFLAGS:-}
  LJ_CI_CPPFLAGS=${CPPFLAGS:-}
  LJ_CI_LDFLAGS=${LDFLAGS:-}
  trap 'lj_ci_finish $?' EXIT
  trap 'exit 129' HUP
  trap 'exit 130' INT
  trap 'exit 143' TERM
  if test -z "${SDKROOT:-}"; then
    SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
    export SDKROOT
  fi
  LJ_CI_SDKROOT=$SDKROOT
  if test "${LJ_TEST_DISABLE_RUN_LOCK:-0}" != 1 &&
     test "${LJ_TEST_RUN_LOCK_HELD:-0}" != 1; then
    lock_timeout=${LJ_TEST_RUN_LOCK_TIMEOUT:-900}
    lock_started=$(date +%s)
    while ! mkdir "$LJ_CI_LOCK_DIR" 2>/dev/null; do
      lock_now=$(date +%s)
      if test "$lock_timeout" -ge 0 &&
         test $((lock_now - lock_started)) -ge "$lock_timeout"; then
        echo "$LJ_CI_NAME timed out waiting for build lock: $LJ_CI_LOCK_DIR" >&2
        test ! -f "$LJ_CI_LOCK_DIR/owner" || cat "$LJ_CI_LOCK_DIR/owner" >&2
        return 2
      fi
      sleep 0.2
    done
    LJ_CI_LOCK_HELD=1
    printf 'time=%s\ncontract=%s\npid=%s\n' \
      "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$LJ_CI_NAME" "$$" \
      >"$LJ_CI_LOCK_DIR/owner"
  fi
}

lj_ci_sha256() {
  line=$(shasum -a 256 "$1") || return $?
  digest=${line%% *}
  case "$digest" in
    *[!0123456789abcdefABCDEF]*|'')
      echo "cannot parse SHA-256 for $1" >&2
      return 1 ;;
  esac
  test "${#digest}" -eq 64 || {
    echo "invalid SHA-256 length for $1" >&2
    return 1
  }
  printf '%s\n' "$digest"
}

lj_ci_profile_hash() {
  label=$1
  config=$(printf '%s\n' "$LJ_CI_NAME" "$label" "$LJ_CI_ARCH" \
    "$LJ_CI_TARGET_SYS" "$LJ_CI_TARGET_FLAGS" "$LJ_CI_XCFLAGS" \
    "$LJ_CI_CC" "$LJ_CI_CFLAGS" "$LJ_CI_CPPFLAGS" "$LJ_CI_LDFLAGS" \
    "$LJ_CI_MINVER" "$LJ_CI_SDKROOT" "$LJ_CI_BUILT_ARCHIVE_SHA" \
    "$LJ_CI_BUILT_RUNTIME_SHA") || return $?
  hashline=$(printf '%s' "$config" | shasum -a 256) || return $?
  hash=${hashline%% *}
  case "$hash" in
    *[!0123456789abcdefABCDEF]*|'') return 1 ;;
  esac
  test "${#hash}" -eq 64 || return 1
  LJ_CI_PROFILE_ID=$hash
}

lj_ci_tmpdir() {
  pattern=$1
  LJ_CI_TMPDIR=$(mktemp -d "${TMPDIR:-/tmp}/$pattern.XXXXXX")
}

lj_ci_set_restore() {
  LJ_CI_RESTORE_TARGET_FLAGS=$1
  LJ_CI_RESTORE_XCFLAGS=$2
}

lj_ci_build_profile() {
  LJ_CI_ARCH=$1
  LJ_CI_TARGET_FLAGS=$2
  LJ_CI_XCFLAGS=$3
  label=$4
  LJ_CI_RESTORE_NEEDED=1
  env MACOSX_DEPLOYMENT_TARGET="$LJ_CI_MINVER" \
    make -C "$LJ_CI_ROOT/src" clean TARGET_SYS="$LJ_CI_TARGET_SYS" \
      CC="$LJ_CI_CC" TARGET_FLAGS="$LJ_CI_TARGET_FLAGS" \
      CFLAGS="$LJ_CI_CFLAGS" CPPFLAGS="$LJ_CI_CPPFLAGS" \
      LDFLAGS="$LJ_CI_LDFLAGS" XCFLAGS="$LJ_CI_XCFLAGS" || return $?
  env MACOSX_DEPLOYMENT_TARGET="$LJ_CI_MINVER" \
    make -C "$LJ_CI_ROOT/src" -j"$LJ_CI_JOBS" \
      TARGET_SYS="$LJ_CI_TARGET_SYS" CC="$LJ_CI_CC" \
      TARGET_FLAGS="$LJ_CI_TARGET_FLAGS" CFLAGS="$LJ_CI_CFLAGS" \
      CPPFLAGS="$LJ_CI_CPPFLAGS" LDFLAGS="$LJ_CI_LDFLAGS" \
      XCFLAGS="$LJ_CI_XCFLAGS" || return $?
  actual=$(lipo -archs "$LJ_CI_ARCHIVE") || return $?
  if test "$actual" != "$LJ_CI_ARCH"; then
    echo "$LJ_CI_NAME expected $LJ_CI_ARCH archive, got '$actual'" >&2
    return 1
  fi
  LJ_CI_BUILT_ARCHIVE_SHA=$(lj_ci_sha256 "$LJ_CI_ARCHIVE") || return $?
  runtime_arch=$(lipo -archs "$LJ_CI_RUNTIME") || return $?
  if test "$runtime_arch" != "$LJ_CI_ARCH"; then
    echo "$LJ_CI_NAME expected $LJ_CI_ARCH runtime, got '$runtime_arch'" >&2
    return 1
  fi
  LJ_CI_BUILT_RUNTIME_SHA=$(lj_ci_sha256 "$LJ_CI_RUNTIME") || return $?
  lj_ci_profile_hash "$label" || return $?
  echo "$LJ_CI_NAME profile=$LJ_CI_PROFILE_ID target=$LJ_CI_ARCH flags='$LJ_CI_XCFLAGS'"
}

# Bind later fixture runs to an archive/runtime built by an enclosing contract.
# This does not rebuild or change that caller's requested restoration profile.
lj_ci_bind_existing_profile() {
  LJ_CI_ARCH=$1
  LJ_CI_TARGET_FLAGS=$2
  LJ_CI_XCFLAGS=$3
  label=$4
  actual=$(lipo -archs "$LJ_CI_ARCHIVE") || return $?
  if test "$actual" != "$LJ_CI_ARCH"; then
    echo "$LJ_CI_NAME expected existing $LJ_CI_ARCH archive, got '$actual'" >&2
    return 1
  fi
  runtime_arch=$(lipo -archs "$LJ_CI_RUNTIME") || return $?
  if test "$runtime_arch" != "$LJ_CI_ARCH"; then
    echo "$LJ_CI_NAME expected existing $LJ_CI_ARCH runtime, got '$runtime_arch'" >&2
    return 1
  fi
  LJ_CI_BUILT_ARCHIVE_SHA=$(lj_ci_sha256 "$LJ_CI_ARCHIVE") || return $?
  LJ_CI_BUILT_RUNTIME_SHA=$(lj_ci_sha256 "$LJ_CI_RUNTIME") || return $?
  lj_ci_profile_hash "$label" || return $?
  echo "$LJ_CI_NAME bound profile=$LJ_CI_PROFILE_ID target=$LJ_CI_ARCH"
}

lj_ci_require_symbols() {
  for symbol do
    nm "$LJ_CI_ARCHIVE" | grep " T _$symbol\$" >/dev/null || {
      echo "$LJ_CI_NAME profile lacks required symbol $symbol" >&2
      return 1
    }
  done
}

# Compile one or more fixture translation units with the exact target, XCFLAGS,
# deployment target, and archive profile selected by lj_ci_build_profile.
lj_ci_compile_fixture() {
  output=$1
  shift
  # CC and flags follow the conventional make/shell word-list contract.
  set -f
  # shellcheck disable=SC2086 # compiler and build flags are intentional lists.
  set -- $LJ_CI_CC $LJ_CI_CFLAGS $LJ_CI_CPPFLAGS \
    -std=gnu11 -O2 -Wall -Wextra -Werror $LJ_CI_TARGET_FLAGS \
    -mmacosx-version-min="$LJ_CI_MINVER" $LJ_CI_XCFLAGS \
    -I"$LJ_CI_ROOT/src" "$@" "$LJ_CI_ARCHIVE" \
    $LJ_CI_LDFLAGS -lm -pthread -o "$output"
  "$@" || { status=$?; set +f; return "$status"; }
  set +f
  actual=$(lipo -archs "$output") || return $?
  if test "$actual" != "$LJ_CI_ARCH"; then
    echo "$LJ_CI_NAME fixture expected $LJ_CI_ARCH, got '$actual'" >&2
    return 1
  fi
  fixture_sha=$(lj_ci_sha256 "$output") || return $?
  printf 'profile=%s\narchive=%s\nfixture=%s\n' \
    "$LJ_CI_PROFILE_ID" "$LJ_CI_BUILT_ARCHIVE_SHA" "$fixture_sha" \
    >"$output.profile" || return $?
}

# Compile an auxiliary object with the caller-selected compiler wordlist and
# target flags.  This is used for intentionally empty/negative translation
# units that must not be linked against the runtime archive.
lj_ci_compile_object() {
  output=$1
  source=$2
  shift 2
  set -f
  # shellcheck disable=SC2086 # compiler and build flags are intentional lists.
  set -- $LJ_CI_CC $LJ_CI_CFLAGS $LJ_CI_CPPFLAGS \
    -std=gnu11 -O2 -Wall -Wextra -Werror $LJ_CI_TARGET_FLAGS \
    -mmacosx-version-min="$LJ_CI_MINVER" $LJ_CI_XCFLAGS \
    -I"$LJ_CI_ROOT/src" "$@" -c "$source" -o "$output"
  "$@" || { status=$?; set +f; return "$status"; }
  set +f
  actual=$(lipo -archs "$output") || return $?
  if test "$actual" != "$LJ_CI_ARCH"; then
    echo "$LJ_CI_NAME object expected $LJ_CI_ARCH, got '$actual'" >&2
    return 1
  fi
}

lj_ci_run_fixture() {
  test -n "$LJ_CI_PROFILE_ID" || {
    echo "$LJ_CI_NAME attempted fixture run without a profile build" >&2
    return 1
  }
  timeout=$1
  fixture=$2
  test -n "$fixture" || return 1
  current=$(lj_ci_sha256 "$LJ_CI_ARCHIVE") || return $?
  test "$current" = "$LJ_CI_BUILT_ARCHIVE_SHA" || {
    echo "$LJ_CI_NAME archive changed after profile build" >&2
    return 1
  }
  runtime_current=$(lj_ci_sha256 "$LJ_CI_RUNTIME") || return $?
  test "$runtime_current" = "$LJ_CI_BUILT_RUNTIME_SHA" || {
    echo "$LJ_CI_NAME Lua runtime changed after profile build" >&2
    return 1
  }
  fixture_sha=$(lj_ci_sha256 "$fixture") || return $?
  expected_stamp=$(printf 'profile=%s\narchive=%s\nfixture=%s\n' \
    "$LJ_CI_PROFILE_ID" "$LJ_CI_BUILT_ARCHIVE_SHA" "$fixture_sha")
  actual_stamp=$(cat "$fixture.profile") || return $?
  test "$actual_stamp" = "$expected_stamp" || {
    echo "$LJ_CI_NAME fixture/profile identity mismatch" >&2
    return 1
  }
  echo "$LJ_CI_NAME run profile=$LJ_CI_PROFILE_ID"
  shift 2
  perl -MTime::HiRes=alarm -e 'alarm shift; exec @ARGV; die "exec: $!\n"' \
    "$timeout" "$fixture" "$@"
}

# Run Lua code or a registered test suite under the exact runtime profile most
# recently built by lj_ci_build_profile or lj_ci_restore.
lj_ci_run_lua() {
  timeout=$1
  shift
  test -n "$LJ_CI_PROFILE_ID" || {
    echo "$LJ_CI_NAME attempted Lua run without a profile build" >&2
    return 1
  }
  archive_sha=$(lj_ci_sha256 "$LJ_CI_ARCHIVE") || return $?
  runtime_sha=$(lj_ci_sha256 "$LJ_CI_RUNTIME") || return $?
  test "$archive_sha" = "$LJ_CI_BUILT_ARCHIVE_SHA" || {
    echo "$LJ_CI_NAME archive changed after profile build" >&2
    return 1
  }
  test "$runtime_sha" = "$LJ_CI_BUILT_RUNTIME_SHA" || {
    echo "$LJ_CI_NAME Lua runtime changed after profile build" >&2
    return 1
  }
  echo "$LJ_CI_NAME Lua run profile=$LJ_CI_PROFILE_ID"
  perl -MTime::HiRes=alarm -e 'alarm shift; exec @ARGV; die "exec: $!\n"' \
    "$timeout" "$LJ_CI_RUNTIME" "$@"
}

lj_ci_restore() {
  target_flags=$1
  xcflags=$2
  env MACOSX_DEPLOYMENT_TARGET="$LJ_CI_MINVER" \
    make -C "$LJ_CI_ROOT/src" clean TARGET_SYS="$LJ_CI_TARGET_SYS" \
      CC="$LJ_CI_CC" TARGET_FLAGS="$target_flags" \
      CFLAGS="$LJ_CI_CFLAGS" CPPFLAGS="$LJ_CI_CPPFLAGS" \
      LDFLAGS="$LJ_CI_LDFLAGS" XCFLAGS="$xcflags" || return $?
  env MACOSX_DEPLOYMENT_TARGET="$LJ_CI_MINVER" \
    make -C "$LJ_CI_ROOT/src" -j"$LJ_CI_JOBS" \
      TARGET_SYS="$LJ_CI_TARGET_SYS" CC="$LJ_CI_CC" \
      TARGET_FLAGS="$target_flags" CFLAGS="$LJ_CI_CFLAGS" \
      CPPFLAGS="$LJ_CI_CPPFLAGS" LDFLAGS="$LJ_CI_LDFLAGS" \
      XCFLAGS="$xcflags" || return $?
  case " $target_flags " in
    *' -arch arm64e '*) expected=arm64e ;;
    *) expected=arm64 ;;
  esac
  actual=$(lipo -archs "$LJ_CI_ARCHIVE") || return $?
  if test "$actual" != "$expected"; then
    echo "$LJ_CI_NAME restore expected $expected archive, got '$actual'" >&2
    return 1
  fi
  runtime_arch=$(lipo -archs "$LJ_CI_RUNTIME") || return $?
  if test "$runtime_arch" != "$expected"; then
    echo "$LJ_CI_NAME restore expected $expected runtime, got '$runtime_arch'" >&2
    return 1
  fi
  LJ_CI_ARCH=$expected
  LJ_CI_TARGET_FLAGS=$target_flags
  LJ_CI_XCFLAGS=$xcflags
  LJ_CI_BUILT_ARCHIVE_SHA=$(lj_ci_sha256 "$LJ_CI_ARCHIVE") || return $?
  LJ_CI_BUILT_RUNTIME_SHA=$(lj_ci_sha256 "$LJ_CI_RUNTIME") || return $?
  lj_ci_profile_hash restore || return $?
  echo "$LJ_CI_NAME restored profile=$LJ_CI_PROFILE_ID target=$expected flags='$xcflags'"
  LJ_CI_RESTORE_NEEDED=0
}

lj_ci_finish() {
  saved_status=$1
  trap - EXIT HUP INT TERM
  if test "${LJ_CI_RESTORE_NEEDED:-0}" = 1; then
    lj_ci_restore "$LJ_CI_RESTORE_TARGET_FLAGS" "$LJ_CI_RESTORE_XCFLAGS" \
      >/dev/null 2>&1 || saved_status=1
  fi
  if test -n "${LJ_CI_TMPDIR:-}"; then rm -rf "$LJ_CI_TMPDIR"; fi
  if test "${LJ_CI_LOCK_HELD:-0}" = 1; then
    rm -f "$LJ_CI_LOCK_DIR/owner"
    rmdir "$LJ_CI_LOCK_DIR" 2>/dev/null || true
  fi
  exit "$saved_status"
}

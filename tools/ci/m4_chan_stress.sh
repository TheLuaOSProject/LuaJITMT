#!/bin/sh
# Run the Lua-defined focused M4 channel substrate stress test.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if hits=$(grep -n 'la_cpu_pause[[:space:]]*(' "$ROOT/src/lj_chan.c" || true); \
    [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'channel waits must park through native-state futex waits, not spin on la_cpu_pause()' >&2
  exit 1
fi

check_channel_wait() {
  fn=$1
  wait=$2
  if ! awk -v fn="$fn" -v wait="$wait" '
    function track_braces(line) {
      opens = gsub(/\{/, "{", line)
      line = $0
      closes = gsub(/\}/, "}", line)
      if (opens)
	body = 1
      depth += opens - closes
      if (body && depth == 0)
	in_fn = 0
    }
    !in_fn && $0 ~ fn "[[:space:]]*\\(" {
      in_fn = 1; body = 0; depth = 0
    }
    in_fn && $0 ~ wait "[[:space:]]*\\(" { found = 1 }
    in_fn { track_braces($0) }
    END { exit found ? 0 : 1 }
  ' "$ROOT/src/lj_chan.c"; then
    printf '%s\n' "src/lj_chan.c:$fn must wait via $wait()" >&2
    exit 1
  fi
}

check_channel_wait chan_wait_rendezvous_send chan_wait_timeout
check_channel_wait lj_chan_send chan_wait
check_channel_wait lj_chan_recv chan_wait
check_channel_wait lj_chan_recv_gc chan_wait
check_channel_wait lj_chan_send_timeout chan_wait_timeout
check_channel_wait lj_chan_recv_timeout chan_wait_timeout
check_channel_wait lj_chan_recv_timeout_gc chan_wait_timeout

exec "$ROOT/tools/ci/lua_test.sh" m4_chan_stress

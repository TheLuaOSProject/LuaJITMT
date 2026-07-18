#!/bin/sh
# Reject retired color-collector entry points in target sources and artifacts.
# src/host/minilua.c is deliberately excluded: minilua is a build-time source
# generator/bootstrap executable and is never linked into a LuaJIT target.

set -eu

script_dir=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
root=$(CDPATH= cd -P "$script_dir/../.." && pwd)
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-gc2-symbols.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

deny='gc_mark gc_mark_claim_white gc_mark_start gc_propagate_gray propagatemark gc_sweep gc_sweepstr gc_fullsweep gc_onestep gc_atomic gc_traverse_tab gc_traverse_func gc_traverse_proto gc_traverse_thread gc_traverse_trace gc_traverse_udata lj_gc_fullgc lj_gc_markobj lj_gc_markobj_deep lj_gc_preserveobj lj_gc_mark_trace_slot lj_gc_sweep_gc2_all_arena_bodies lj_gc_resurrect_if_dead'
deny_ere=$(printf '%s\n' "$deny" | sed 's/[[:space:]][[:space:]]*/|/g')
token_ere="(^|[^[:alnum:]_])($deny_ere)([^[:alnum:]_]|$)"
source_hits="$tmpdir/source-hits"
state_hits="$tmpdir/state-hits"
: >"$source_hits"
: >"$state_hits"

cd "$root"
for source in $(find src -type f \
    \( -name '*.c' -o -name '*.h' -o -name '*.dasc' -o -name '*.S' \) \
    ! -path 'src/host/minilua.c' -print); do
  if LC_ALL=C grep -nHE "$token_ere" "$source" >>"$source_hits"; then
    :
  else
    rc=$?
    if [ "$rc" -ne 1 ]; then
      echo "gc2 retired-symbol gate: cannot scan $source" >&2
      exit 2
    fi
  fi

  # g->gc.state is retained for layout/source compatibility only. Strip C
  # comments and literals, then recognize assignments from the token stream so
  # whitespace, line breaks and parenthesized lvalues cannot hide a write.
  case "$source" in
  *.c|*.h)
    if LC_ALL=C awk '
      function state_hit(line, why) {
        print FILENAME ":" line ": " why
      }

      function shift_token(tok) {
        prev6 = prev5
        prev5 = prev4
        prev4 = prev3
        prev3 = prev2
        prev2 = prev1
        prev1 = tok
      }

      function is_write_op(tok) {
        return tok == "++" || tok == "--" || tok == "+=" ||
               tok == "-=" || tok == "*=" || tok == "/=" ||
               tok == "%=" || tok == "<<=" || tok == ">>=" ||
               tok == "&=" || tok == "^=" || tok == "|="
      }

      function consume_token(tok, line) {
        # Validate the complete RHS, not just its first token. This rejects
        # expressions such as GCSpause + 1 while accepting redundant parens.
        if (assign_rhs == 1) {
          if (tok == "(") {
            rhs_parens++
            shift_token(tok)
            return
          }
          if (tok == "GCSpause") {
            assign_rhs = 2
            shift_token(tok)
            return
          }
          state_hit(state_line,
            "legacy gc.state write assigns something other than GCSpause")
          assign_rhs = 0
          rhs_parens = 0
        } else if (assign_rhs == 2) {
          if (tok == ")" && rhs_parens > 0) {
            rhs_parens--
            shift_token(tok)
            return
          }
          if (rhs_parens == 0 &&
              (tok == ";" || tok == "," || tok == ")")) {
            assign_rhs = 0
            shift_token(tok)
            return
          }
          state_hit(state_line,
            "legacy gc.state write assigns an expression other than GCSpause")
          assign_rhs = 0
          rhs_parens = 0
        }

        if (state_lvalue) {
          # Closing parens are valid between a parenthesized lvalue and its
          # assignment operator: (g->gc.state) = GCSpause.
          if (tok == ")") {
            shift_token(tok)
            return
          }
          if (tok == "=") {
            state_lvalue = 0
            assign_rhs = 1
            rhs_parens = 0
            shift_token(tok)
            return
          }
          if (is_write_op(tok))
            state_hit(state_line,
              "legacy gc.state is modified instead of assigned GCSpause")
          state_lvalue = 0
        }

        # Match any pointer expression ending in ->gc.state. The normal form
        # is g->gc.state, but accepting any base closes trivial alias spelling
        # holes in this source-level invariant.
        if (tok == "state" && prev1 == "." && prev2 == "gc" &&
            prev3 == "->") {
          state_lvalue = 1
          state_line = line
          if (prev5 == "++" || prev5 == "--" ||
              (prev5 == "(" && (prev6 == "++" || prev6 == "--"))) {
            state_hit(state_line,
              "legacy gc.state is modified instead of assigned GCSpause")
            state_lvalue = 0
          }
        }

        shift_token(tok)
      }

      function lex_line(text, line,    c, nextc, op, i, n, start) {
        n = length(text)
        for (i = 1; i <= n; ) {
          c = substr(text, i, 1)
          nextc = i < n ? substr(text, i + 1, 1) : ""

          if (in_comment) {
            if (c == "*" && nextc == "/") {
              in_comment = 0
              i += 2
            } else {
              i++
            }
            continue
          }

          if (quote != "") {
            if (escaped) {
              escaped = 0
            } else if (c == "\\") {
              escaped = 1
            } else if (c == quote) {
              quote = ""
              consume_token("@literal@", quote_line)
            }
            i++
            continue
          }

          if (c == "/" && nextc == "*") {
            in_comment = 1
            i += 2
            continue
          }
          if (c == "/" && nextc == "/")
            break
          if (c == "\"" || c == squote) {
            quote = c
            quote_line = line
            escaped = 0
            i++
            continue
          }
          if (c ~ /[[:space:]]/) {
            i++
            continue
          }
          if (c ~ /[[:alpha:]_]/) {
            start = i
            do {
              i++
              c = i <= n ? substr(text, i, 1) : ""
            } while (c ~ /[[:alnum:]_]/)
            consume_token(substr(text, start, i - start), line)
            continue
          }

          op = i + 2 <= n ? substr(text, i, 3) : ""
          if (op == "<<=" || op == ">>=") {
            consume_token(op, line)
            i += 3
            continue
          }
          op = i + 1 <= n ? substr(text, i, 2) : ""
          if (op == "->" || op == "++" || op == "--" ||
              op == "+=" || op == "-=" || op == "*=" ||
              op == "/=" || op == "%=" || op == "&=" ||
              op == "^=" || op == "|=" || op == "==" ||
              op == "!=" || op == "<=" || op == ">=" ||
              op == "&&" || op == "||" || op == "<<" || op == ">>") {
            consume_token(op, line)
            i += 2
            continue
          }

          consume_token(c, line)
          i++
        }
        # A backslash at physical end-of-line escapes the newline, not the
        # first character of the continuation record.
        if (quote != "" && escaped)
          escaped = 0
      }

      BEGIN { squote = sprintf("%c", 39) }
      { lex_line($0, FNR) }
      END {
        if (assign_rhs != 0)
          state_hit(state_line,
            "legacy gc.state assignment is incomplete; expected GCSpause")
      }
    ' "$source" >>"$state_hits"; then
      :
    else
      echo "gc2 retired-symbol gate: cannot check gc.state writes in $source" >&2
      exit 2
    fi
    ;;
  esac
done

if [ -s "$source_hits" ]; then
  echo "gc2 retired-symbol gate: retired entry point remains in target source:" >&2
  sed 's/^/  /' "$source_hits" >&2
  exit 1
fi

if [ -s "$state_hits" ]; then
  echo "gc2 retired-symbol gate: non-idle legacy gc.state write remains in target source:" >&2
  sed 's/^/  /' "$state_hits" >&2
  exit 1
fi

if [ "$#" -eq 0 ]; then
  echo "gc2 retired-symbol gate passed (target sources; host/minilua excluded)"
  exit 0
fi

if [ -n "${NM:-}" ]; then
  nm_tool=$NM
elif command -v llvm-nm >/dev/null 2>&1; then
  nm_tool=$(command -v llvm-nm)
elif command -v nm >/dev/null 2>&1; then
  nm_tool=$(command -v nm)
else
  echo "gc2 retired-symbol gate: neither llvm-nm nor nm is available" >&2
  exit 2
fi

artifact_hits="$tmpdir/artifact-hits"
nm_output="$tmpdir/nm-output"
: >"$artifact_hits"
for artifact in "$@"; do
  if [ ! -f "$artifact" ]; then
    echo "gc2 retired-symbol gate: missing target artifact: $artifact" >&2
    exit 2
  fi
  if ! "$nm_tool" -a "$artifact" >"$nm_output" 2>&1; then
    echo "gc2 retired-symbol gate: $nm_tool cannot inspect $artifact" >&2
    sed 's/^/  /' "$nm_output" >&2
    exit 2
  fi
  awk -v artifact="$artifact" -v deny="$deny" '
    BEGIN {
      n = split(deny, names, " ")
    }
    {
      symbol = $NF
      sub(/^_+/, "", symbol)
      sub(/^imp_+/, "", symbol)
      sub(/^_+/, "", symbol)
      for (i = 1; i <= n; i++) {
        name = names[i]
        suffix = substr(symbol, length(name) + 1, 1)
        if (symbol == name ||
            (substr(symbol, 1, length(name)) == name &&
             (suffix == "." || suffix == "$" || suffix == "@"))) {
          print artifact ": " $0
          break
        }
      }
    }
  ' "$nm_output" >>"$artifact_hits"
done

if [ -s "$artifact_hits" ]; then
  echo "gc2 retired-symbol gate: retired entry point remains in target artifact:" >&2
  sed 's/^/  /' "$artifact_hits" >&2
  exit 1
fi

echo "gc2 retired-symbol gate passed (target artifacts; host/minilua excluded)"

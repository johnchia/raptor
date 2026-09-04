#!/bin/bash
# json-gate: no hand-written JSON in production C.
#
# Structured formats are built by serializers, never string assembly:
# a "%s" into a JSON literal is one edit away from injection, and the
# safety of today's literal requires provenance reasoning no reviewer
# should have to repeat.
#
# THE RULE IS TWO RULES, and json-scan.py beside this file implements
# both over runs of concatenated string literals rather than over
# lines:
#
#   hand-written JSON       a run opening an object onto a quoted key.
#                           The convention: documents come from cJSON,
#                           constant ones included, because it is the
#                           next edit that adds the "%s".
#   interpolated into JSON  a run carrying a JSON key separator and a
#                           printf conversion. This is the one with
#                           teeth -- a document assembled around a
#                           value at runtime, whatever the braces look
#                           like.
#
# The line grep this replaced matched `{\"` on a single line, which is
# the idiomatic spelling and nothing else: a space after the brace, a
# newline, the brace in a macro or the brace as an argument all went
# through it, and so did any run whose conversion sat on a later line
# than its brace. C glues adjacent literals together, so the run is the
# unit that matters.
#
# Three documented exemptions: raptorctl_help.c prints example -j
# syntax (display text, not construction), raptor-ipc's transport error
# frame in rss_ctrl.c (a dependency-free layer emitting a constant
# shape with one integer), and a .cmd_tpl assignment -- a Home
# Assistant command template, whose value is a Jinja expression rather
# than a JSON value, so the text is not a document until Home Assistant
# renders it and no serializer can produce it. Only the template is
# exempt; the finished payloads beside it in rmq_ha.c are built by
# cJSON.
#
# Single source of truth: test-all.sh stage 0 and the conformity git
# hooks both call this script, so the rule cannot drift between them.
#
# Usage:
#   json-gate.sh --tree <dir>...    scan whole trees (CI / suite mode)
#   json-gate.sh --files <file>...  scan specific files (hook mode)
#
# Prints violations and exits 1 when any exist; silent exit 0 when clean.
set -u

MODE=${1:---tree}
shift || true
[ $# -ge 1 ] || exit 0

# Two kinds of exemption, and they are not interchangeable: EXEMPT_FILES
# excuses a whole file, EXEMPT_LINES excuses one construct wherever it
# appears. Both modes below apply both, so the hooks and the suite agree
# about what passes -- the file filter alone used to let a construct
# through on --tree and flag it on --files.
#
# EXEMPT_LINES is read by grep -E and by python's re, so it stays in the
# dialect both understand: [ \t] rather than [[:space:]] or \s.
EXEMPT_FILES='raptorctl_help\.c|rss_ctrl\.c'
EXEMPT_LINES='\.cmd_tpl[ \t]*='
EXCLUDE_DIRS='/tests/|/fuzz/|/\.deps/|/build/|/asan-out/|/third_party/'

SCAN="$(dirname "$0")/json-scan.py"

case "$MODE" in
--tree)
    FILES=$(find "$@" -type f \( -name '*.c' -o -name '*.h' \) 2> /dev/null |
        grep -vE "$EXCLUDE_DIRS" | grep -vE "$EXEMPT_FILES" || true)
    ;;
--files)
    FILES=$(printf '%s\n' "$@" | grep -E '\.(c|h)$' |
        grep -vE "$EXCLUDE_DIRS" | grep -vE "^(tests|fuzz)/" | grep -vE "$EXEMPT_FILES" || true)
    ;;
*)
    echo "json-gate: unknown mode $MODE" >&2
    exit 2
    ;;
esac

[ -n "$FILES" ] || exit 0

# The suite exercises the rule itself before applying it. A gate that
# quietly stops matching is worse than no gate -- this one did exactly
# that for three weeks -- and the cases are cheap. Not in --files mode:
# the hooks are meant to be instant.
if [ "$MODE" = --tree ] && command -v python3 > /dev/null 2>&1 && [ -f "$SCAN" ]; then
    if ! OUT=$(python3 "$SCAN" --selftest); then
        printf '%s\n' "$OUT" >&2
        echo "json-gate: the rule's own cases fail; fix json-scan.py before trusting it" >&2
        exit 1
    fi
fi

# Fails open the way every conformity check does -- but open here means
# the old single-line rule, not nothing: a machine without python3 still
# refuses the spelling that actually occurs.
if command -v python3 > /dev/null 2>&1 && [ -f "$SCAN" ]; then
    # shellcheck disable=SC2086
    HITS=$(python3 "$SCAN" "$EXEMPT_LINES" $FILES || true)
else
    echo "json-gate: python3 not found; falling back to the single-line rule" >&2
    # shellcheck disable=SC2086
    HITS=$(/usr/bin/grep -n '{\\"' $FILES 2> /dev/null | grep -vE "$EXEMPT_LINES" || true)
fi

if [ -n "$HITS" ]; then
    echo "$HITS"
    echo "Build JSON with cJSON (rss_ctrl_cmd*/rss_ctrl_resp_*), never by hand." >&2
    exit 1
fi
exit 0

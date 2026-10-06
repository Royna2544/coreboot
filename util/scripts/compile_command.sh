#!/usr/bin/env sh
## SPDX-License-Identifier: GPL-2.0-only

# Record one compiler invocation as a compilation database entry, then run it.
#
# Usage: compile_command.sh <entry.json> <source> <compiler> [args...]
#
# The entry is written in the format of a single element of
# compile_commands.json. A leading ccache wrapper is left out of the
# recorded arguments, but is still used to run the command.

ENTRY="$1"
SOURCE="$2"
shift 2

RECORD_START=1
case "${1##*/}" in
ccache) RECORD_START=2 ;;
esac

printf '%s\n' "${PWD}" "${SOURCE}" "$@" | awk -v start="${RECORD_START}" '
function esc(s) {
	gsub(/\\/, "\\\\", s)
	gsub(/"/, "\\\"", s)
	gsub(/\t/, "\\t", s)
	return s
}
NR == 1 { dir = esc($0); next }
NR == 2 { file = esc($0); next }
NR - 2 >= start { args = args (args == "" ? "" : ", ") "\"" esc($0) "\"" }
END {
	printf "{\"directory\": \"%s\", \"file\": \"%s\", \"arguments\": [%s]}\n", dir, file, args
}' > "${ENTRY}" || exit 1

exec "$@"

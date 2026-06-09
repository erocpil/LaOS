#!/usr/bin/env bash

set -euo pipefail

if (($# == 0)); then
	echo "usage: $0 <task.conf>..." >&2
	exit 2
fi

status=0

for file in "$@"; do
	line_no=0
	while IFS= read -r line || [[ -n "$line" ]]; do
		line_no=$((line_no + 1))

		# Trim leading/trailing horizontal whitespace for classification.
		trimmed="${line#"${line%%[!$' \t']*}"}"
		trimmed="${trimmed%"${trimmed##*[!$' \t']}"}"

		[[ -z "$trimmed" || "${trimmed:0:1}" == "#" ]] && continue

		if [[ "${trimmed:0:1}" == "@" ]]; then
			if [[ "$trimmed" =~ ^@version[[:space:]]+1([[:space:]]*(#.*)?)?$ ]]; then
				continue
			fi
			if [[ "$trimmed" =~ ^@module_missing[[:space:]]+(skip|panic)([[:space:]]*(#.*)?)?$ ]]; then
				continue
			fi
			if [[ "$trimmed" =~ ^@test[[:space:]]+[a-zA-Z_][a-zA-Z0-9_]*([[:space:]]+[a-zA-Z_][a-zA-Z0-9_]*=[^[:space:]]+)*([[:space:]]*(#.*)?)?$ ]]; then
				continue
			fi
			echo "$file:$line_no: unsupported directive: $trimmed" >&2
			status=1
			continue
		fi

		read -r cpu module type magic _rest <<< "$trimmed"
		if [[ -z "${cpu:-}" || -z "${module:-}" || -z "${type:-}" || -z "${magic:-}" ]]; then
			echo "$file:$line_no: expected CPU MODULE[:ENTRY] TYPE MAGIC [ARGS...]" >&2
			status=1
			continue
		fi
		if [[ ! "$cpu" =~ ^[0-9]+$ ]]; then
			echo "$file:$line_no: invalid CPU: $cpu" >&2
			status=1
		fi
		if [[ ! "$module" =~ ^[^[:space:]:]+(:[^[:space:]:]+)?$ ]]; then
			echo "$file:$line_no: invalid MODULE[:ENTRY]: $module" >&2
			status=1
		fi
		if [[ ! "$type" =~ ^[0-9]+$ ]]; then
			echo "$file:$line_no: invalid TYPE: $type" >&2
			status=1
		fi
		if [[ ! "$magic" =~ ^(0[xX])?[0-9a-fA-F]+$ ]]; then
			echo "$file:$line_no: invalid MAGIC: $magic" >&2
			status=1
		fi
	done < "$file"
done

if ((status == 0)); then
	echo "  PASS: task.conf DSL v1 fixtures"
fi

exit "$status"

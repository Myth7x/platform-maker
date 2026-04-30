#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ $# -lt 1 ]]; then
	echo "Usage: $0 <server|client> [args...]"
	exit 1
fi

TARGET="$1"
shift

case "${TARGET}" in
	server)
		exec "${ROOT_DIR}/output/bin/opm_server" "$@"
		;;
	client)
		exec "${ROOT_DIR}/output/bin/opm_client" "$@"
		;;
	*)
		echo "Unknown target: ${TARGET}"
		echo "Usage: $0 <server|client> [args...]"
		exit 1
		;;
esac

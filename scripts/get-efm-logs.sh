#!/usr/bin/env bash
# get-efm-logs.sh
# Tails the EFM application log over SSH. Useful for diagnosing what EFM
# is doing while MicroFi agents are heartbeating.
#
# Usage:
#   ./get-efm-logs.sh
#
# Requires: sshpass  (apt install sshpass  /  brew install hudochenkov/sshpass/sshpass)

set -euo pipefail

# Resolve this script's directory so the secrets file is found regardless of cwd.
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
SECRETS_FILE="${SCRIPT_DIR}/secrets.local.sh"

if [[ ! -f "$SECRETS_FILE" ]]; then
    echo "Missing $SECRETS_FILE. Copy secrets.local.sh.example and fill in your values." >&2
    exit 1
fi
# shellcheck source=/dev/null
source "$SECRETS_FILE"

if ! command -v sshpass >/dev/null 2>&1; then
    echo "sshpass not found. Install with:" >&2
    echo "  Debian/Ubuntu: sudo apt install sshpass" >&2
    echo "  macOS:         brew install hudochenkov/sshpass/sshpass" >&2
    exit 1
fi

# Use SSHPASS env var rather than -p to keep the password out of `ps` output.
SSHPASS="$SSH_PASSWORD" sshpass -e \
    ssh -o StrictHostKeyChecking=accept-new \
        "${SSH_USER}@${EFM_HOST}" \
        "tail -400 ~/efm/logs/efm-app.log"

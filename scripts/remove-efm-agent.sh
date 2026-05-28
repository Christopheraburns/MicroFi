#!/usr/bin/env bash
# remove-efm-agent.sh
# Deletes an EFM agent and its agent class in the correct order (agent first).
#
# Usage:
#   ./remove-efm-agent.sh <agent-id> <agent-class>
#
# Example:
#   ./remove-efm-agent.sh microfi-prime ESP32

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 <agent-id> <agent-class>" >&2
    echo "Example: $0 microfi-prime ESP32" >&2
    exit 1
fi

AGENT_ID="$1"
AGENT_CLASS="$2"

# Resolve this script's directory so the secrets file is found regardless of cwd.
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
SECRETS_FILE="${SCRIPT_DIR}/secrets.local.sh"

if [[ ! -f "$SECRETS_FILE" ]]; then
    echo "Missing $SECRETS_FILE. Copy secrets.local.sh.example and fill in your values." >&2
    exit 1
fi
# shellcheck source=/dev/null
source "$SECRETS_FILE"

echo "Deleting agent '${AGENT_ID}'..."
curl --fail --silent --show-error -X DELETE "${EFM_BASE_URL}/agents/${AGENT_ID}"
echo "Agent '${AGENT_ID}' deleted."

echo "Deleting agent class '${AGENT_CLASS}'..."
curl --fail --silent --show-error -X DELETE "${EFM_BASE_URL}/agent-classes/${AGENT_CLASS}"
echo "Agent class '${AGENT_CLASS}' deleted."

#!/usr/bin/env bash
# reset-dev-environment.sh
#
# MicroFi dev-reset: recompiles and reflashes the ESP32, then wipes EFM
# state and restarts EFM so the new agent is the first to register.
# Bash port of Reset-DevEnvironment.ps1.
#
# Modes (--mode):
#   full       (default)  Increment agent ID, recompile firmware, flash
#                         ESP32, wipe LittleFS, THEN stop EFM, wipe DB,
#                         restart EFM, and open serial monitor once EFM
#                         is ready.
#
#                         Order matters: flashing first ensures the old
#                         firmware stops heartbeating before EFM is wiped,
#                         so only the new agent ID ever registers on the
#                         fresh DB. Resetting EFM first caused the old
#                         agent to re-register as an orphan during the
#                         compile window.
#
#   efm-only             Stop EFM, wipe DB, restart EFM only. Use when you
#                        need a clean EFM state without touching firmware.
#                        Does NOT wipe the device filesystem.
#
#   flash-only           Increment agent ID, recompile, flash, wipe
#                        LittleFS, monitor. Use when EFM state is fine
#                        and you only changed firmware code.
#
# Flags:
#   --skip-littlefs-wipe  Skip the LittleFS filesystem wipe that normally
#                         runs after the firmware upload in full and
#                         flash-only modes. Use when you deliberately want
#                         to preserve persisted files (.flowdef, .flowid,
#                         FlowFile records) across a reflash.
#
# Usage:
#   ./reset-dev-environment.sh                           # full mode
#   ./reset-dev-environment.sh --mode efm-only
#   ./reset-dev-environment.sh --mode flash-only
#   ./reset-dev-environment.sh --skip-littlefs-wipe      # keep filesystem
#
# Requires: sshpass, curl, sed, grep, and the PlatformIO CLI on PATH or at
# the path configured in secrets.local.sh.

set -euo pipefail

# --- ARG PARSING --------------------------------------------------------------

MODE="full"
SKIP_LITTLEFS_WIPE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode)
            MODE="$2"
            shift 2
            ;;
        --mode=*)
            MODE="${1#*=}"
            shift
            ;;
        --skip-littlefs-wipe)
            SKIP_LITTLEFS_WIPE=1
            shift
            ;;
        -h|--help)
            sed -n '2,45p' "$0" | sed 's/^# //; s/^#//'
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            echo "Usage: $0 [--mode full|efm-only|flash-only] [--skip-littlefs-wipe]" >&2
            exit 1
            ;;
    esac
done

case "$MODE" in
    full|efm-only|flash-only) ;;
    *)
        echo "Invalid --mode: $MODE  (use: full, efm-only, flash-only)" >&2
        exit 1
        ;;
esac

# --- CONFIGURATION ------------------------------------------------------------

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
SECRETS_FILE="${SCRIPT_DIR}/secrets.local.sh"

if [[ ! -f "$SECRETS_FILE" ]]; then
    echo "Missing $SECRETS_FILE. Copy secrets.local.sh.example and fill in your values." >&2
    exit 1
fi
# shellcheck source=/dev/null
source "$SECRETS_FILE"

# Values from secrets.local.sh (gitignored): EFM_HOST, SSH_USER, SSH_PASSWORD,
# EFM_BASE_URL, PIO_EXE.

EFM_HOME="~/efm"             # remote path; tilde is expanded by the remote shell
EFM_PORT=10090

# MicroFi project root is one level up from this script's directory.
MICROFI_DIR="$( cd "${SCRIPT_DIR}/.." && pwd )"
PIO_ENV="esp32s3"

EFM_READY_TIMEOUT_SEC=90
EFM_POLL_INTERVAL_SEC=5

# --- HELPERS ------------------------------------------------------------------

# ANSI color codes; honored by any terminal that understands them.
C_CYAN="\033[1;36m"
C_GREEN="\033[1;32m"
C_RED="\033[1;31m"
C_GRAY="\033[0;37m"
C_DGREEN="\033[2;32m"
C_DCYAN="\033[2;36m"
C_RESET="\033[0m"

write_step() { printf "\n  ${C_CYAN}>> %s${C_RESET}\n" "$1"; }
write_ok()   { printf "    ${C_GREEN}[OK] %s${C_RESET}\n" "$1"; }
write_fail() { printf "    ${C_RED}[FAIL] %s${C_RESET}\n" "$1" >&2; }
write_info() { printf "    ${C_GRAY}%s${C_RESET}\n" "$1"; }

# Run a remote command over SSH; fail loudly on non-zero exit.
invoke_ssh() {
    local command="$1"
    local desc="$2"
    write_info "$desc"
    if ! SSHPASS="$SSH_PASSWORD" sshpass -e \
            ssh -o StrictHostKeyChecking=accept-new \
                "${SSH_USER}@${EFM_HOST}" \
                "$command"; then
        write_fail "SSH command failed: $desc"
        write_fail "  command: $command"
        exit 1
    fi
}

# Same as invoke_ssh but allows non-zero exit (used for best-effort stops/cleanups).
invoke_ssh_soft() {
    local command="$1"
    local desc="$2"
    write_info "$desc"
    SSHPASS="$SSH_PASSWORD" sshpass -e \
        ssh -o StrictHostKeyChecking=accept-new \
            "${SSH_USER}@${EFM_HOST}" \
            "$command" || true
}

# --- MAIN ---------------------------------------------------------------------

printf "\n${C_DCYAN}================================================${C_RESET}\n"
printf "${C_DCYAN}   MicroFi Dev Environment Reset  [%s]      ${C_RESET}\n" "$MODE"
printf "${C_DCYAN}================================================${C_RESET}\n"

NEW_ID=""   # set by agent-ID section; may stay empty in efm-only mode

# ---- PIO BLOCK (skipped in efm-only mode) ------------------------------------
# Runs BEFORE the EFM block in full mode so the old firmware stops
# heartbeating before EFM is wiped. This prevents the old agent ID from
# re-registering on the fresh DB during the compile window.
if [[ "$MODE" != "efm-only" ]]; then

    # 1. Verify PlatformIO CLI is reachable.
    write_step "Locating PlatformIO"
    if [[ -x "$PIO_EXE" ]]; then
        write_ok "Found: $PIO_EXE"
    elif command -v pio >/dev/null 2>&1; then
        PIO_EXE="$(command -v pio)"
        write_ok "Found on PATH: $PIO_EXE"
    else
        write_fail "PlatformIO CLI not found at '$PIO_EXE' and not on PATH."
        write_fail "Update PIO_EXE in secrets.local.sh, or install PlatformIO."
        exit 1
    fi

    if [[ ! -d "$MICROFI_DIR" ]]; then
        write_fail "MicroFi project not found at: $MICROFI_DIR"
        exit 1
    fi

    # 2. Increment the agent ID in sdkconfig so EFM sees a fresh registration.
    #    Reads CONFIG_MICROFI_AGENT_ID from sdkconfig.<env>, bumps the trailing
    #    number (e.g. microfi-prime3 -> microfi-prime4), and writes it back.
    #    Also patches the generated sdkconfig.h in the PIO build tree so that
    #    only agent_id.cpp needs to recompile rather than the whole firmware.
    write_step "Incrementing agent ID"

    SDKCONFIG_FILE="${MICROFI_DIR}/sdkconfig.${PIO_ENV}"
    SDKCONFIG_H_FILE="${MICROFI_DIR}/.pio/build/${PIO_ENV}/config/sdkconfig.h"

    if [[ ! -f "$SDKCONFIG_FILE" ]]; then
        write_fail "sdkconfig not found at: $SDKCONFIG_FILE"
        exit 1
    fi

    CURRENT_ID="$(grep -E '^CONFIG_MICROFI_AGENT_ID="' "$SDKCONFIG_FILE" \
        | sed -E 's/^CONFIG_MICROFI_AGENT_ID="([^"]*)"$/\1/' || true)"

    if [[ -n "$CURRENT_ID" ]]; then
        # Split trailing digits from prefix. The [^0-9] in the prefix capture
        # forces the boundary, so "microfi-prime33" splits at "prime"|"33"
        # rather than greedy-eating one digit into the prefix.
        if [[ "$CURRENT_ID" =~ ^(.*[^0-9])([0-9]+)$ ]]; then
            ID_PREFIX="${BASH_REMATCH[1]}"
            ID_NUM="${BASH_REMATCH[2]}"
            NEW_ID="${ID_PREFIX}$((ID_NUM + 1))"
        elif [[ "$CURRENT_ID" =~ ^([0-9]+)$ ]]; then
            NEW_ID="$((CURRENT_ID + 1))"
        else
            NEW_ID="${CURRENT_ID}2"
        fi

        # Patch sdkconfig.<env>.  sed -i form is portable enough across GNU/BSD
        # if we feed an empty extension; macOS sed requires the empty arg.
        if sed --version >/dev/null 2>&1; then
            sed -i -E "s|^CONFIG_MICROFI_AGENT_ID=\"[^\"]*\"|CONFIG_MICROFI_AGENT_ID=\"${NEW_ID}\"|" "$SDKCONFIG_FILE"
        else
            sed -i '' -E "s|^CONFIG_MICROFI_AGENT_ID=\"[^\"]*\"|CONFIG_MICROFI_AGENT_ID=\"${NEW_ID}\"|" "$SDKCONFIG_FILE"
        fi
        write_ok "sdkconfig.${PIO_ENV} : ${CURRENT_ID} -> ${NEW_ID}"

        # Patch the generated sdkconfig.h so only agent_id.cpp recompiles.
        if [[ -f "$SDKCONFIG_H_FILE" ]]; then
            if sed --version >/dev/null 2>&1; then
                sed -i -E "s|#define CONFIG_MICROFI_AGENT_ID \"[^\"]*\"|#define CONFIG_MICROFI_AGENT_ID \"${NEW_ID}\"|" "$SDKCONFIG_H_FILE"
            else
                sed -i '' -E "s|#define CONFIG_MICROFI_AGENT_ID \"[^\"]*\"|#define CONFIG_MICROFI_AGENT_ID \"${NEW_ID}\"|" "$SDKCONFIG_H_FILE"
            fi
            write_ok "sdkconfig.h      : patched"
        else
            write_info "sdkconfig.h not found (first build?) -- PIO will generate it"
        fi
    else
        write_info "CONFIG_MICROFI_AGENT_ID not set in sdkconfig -- using MAC-derived ID"
    fi

    # 3. Clean the PlatformIO build cache before compiling.
    write_step "Cleaning PlatformIO build cache"
    write_info "Running: $PIO_EXE run --project-dir $MICROFI_DIR --target clean -e $PIO_ENV"
    if ! "$PIO_EXE" run --project-dir "$MICROFI_DIR" --target clean -e "$PIO_ENV"; then
        write_fail "Clean failed -- aborting"
        exit 1
    fi
    write_ok "Build cache cleared"

    # 4. Compile and flash the ESP32.
    write_step "Compiling and flashing ESP32"
    write_info "Running: $PIO_EXE run --project-dir $MICROFI_DIR -t upload -e $PIO_ENV"
    echo
    if ! "$PIO_EXE" run --project-dir "$MICROFI_DIR" -t upload -e "$PIO_ENV"; then
        write_fail "PlatformIO upload failed -- aborting reset"
        exit 1
    fi
    echo
    write_ok "Firmware flashed -- ESP32 is now running new agent ID"

    # 4b. Wipe the LittleFS partition (erases .flowdef, .flowid, and all
    #     persisted FlowFile records, leaving firmware intact).
    #     Skipped only if --skip-littlefs-wipe is explicitly passed.
    if [[ $SKIP_LITTLEFS_WIPE -eq 0 ]]; then
        write_step "Wiping LittleFS partition (uploadfs with empty data/)"
        write_info "Running: $PIO_EXE run --project-dir $MICROFI_DIR -t uploadfs -e $PIO_ENV"
        echo
        if ! "$PIO_EXE" run --project-dir "$MICROFI_DIR" -t uploadfs -e "$PIO_ENV"; then
            write_fail "uploadfs failed -- LittleFS NOT wiped"
        else
            write_ok "LittleFS wiped -- device will boot with no saved flow or FlowFile records"
        fi
    else
        write_info "Skipping LittleFS wipe (--skip-littlefs-wipe set)"
    fi

fi  # end PIO block

# ---- EFM BLOCK (skipped in flash-only mode) ----------------------------------
# In full mode this runs AFTER the flash so the old firmware is already
# replaced before EFM is wiped; only the new agent ID will register on the
# clean DB.
if [[ "$MODE" != "flash-only" ]]; then

    # 5. Ensure sshpass is available.
    write_step "Checking for sshpass"
    if ! command -v sshpass >/dev/null 2>&1; then
        write_fail "sshpass not found. Install with:"
        write_fail "  Debian/Ubuntu: sudo apt install sshpass"
        write_fail "  macOS:         brew install hudochenkov/sshpass/sshpass"
        exit 1
    fi
    write_ok "sshpass found"

    write_step "Connecting to ${EFM_HOST} as ${SSH_USER}"

    # 6. Stop EFM (soft -- it might already be down).
    write_step "Stopping EFM"
    invoke_ssh_soft "${EFM_HOME}/bin/efm.sh stop" "Sending stop to EFM"
    write_ok "EFM stop signal sent (or was already down)"

    # Give the JVM a moment to release file locks before we delete.
    sleep 3

    # 7. Delete the H2 database files.
    write_step "Deleting EFM database"
    DB_PATH="${EFM_HOME}/database"
    invoke_ssh_soft "ls -lh ${DB_PATH}/*.db 2>/dev/null || echo '(no .db files found)'" "Listing existing DB files"
    invoke_ssh "rm -f ${DB_PATH}/efm.mv.db ${DB_PATH}/efm.trace.db" "Removing efm.mv.db and efm.trace.db"
    write_ok "Database files deleted"

    # 8. Start EFM.
    write_step "Starting EFM"
    invoke_ssh "${EFM_HOME}/bin/efm.sh start" "Launching EFM daemon"
    write_ok "EFM start command issued"

    # 9. Poll until the EFM REST API responds.
    write_step "Waiting for EFM to become ready (timeout ${EFM_READY_TIMEOUT_SEC}s)"
    EFM_URL="http://${EFM_HOST}:${EFM_PORT}/efm/api/agent-classes"
    DEADLINE=$(( $(date +%s) + EFM_READY_TIMEOUT_SEC ))
    EFM_READY=0

    while [[ $(date +%s) -lt $DEADLINE ]]; do
        if curl --silent --fail --max-time 5 -o /dev/null "$EFM_URL"; then
            EFM_READY=1
            break
        fi
        write_info "EFM not ready yet, retrying in ${EFM_POLL_INTERVAL_SEC}s..."
        sleep "$EFM_POLL_INTERVAL_SEC"
    done

    if [[ $EFM_READY -ne 1 ]]; then
        write_fail "EFM did not become ready within ${EFM_READY_TIMEOUT_SEC}s"
        exit 1
    fi
    write_ok "EFM is up and accepting requests"

fi  # end EFM block

# ---- MONITOR / COMPLETION ----------------------------------------------------

if [[ "$MODE" == "efm-only" ]]; then
    printf "\n${C_DGREEN}================================================${C_RESET}\n"
    printf "${C_DGREEN}   EFM Reset Complete                          ${C_RESET}\n"
    printf "${C_DGREEN}================================================${C_RESET}\n\n"
    printf "  EFM:   http://%s:%s/efm/ui\n" "$EFM_HOST" "$EFM_PORT"
    printf "  Agent: will re-register on next heartbeat\n\n"
    exit 0
fi

# flash-only and full both open the serial monitor here.
# In full mode EFM is already up, so the first heartbeats are captured live.
write_step "Opening serial monitor  (Ctrl+C to exit)"
write_info "Running: $PIO_EXE run --project-dir $MICROFI_DIR -t monitor -e $PIO_ENV"
echo
"$PIO_EXE" run --project-dir "$MICROFI_DIR" -t monitor -e "$PIO_ENV"

printf "\n${C_DGREEN}================================================${C_RESET}\n"
printf "${C_DGREEN}   Reset Complete [%s]                      ${C_RESET}\n" "$MODE"
printf "${C_DGREEN}================================================${C_RESET}\n\n"
printf "  EFM:   http://%s:%s/efm/ui\n" "$EFM_HOST" "$EFM_PORT"
if [[ -n "$NEW_ID" ]]; then
    printf "  Agent: %s  (registered on first heartbeat)\n" "$NEW_ID"
fi
echo

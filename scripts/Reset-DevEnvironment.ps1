#Requires -Version 5.1
<#
.SYNOPSIS
    MicroFi dev-reset: recompiles and reflashes the ESP32, then wipes EFM
    state and restarts EFM so the new agent is the first to register.

.DESCRIPTION
    Supports three modes via the -Mode parameter:

      Full       (default) - Increment agent ID, recompile firmware, flash
                             ESP32, THEN stop EFM, wipe DB, restart EFM, and
                             open serial monitor once EFM is ready.

                             Order matters: flashing first ensures the old
                             firmware stops heartbeating before EFM is wiped,
                             so only the new agent ID ever registers on the
                             fresh DB. Resetting EFM first caused the old
                             agent to re-register as an orphan during the
                             compile window.

      EfmOnly              - Stop EFM, wipe DB, restart EFM only.
                             Use when you need a clean EFM state without
                             touching firmware (e.g. after manifest changes
                             that don't require a reflash).

      FlashOnly            - Increment agent ID, recompile, flash, monitor.
                             Use when EFM state is fine and you only changed
                             firmware code.

.PARAMETER Mode
    Full | EfmOnly | FlashOnly  (default: Full)

.NOTES
    Requires the Posh-SSH module (installed automatically on first run).
    SSH credentials are set in the CONFIGURATION section below.
    Serial monitor blocks until Ctrl+C.
#>
param(
    [ValidateSet("Full", "EfmOnly", "FlashOnly")]
    [string]$Mode = "Full"
)

# --- CONFIGURATION ------------------------------------------------------------

$secretsFile = Join-Path $PSScriptRoot 'secrets.local.ps1'
if (-not (Test-Path $secretsFile)) {
    throw "Missing $secretsFile. Copy secrets.local.ps1.example and fill in your values."
}
. $secretsFile

# Values pulled from secrets.local.ps1 (gitignored):
#   $EfmHost, $SshUser, $SshPassword

$EfmHome        = "~/efm"
$EfmPort        = 10090

$MicroFiDir = "C:\code\MicroFi"
$PioExe     = "C:\Users\chris\.platformio\penv\Scripts\pio.exe"
$PioEnv     = "esp32s3"

$EfmReadyTimeoutSec = 90
$EfmPollIntervalSec = 5

# --- HELPERS ------------------------------------------------------------------

function Write-Step([string]$msg) {
    Write-Host ""
    Write-Host "  >> $msg" -ForegroundColor Cyan
}

function Write-Ok([string]$msg) {
    Write-Host "    [OK] $msg" -ForegroundColor Green
}

function Write-Fail([string]$msg) {
    Write-Host "    [FAIL] $msg" -ForegroundColor Red
}

function Write-Info([string]$msg) {
    Write-Host "    $msg" -ForegroundColor Gray
}

function Invoke-Ssh([object]$session, [string]$command, [string]$desc) {
    Write-Info $desc
    $result = Invoke-SSHCommand -SessionId $session.SessionId -Command $command
    if ($result.ExitStatus -ne 0) {
        Write-Fail "Command failed (exit $($result.ExitStatus)): $command"
        Write-Fail "stderr: $($result.Error)"
        throw "SSH command failed: $desc"
    }
    if ($result.Output) {
        foreach ($line in $result.Output) {
            Write-Info "  $line"
        }
    }
    return $result
}

# --- MAIN ---------------------------------------------------------------------

Write-Host ""
Write-Host "================================================" -ForegroundColor DarkCyan
Write-Host "   MicroFi Dev Environment Reset  [$Mode]      " -ForegroundColor DarkCyan
Write-Host "================================================" -ForegroundColor DarkCyan

$newId = $null   # set by agent-ID section; may stay null in EfmOnly mode

# ---- PIO BLOCK (skipped in EfmOnly mode) -------------------------------------
# Runs BEFORE the EFM block in Full mode so the old firmware stops
# heartbeating before EFM is wiped. This prevents the old agent ID from
# re-registering on the fresh DB during the compile window.
if ($Mode -ne "EfmOnly") {

# 1. Verify pio.exe exists at the configured path
Write-Step "Locating PlatformIO"
if (-not (Test-Path $PioExe)) {
    Write-Fail "pio.exe not found at: $PioExe"
    Write-Fail "Update PioExe in the CONFIGURATION section at the top of this script."
    exit 1
}
Write-Ok "Found: $PioExe"

if (-not (Test-Path $MicroFiDir)) {
    Write-Fail "MicroFi project not found at: $MicroFiDir"
    exit 1
}

# 2. Increment the agent ID in sdkconfig so EFM sees a fresh registration
#    Reads CONFIG_MICROFI_AGENT_ID from sdkconfig.<env>, bumps the trailing
#    number (e.g. microfi-prime3 -> microfi-prime4), and writes it back.
#    Also patches the generated sdkconfig.h in the PIO build tree so that
#    only agent_id.cpp needs to recompile rather than the whole firmware.
Write-Step "Incrementing agent ID"

$sdkconfigFile  = "$MicroFiDir\sdkconfig.$PioEnv"
$sdkconfigHFile = "$MicroFiDir\.pio\build\$PioEnv\config\sdkconfig.h"

if (-not (Test-Path $sdkconfigFile)) {
    Write-Fail "sdkconfig not found at: $sdkconfigFile"
    exit 1
}

$sdkRaw = Get-Content $sdkconfigFile -Raw
if ($sdkRaw -match 'CONFIG_MICROFI_AGENT_ID="([^"]*)"') {
    $currentId = $Matches[1]

    # Split trailing digits from prefix: "microfi-prime3" -> "microfi-prime", 3
    if ($currentId -match '^(.*?)(\d+)$') {
        $idPrefix = $Matches[1]
        $idNum    = [int]$Matches[2]
        $newId    = $idPrefix + ($idNum + 1)
    } else {
        # No trailing number -- just append 2
        $newId = $currentId + '2'
    }

    # Patch sdkconfig.<env>
    $sdkRaw = $sdkRaw -replace `
        ('CONFIG_MICROFI_AGENT_ID="[^"]*"'), `
        ('CONFIG_MICROFI_AGENT_ID="' + $newId + '"')
    Set-Content $sdkconfigFile $sdkRaw -NoNewline
    Write-Ok "sdkconfig.$PioEnv : $currentId -> $newId"

    # Patch the generated sdkconfig.h so only agent_id.cpp recompiles
    if (Test-Path $sdkconfigHFile) {
        $hRaw = Get-Content $sdkconfigHFile -Raw
        $hRaw = $hRaw -replace `
            ('#define CONFIG_MICROFI_AGENT_ID "[^"]*"'), `
            ('#define CONFIG_MICROFI_AGENT_ID "' + $newId + '"')
        Set-Content $sdkconfigHFile $hRaw -NoNewline
        Write-Ok "sdkconfig.h      : patched"
    } else {
        Write-Info "sdkconfig.h not found (first build?) -- PIO will generate it"
    }
} else {
    Write-Info "CONFIG_MICROFI_AGENT_ID not set in sdkconfig -- using MAC-derived ID"
}

# 3. Clean the PlatformIO build cache before compiling.
#    Forces a full recompile so sdkconfig changes (flash size, PSRAM, etc.)
#    and header changes (PropertyDescriptor, etc.) are always reflected in
#    the binary. Without this, stale .o files can silently persist.
Write-Step "Cleaning PlatformIO build cache"

$pioCleanArgs = @("run", "--project-dir", $MicroFiDir, "--target", "clean")
if ($PioEnv -ne "") {
    $pioCleanArgs += @("-e", $PioEnv)
}
Write-Info "Running: $PioExe $($pioCleanArgs -join ' ')"
& $PioExe @pioCleanArgs
if ($LASTEXITCODE -ne 0) {
    Write-Fail "Clean failed (exit $LASTEXITCODE) -- aborting"
    exit $LASTEXITCODE
}
Write-Ok "Build cache cleared"

# 4. Compile and flash the ESP32 (upload only -- monitor opens later in Full
#    mode, after EFM is back up; in FlashOnly mode the monitor opens here).
Write-Step "Compiling and flashing ESP32"

$pioUploadArgs = @("run", "--project-dir", $MicroFiDir, "-t", "upload")
if ($PioEnv -ne "") {
    $pioUploadArgs += @("-e", $PioEnv)
}

Write-Info "Running: $PioExe $($pioUploadArgs -join ' ')"
Write-Host ""

& $PioExe @pioUploadArgs
$pioExit = $LASTEXITCODE

Write-Host ""
if ($pioExit -ne 0) {
    Write-Fail "PlatformIO upload failed (exit $pioExit) -- aborting reset"
    exit $pioExit
}
Write-Ok "Firmware flashed -- ESP32 is now running new agent ID"

} # end PIO block


# ---- EFM BLOCK (skipped in FlashOnly mode) -----------------------------------
# In Full mode this runs AFTER the flash so the old firmware is already
# replaced before EFM is wiped; only the new agent ID will register on the
# clean DB.
if ($Mode -ne "FlashOnly") {

# 5. Ensure Posh-SSH is available
Write-Step "Checking for Posh-SSH module"
if (-not (Get-Module -ListAvailable -Name Posh-SSH)) {
    Write-Info "Posh-SSH not found -- installing from PSGallery..."
    Install-Module -Name Posh-SSH -Scope CurrentUser -Force -AllowClobber
    Write-Ok "Posh-SSH installed"
} else {
    Write-Ok "Posh-SSH already installed"
}
Import-Module Posh-SSH -ErrorAction Stop

# 6. Build a PSCredential from the configured password.
#    New-SSHSession requires a [PSCredential] object -- a plain string won't work.
Write-Step "Preparing SSH credentials for $SshUser@$EfmHost"
$securePass = ConvertTo-SecureString $SshPassword -AsPlainText -Force
$credential = New-Object System.Management.Automation.PSCredential($SshUser, $securePass)

# 7. Open SSH session
Write-Step "Connecting to $EfmHost"
$session = New-SSHSession -ComputerName $EfmHost `
                          -Credential $credential `
                          -AcceptKey `
                          -ErrorAction Stop
Write-Ok "Connected (session $($session.SessionId))"

try {
    # 8. Stop EFM
    Write-Step "Stopping EFM"
    Invoke-SSHCommand -SessionId $session.SessionId `
        -Command "$EfmHome/bin/efm.sh stop" | Out-Null
    Write-Ok "EFM stopped (or was already down)"

    # Give the JVM a moment to release file locks before we delete
    Start-Sleep -Seconds 3

    # 9. Delete the H2 database files
    Write-Step "Deleting EFM database"
    $dbPath = "$EfmHome/database"

    $lsBefore = Invoke-SSHCommand -SessionId $session.SessionId `
        -Command "ls -lh $dbPath/*.db 2>/dev/null || echo '(no .db files found)'"
    foreach ($line in $lsBefore.Output) { Write-Info $line }

    Invoke-SSHCommand -SessionId $session.SessionId `
        -Command "rm -f $dbPath/efm.mv.db $dbPath/efm.trace.db" | Out-Null
    Write-Ok "Database files deleted"

    # 10. Start EFM
    Write-Step "Starting EFM"
    Invoke-Ssh $session "$EfmHome/bin/efm.sh start" "Launching EFM daemon"
    Write-Ok "EFM start command issued"

    # 11. Poll until EFM REST API responds
    Write-Step "Waiting for EFM to become ready (timeout ${EfmReadyTimeoutSec}s)"
    $efmUrl   = "http://${EfmHost}:${EfmPort}/efm/api/agent-classes"
    $deadline = (Get-Date).AddSeconds($EfmReadyTimeoutSec)
    $efmReady = $false

    while ((Get-Date) -lt $deadline) {
        try {
            $resp = Invoke-WebRequest -Uri $efmUrl `
                                      -UseBasicParsing `
                                      -TimeoutSec 5 `
                                      -ErrorAction Stop
            if ($resp.StatusCode -eq 200) {
                $efmReady = $true
                break
            }
        } catch {
            # Not up yet -- keep polling
        }
        Write-Info "EFM not ready yet, retrying in ${EfmPollIntervalSec}s..."
        Start-Sleep -Seconds $EfmPollIntervalSec
    }

    if (-not $efmReady) {
        Write-Fail "EFM did not become ready within ${EfmReadyTimeoutSec}s"
        throw "EFM health check timed out"
    }
    Write-Ok "EFM is up and accepting requests"

} finally {
    # Always close SSH session cleanly
    Remove-SSHSession -SessionId $session.SessionId | Out-Null
    Write-Info "SSH session closed"
}

} # end EFM block


# ---- MONITOR / COMPLETION ----------------------------------------------------

if ($Mode -eq "EfmOnly") {
    Write-Host ""
    Write-Host "================================================" -ForegroundColor DarkGreen
    Write-Host "   EFM Reset Complete                          " -ForegroundColor DarkGreen
    Write-Host "================================================" -ForegroundColor DarkGreen
    Write-Host ""
    Write-Host "  EFM:   http://${EfmHost}:${EfmPort}/efm/ui" -ForegroundColor White
    Write-Host "  Agent: will re-register on next heartbeat" -ForegroundColor White
    Write-Host ""
    return
}

# FlashOnly and Full both open the serial monitor here.
# In Full mode EFM is already up, so the first heartbeats are captured live.
Write-Step "Opening serial monitor  (Ctrl+C to exit)"

$pioMonitorArgs = @("run", "--project-dir", $MicroFiDir, "-t", "monitor")
if ($PioEnv -ne "") {
    $pioMonitorArgs += @("-e", $PioEnv)
}

Write-Info "Running: $PioExe $($pioMonitorArgs -join ' ')"
Write-Host ""

& $PioExe @pioMonitorArgs

Write-Host ""
Write-Host "================================================" -ForegroundColor DarkGreen
Write-Host "   Reset Complete [$Mode]                      " -ForegroundColor DarkGreen
Write-Host "================================================" -ForegroundColor DarkGreen
Write-Host ""
Write-Host "  EFM:   http://${EfmHost}:${EfmPo
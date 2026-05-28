# Get-EFM-Logs.ps1
# Tails the EFM application log over SSH. Useful for diagnosing what EFM
# is doing while MicroFi agents are heartbeating.
#
# Usage:
#   .\Get-EFM-Logs.ps1

Import-Module Posh-SSH

# Pull $EfmHost, $SshUser, $SshPassword from the gitignored secrets file.
$secretsFile = Join-Path $PSScriptRoot 'secrets.local.ps1'
if (-not (Test-Path $secretsFile)) {
    throw "Missing $secretsFile. Copy 
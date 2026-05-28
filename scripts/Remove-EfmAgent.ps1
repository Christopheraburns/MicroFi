# Remove-EfmAgent.ps1
# Deletes an EFM agent and its agent class in the correct order (agent first).
#
# Usage:
#   .\Remove-EfmAgent.ps1 -AgentId microfi-prime -AgentClass ESP32

param(
    [Parameter(Mandatory)][string]$AgentId,
    [Parameter(Mandatory)][string]$AgentClass
)

# Pull $EfmBaseUrl from the gitignored secrets file. ($SshUser/$SshPassword
# are also loaded but unused here — this script only talks to EFM over HTTP.)
$secretsFile = Join-Path $PSScriptRoot 'secrets.local.ps1'
if (-not (Test-Path $secretsFile)) {
    throw "Missing $secretsFile. Copy secrets.local.ps1.example and fill in your values."
}
. $secretsFile

Write-Host "Deleting agent '$AgentId'..."
Invoke-RestMethod -Method DELETE -Uri "$EfmBaseUrl/agents/$AgentId"
Write-Host "Agent '$AgentId' deleted."

Write-Host "Deleting agent class '$AgentClass'..."
Invoke-RestMethod -Method DELETE -Uri "$EfmBaseUrl/agent-classes/$AgentClass"
Write-Host "Agent class '$AgentClass' deleted."

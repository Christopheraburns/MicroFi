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
    throw "Missing $secretsFile. Copy secrets.local.ps1.example and fill in your values."
}
. $secretsFile

$securePass = ConvertTo-SecureString $SshPassword -AsPlainText -Force
$cred = New-Object System.Management.Automation.PSCredential($SshUser, $securePass)

$s = New-SSHSession -ComputerName $EfmHost -Credential $cred -AcceptKey

(Invoke-SSHCommand -SessionId $s.SessionId -Command "tail -400 ~/efm/logs/efm-app.log").Output

Remove-SSHSession -SessionId $s.SessionId

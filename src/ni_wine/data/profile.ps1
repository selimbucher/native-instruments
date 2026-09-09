# ni-wine's PowerShell profile for the Wine prefix.
#
# Installed by ni-wine over the profile.ps1 that the winetricks `powershell`
# verb ships (its powershell.exe wrapper strips -NoProfile, so this file is
# loaded for every PowerShell call in the prefix).  Native Access is the only
# PowerShell user here, and the upstream profile breaks it twice:
#
#  * Get-CimInstance is routed to a WMI shim that needs .NET Framework 4.8 and
#    otherwise pops a MessageBox (older copies) or returns nothing.  The
#    Native Access installer's "is Native Access already running?" check
#    (Get-CimInstance Win32_Process | ? Path ...) then hangs on the dialog.
#    pwsh's real CIM client cannot work under Wine at all: it needs mi.dll,
#    which Wine does not ship.
#  * Start-Process is replaced by a shim that rejects the path
#    @vscode/sudo-prompt passes as  -FilePath "'C:\...\execute.bat'"  (the
#    quotes reach the parameter literally) with "File not found", so Native
#    Access's own daemon install fails with "Please grant permission ...".
#
# Everything below is deliberately minimal and Wine-specific.

# Drop the stale ~/Documents/PowerShell/Modules entry Wine prefixes carry.
$env:PSModulePath = (($env:PSModulePath -split ';' | Select-Object -Skip 1 | Sort-Object -Unique) -join ';')

# Plain-text console output (Wine bug 49780).
Remove-Module PSReadLine -Force -ErrorAction SilentlyContinue

function Register-WMIEvent {
    [Console]::Error.WriteLine('WARNING: Register-WMIEvent is not available under Wine; ignored.')
    exit 0
}

# Win32_Process answered from Get-Process; other classes return nothing.
function Get-WineProcessList {
    Get-Process | ForEach-Object {
        [pscustomobject]@{
            ProcessId       = $_.Id
            Name            = $_.ProcessName + '.exe'
            Path            = $_.Path
            ExecutablePath  = $_.Path
            CommandLine     = $null
            ParentProcessId = $null
        }
    }
}

function Get-CimInstance {
    [CmdletBinding()]
    param(
        [Parameter(Position = 0)] [string]$ClassName,
        [string]$Namespace,
        [string]$Filter,
        [string[]]$Property,
        [string]$Query,
        [string[]]$ComputerName,
        [switch]$KeyOnly,
        [int]$OperationTimeoutSec
    )
    if (-not $ClassName -and $Query -match '(?i)\bfrom\s+(\w+)') { $ClassName = $Matches[1] }
    if ($ClassName -ieq 'Win32_Process') { return Get-WineProcessList }
    [Console]::Error.WriteLine("WARNING: Get-CimInstance $ClassName is not supported under Wine (ni-wine profile); returning nothing.")
}

function Get-WmiObject {
    [CmdletBinding()]
    param(
        [Parameter(Position = 0)] [string]$Class,
        [Parameter(Position = 1)] [string[]]$Property,
        [string]$Namespace,
        [string]$Filter,
        [string]$Query,
        [string[]]$ComputerName
    )
    if (-not $Class -and $Query -match '(?i)\bfrom\s+(\w+)') { $Class = $Matches[1] }
    if ($Class -ieq 'Win32_Process') { return Get-WineProcessList }
    [Console]::Error.WriteLine("WARNING: Get-WmiObject $Class is not supported under Wine (ni-wine profile); returning nothing.")
}

Set-Alias gwmi Get-WmiObject
Set-Alias gcim Get-CimInstance

# Start-Process: strip the quotes sudo-prompt leaves around -FilePath, then
# hand everything to the real cmdlet (Wine implements -Verb runAs).
function Start-Process {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true, Position = 0)] [string]$FilePath,
        [Parameter(Position = 1)] [string[]]$ArgumentList,
        [System.Management.Automation.PSCredential]$Credential,
        [switch]$LoadUserProfile,
        [switch]$NoNewWindow,
        [switch]$PassThru,
        [string]$RedirectStandardError,
        [string]$RedirectStandardInput,
        [string]$RedirectStandardOutput,
        [hashtable]$Environment,
        [switch]$UseNewEnvironment,
        [switch]$Wait,
        [string]$WindowStyle,
        [string]$WorkingDirectory,
        [string]$Verb
    )
    $PSBoundParameters['FilePath'] = $FilePath.Trim("'", '"')
    Microsoft.PowerShell.Management\Start-Process @PSBoundParameters
}

[CmdletBinding()]
param(
    [string]$OutputPath = '',
    [switch]$NoNvidia
)

$ErrorActionPreference = 'Stop'

function Add-Line {
    param([System.Collections.Generic.List[string]]$Lines, [string]$Text = '')
    $Lines.Add($Text) | Out-Null
}

function Add-CommandBlock {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [string]$Title,
        [scriptblock]$Command
    )

    Add-Line $Lines "### $Title"
    Add-Line $Lines '```text'
    try {
        $output = & $Command 2>&1
        if ($null -eq $output -or @($output).Count -eq 0) {
            Add-Line $Lines '(no output)'
        }
        else {
            foreach ($item in @($output)) {
                Add-Line $Lines ($item.ToString())
            }
        }
    }
    catch {
        Add-Line $Lines ("ERROR: " + $_.Exception.Message)
    }
    Add-Line $Lines '```'
    Add-Line $Lines
}

function Convert-WmiChars {
    param([object]$Value)
    if ($null -eq $Value) { return '' }
    $chars = foreach ($n in @($Value)) {
        if ([int]$n -ne 0) { [char][int]$n }
    }
    return -join $chars
}

$lines = [System.Collections.Generic.List[string]]::new()
Add-Line $lines '# MGPU system report'
Add-Line $lines
Add-Line $lines ("Generated UTC: {0}" -f [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'))
Add-Line $lines ("PowerShell: {0}" -f $PSVersionTable.PSVersion.ToString())
Add-Line $lines
Add-Line $lines '> Read-only capture for reproducing MGPU Bridge topology. It changes no display, driver, GPU, or power setting.'
Add-Line $lines '> DXGI LUIDs are session-local and are intentionally taken from the MGPU/ReShade log; pair this report with that log.'
Add-Line $lines

Add-CommandBlock $lines 'Windows' {
    $os = Get-CimInstance Win32_OperatingSystem | Select-Object -First 1
    $cs = Get-CimInstance Win32_ComputerSystem | Select-Object -First 1
    [pscustomobject]@{
        Caption = $os.Caption
        Version = $os.Version
        BuildNumber = $os.BuildNumber
        Manufacturer = $cs.Manufacturer
        Model = $cs.Model
        TotalPhysicalMemoryGiB = [Math]::Round(([double]$cs.TotalPhysicalMemory / 1GB), 2)
    } | Format-List | Out-String -Width 240
}

Add-CommandBlock $lines 'CPU and baseboard' {
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $board = Get-CimInstance Win32_BaseBoard | Select-Object -First 1
    [pscustomobject]@{
        CPU = $cpu.Name
        BoardManufacturer = $board.Manufacturer
        BoardProduct = $board.Product
        BoardVersion = $board.Version
    } | Format-List | Out-String -Width 240
}

Add-CommandBlock $lines 'Display adapters (CIM)' {
    Get-CimInstance Win32_VideoController |
        Select-Object Name, PNPDeviceID, DriverVersion, AdapterRAM,
            CurrentHorizontalResolution, CurrentVerticalResolution, CurrentRefreshRate |
        Format-List | Out-String -Width 320
}

Add-CommandBlock $lines 'Display devices and PCI location paths' {
    $devices = Get-PnpDevice -Class Display -PresentOnly -ErrorAction Stop
    foreach ($device in $devices) {
        $location = $null
        try {
            $location = (Get-PnpDeviceProperty -InstanceId $device.InstanceId `
                -KeyName 'DEVPKEY_Device_LocationPaths' -ErrorAction Stop).Data
        }
        catch {
            $location = "<unavailable: $($_.Exception.Message)>"
        }
        if ($location -is [array]) { $location = $location -join '; ' }
        [pscustomobject]@{
            FriendlyName = $device.FriendlyName
            InstanceId = $device.InstanceId
            Status = $device.Status
            LocationPaths = $location
        }
    } | Format-List | Out-String -Width 360
}

Add-CommandBlock $lines 'Attached monitor identities' {
    $monitors = Get-CimInstance -Namespace 'root\wmi' -ClassName WmiMonitorID -ErrorAction Stop
    foreach ($monitor in $monitors) {
        [pscustomobject]@{
            InstanceName = $monitor.InstanceName
            Manufacturer = Convert-WmiChars $monitor.ManufacturerName
            ProductCode = Convert-WmiChars $monitor.ProductCodeID
            Name = Convert-WmiChars $monitor.UserFriendlyName
            Serial = Convert-WmiChars $monitor.SerialNumberID
            Active = $monitor.Active
        }
    } | Format-List | Out-String -Width 320
}

if ($NoNvidia) {
    Add-Line $lines '### NVIDIA telemetry'
    Add-Line $lines 'Skipped by -NoNvidia.'
    Add-Line $lines
}
else {
    $nvidiaSmi = Get-Command 'nvidia-smi.exe' -ErrorAction SilentlyContinue
    if ($null -eq $nvidiaSmi) {
        Add-Line $lines '### NVIDIA telemetry'
        Add-Line $lines 'nvidia-smi.exe was not found. This is not a failure; non-NVIDIA and CI systems are valid capture hosts.'
        Add-Line $lines
    }
    else {
        $smi = $nvidiaSmi.Source
        Add-CommandBlock $lines 'NVIDIA inventory' {
            & $smi '--query-gpu=index,name,uuid,pci.bus_id,driver_version,memory.total' '--format=csv,noheader'
        }
        Add-CommandBlock $lines 'NVIDIA topology matrix' {
            & $smi 'topo' '-m'
        }
        Add-CommandBlock $lines 'NVIDIA PCI detail' {
            & $smi '-q' '-d' 'PCI'
        }
    }
}

$report = $lines -join [Environment]::NewLine
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $report
}
else {
    $full = [IO.Path]::GetFullPath($OutputPath)
    $parent = Split-Path -Parent $full
    if (-not [string]::IsNullOrWhiteSpace($parent) -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    Set-Content -LiteralPath $full -Value $report -Encoding UTF8
    Write-Output $full
}

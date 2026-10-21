# Lista as DWM1001-DEV conectadas: porta COM (console) e serial do J-Link
# (para "west flash --dev-id <serial>").
#
# Uso: .\scripts\list-boards.ps1

$ports = @(Get-CimInstance Win32_PnPEntity |
    Where-Object { $_.Name -match 'JLink CDC UART Port \((COM\d+)\)' })

if ($ports.Count -eq 0) {
    Write-Host 'Nenhuma J-Link encontrada.'
    return
}

$ports | ForEach-Object {
    $com = [regex]::Match($_.Name, 'COM\d+').Value
    $parent = (Get-PnpDeviceProperty -InstanceId $_.PNPDeviceID -KeyName 'DEVPKEY_Device_Parent').Data
    [pscustomobject]@{
        Porta       = $com
        SerialJLink = (($parent -split '\\')[-1]).TrimStart('0')
    }
} | Sort-Object Porta | Format-Table -AutoSize

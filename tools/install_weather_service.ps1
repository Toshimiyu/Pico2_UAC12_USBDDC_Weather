# Registers the weather service as a per-user logon task that restarts on failure.
param(
    [string]$Port = "",
    [switch]$Uninstall
)

$taskName = "Pico2WeatherService"
if ($Uninstall) {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
    return
}

$pythonw = (Get-Command pythonw.exe -ErrorAction Stop).Source
$script = Join-Path $PSScriptRoot "weather_service.py"
$config = Join-Path $env:LOCALAPPDATA "Pico2WeatherService\forecast_area.json"
if (-not (Test-Path -LiteralPath $config -PathType Leaf)) {
    throw "Forecast area is not configured. Run: py tools\weather_service.py --setup"
}
$log = Join-Path $env:LOCALAPPDATA "Pico2WeatherService\weather_service.log"
New-Item -ItemType Directory -Force -Path (Split-Path $log) | Out-Null

$arguments = "`"$script`" --log-file `"$log`""
if ($Port) { $arguments += " --port $Port" }

$action = New-ScheduledTaskAction -Execute $pythonw -Argument $arguments
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -StartWhenAvailable -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) `
    -ExecutionTimeLimit ([TimeSpan]::Zero)
Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName $taskName
Write-Host "Registered and started '$taskName'. Log: $log"

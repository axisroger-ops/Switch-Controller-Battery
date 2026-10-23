# install.ps1 - Installation en un clic de Switch Controller Battery
param(
    [string]$InstallDir = "C:\Program Files\Switch Controller Battery"
)
$ErrorActionPreference = "Stop"
$dir = $PSScriptRoot

Write-Host "Arrêt des instances en cours..."
Stop-Process -Name "SwitchControllerBattery", "SwitchProBattery", "ShellHost", "SystemSettings" -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400

# Déploiement propre dans le dossier choisi
$stageDir = $InstallDir
if (!(Test-Path $stageDir)) {
    New-Item -ItemType Directory -Path $stageDir -Force | Out-Null
}

Copy-Item "$dir\SwitchControllerBatteryHook.dll" -Destination "$stageDir\SwitchControllerBatteryHook.dll" -Force
Copy-Item "$dir\SwitchControllerBattery.exe" -Destination "$stageDir\SwitchControllerBattery.exe" -Force
Copy-Item "$dir\uninstall.ps1" -Destination "$stageDir\uninstall.ps1" -Force
if (Test-Path "$dir\app.ico") {
    Copy-Item "$dir\app.ico" -Destination "$stageDir\app.ico" -Force
}

# Permissions AppContainer pour Windows 11
icacls $stageDir /grant "*S-1-15-2-1:(OI)(CI)(RX)" /Q | Out-Null
icacls "$stageDir\SwitchControllerBatteryHook.dll" /grant "*S-1-15-2-1:(RX)" /Q | Out-Null

# Création du raccourci dans le Menu Démarrer avec icône transparente
$startMenuPrograms = [System.IO.Path]::Combine($env:APPDATA, "Microsoft\Windows\Start Menu\Programs")
$shortcutPath = [System.IO.Path]::Combine($startMenuPrograms, "Switch Controller Battery.lnk")
$wsh = New-Object -ComObject WScript.Shell
$sc = $wsh.CreateShortcut($shortcutPath)
$sc.TargetPath = "$stageDir\SwitchControllerBattery.exe"
$sc.WorkingDirectory = $stageDir
$sc.IconLocation = "$stageDir\app.ico,0"
$sc.Description = "Gestionnaire de batterie pour manette Switch Pro"
$sc.Save()

# Enregistrement pour démarrage automatique avec Windows
$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
Set-ItemProperty -Path $runKey -Name "SwitchControllerBattery" -Value "`"$stageDir\SwitchControllerBattery.exe`" --background"

# Enregistrement officiel dans la liste des Applications installées de Windows (Paramètres > Applications)
$uninstallKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\SwitchControllerBattery"
if (!(Test-Path $uninstallKey)) {
    New-Item -Path $uninstallKey -Force | Out-Null
}
Set-ItemProperty -Path $uninstallKey -Name "DisplayName" -Value "Switch Controller Battery"
Set-ItemProperty -Path $uninstallKey -Name "DisplayVersion" -Value "1.0.0"
Set-ItemProperty -Path $uninstallKey -Name "Publisher" -Value "Open Source"
Set-ItemProperty -Path $uninstallKey -Name "DisplayIcon" -Value "$stageDir\app.ico,0"
Set-ItemProperty -Path $uninstallKey -Name "InstallLocation" -Value "$stageDir"
Set-ItemProperty -Path $uninstallKey -Name "UninstallString" -Value "powershell.exe -ExecutionPolicy Bypass -File `"$stageDir\uninstall.ps1`""
Set-ItemProperty -Path $uninstallKey -Name "QuietUninstallString" -Value "powershell.exe -ExecutionPolicy Bypass -File `"$stageDir\uninstall.ps1`" -Silent"
Set-ItemProperty -Path $uninstallKey -Name "NoModify" -Value 1 -Type DWord
Set-ItemProperty -Path $uninstallKey -Name "NoRepair" -Value 1 -Type DWord
Set-ItemProperty -Path $uninstallKey -Name "EstimatedSize" -Value 180 -Type DWord

Write-Host "SUCCÈS: Switch Controller Battery installé et enregistré dans Windows !"
Write-Host "Ouverture de la fenêtre de configuration..."
Start-Process -FilePath "$stageDir\SwitchControllerBattery.exe"

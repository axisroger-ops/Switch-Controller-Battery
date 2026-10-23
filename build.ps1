$ErrorActionPreference = "Stop"
$dir = $PSScriptRoot

# Locate clang++ compiler
$clang = "clang++"
$testClang = Get-Command clang++ -ErrorAction SilentlyContinue
if (!$testClang) {
    if (Test-Path "C:\Program Files\Windhawk\Compiler\bin\clang++.exe") {
        $clang = "C:\Program Files\Windhawk\Compiler\bin\clang++.exe"
    } elseif (Test-Path "C:\Program Files\LLVM\bin\clang++.exe") {
        $clang = "C:\Program Files\LLVM\bin\clang++.exe"
    } else {
        throw "clang++ compiler not found! Please install LLVM or add clang++ to your PATH."
    }
}

Write-Host "Arrêt des instances en cours..."
Stop-Process -Name "SwitchControllerBattery", "SwitchProBattery", "ShellHost", "SystemSettings" -Force -ErrorAction SilentlyContinue

Write-Host "Compilation de SwitchControllerBatteryHook.dll..."
& $clang -shared --target=x86_64-w64-mingw32 -O2 -Os -s -fno-exceptions -fno-rtti `
    "$dir\SwitchControllerBatteryHook.cpp" `
    -o "$dir\SwitchControllerBatteryHook.dll" `
    -lruntimeobject -lkernel32 -luser32

if ($LASTEXITCODE -ne 0) { throw "Erreur lors de la compilation de SwitchControllerBatteryHook.dll" }

Write-Host "Compilation de SwitchControllerBattery.exe..."
& $clang --target=x86_64-w64-mingw32 -O2 -Os -s -fno-exceptions -fno-rtti -mwindows -municode `
    "$dir\SwitchControllerBattery.cpp" `
    -o "$dir\SwitchControllerBattery.exe" `
    -lsetupapi -lhid -lcfgmgr32 -lkernel32 -luser32 -ldwmapi -lcomctl32

if ($LASTEXITCODE -ne 0) { throw "Erreur lors de la compilation de SwitchControllerBattery.exe" }

# Déploiement propre dans C:\ProgramData\SwitchControllerBattery
$stageDir = "C:\ProgramData\SwitchControllerBattery"
if (!(Test-Path $stageDir)) {
    New-Item -ItemType Directory -Path $stageDir -Force | Out-Null
}
Copy-Item "$dir\SwitchControllerBatteryHook.dll" -Destination "$stageDir\SwitchControllerBatteryHook.dll" -Force
if (Test-Path "$dir\app.ico") {
    Copy-Item "$dir\app.ico" -Destination "$stageDir\app.ico" -Force
}
icacls $stageDir /grant "*S-1-15-2-1:(OI)(CI)(RX)" /Q | Out-Null
icacls "$stageDir\SwitchControllerBatteryHook.dll" /grant "*S-1-15-2-1:(RX)" /Q | Out-Null

# Création du raccourci dans le Menu Démarrer avec icône transparente
$startMenuPrograms = [System.IO.Path]::Combine($env:APPDATA, "Microsoft\Windows\Start Menu\Programs")
$shortcutPath = [System.IO.Path]::Combine($startMenuPrograms, "Switch Controller Battery.lnk")
$wsh = New-Object -ComObject WScript.Shell
$sc = $wsh.CreateShortcut($shortcutPath)
$sc.TargetPath = "$dir\SwitchControllerBattery.exe"
$sc.WorkingDirectory = $dir
$sc.IconLocation = "$stageDir\app.ico,0"
$sc.Description = "Gestionnaire de batterie pour manette Switch Pro"
$sc.Save()

# Enregistrement pour démarrage automatique avec Windows
$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
Set-ItemProperty -Path $runKey -Name "SwitchControllerBattery" -Value "`"$dir\SwitchControllerBattery.exe`" --background"

# Nettoyage des anciens binaires temporaires
Remove-Item "$dir\SwitchPro*.*" -Force -ErrorAction SilentlyContinue

Write-Host "SUCCÈS: Switch Controller Battery installé !"
Write-Host "Ouverture de la fenêtre de configuration..."
Start-Process -FilePath "$dir\SwitchControllerBattery.exe"

param(
    [switch]$Silent
)

Add-Type -AssemblyName System.Windows.Forms -ErrorAction SilentlyContinue

if (!$Silent) {
    $choice = [System.Windows.Forms.MessageBox]::Show(
        "Voulez-vous vraiment désinstaller Switch Controller Battery de votre ordinateur ?",
        "Désinstallation de Switch Controller Battery",
        [System.Windows.Forms.MessageBoxButtons]::YesNo,
        [System.Windows.Forms.MessageBoxIcon]::Question
    )
    if ($choice -ne [System.Windows.Forms.DialogResult]::Yes) {
        exit 0
    }
}

Write-Host "Arrêt des processus en cours..."
Stop-Process -Name "SwitchControllerBattery", "SwitchProBattery", "ShellHost", "SystemSettings" -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400

Write-Host "Suppression du démarrage automatique..."
Remove-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "SwitchControllerBattery" -Force -ErrorAction SilentlyContinue

Write-Host "Suppression du raccourci dans le Menu Démarrer..."
$shortcut = [System.IO.Path]::Combine($env:APPDATA, "Microsoft\Windows\Start Menu\Programs\Switch Controller Battery.lnk")
if (Test-Path $shortcut) {
    Remove-Item $shortcut -Force -ErrorAction SilentlyContinue
}

Write-Host "Suppression de l'entrée dans les applications installées Windows..."
Remove-Item -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\SwitchControllerBattery" -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "Nettoyage des fichiers déployés..."
$stageDir = $PSScriptRoot
if ($stageDir -and (Test-Path $stageDir) -and $stageDir -ne "C:\" -and $stageDir -ne "C:\Windows") {
    Remove-Item $stageDir -Recurse -Force -ErrorAction SilentlyContinue
}
if (Test-Path "C:\ProgramData\SwitchControllerBattery") {
    Remove-Item "C:\ProgramData\SwitchControllerBattery" -Recurse -Force -ErrorAction SilentlyContinue
}

if (!$Silent) {
    [System.Windows.Forms.MessageBox]::Show(
        "Switch Controller Battery a été désinstallé avec succès.",
        "Désinstallation terminée",
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Information
    )
}

Write-Host "SUCCÈS : Switch Controller Battery a été complètement retiré de votre système."

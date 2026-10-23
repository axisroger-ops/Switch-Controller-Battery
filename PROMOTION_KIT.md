# 🚀 Kit de Promotion & Lancement — Switch Controller Battery

Ce document contient tout le matériel marketing prêt à copier-coller pour maximiser la visibilité de votre projet sur GitHub, Reddit, Hacker News et les sites de logiciels.

---

## 1. ⚙️ Configuration du Dépôt GitHub

### Description (Section « About » à droite sur GitHub) :
```text
🎮 Native Windows 11 battery indicator & control panel for Nintendo Switch Pro Controller (0.00% CPU, ~1.5MB RAM, zero dependencies).
```

### Topics / Tags GitHub (à copier dans les tags du repo) :
```text
nintendo-switch, switch-pro-controller, windows-11, battery-indicator, bluetooth-battery, gamepad, winrt, cpp, lightweight, quick-settings
```

---

## 2. 📦 Notes de Version pour GitHub Release (v1.0.0)

> **Titre de la Release :** `v1.0.0 — Native Windows 11 Battery Indicator for Switch Pro Controller`  
> **Fichiers joints à attacher :**  
> 1. `SwitchControllerBattery-Setup-v1.0.0.exe` (Installeur Windows en un clic)  
> 2. `SwitchControllerBattery-v1.0.0.zip` (Version portable sans installation)

### Description de la release :
```markdown
### 🎮 Switch Controller Battery v1.0.0

First official release of **Switch Controller Battery**, an ultra-lightweight native utility that displays the real battery percentage and native battery gauge for the **Nintendo Switch Pro Controller** in Windows 11!

#### ⬇️ Choose Your Download:
* **`SwitchControllerBattery-Setup-v1.0.0.exe`** (Recommended) : Standard Windows Setup installer with Start Menu shortcut, auto-startup option, and clean uninstaller in Windows Settings.
* **`SwitchControllerBattery-v1.0.0.zip`** : Portable version for advanced users (zero install).

#### ✨ Highlights
- 🔋 **Native UI Integration**: Displays live battery in Quick Settings (`Win + A`) and the Windows 11 Settings app.
- ⚡ **0.00% CPU Usage**: Event-driven kernel PnP notifications (`CM_Register_Notification`). No polling loops.
- 🪶 **Ultra-lightweight**: ~1.5 MB RAM footprint, ~38 KB total binary size, 0 external dependencies.
- 💾 **Zero SSD Wear**: Pure in-memory synchronization via `WriteProcessMemory`.
- 🎛️ **Windows 11 Dark Mode Control Panel**: Minimalist interface accessible from the Start Menu.
```

---

## 3. 🔴 Reddit Post : r/Windows11

* **Subreddit :** [r/Windows11](https://www.reddit.com/r/Windows11/)
* **Flair suggéré :** `Concept / Creation` ou `Discussion / Help`
* **Conseil clé :** Attachez une courte vidéo de 10 secondes ou un GIF montrant l'ouverture de `Win + A` avec le pourcentage de batterie.

### Titre :
> **[Open Source] I made an ultra-lightweight (0% CPU, ~1.5MB RAM) native battery percentage indicator for the Switch Pro Controller in Windows 11 Quick Settings**

### Contenu :
```markdown
Hey everyone!

If you use a **Nintendo Switch Pro Controller** on Windows 11 over Bluetooth, you’ve probably noticed that Windows never displays its battery percentage. This is because Nintendo doesn’t implement the standard Bluetooth Battery Service (BAS) profile, relying on proprietary HID input reports instead.

I got tired of third-party tools that either run heavy Electron/WPF frameworks or consume constant CPU cycles just to poll battery status, so I built **Switch Controller Battery**:

### 🎯 What it does:
- Shows the **actual battery percentage and native Windows 11 gauge icon** directly in:
  - **Quick Settings** (`Win + A > Bluetooth`)
  - **Settings App** (`Settings > Bluetooth & devices`)
- Automatically hides the gauge when the controller turns off or sleeps.
- Includes a tiny, native Windows 11 Dark Mode control panel from the Start Menu.

### ⚙️ How it works under the hood (0% CPU & zero dependencies):
- **Kernel PnP Event-Driven**: Uses `CM_Register_Notification` (`GUID_DEVINTERFACE_HID`). While your controller is asleep, the daemon stays in a deep kernel sleep (`WaitForSingleObject`) consuming literally **0.00% CPU**.
- **In-Memory Sync**: Uses `WriteProcessMemory` directly into Windows interface processes to avoid any SSD wear or disk polling.
- **Micro WinRT Inline Hook**: A ~14 KB DLL hooks into `DeviceFlows.DataModel.dll` using a dynamic PE pattern scan (so it won't break with Windows Updates).
- **Resource Footprint**: ~1.5 MB of RAM, zero external runtimes (.NET, Node, MinHook not needed).

The project is 100% free, independent and open source:  
👉 **GitHub:** [https://github.com/axisroger-ops/Switch-Controller-Battery](https://github.com/axisroger-ops/Switch-Controller-Battery)

Hope this helps anyone else gaming with a Switch Pro Controller on PC! Feedback and PRs welcome!
```

---

## 4. 🎮 Reddit Post : r/pcgaming & r/pcmasterrace

* **Subreddits :** [r/pcgaming](https://www.reddit.com/r/pcgaming/) & [r/pcmasterrace](https://www.reddit.com/r/pcmasterrace/)

### Titre :
> **Tired of not knowing your Switch Pro Controller's battery on PC? I made a lightweight native Windows 11 battery indicator (0% CPU, open source)**

### Contenu :
```markdown
Hi all!

Windows 11 natively supports battery percentages for Xbox and standard Bluetooth controllers, but completely ignores the Nintendo Switch Pro Controller over Bluetooth.

I developed an open-source utility called **Switch Controller Battery** that fixes this without needing heavy background utilities or overlays:

- 🔋 Displays your Switch Pro Controller's real battery level right in **Windows 11 Quick Settings (`Win + A`)** and **Settings > Bluetooth**.
- 🪶 Takes only **~1.5 MB RAM** and **0.00% CPU** (passive PnP hardware events, no busy polling).
- 🚀 Pure native Win32/C++ with zero external dependencies.

Precompiled binaries and source code are available on GitHub:  
👉 **GitHub:** [https://github.com/axisroger-ops/Switch-Controller-Battery](https://github.com/axisroger-ops/Switch-Controller-Battery)
```

---

## 5. 👾 Reddit Post : r/emulation

* **Subreddit :** [r/emulation](https://www.reddit.com/r/emulation/)

### Titre :
> **Free tool: Native Windows 11 battery indicator for Switch Pro Controller (0.00% CPU, open-source)**

### Contenu :
```markdown
Hey everyone,

A lot of us use the official Switch Pro Controller when playing on PC emulators, but Windows 11 never reports the gamepad battery percentage in the Bluetooth tray.

I wrote a small open-source native C++ service that reads the proprietary Switch HID battery reports and hooks directly into the Windows 11 Bluetooth menu (`Win + A`):

- 🔋 **Real-time battery percentage** in Windows Quick Settings & Settings app.
- ⚡ **0.00% CPU** consumption while idle or playing.
- 📦 **No dependencies** (clean ~1.5 MB RAM footprint).

Check it out on GitHub:  
👉 [https://github.com/axisroger-ops/Switch-Controller-Battery](https://github.com/axisroger-ops/Switch-Controller-Battery)
```

---

## 6. 🟠 Hacker News (Show HN)

* **Site :** [news.ycombinator.com](https://news.ycombinator.com/submit)

### Titre :
> **Show HN: Native Windows 11 Switch Pro battery indicator via a 20-byte WinRT inline hook**

### URL :
> `https://github.com/axisroger-ops/Switch-Controller-Battery`

### Premier commentaire (explication technique) :
```text
I built this small open-source utility to solve a frustrating limitation in Windows 11: Bluetooth Nintendo Switch Pro Controllers never report battery level because they don't implement the standard Bluetooth BAS (Battery Service) profile, using proprietary HID input reports instead.

Key technical implementation choices:
1. Event-Driven PnP Monitoring: Instead of periodic timer polling, it registers with CM_Register_Notification(GUID_DEVINTERFACE_HID). When the controller isn't active, the background daemon stays in deep kernel sleep (0.00% CPU).
2. Dynamic PE Byte-Scan: Windows' Bluetooth UI model is driven by DeviceFlows.DataModel.dll. Rather than hardcoding function offsets, the hook scans the .text PE section at runtime to locate IDeviceBase::get_BatteryLife and IDeviceBase::get_ShowBatteryStatus.
3. 20-byte Inline Trampoline: Uses an inline jump compliant with x64 stack conventions without third-party hooking libraries (MinHook, Detours).
4. Zero Disk I/O: Battery percentage is directly synchronized into ShellHost.exe and SystemSettings.exe via WriteProcessMemory, keeping SSD wear at 0.

Written in pure C++ Win32 with zero dependencies (~38 KB total binary size, ~1.5 MB RAM).
```

---

## 7. 🌐 Fiche AlternativeTo.net

* **Soumettre sur :** [AlternativeTo.net/suggest-software/](https://alternativeto.net/suggest-software/)
* **Nom :** `Switch Controller Battery`
* **Licence :** Open Source (MIT)
* **Plateforme :** Windows 11
* **Description courte :**
  > Lightweight native Windows 11 battery indicator and control panel for Nintendo Switch Pro Controller over Bluetooth.
* **Tags :** `nintendo-switch`, `game-controller`, `battery-monitor`, `windows-11`, `bluetooth-device`
* **Alternatives à remplacer :** `BetterJoy`, `DS4Windows`, `Steam Gamepad Overlay`.

---

## 8. 🐦 Fil Twitter / X

```text
🎮🔋 Tired of Windows 11 not showing your Nintendo Switch Pro Controller's battery over Bluetooth?

I created Switch Controller Battery — an ultra-lightweight open-source utility that shows real battery % directly in Windows 11 Quick Settings (Win+A)!

⚡ 0.00% CPU
🪶 ~1.5 MB RAM
💾 0 disk writes
✨ 100% native Win32/C++

Grab it here: https://github.com/axisroger-ops/Switch-Controller-Battery #Windows11 #NintendoSwitch #OpenSource #PCGaming
```

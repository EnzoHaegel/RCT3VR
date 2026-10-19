# RCT3 VR installer: copies the mod (d3d9.dll + rct3vr.ini) next to RCT3.exe, or removes it.
param(
    [string]$GameDir = '',
    [switch]$Uninstall,
    [switch]$Quiet        # no questions, no pause (for scripted use)
)

$ErrorActionPreference = 'Stop'
$FilesDir = $PSScriptRoot
$Fr = (Get-UICulture).TwoLetterISOLanguageName -eq 'fr'

function T([string]$en, [string]$fr) { if ($Fr) { $fr } else { $en } }
function Say([string]$en, [string]$fr, [string]$color = 'Gray') { Write-Host (T $en $fr) -ForegroundColor $color }
function Done([int]$code) {
    if (-not $Quiet) { Write-Host ''; Read-Host (T 'Press Enter to close' 'Appuie sur Entrée pour fermer') | Out-Null }
    exit $code
}

function Test-Rct3Folder([string]$dir) {
    return $dir -and (Test-Path -LiteralPath (Join-Path $dir 'RCT3.exe'))
}

# Every folder holding RCT3.exe that Epic, Steam or GOG knows about.
function Find-GameFolders {
    $found = New-Object System.Collections.Generic.List[string]
    $add = { param($d) if ((Test-Rct3Folder $d) -and -not ($found -contains $d)) { $found.Add($d) } }

    $epic = Join-Path $env:ProgramData 'Epic\EpicGamesLauncher\Data\Manifests'
    if (Test-Path $epic) {
        foreach ($item in Get-ChildItem $epic -Filter *.item -ErrorAction SilentlyContinue) {
            try { & $add ((Get-Content $item.FullName -Raw | ConvertFrom-Json).InstallLocation) } catch {}
        }
    }

    $steam = $null
    try { $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath } catch {}
    if ($steam) {
        $libraries = @($steam)
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) { $libraries += $m.Groups[1].Value -replace '\\\\', '\' }
        }
        foreach ($lib in $libraries | Select-Object -Unique) {
            $common = Join-Path $lib 'steamapps\common'
            if (Test-Path $common) { foreach ($d in Get-ChildItem $common -Directory -ErrorAction SilentlyContinue) { & $add $d.FullName } }
        }
    }

    foreach ($key in 'HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games', 'HKLM:\SOFTWARE\GOG.com\Games') {
        if (Test-Path $key) {
            foreach ($g in Get-ChildItem $key -ErrorAction SilentlyContinue) {
                try { & $add (Get-ItemProperty $g.PSPath -ErrorAction Stop).path } catch {}
            }
        }
    }

    foreach ($d in "$env:ProgramFiles\Epic Games\RollerCoasterTycoon3", "${env:ProgramFiles(x86)}\Atari\RollerCoaster Tycoon 3",
                   'C:\GOG Games\RollerCoaster Tycoon 3 Complete Edition') { & $add $d }
    return ,$found
}

function Select-GameFolder {
    $folders = Find-GameFolders
    if ($folders.Count -eq 1) { return $folders[0] }
    if ($folders.Count -gt 1 -and -not $Quiet) {
        Say 'Several RollerCoaster Tycoon 3 installs were found:' 'Plusieurs installations de RollerCoaster Tycoon 3 ont été trouvées :'
        for ($i = 0; $i -lt $folders.Count; $i++) { Write-Host "  [$($i + 1)] $($folders[$i])" }
        $choice = Read-Host (T 'Number to use' 'Numéro à utiliser')
        if ($choice -match '^\d+$' -and [int]$choice -ge 1 -and [int]$choice -le $folders.Count) { return $folders[[int]$choice - 1] }
        return $null
    }
    if ($Quiet) { return $null }
    Say 'RollerCoaster Tycoon 3 was not found automatically. Pick the folder that contains RCT3.exe.' `
        'RollerCoaster Tycoon 3 n''a pas été trouvé automatiquement. Choisis le dossier qui contient RCT3.exe.' 'Yellow'
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = T 'Folder containing RCT3.exe' 'Dossier contenant RCT3.exe'
    if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $dialog.SelectedPath }
    return $null
}

function Test-Writable([string]$dir) {
    $probe = Join-Path $dir ('rct3vr_write_test_' + [guid]::NewGuid().ToString('N') + '.tmp')
    try { [IO.File]::WriteAllText($probe, 'x'); Remove-Item -LiteralPath $probe -Force; return $true } catch { return $false }
}

function Restart-Elevated {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-GameDir', "`"$GameDir`"")
    if ($Uninstall) { $argList += '-Uninstall' }
    Say 'Administrator rights are needed to write into the game folder...' 'Les droits administrateur sont nécessaires pour écrire dans le dossier du jeu...' 'Yellow'
    Start-Process powershell.exe -Verb RunAs -ArgumentList $argList | Out-Null
    exit 0
}

function Test-OurDll([string]$path) {
    return (Test-Path -LiteralPath $path) -and ((Get-Item -LiteralPath $path).VersionInfo.ProductName -eq 'RCT3 VR')
}

# 32-bit OpenXR runtimes the mod can use (informational).
function Find-VrRuntimes {
    $runtimes = @()
    $candidates = @(
        @('Virtual Desktop (VDXR)', "$env:ProgramFiles\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr-32.json"),
        @('SteamVR', "${env:ProgramFiles(x86)}\Steam\steamapps\common\SteamVR\steamxr_win32.json"),
        @('Meta Quest Link', "$env:ProgramFiles\Oculus\Support\oculus-runtime\oculus_openxr_32.json")
    )
    foreach ($c in $candidates) { if (Test-Path -LiteralPath $c[1]) { $runtimes += $c[0] } }
    try {
        $active = (Get-ItemProperty 'HKLM:\SOFTWARE\Khronos\OpenXR\1' -ErrorAction Stop).ActiveRuntime
        if ($active) { $runtimes += (T "active OpenXR runtime: $active" "runtime OpenXR actif : $active") }
    } catch {}
    return $runtimes
}

# ---------------------------------------------------------------------------

Write-Host ''
Write-Host '  RCT3 VR' -ForegroundColor Cyan -NoNewline
Write-Host ("  " + (T 'ride cameras in virtual reality for RollerCoaster Tycoon 3' 'les caméras des attractions de RollerCoaster Tycoon 3 en réalité virtuelle'))
Write-Host ''

if (-not (Test-Path -LiteralPath (Join-Path $FilesDir 'd3d9.dll'))) {
    Say 'd3d9.dll is missing next to the installer: extract the whole zip first.' 'd3d9.dll est introuvable a cote de l''installateur : extrais d''abord tout le zip.' 'Red'
    Done 1
}

if (-not $GameDir) { $GameDir = Select-GameFolder }
if (-not (Test-Rct3Folder $GameDir)) {
    Say 'No RCT3.exe in that folder. Nothing was changed.' 'Pas de RCT3.exe dans ce dossier. Rien n''a été modifié.' 'Red'
    Done 1
}
$GameDir = (Resolve-Path -LiteralPath $GameDir).Path
$exe = Get-Item -LiteralPath (Join-Path $GameDir 'RCT3.exe')
Say "Game: $GameDir (RCT3.exe $($exe.VersionInfo.FileVersion))" "Jeu : $GameDir (RCT3.exe $($exe.VersionInfo.FileVersion))"

if (Get-Process -Name RCT3 -ErrorAction SilentlyContinue) {
    Say 'RollerCoaster Tycoon 3 is running: close it first.' 'RollerCoaster Tycoon 3 est lancé : ferme-le d''abord.' 'Red'
    Done 1
}
if (-not (Test-Writable $GameDir)) { Restart-Elevated }

$dll = Join-Path $GameDir 'd3d9.dll'
$backup = Join-Path $GameDir 'd3d9.dll.rct3vr-backup'
$ini = Join-Path $GameDir 'rct3vr.ini'
$installed = Test-OurDll $dll

if (-not $Uninstall -and $installed -and -not $Quiet) {
    $current = (Get-Item -LiteralPath $dll).VersionInfo.FileVersion
    Say "RCT3 VR $current is already installed." "RCT3 VR $current est déjà installé."
    Write-Host (T '  [1] Update / reinstall   [2] Uninstall   [3] Quit' '  [1] Mettre à jour / réinstaller   [2] Désinstaller   [3] Quitter')
    switch (Read-Host (T 'Choice' 'Choix')) {
        '1' {}
        '2' { $Uninstall = $true }
        default { Done 0 }
    }
}

if ($Uninstall) {
    if (-not $installed) {
        Say 'RCT3 VR is not installed in this folder.' 'RCT3 VR n''est pas installé dans ce dossier.' 'Yellow'
        Done 0
    }
    Remove-Item -LiteralPath $dll -Force
    if (Test-Path -LiteralPath $backup) {
        Move-Item -LiteralPath $backup -Destination $dll -Force
        Say 'Your previous d3d9.dll was restored.' 'Ton ancien d3d9.dll a été restauré.'
    }
    foreach ($f in $ini, (Join-Path $GameDir 'rct3vr.log')) { if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force } }
    Get-ChildItem -LiteralPath $GameDir -Directory -Filter 'rct3vr_capture_*' -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
    Say 'RCT3 VR was uninstalled. The game is back to normal.' 'RCT3 VR a été désinstallé. Le jeu est revenu à la normale.' 'Green'
    Done 0
}

# Another d3d9.dll (ReShade, dxwrapper...) would be replaced: keep it aside.
if ((Test-Path -LiteralPath $dll) -and -not $installed) {
    Move-Item -LiteralPath $dll -Destination $backup -Force
    Say 'Another d3d9.dll was present (another mod?). It was renamed to d3d9.dll.rct3vr-backup and is restored on uninstall.' `
        'Un autre d3d9.dll était présent (un autre mod ?). Il a été renommé en d3d9.dll.rct3vr-backup et sera restauré à la désinstallation.' 'Yellow'
}

Copy-Item -LiteralPath (Join-Path $FilesDir 'd3d9.dll') -Destination $dll -Force
if (Test-Path -LiteralPath $ini) {
    Say 'Your rct3vr.ini settings were kept.' 'Tes réglages rct3vr.ini ont été conservés.'
} else {
    Copy-Item -LiteralPath (Join-Path $FilesDir 'rct3vr.ini') -Destination $ini -Force
}
$version = (Get-Item -LiteralPath $dll).VersionInfo.FileVersion
Say "RCT3 VR $version installed." "RCT3 VR $version installé." 'Green'

$runtimes = Find-VrRuntimes
if ($runtimes.Count -eq 0) {
    Say 'No 32-bit OpenXR runtime found: install Virtual Desktop, SteamVR or the Meta Quest app.' `
        'Aucun runtime OpenXR 32 bits trouvé : installe Virtual Desktop, SteamVR ou l''application Meta Quest.' 'Yellow'
} else {
    Say ('VR found: ' + ($runtimes -join ', ')) ('VR trouvée : ' + ($runtimes -join ', '))
}
Write-Host ''
Say 'Start your headset streaming, launch the game, open a ride camera: VR switches on by itself.' `
    'Connecte ton casque, lance le jeu et ouvre la caméra d''une attraction : la VR s''active toute seule.' 'Cyan'
Done 0

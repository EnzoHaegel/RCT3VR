# RCT3 VR

**Monte dans les attractions de RollerCoaster Tycoon 3 en réalité virtuelle.**

🇬🇧 [English version](README.md)

Quand tu ouvres la caméra d'une attraction (vue coaster), l'image passe dans ton casque VR en vraie 3D et tu regardes autour de toi en tournant la tête. Le reste du jeu ne change pas et se joue normalement à l'écran.

Aucun fichier du jeu n'est modifié : le mod est un `d3d9.dll` placé à côté de `RCT3.exe`, tout se passe en mémoire pendant que le jeu tourne.

## Ce qu'il faut

- **RollerCoaster Tycoon 3 Complete Edition** (testé avec Epic Games ; Steam et GOG utilisent le même jeu)
- Windows 10 ou 11, une carte graphique récente
- Un casque VR connecté au PC, avec l'un de ces logiciels :
  - **Virtual Desktop** (Quest, Pico…), testé
  - **SteamVR** (Index, Vive, Quest via SteamVR…)
  - **Meta Quest Link** (application Meta Quest)

## Installation

1. Télécharge `RCT3VR-x.y.z.zip` depuis la [dernière release](https://github.com/EnzoHaegel/RCT3VR/releases/latest).
2. Extrais **tout** le zip.
3. Double-clique sur **`Installer.bat`**. Il trouve le jeu tout seul (Epic, Steam, GOG), sinon il te demande le dossier qui contient `RCT3.exe`.
   Si Windows affiche un avertissement : *Informations complémentaires* → *Exécuter quand même*.

Relancer `Installer.bat` met le mod à jour et garde tes réglages.

## Jouer

1. Mets ton casque et lance le streaming (Virtual Desktop, Quest Link, SteamVR…).
2. Lance le jeu et charge un parc.
3. Ouvre la caméra d'une attraction : **la VR s'active toute seule**, et se coupe quand tu quittes la caméra.

## Touches

| Entrée | Action |
|---|---|
| **A** (manette droite, bouton du bas) | Caméra suivante |
| **B** (manette droite, bouton du haut) | Quitter la caméra de l'attraction |
| **Clic du joystick droit** | Recentrer la vue |
| Joystick droit, hors VR (vue du parc d'en haut) | Se déplacer dans le parc |
| **F8** | Recentrer la vue |
| **F9** | Désactiver / réactiver le mod |

X et Y (manette gauche) sont libres : configure-les dans `rct3vr.ini`. Les boutons de la manette appuient pour toi sur les touches du jeu : la fenêtre du jeu doit avoir le focus (clique dedans si un bouton ne répond pas).

Touches du jeu utilisables sur un bouton : `N` caméra suivante · `M` caméra précédente · `B` type de caméra · `Shift+N` attraction suivante · `Ctrl+C` quitter la caméra.

## Réglages

`rct3vr.ini` dans le dossier du jeu (à ouvrir avec le Bloc-notes) :

| Réglage | Défaut | Effet |
|---|---|---|
| `[Controller] ButtonA/B/X/Y`, `RightStick`, `LeftStick` | `N`, `Ctrl+C`, –, –, `Recenter`, – | Une combinaison de touches du jeu, `Recenter`, ou vide. Exemple : `ButtonX=M` |
| `WorldScale` | `1.0` | Taille du monde (plus grand = tu te sens plus petit) |
| `GameFov` | `140` | Baisse-le si les détails lointains disparaissent, monte-le si des objets disparaissent sur les côtés |
| `MSAA` | `4` | Anti-crénelage : 0, 2, 4 ou 8 |
| `RenderScale` | `1.0` | Résolution (1.0 = celle du casque) |
| `AutoEnable` | `1` | `0` = VR toujours active, F9 pour l'allumer / l'éteindre |
| `ToggleKey` / `RecenterKey` | `0x78` / `0x77` | Touches clavier ([codes virtual-key](https://learn.microsoft.com/fr-fr/windows/win32/inputdev/virtual-key-codes)) |
| `ForceWindowed` | `1` | Fenêtre sans bordure (sinon le jeu se fige quand il perd le focus). `0` = plein écran classique |

## En cas de problème

- **Rien dans le casque** : lance le streaming VR *avant* d'ouvrir la caméra de l'attraction.
- **Saccades** : ouvre `rct3vr.log` dans le dossier du jeu ; la ligne `GPU texture sharing available` indique que le transfert rapide fonctionne.
- **L'antivirus se méfie de `d3d9.dll`** (comme pour ReShade) : c'est le mod.
- Besoin d'aide ? Joins `rct3vr.log`.

## Désinstallation

Double-clique sur **`Uninstall.bat`** (ou `Installer.bat` → *Désinstaller*). Le jeu redevient exactement comme avant ; un ancien `d3d9.dll` (ReShade…) est remis en place.

## Compiler

Voir [Build from source](README.md#build-from-source).

---

Mod non officiel, sans lien avec Frontier ni Atari.

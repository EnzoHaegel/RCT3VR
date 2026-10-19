RCT3 VR — les caméras des attractions de RollerCoaster Tycoon 3 en réalité virtuelle
===================================================================================

Quand tu montes dans une attraction (vue coaster / caméra embarquée), l'image passe
dans ton casque VR : vraie 3D, tu regardes autour de toi en tournant la tête. Le reste
du jeu ne change pas et se joue normalement à l'écran.

CE QU'IL FAUT
- RollerCoaster Tycoon 3 Complete Edition (testé avec la version Epic Games ; les
  versions Steam et GOG utilisent normalement le même jeu).
- Windows 10 ou 11, une carte graphique récente.
- Un casque VR connecté au PC, avec l'un de ces logiciels :
    * Virtual Desktop (Quest, Pico…) — testé ;
    * SteamVR (Index, Vive, Quest via SteamVR…) ;
    * Meta Quest Link (application Meta Quest).

INSTALLATION
1. Extrais tout le zip dans un dossier.
2. Double-clique sur Installer.bat. Il trouve le jeu tout seul (Epic, Steam, GOG) ;
   sinon il te demande le dossier qui contient RCT3.exe.
   Windows peut afficher un avertissement (fichier téléchargé) : « Informations
   complémentaires » puis « Exécuter quand même ».

UTILISATION
1. Mets ton casque et lance le streaming (Virtual Desktop, Quest Link, SteamVR…).
2. Lance le jeu normalement et charge un parc.
3. Ouvre la caméra d'une attraction : la VR s'active toute seule. Quand tu quittes la
   caméra, tu reviens à l'écran normal.

Manettes (dans la VR) :
- A (bouton du bas, manette droite) : caméra suivante
- B (bouton du haut, manette droite) : quitter la caméra de l'attraction
- Clic du joystick droit : recentrer la vue
Manette hors VR (vue du parc d'en haut) :
- Joystick droit : se déplacer dans le parc (haut, gauche, bas, droite)
Clavier : F8 recentrer la vue, F9 désactiver / réactiver le mod.

Les boutons de la manette appuient pour toi sur les touches du jeu : la fenêtre du jeu
doit avoir le focus (clique dedans si un bouton ne répond pas).

RÉGLAGES — rct3vr.ini (dans le dossier du jeu, à ouvrir avec le Bloc-notes)
- [Controller] : action de chaque bouton. Touches du jeu par défaut : N caméra suivante,
  M caméra précédente, B type de caméra, Shift+N attraction suivante, Ctrl+C quitter.
  Exemple : ButtonX=M
- WorldScale : taille du monde (plus grand = tu te sens plus petit).
- GameFov : champ de vision utilisé par le jeu pour décider quoi dessiner (140 par
  défaut). Baisse-le si les détails lointains disparaissent, monte-le si des objets
  disparaissent sur les côtés quand tu tournes la tête.
- MSAA : anti-crénelage (0, 2, 4 ou 8). RenderScale : résolution (1.0 = celle du casque).
- AutoEnable=0 : la VR reste active tout le temps (F9 pour l'allumer / l'éteindre).

LE JEU S'AFFICHE EN FENÊTRE SANS BORDURE
C'est voulu (le jeu continuerait sinon à se figer quand il perd le focus). Pour
revenir au plein écran classique : ForceWindowed=0 dans rct3vr.ini.

EN CAS DE PROBLÈME
- Rien dans le casque : lance le streaming VR AVANT d'ouvrir la caméra de l'attraction.
- Saccades : regarde rct3vr.log (dans le dossier du jeu). La ligne « GPU texture
  sharing available » indique que le transfert rapide fonctionne.
- Ton antivirus peut se méfier de d3d9.dll (comme pour ReShade) : c'est le mod.
- Joins rct3vr.log à ta demande d'aide.

DÉSINSTALLATION
Double-clique sur Uninstall.bat (ou Installer.bat puis « Désinstaller »). Le jeu
redevient exactement comme avant ; si un autre d3d9.dll (ReShade…) était présent, il
est remis en place.

Mod non officiel, sans lien avec Frontier ni Atari. Aucun fichier du jeu n'est
modifié : tout se passe en mémoire pendant que le jeu tourne.

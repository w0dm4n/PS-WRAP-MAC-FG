# PS-WRAP-MAC-FG — GTA 6 (PS5) à 60 fps sur Mac

> **EN —** A fork of [PS-WRAP-MAC](https://github.com/iDevGIS/PS-WRAP) (itself a fork of [chiaki-ng](https://github.com/streetpea/chiaki-ng)) for Apple Silicon Macs. It streams your PS5 over Remote Play and **doubles the frame rate with frame generation**: GTA 6 runs at 30 fps on PS5, and you see ~60 fps on the Mac. Interpolation uses Apple's VideoToolbox (macOS 26+), with a RIFE fallback. It also reduces halos around the character and adds FSRCNNX upscaling. No capture card is needed. The instructions below are in French.

GTA 6 tourne à **30 fps** sur PS5. Ce fork du client Remote Play PS-WRAP-MAC calcule **une image intermédiaire entre chaque paire d'images** du stream, sur ton Mac. Tu passes ainsi de 30 à **~60 fps affichés**, sans carte d'acquisition et sans toucher à la console.

## Ce que ça apporte par rapport à PS-WRAP-MAC

| | PS-WRAP-MAC 0.14 | **PS-WRAP-MAC-FG** |
|---|---|---|
| Moteur de frame gen | shader optical-flow maison | **Apple VideoToolbox** (`VTFrameProcessor` low-latency), repli **RIFE 4.25** (Metal) |
| Halos et fantômes autour du perso | visibles | supprimés (images « tenues » quand l'interpolation n'est pas fiable) |
| Bandes violettes, bords qui bavent en pano | oui | corrigé (remplissage des bords) |
| Régularité de l'affichage | ~54 fps, 58 % des images à l'heure | **~58–60 fps, ~94 % à ±4 ms** |
| Upscaling | preset High Quality | **High Quality + Spatial (FSRCNNX)** par défaut |
| Freeze H.265 sur perte Wi-Fi | possible | « IDR on FEC failure » activé par défaut, plus un watchdog vidéo |
| Écran en veille quand tu es AFK | le stream se fige | l'écran reste allumé pendant le stream |
| Son perdu après un blocage audio | ne revient pas | se rouvre automatiquement |

Ces mesures viennent d'enregistrements ScreenCaptureKit à 120 Hz, analysés image par image en jeu (GTA 6, Wi-Fi 5 GHz, MacBook Pro M4 Pro).

## Prérequis

- **Mac Apple Silicon** (M1 ou plus récent ; testé sur M4 Pro).
- **macOS 26 ou plus récent** pour le moteur Apple, le meilleur. Sur macOS plus ancien, l'app bascule sur RIFE ou sur le shader.
- **PS5** avec Remote Play activé.
- Réseau : **Ethernet sur la PS5** idéalement. Sinon, du Wi-Fi 5 GHz pour la PS5 comme pour le Mac.

## 1. Préparer la PS5

1. **Paramètres → Système → Lecture à distance** → active **Activer la lecture à distance**.
2. **Paramètres → Système → Économie d'énergie → Fonctionnalités disponibles en mode repos** → active **Rester connecté à Internet** et **Activer la mise sous tension de la PS5 depuis le réseau**. Tu pourras ainsi réveiller la console depuis le Mac.
3. Garde **Paramètres → Système → Lecture à distance → Associer l'appareil** sous la main : il donne le code PIN demandé à l'étape 3.

## 2. Compiler l'app

```bash
git clone https://github.com/w0dm4n/PS-WRAP-MAC-FG.git
cd PS-WRAP-MAC-FG
git submodule update --init --recursive

scripts/setup-mac.sh                 # une seule fois : Homebrew deps (Qt 6, FFmpeg, SDL2, MoltenVK…)
scripts/build-libplacebo-mac.sh      # une seule fois : libplacebo 7.360 (la 7.387 de Homebrew se bloque au lancement)
python3 -m venv .venv && .venv/bin/pip install protobuf
PATH="$PWD/.venv/bin:$PATH" scripts/deploy-mac-fg.sh
```

`deploy-mac-fg.sh` compile l'app et le wrapper RIFE (il télécharge [rife-metal](https://github.com/cinemore/rife-metal) v0.1.8 et lui applique un petit patch). Il produit une app autonome et signée ad hoc dans **`dist/PS-WRAP-MAC-FG.app`**.

Cette version a sa propre configuration (`com.ps-wrap-fg.PS-WRAP`) et son propre trousseau. Elle n'écrase jamais les réglages de PS-WRAP-MAC officiel, et elle réutilise l'association PS5 de l'officiel s'il est installé.

## 3. Associer la PS5

Lance `dist/PS-WRAP-MAC-FG.app`. La PS5 apparaît dans la liste quand elle est allumée sur le même réseau. Clique dessus et entre ton identifiant PSN, puis le PIN affiché sur la PS5 (**Associer l'appareil**).

> Lance l'app depuis le Finder ou avec `open`, pas directement depuis un terminal. macOS donne l'autorisation « Réseau local » à l'app qui fait le lancement. Lancée depuis un terminal, l'app échoue avec *No route to host*.

## 4. Réglages conseillés

Ce sont les réglages par défaut de cette version. Vérifie-les dans **Réglages** et dans le menu du stream :

| Réglage | Valeur |
|---|---|
| Résolution / fps du stream | **1080p / 30 fps**. GTA 6 est à 30 fps, et c'est le Mac qui double. |
| Débit | **50 Mbps** (descends à 30–40 si ça saccade en Wi-Fi) |
| Codec | **H.265** |
| Request IDR Frame on FEC Failure | **Activé** (évite les freezes en H.265) |
| Rendu (Placebo preset) | **High Quality + Spatial** (upscaling FSRCNNX) |
| Frame Gen | **Activé**, moteur **Auto** (Apple → RIFE → shader) |
| Snaps | **Clean** (aucun fantôme sur les changements de caméra). **Smooth** est plus fluide, mais peut laisser une trace d'un frame. |
| Overlay manette | Désactivé |

### Dans GTA 6

- **Flou de mouvement : désactivé.** Le flou trompe l'interpolation et crée des traînées.
- Le reste n'a pas d'impact sur le frame gen. Garde le mode d'image que tu préfères.

### Lancer directement le stream en plein écran

```bash
open -n dist/PS-WRAP-MAC-FG.app --args --fullscreen stream "<nom de ta PS5>" <IP de ta PS5>
```

- `--fullscreen` garde les bonnes proportions. La PS5 envoie du 16:9 et l'écran d'un MacBook est en 16:10, d'où de fines bandes noires en haut et en bas.
- `--zoom` remplit l'écran en coupant ~6 % de chaque côté (la minimap peut être rognée).
- `--stretch` remplit l'écran en déformant l'image. Déconseillé.

Le nom et l'IP de ta PS5 sont visibles dans l'app, ou dans ta box internet.

## Limites connues

- **60 fps au maximum.** Le jeu sort 30 images par seconde et on en ajoute une entre chaque paire. Aller au-delà ajouterait de la latence et des artefacts.
- **Latence :** l'interpolation attend l'image suivante, ce qui ajoute environ une image (~33 ms) à la latence du Remote Play. Ça va pour GTA, moins pour un jeu compétitif.
- Les **objets très proches de la caméra** qui bougent vite (buissons, piquets, reflets) peuvent encore baver légèrement.
- Sur un **changement de caméra brusque**, l'app affiche brièvement 30 fps plutôt qu'une image fausse (mode Clean).
- Les **pertes Wi-Fi** font des creux (40–50 fps quelques instants). L'Ethernet règle ça.

## Variables d'environnement (debug)

| Variable | Effet |
|---|---|
| `PSWRAP_VT_EDGEFILL=0` | désactive le remplissage des bords |
| `PSWRAP_NO_REFRESH_HINT=1` | désactive le verrouillage de l'écran ProMotion à 120 Hz pendant le frame gen |
| `PSWRAP_FG_MARK=1` | marque les images générées (pour vérifier dans un enregistrement) |
| `PSWRAP_VT_DEBUG=1`, `PSWRAP_PRESENT_LOG=1` | journaux détaillés de l'interpolation et de la présentation |

## Crédits et licence

- [PS-WRAP](https://github.com/iDevGIS/PS-WRAP) par iDevGIS : la base de ce fork (voir [README.PS-WRAP.md](README.PS-WRAP.md)).
- [chiaki-ng](https://github.com/streetpea/chiaki-ng) / Chiaki : le client Remote Play open source.
- [libplacebo](https://code.videolan.org/videolan/libplacebo), [FSRCNNX](https://github.com/igv/FSRCNN-TensorFlow).
- [rife-metal](https://github.com/cinemore/rife-metal) (Apache-2.0) et les poids [Practical-RIFE](https://github.com/hzwer/Practical-RIFE) 4.25 (MIT).

Distribué sous **GNU AGPL v3** (voir [LICENSE](LICENSE)), comme le projet d'origine.

Projet non officiel, sans lien avec Sony Interactive Entertainment ni Rockstar Games. PlayStation, PS5 et DualSense sont des marques de Sony ; Grand Theft Auto est une marque de Take-Two Interactive.

# GRIDLIGHT — séquenceur à grille lumineuse pour M-VAVE FM-1

Firmware alternatif qui transforme le FM-1 en instrument à grille lumineuse 16 × 16, sur le modèle du TENORI-ON de Yamaha.

On y retrouve :

- 256 LEDs rondes, blanches ou ambre, qui s'illuminent et font des ondes autour des notes jouées ;
- 16 couches et 16 blocs ;
- les six modes de jeu de l'original, rangés sur les mêmes couches : SCORE (1-7), RANDOM (8-11), DRAW (12-13), BOUNCE (14), PUSH (15) et SOLO (16) ;
- les réglages des boutons L1-L5 et R1-R5 : son, durée des notes, octave, boucle, vitesse, tempo, transposition, volume, couche et bloc ;
- 9 gammes, le swing, une réverbération et un chorus.

C'est un projet indépendant, sans lien avec Yamaha ni M-VAVE. Les 28 sons et les 4 kits de batterie sont synthétisés (aucun échantillon).

Il est bâti sur [foma](https://github.com/konsumer/foma) (plateforme Felucca), avec installeur web, mode secours et retour au firmware d'origine.

## Jouer sans écran tactile

Le FM-1 n'a pas de grille à toucher. **Une ligne de la grille est donc marquée en orange au bord de l'écran, et ses 16 cases sont les 16 touches blanches** (F3 à G5, de gauche à droite).

- **SCORE, RANDOM, DRAW, PUSH** : la ligne est une **colonne** (repère sous la grille). Les touches blanches sont ses rangées, de la plus grave (en bas) à la plus aiguë.
- **BOUNCE, SOLO** : la ligne est une **rangée** (repère sur les côtés). Les touches blanches sont ses colonnes, de gauche à droite.
- **SELECT**, ou **OCT− / OCT+**, déplace la ligne.
- Les touches blanches allumées montrent les cases de la ligne qui portent une note. Les touches noires ne servent pas.

| Mode | Couches | Ce que font les touches |
|---|---|---|
| SCORE | 1-7 | allument ou éteignent une note. Colonnes = temps, rangées = hauteur. Avec REC pendant la lecture, la note se pose sur la colonne qui joue : on enregistre en direct. |
| RANDOM | 8-11 | ajoutent ou retirent une note. Une lumière voyage de note en note dans l'ordre où elles ont été posées : plus elles sont loin l'une de l'autre, plus elle met de temps. SPIN (page FX) fait tourner la figure, et avec elle les notes. |
| DRAW | 12-13 | jouent la note. Avec REC pendant la lecture, la note est enregistrée à son moment exact (64 notes maximum), puis rejouée en boucle. SEL efface la couche. |
| BOUNCE | 14 | posent une balle dans la colonne de la touche, à la hauteur de la ligne marquée. Elle tombe et sonne en touchant le sol : plus elle part haut, plus l'intervalle est long. Sur la rangée du bas : plus de balle. |
| PUSH | 15 | tiennent une note qui s'éclaircit et dont la lumière s'élargit tant qu'on appuie. Tenue plus de deux temps, elle reste ; on la rappuie pour l'arrêter. |
| SOLO | 16 | répètent la note tant qu'on tient la touche, plus vite si la ligne est haute. Sur la rangée du bas, la note ne joue qu'une fois. Les répétitions sont calées sur le tempo. |

## Commandes

| Commande FM-1 | Fonction (équivalent TENORI-ON) |
|---|---|
| PRESETS | couche 1 à 16 (R1), avec son mode |
| ALGORITHM | bloc 1 à 16 (R5). En lecture, le changement se fait au temps suivant. |
| PLAY | lecture / arrêt (OK) |
| REC | enregistrement : SCORE en direct, DRAW |
| HOME | LAYER : SOUND (L1), LENGTH (L2, 20 ms à 5 s), OCTAVE (L3, −5 à +5), VOLUME (R4) |
| ENV | LOOP : START et END (L4), SPEED (L5 : 1/4, 1/8, 1/16, 1/32), PAN |
| LFO | SONG : TEMPO (R2, 40 à 240), TRANSPOSE (R3, −7 à +8), SCALE (9 gammes), SWING |
| FX | EFFECTS : REVERB, CHORUS, SPIN (RANDOM), LIGHT (blanc ou ambre) |
| SEL | efface la couche ; maintenu 1 s, efface le bloc (CLEAR) |
| GLO | coupe ou rétablit le son de la couche |
| SEQ | montre les notes de toutes les couches à la fois |
| EDIT | copie le bloc dans le suivant |
| ARP | lumières blanches / ambre |
| SAVE | garde la chanson (16 blocs) en mémoire flash |
| MASTER | volume |
| MIDI | en entrée, le canal n joue le son de la couche n ; en sortie, chaque couche envoie ses notes sur son canal |

Chaque couche a sa propre boucle et sa propre vitesse : des couches de longueurs différentes se décalent, comme sur l'original.

**Sons** : PIANO, E.PIANO, CELESTA, MUSIC BOX, MARIMBA, VIBES, GLASS, KALIMBA, STEEL DRUM, PLUCK, HARP, KOTO, CLAV, SOFT SQUARE, SINE LEAD, FLUTE, WARM PAD, STRINGS, CHOIR, SYNTH BRASS, SAW LEAD, ORGAN, BASS, SUB BASS, FM BASS, LOW BOOM, BLIP, SPARKLE, plus les kits DRUMS 808, DRUMS 909, DRUMS 606 et PERCUSSION. Avec un kit, chaque rangée est une percussion.

**Gammes** : majeur (les touches blanches, comme l'original par défaut), mineur, mineur harmonique, dorien, pentatonique, pentatonique mineure, blues, japonaise, chromatique.

**Morceau de démonstration** : au premier démarrage, le bloc 1 contient un petit morceau (piano, marimba, basse, batterie, cloches en RANDOM, balles en BOUNCE). Appuie sur PLAY pour l'entendre.

## Obtenir et installer le firmware

- **GitHub** : pousse ce dossier sur la branche `main` d'un dépôt. Le workflow compile tout.
  - Le `.fwsc` est dans l'artefact `gridlight-fwsc` et sur la branche `firmware`.
  - Avec Settings → Pages → Source : GitHub Actions, l'émulateur et l'installeur sont publiés sur GitHub Pages.
- **Docker Desktop** : `./fm1 build` ou `./fm1 dev` (http://localhost:8000).

L'installation se fait depuis la page Install (Chrome ou Edge, FM-1 branché en USB).

**Secours** : maintiens OCT− à l'allumage ; l'onglet Rescue remet le firmware M-VAVE V15. Installer un firmware non officiel se fait à tes risques.

## Développement

- `src/seq.c` : l'horloge (96 pas par temps) et les six modes. Ils tournent dans `audio()` ; `loop()` ne fait que mettre en file ce que font les mains.
- `src/snd.c` : 16 voix de synthé et 8 voix de batterie, la réverbération et le chorus (repris de GRVBOX) ; `src/tones.c` : les 28 sons.
- `src/main.c` : la grille de LEDs, la ligne liée au clavier, les pages, la sauvegarde compacte et le morceau de démonstration.
- `tests/run.sh` : joue un morceau dans les six modes et chaque son dans des WAV, vérifie le nombre de notes, l'enregistrement DRAW et la tenue PUSH, puis fait un fuzz (UBSan).
- Identité du paquet : `FM-1_46` (dans `firmware.json`).

## Licence

GPL-3.0-only. Crédits : [LICENSING.md](LICENSING.md).

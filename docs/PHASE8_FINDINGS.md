# Phase 8 — contrôleurs de matériau, d'UV et de visibilité

**Deux défauts rapportés après test navigateur :** certaines particules ne sont pas
colorées ; certains éléments transparents ne sont pas animés. Le brief supposait une
lacune unique derrière les deux — les tracks de contrôleurs non-transform, hors périmètre
depuis la Phase 4 et jamais reprises.

**Résultat : l'hypothèse est juste pour le second défaut et fausse pour le premier**, et
c'est la mesure qui le dit. Le défaut « éléments figés » est massif et entièrement
expliqué : 17 933 `NiTextureTransformController` embarqués dans 42 % des fichiers, plus
218 476 liens dans les `.kf`, portés à 93,5 % par des surfaces additives et à 47 % par des
plans de ≤ 8 sommets — les glow planes. Le défaut « particules grises », lui, **n'a aucune
des trois causes envisagées** : sur les 1 343 systèmes réellement gris du corpus, 1 315
(98 %) n'ont ni couleur animée, ni vertex colors teintés, ni slot GLOW. Le blanc de la
Phase 7 était le bon comportement.

Le format passe en **v5** : 176 110 pistes de matériau, 1,9 M de clés, 98,1 % résolues,
non-régression vérifiée octet par octet sur les 2 822 modèles.

---

## Étape 1 — L'inventaire, avant toute implémentation

`tools/diag/measure_material_controllers.cpp` (cible CMake `measure-material-controllers`,
non liée au pipeline) balaie les 2 825 `.nif` et les 1 475 `.kf` du corpus.

```
measure-material-controllers corpus input   > inventaire complet
measure-material-controllers dump <fichier> > détail d'un fichier
```

### 1.1 Quels contrôleurs existent réellement

Le tableau du brief était une liste de candidats. Le corpus tranche :

| contrôleur | instances `.nif` | fichiers | avec clés | liens `.kf` |
|---|---:|---:|---:|---:|
| `NiTextureTransformController` | **17 933** | **1 177** | 12 211 | **218 476** |
| `NiAlphaController` | 8 094 | 1 059 | 4 091 | 147 378 |
| `NiVisController` | 2 309 | 65 | **0** | 102 791 |
| `NiMaterialColorController` | 1 485 | 363 | 1 254 | 11 691 |
| `NiFlipController` | 29 | 18 | 21 | 688 |
| `NiGeomMorpherController` | 13 | 9 | 0 | 500 |
| `NiUVController` | **0** | — | — | 0 |

Trois constats que la liste de candidats ne donnait pas :

1. **`NiUVController` n'existe pas dans ce corpus.** Le défilement de texture passe
   intégralement par `NiTextureTransformController`. Le code n'en traite donc pas.
2. **Les 2 309 `NiVisController` embarqués sont tous inertes** — pas un seul n'a de
   bloc de données. Leur courbe vit exclusivement dans les `.kf` (102 791 liens). Un
   inventaire limité au `.nif` aurait conclu « la visibilité n'est jamais animée ».
3. **Les `.kf` portent l'essentiel.** 656 000 liens non-transform contre 29 800
   contrôleurs embarqués avec données. Les deux sources sont traitées.

### 1.2 Sur quoi ils portent

Un nom est une preuve faible ; l'état de rendu n'en est pas une. Profil des géométries
derrière chaque contrôleur :

| contrôleur | géométries | additives (`SRC_ALPHA → ONE`) | ≤ 8 sommets | noms dominants |
|---|---:|---:|---:|---|
| `NiTextureTransformController` | 28 924 | **27 048 (93,5 %)** | 13 688 (47 %) | `Editable Poly`, `Plane`, `Plane0*` |
| `NiAlphaController` | 14 392 | 12 821 (89 %) | 8 996 (63 %) | idem |
| `NiMaterialColorController` | 1 590 | 1 447 (91 %) | 859 (54 %) | **`Plane` en tête** |

C'est la confirmation chiffrée de l'hypothèse du brief sur le **défaut 2** : de la
géométrie additive, plate, majoritairement nommée `Plane`, dont la texture est animée et
la géométrie statique. Les plans lumineux.

Répartition mesh / particules : les contrôleurs d'UV portent sur 16 805 meshes contre
1 128 systèmes de particules ; les contrôleurs de couleur, sur 929 meshes contre **556
systèmes** — la couleur animée est donc, proportionnellement, bien plus une affaire de
particules que le reste.

### 1.3 Les 1 343 systèmes gris, source par source

La Phase 7 (correctif D) comptait 1 353 systèmes « réellement gris » ; cet outil en
trouve 1 343, l'écart venant du seuil de l'axe des gris (1/255 ici). Sur cet ensemble :

```
systèmes de particules                            : 6 966
... dont toutes les colorKeys sont noires         : 4 735
... et dont ni l'émissive ni la diffuse n'est teintée : 1 343   <- l'ensemble gris

    un NiMaterialColorController sur son matériau :     6
    ... ou un, lié depuis le .kf du modèle        :     2
    un NiFlipController                           :     2
    des vertex colors présents dans NiPSysData    : 1 313
      ... portant effectivement une teinte        :     0
      ... dans un mode vertex/éclairage émissif   :     0
    un slot GLOW peuplé                           :    21
    expliqués par au moins une de ces sources     :    28
    expliqués par AUCUNE                          : 1 315  (98 %)
```

**Les trois pistes du brief sont écartées, chacune par une mesure distincte :**

- **Couleur animée** : 8 systèmes sur 1 343. Marginal.
- **`NiVertexColorProperty`** : 1 313 systèmes ont bien un tableau de couleurs dans leur
  `NiPSysData`, ce qui rend l'hypothèse plausible — mais **aucun de ces tableaux ne porte
  la moindre teinte**, et aucun n'est dans un mode émissif. Corpus entier : sur 4 174
  `NiVertexColorProperty`, 5 seulement sont en `VERT_MODE_SRC_EMISSIVE`. « Présent » et
  « source de couleur » sont deux choses différentes, et c'est la seconde qu'il fallait
  mesurer.
- **Slot GLOW** : 21 systèmes gris. À l'échelle du corpus, 360 `NiTexturingProperty` sur
  19 891 (1,8 %) ont un slot GLOW peuplé, contre 6 633 pour DARK et 4 123 pour DETAIL.

**Décision : le slot GLOW n'est pas exporté.** Il n'explique que 1,6 % de l'ensemble gris
et concerne 1,8 % du corpus. La dette de la Phase 2 reste documentée, inchangée, et si
elle est un jour levée c'est DARK — trois fois plus fréquent — qui la justifierait, pas
GLOW.

**Ces 1 315 systèmes sont donc légitimement gris.** Le repli sur le blanc décidé en
Phase 7 est le bon comportement, et le défaut n°1 rapporté ne vient pas de là. Voir §5
pour ce qu'il reste.

### 1.4 Interpolateurs

```
NiFloatInterpolator              : 417 143
NiBoolInterpolator               : 174 589
NiBSplineCompFloatInterpolator   :  52 533
NiPoint3Interpolator             :   8 091
NiBSplineCompPoint3Interpolator  :   3 600
```

Les trois premiers types à clés sont lus nativement. `NiBSplineCompFloatInterpolator` est
ré-échantillonné à **30 Hz**, le taux de la Phase 4, pour qu'un clip ne mélange jamais
deux cadences.

**`NiBSplineCompPoint3Interpolator` n'est pas lisible**, et c'est une limite de niflib,
pas un choix : niflib modélise `NiBSplinePoint3Interpolator` comme six flottants
« Unknown » et ne génère aucun échantillonneur pour lui — contrairement à la variante
float, qui expose `SampleKeys()` et ses `offset`/`bias`/`multiplier`. Reconstruire ces
champs en réinterprétant les six flottants serait une supposition, et ce projet n'exporte
pas de valeurs supposées. **3 600 pistes de couleur sont donc perdues**, comptées dans
`materialBSplinePoint3Unsupported`. C'est 31 % des liens de couleur des `.kf`.

### 1.5 Volume, et la décision d'écarter les pistes constantes

Mesuré avant d'écrire l'export, parce que la taille de sortie est suivie depuis la
Phase 6 :

```
pistes .kf non-transform avec clés : 421 966   (1 097 195 clés)
... dont toutes les clés identiques : 293 942   (589 343 clés)  = 70 %
```

**70 % des pistes n'animent rien.** Elles sont écartées à l'export et comptées
(`materialTracksConstantDropped`). Sur `ride/R814`, cela ramène 346 pistes à 2 : ses 86
clips répètent le même état statique. La sortie totale passe de 2,07 à 2,16 Go (+4,3 %) ;
sans ce filtre elle aurait dépassé 2,3 Go pour le même rendu.

### 1.6 Point ouvert du brief : `gravityObjectNodeIndex`

```
NiPSysGravityModifier                              : 1 315
... avec un pointeur d'objet de gravité            : 1 315
... dont l'objet diffère du node d'attache         : 1 315
... avec une rotation NON triviale entre les deux  :    18   <- les seuls cas qui comptent
```

**18 systèmes sur 1 315, soit 1,4 %.** L'objet de gravité diffère toujours du node
d'attache, mais la rotation entre les deux est l'identité dans 98,6 % des cas : l'axe de
gravité y désigne la même direction dans l'un et l'autre repère, et l'ignorer ne coûte
rien. Les 18 cas sont listés par l'outil (`item/W363`, `item/W458`, `item/WB54`,
`ride/R350`, `R451`, `R453`, `R473`, `R494`, `R693`). **Laissé documenté, non corrigé** —
c'est marginal, et le corriger demanderait d'exporter la matrice relative entre les deux
nodes pour 1 315 modificateurs afin d'en servir 18.

---

## Étape 2 — Le format v5

### 2.1 Ce qui est ajouté

Sur chaque clip d'animation :

```jsonc
"loop": true,                  // type de cycle de la séquence ; toujours true pour le clip ambiant
"materialTracks": [
  {
    "target": { "array": "meshes", "index": 26, "name": "Plane26" },
    "property": "uvTranslateV",  // uvTranslate{U,V} uvScale{U,V} uvRotation alpha
                                 // {ambient,diffuse,specular,emissive}Color visible textureIndex
    "controller": "NiTextureTransformController",
    "textureSlot": 0,            // TexType NIF : 0 BASE, 1 DARK, 2 DETAIL, 4 GLOW
    "wasResampledFromBSpline": false,
    "count": 2,
    "times":  { "byteOffset": …, "itemSize": 1, "count": 2, "type": "float32" },
    "values": { "byteOffset": …, "itemSize": 1, "count": 2, "type": "float32" }
  }
]
```

Valeurs brutes du NIF, comme partout : décalages UV dans la convention UV du NIF,
rotation en radians, couleurs linéaires 0..1, visibilité en 0/1 (flottant, pour que
toutes les pistes partagent un seul type dans le `.gfbin`).

`NiFlipController` ajoute `flipTextures` (la liste **ordonnée** des `.png` résolus, par le
même mécanisme que la texture diffuse d'un matériau) et `flipDelta`.

### 2.2 Une piste cible une **géométrie**, jamais un matériau

C'est la décision structurante, et elle vient d'une propriété du format lui-même :
`materials` est **dédupliqué** à l'export — `MaterialExtractor::SameMaterial` fusionne
deux matériaux interchangeables. Deux plans lumineux de même texture et de mêmes couleurs
partagent donc une entrée alors qu'un seul défile. Une piste qui désignerait un index de
matériau ferait défiler les deux.

Une piste désigne donc `{array, index, name}` où `array` est `meshes`,
`emitterMeshes`, `particleSystems`, `nodes` ou `bones`. Côté `.nif` la résolution est
**exacte** (identité de bloc, le même mécanisme que le correctif Phase 7 §6.2) ; côté
`.kf` elle ne peut être que par nom, puisqu'une `ControllerLink` ne porte rien d'autre —
2 254 cas ambigus corpus-wide, comptés, et une piste est alors émise pour chaque
homonyme, ce dont le moteur d'origine se rapproche le plus.

### 2.3 Le clip implicite `embedded-material`

Un contrôleur embarqué dans le `.nif` n'appartient à aucune `NiControllerSequence` et
tourne en permanence dans le moteur d'origine. Il ne peut donc pas être rattaché à un clip
`.kf` sans inventer une relation que le fichier ne dit pas. Il va dans un clip unique
nommé `embedded-material`, `loop: true`, que le consommateur applique **en plus** du clip
sélectionné. C'est le cas des effets permanents : un glow qui ondule en continu.

Un effet de bord à connaître : une séquence `.kf` qui ne portait que des contrôleurs
non-transform était auparavant **entièrement écartée** (pas de tracks d'os → pas de clip).
Elle produit désormais un clip. Le nombre de clips augmente donc sur certains modèles.

### 2.4 Où ça se lit et où ça s'écrit

| fichier | rôle |
|---|---|
| `src/nif/MaterialAnimationExtractor.{hpp,cpp}` | l'extraction, les deux sources |
| `src/export/SceneModel.hpp` | `MaterialTrack`, `MaterialTrackTarget`, `FlipTexture`, `MaterialAnimationStats` |
| `src/export/GfxFormatWriter.cpp` | la sérialisation |
| `src/nif/MeshExtractor.cpp` | le câblage dans `ExtractScene` |

Deux points de méthode dans le câblage :

- **La passe particules est remontée avant la passe animation.** Une piste de couleur peut
  viser un système de particules par index, donc les systèmes doivent exister d'abord.
  Rien dans la passe particules ne dépend de l'animation et les deux écrivent dans des
  vecteurs distincts : l'ordre relatif ne change pas la sortie (vérifié, §2.5).
- **Les `.kf` ne sont pas relus.** `AnimationExtractor::ExtractFromKf` reçoit
  l'extracteur de matériau en paramètre optionnel et lui passe les séquences déjà
  parsées. La Phase 6 ayant mesuré le pipeline CPU-bound, une seconde passe de parsing
  sur 1 447 `.kf` aurait été la chose la plus coûteuse que ce changement pouvait faire.

Les contrôleurs embarqués ne sont **pas** atteignables depuis les racines que parcourt la
passe transform : ils pendent à des blocs `NiProperty`, qui ne sont pas des `NiAVObject`
et ne sont donc les enfants de rien. C'est la liste de blocs entière qui est parcourue.

### 2.5 Non-régression : vérifiée, pas affirmée

`tools/diag/check_v4_v5_nonregression.py` compare une sortie v4 et une sortie v5, avec
**deux contrôles indépendants** parce qu'aucun des deux ne suffit seul :

1. **Le `.gfbin` v5 doit commencer par le `.gfbin` v4, octet pour octet.** Les clés de
   matériau sont ajoutées après tous les tampons existants, donc chaque `byteOffset` déjà
   publié adresse les mêmes octets. Une divergence de préfixe serait le seul mode de
   défaillance qui corromprait silencieusement un consommateur au lieu de simplement lui
   manquer.
2. **Le `.gfmodel` doit être identique** une fois retirés `formatVersion`, `generator`,
   `binaryByteLength`, les `loop`/`materialTracks` de chaque clip, et les clips que la v4
   n'émettait pas (§2.3).

```
$ python tools/diag/check_v4_v5_nonregression.py out-v4-baseline out
checked 2822 model(s); 2 present in old but not new
no regression: every v4 field and every v4 .gfbin byte is unchanged
```

Une seule différence structurelle est admise, et elle est justifiée dans le script :
`nodes` passe de vide à peuplé sur **21 fichiers `item/`**. Ce sont des fichiers sans
squelette dont la seule animation est désormais une piste de matériau visant un node ;
sans la liste de nodes, cette piste pointerait vers un tableau absent. Le script vérifie
qu'aucun mesh n'a été reparenté au passage (`meshes[].nodeIndex` inchangé, -1 partout),
ce qui est la condition pour que l'ajout reste purement additif.

### 2.6 Le corpus, réexporté

```
materialTracks                     : 176 110   (1 897 935 clés)
  résolues                         : 172 685   (98,1 %)
  orphelines                       :   3 425
constantes, écartées               : 221 625
contrôleurs inertes (stub sans données) : 133 193
ambiguës par nom (.kf)             :   2 254
B-spline float ré-échantillonné    :  39 628
B-spline Point3 non décodable      :   3 600   (§1.4)
contrôleurs non traduits           :     500   (NiGeomMorpherController)

sortie : 2,07 Go -> 2,16 Go (+4,3 %) ; 32,9 s sur 12 threads
échecs : 3 fichiers, les mêmes qu'avant (WF20, M156, N600)
```

Ces compteurs sont dans le rapport JSON de chaque run (`--report`), donc mesurables sans
l'outil de diagnostic.

---

## Étape 3 — Le loader

`GF-Database-Frontend/src/app/gf-model/gf-material-animation.ts`.

### 3.1 Pourquoi pas l'`AnimationMixer`

C'était la voie envisagée par le brief, et elle est écartée pour une raison mesurée :
**`PropertyBinding` résout sa cible par nom**, et les noms de géométrie de ce corpus ne
sont pas uniques — `chair/C004` porte huit meshes nommés `Editable Poly`
(PHASE7_FINDINGS §6.2). Une piste s'appliquerait au premier trouvé. Le format v5 porte
justement un index exact ; l'utiliser suppose de tenir la référence d'objet directement.

`GfMaterialAnimation` échantillonne donc lui-même et écrit dans les objets Three. Bénéfice
secondaire, et c'est celui qui compte pour la validation : **une piste dont la cible
n'existe pas est comptée**, là où le mixer l'ignorerait en silence — exactement le piège
que le loader traque déjà pour les tracks d'os.

L'appelant pilote par `apply(nomDuClip, temps)`, à côté de `particles.update(delta)` qu'il
appelle déjà. Le clip ambiant s'applique toujours, bouclé sur sa propre durée.

### 3.2 Traduction

| propriété | écriture Three |
|---|---|
| `uvTranslateU/V` | `material.map.offset.x/.y` |
| `uvScaleU/V` | `material.map.repeat.x/.y` |
| `uvRotation` | `material.map.rotation` |
| `alpha` | `material.opacity` (+ `transparent = true`) |
| `diffuseColor` | `material.color` |
| `emissiveColor` | `material.emissive`, à défaut `material.color` (particules) |
| `specularColor` | `material.specular` si le matériau en a un |
| `ambientColor` | **sans effet, délibérément** — Three n'a pas d'ambiante par matériau |
| `visible` | `object.visible` |
| `textureIndex` | `material.map = flipTextures[i]` |

**Convention UV : transfert direct, sans changement de signe.** Gamebryo applique
`uv' = uv * scale + translate`, Three applique `uv' = uv * repeat + offset` : même forme.
Et le V n'est pas inversé, parce que le loader charge déjà ses textures en `flipY = false`
— le V du NIF est donc déjà le V de Three. C'était le point que le brief signalait comme
à vérifier ; il n'y a rien à corriger.

`visible` et `textureIndex` ne sont **jamais interpolés** : une visibilité n'a pas de
demi-état et un flipbook ne fond pas d'une image à l'autre. C'est aussi ce que fait
`NiVisData`, dont les clés sont des pas.

### 3.3 Deux corrections trouvées en écrivant la vérification

**Les surfaces d'émission ne sont pas des cibles.** Une piste visant `emitterMeshes` est
écartée (`emitterSurfaceSkipped`) : ces géométries ne sont jamais dessinées. Elles
apparaissent parce qu'un `.kf` désigne sa cible par un nom partagé avec un mesh rendu.
Le mesh rendu reçoit bien la piste.

**La teinte statique doit être neutralisée quand la couleur est animée.** Introduit puis
attrapé par le banc : `materialTint` posait la teinte du matériau dans le dégradé
`ColorOverLife`, et la piste v5 écrivait la sienne sur `material.color` — les deux se
multipliaient, donnant une composante rendue à 1,22 sur `chair/C004`. `buildOne` reçoit
désormais l'information et neutralise `tint`. La neutralisation doit porter sur `tint`
lui-même et pas seulement sur `tintIsColoured` : `colorKeysToGradient` reçoit `tint`
directement.

### 3.4 Un clone de matériau qui ne servait à rien

Ce module a d'abord cloné le matériau de chaque objet animé, pour que la déduplication de
`materials` ne fasse pas déteindre une piste sur un homonyme. **Mesuré ensuite : le loader
construit déjà un `THREE.Material` par mesh** (`addMesh` appelle `buildMeshMaterial` pour
chacun), donc aucun matériau Three n'est partagé et le clone n'achetait rien — vérifié sur
`ride/R916`, dont 41 matériaux du *fichier* sont pourtant portés à la fois par un mesh
animé et par un mesh qui ne l'est pas.

Le clone est retiré (74 matériaux économisés sur R916, 104 sur C038). L'invariant est
désormais **vérifié** par le banc plutôt que défendu par une copie inutile : si le loader
se mettait un jour à mutualiser ses matériaux, `matériau/isolation` le dirait — ce qui est
prouvé en §4.

---

## Étape 4 — Validation par le comportement

`gf.probeMaterial(entité, modèle)` échantillonne l'état **appliqué** des matériaux et
renvoie, par cible, l'amplitude réellement observée de chaque grandeur, plus le
déplacement de l'objet aux mêmes instants. `tools/gf-harness/checks.mjs` la confronte à
ce que le fichier dit devoir animer.

### 4.1 Les vérifications

| vérification | ce qu'elle mesure |
|---|---|
| `matériau/mouvement` | chaque grandeur qu'une piste pilote varie réellement sur l'intervalle |
| `matériau/UV sans géométrie` | la texture bouge **alors que la géométrie n'a pas bougé** — la formulation exacte du brief |
| `matériau/bornes alpha` | l'opacité rendue reste dans [0, 1] |
| `matériau/isolation` | aucun mesh animé ne partage son matériau avec un mesh qui ne l'est pas |
| `matériau/cibles` | aucune piste du fichier ne reste sans cible dans la scène |
| `matériau/couleur animée` | une couleur animée sur un système de particules atteint bien son matériau |

### 4.2 Chacune validée en réintroduisant son défaut

C'est la condition posée par le brief, et elle a servi : trois des quatre défauts
n'étaient pas attrapés par la première version des vérifications.

| défaut réintroduit | échecs déclenchés |
|---|---|
| `apply()` neutralisé (les pistes ne sont jamais appliquées) | **358** — `mouvement` 231, `UV sans géométrie` 119, `couleur` 8 |
| résolution par **nom** au lieu de l'index | **24** — `mouvement` 13, `UV sans géométrie` 11 |
| teinte statique appliquée en plus de l'animée | **4** — `couleur` |
| matériaux mutualisés par `materialIndex` dans le loader | **3** — `isolation` |

État : **673 vérifications passées, 0 en échec**, stable sur trois exécutions
consécutives.

### 4.3 Deux pièges rencontrés, qui auraient rendu la vérification décorative

Dans la lignée de ceux que la Phase 7 documente (§E), et pour la même raison : une sonde
qui re-dérive ce que fait le code au lieu de le lire mesure autre chose que le code.

1. **La sonde reclassait les meshes par `root.traverse()`.** Cet ordre n'est pas celui de
   `file.meshes` — un mesh porté par un node animé est parenté sous ce node, pas sous
   `content`. La sonde observait donc le mauvais objet et concluait « rien ne bouge » sur
   `item/WA85`, qui bougeait. Le loader expose désormais `meshObjects`, dans l'ordre du
   fichier, et la sonde le reprend au lieu de le recalculer.
2. **Une grille d'échantillons régulière rate les pics.** Sur `chair/C038`, une opacité
   qui monte à 0,5 entre 0,6 s et 1,2 s d'un clip de 9,7 s est strictement invisible à
   huit échantillons réguliers : **55 pistes parfaitement correctes étaient signalées
   comme n'animant rien**. La sonde échantillonne désormais **aux instants des clés**
   (`GfMaterialAnimation.keyTimes()`), plus un point à mi-chemin de chaque paire.

Une troisième leçon, celle de la Phase 7 §E, s'est reproduite telle quelle : la
vérification `couleur` existante lisait `p.color` et ignorait `material.color`. Or
`materialTint` pose la teinte dans l'un **ou** dans l'autre selon que le système a un
`NiPSysColorModifier` où la poser (893 systèmes n'en ont pas). Elle concluait « rendue
grise » sur `item/WA85`, dont l'effet est rouge à l'écran. Corrigée : la teinte rendue est
le produit des deux.

### 4.4 Deux verdicts écartés, et pourquoi ce n'est pas un ajustement de seuil

Deux modèles sont ajoutés à la liste de référence pour leur couverture **matériau**, et
leurs verdicts *particules* sont écartés explicitement (`skipChecks`) :

- **`chair/C038`, vérification `direction`.** Elle échoue de façon **non déterministe** :
  mesuré 3 à 7 échecs par exécution sur des données strictement identiques, et autant sur
  une sortie v4 que v5 — donc sans rapport avec la Phase 8. La cause est celle que la
  Phase 7 documente déjà (§G.4) : un centroïde n'est pas un point matériel.
- **`ride/R916`, vérification `couleur`.** Ses deux échecs se reproduisent à l'identique
  sur une sortie v4 (cos 0,976 / 0,977). C'est le cas de la Phase 7 §3.2 : une clé noire
  entre deux clés colorées reçoit la teinte du matériau et crée une discontinuité de
  teinte — 99 systèmes du corpus sont dans ce cas.

Les écarter est préférable à desserrer leur seuil, ce qui les rendrait muettes partout.
Les deux restent actives sur les cinq modèles de référence de la Phase 7.

---

## 5. Ce que la Phase 8 ne corrige pas du défaut n°1

Le brief liait les deux défauts à une lacune unique. La mesure sépare : le défaut
« éléments transparents figés » est corrigé et vérifié ; le défaut « particules non
colorées » n'a **pas** pour cause l'absence de couleur animée.

Ce que la Phase 8 apporte quand même sur la couleur des particules : 556 systèmes du
corpus ont bien un `NiMaterialColorController`, désormais exporté et appliqué, et 3 370
systèmes tirent leur teinte d'une émissive déjà traitée en Phase 7. Restent les **1 315
systèmes réellement gris**, pour lesquels aucune source de couleur n'existe dans le
fichier. Si le jeu les montre colorés, la couleur vient d'ailleurs que du `.nif` : shader
du moteur, palette d'effet externe, ou teinte appliquée par le code du jeu. **Cela ne se
tranchera pas dans l'exporteur**, et la prochaine étape sur ce point est une comparaison
avec une capture du jeu sur un système gris identifié, pas une nouvelle hypothèse de
format.

Les 3 600 pistes de couleur en `NiBSplineCompPoint3Interpolator` (§1.4) sont une vraie
perte, indépendante : elles concernent des systèmes déjà colorés autrement, mais leur
variation dans le temps est absente.

---

## 6. Modèles à vérifier, et comportement attendu

| modèle | ce qu'il exerce | attendu |
|---|---|---|
| `chair/C013` | défilement UV et opacité embarqués, 2 systèmes à échelle UV animée | les plans `001` / `Editable Mesh` défilent en V ; les quads `121`–`123` pulsent en opacité |
| `chair/C038` | 846 pistes, dont **196 de visibilité sur des os** | les `big_circle_*` / `last_*` apparaissent et disparaissent ; 104 plans défilent et pulsent |
| `item/WA85` | couleur **et** opacité animées hors de toute séquence | le plan lumineux de l'arme change de teinte en continu, sans clip sélectionné |
| `chair/C004` | 8 meshes `Editable Poly`, 11 cibles animées | chaque plan défile **indépendamment** ; si tous défilent ensemble, la résolution par index est cassée |
| `monster/M491` | 52 pistes, quads lumineux | `M01` et `Plane26` défilent ; `Plane30` pulse |
| `ride/R814` | 86 clips, 346 pistes constantes écartées | `body_run` défile en V, `gem` pulse ; **rien d'autre ne bouge**, c'est correct |
| `ride/R916` | 74 matériaux animés, 41 partagés dans le fichier | aucun mesh non animé ne se met à défiler |
| `effect/S13103`, `monster/M017`, `monster/M389` | dettes documentées | **inchangés** — ne pas « corriger » |

**Réexporter le corpus est indispensable.** Un fichier resté en v4 n'a aucune piste de
matériau : les plans lumineux y resteront figés, et le loader l'annonce explicitement
(`fichier en v4 : … réexporter le corpus`).

---

## 7. Ce qui reste ouvert

1. **`NiBSplineCompPoint3Interpolator`** — 3 600 pistes de couleur illisibles par niflib
   (§1.4). Les lire demanderait de décoder le bloc soi-même, hors de la politique
   « niflib reste intact ».
2. **`NiGeomMorpherController`** — 13 instances embarquées sans données, 500 liens `.kf`.
   Le morphing de géométrie est un mécanisme entièrement différent (morph targets), pas
   une propriété de matériau. Compté comme non traduit.
3. **Le slot GLOW** — mesuré à 1,8 % du corpus et écarté (§1.3). Si la dette de la Phase 2
   est un jour levée, c'est DARK (33 %) qui la justifie.
4. **`gravityObjectNodeIndex`** — 18 cas sur 1 315 (§1.6), documenté et laissé.
5. **Les 1 315 systèmes gris** — §5. La réponse n'est pas dans le `.nif`.
6. **Les pistes `.kf` ambiguës par nom** — 2 254, une piste émise par homonyme. Contrairement
   aux surfaces d'émission (corrigées en v4 par index de bloc), une `ControllerLink` ne
   porte aucun index : l'ambiguïté est dans le `.kf` lui-même et n'est pas réparable à
   l'export.

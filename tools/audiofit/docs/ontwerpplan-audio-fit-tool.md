# Ontwerpplan: Audio Fit Tool

Versie 0.7 — 17 november 2026
Status: fase 1 gebouwd (v0.1.0). Geschreven als bouwinstructie voor een AI-codeassistent.

Wijzigingen t.o.v. 0.6 (na gebruikerstest):
- Nieuwe blokken standaard fit-modus `manual` ("origineel"): fragmenten op originele lengte; rekken alleen op verzoek (duurveld, randgreep, of "Pas blok in" = `proportional`).
- Indeling blokgericht (§9): blokkenlijst links, actief blok groot in het midden, inspector rechts, bronnen in een inklapbare la onderin.
- Verwijderen met Delete/Backspace en rechtsklikmenu in de tijdlijn; anker verwijderen vraagt wat er met de stem moet gebeuren.

Wijzigingen t.o.v. 0.5:
- Fase 5: voice-over-recorder met loopback-kalibratie van de latency (§6.11).

Wijzigingen t.o.v. 0.4:
- Open vragen §14 beantwoord: er klinkt altijd muziek; voice-over ligt als tweede laag over een deel van het blok, over andere muziek dan het deel zonder voice-over. Nieuw §6.10 (stemspoor) en recept `version: 2`.
- Voice-over hangt aan een muziekfragment (anker + offset) en schuift mee met de fit.
- Stem standaard op oorspronkelijke snelheid; per stemfragment optioneel een doelduur (licht rekken/indikken, preset `voice`).
- Automatische ducking van de muziek onder de stem, instelbaar per blok.
- Overlappende stemfragmenten: waarschuwing die de gebruiker kan accepteren.
- Rekenwerk in een Worker met zichtbare voortgang (balk + percentage) en UI die bruikbaar blijft.
- Tijden in het recept met 6 decimalen (was 3), zodat snap-punten behouden blijven.

Wijzigingen t.o.v. 0.3:
- Node.js en ffmpeg zijn vereisten voor de CLI (aanwezig of te installeren); opdracht `doctor` controleert dit.
- Materiaal is muziek en voice-over → twee WSOLA-presets per fragment: `music` en `voice`.
- Geen batchfunctie: het gaat om incidenteel een beperkt aantal blokken.
- Nieuwe open vraag over voice-over als laag bóven muziek (§14).

Wijzigingen t.o.v. 0.2:
- Toonhoogte blijft gelijk → WSOLA is de standaard tijdmodus. Varispeed blijft als optie per fragment.
- AI-aansturing: route A (kopiëren/plakken) plus een commandline-programma dat Claude Code kan besturen. Het ingebouwde API-paneel (route B) vervalt; de tool heeft geen netwerkverkeer.
- De AI kiest geen fragmenten → de audio-analyse voor de AI vervalt.
- Markers: door de gebruiker te plaatsen, benoemen en te gebruiken als knip- en verwijspunt.
- Overlap = fragmenten binnen een blok (bevestigd).

---

## 1. Doel

Audiobestanden (WAV, MP3) inladen, er nette fragmenten uit knippen en die in tijd rekken of indikken tot een exact opgegeven duur, met behoud van toonhoogte. Einddoel: blokken van een vaste duur (bijv. 15,000 s) vullen met een of meer fragmenten, eventueel overlappend, en die als WAV exporteren. De gebruiker kiest de fragmenten op het gehoor; een Claude-AI kan het samenstellen, fitten en exporteren aansturen.

Kernvoorbeeld: fragment van 12 s → 10 s, zelfde inhoud en toonhoogte, 20% sneller.

## 2. Twee onderdelen, één kern

| Onderdeel | Bestand | Functie |
|---|---|---|
| **Editor** | `audiofit.html` | Grafisch: laden, luisteren, knippen, markers, blokken, overgangen, voorbeluisteren, exporteren. Eén losstaand bestand, werkt offline. |
| **CLI** | `audiofit-cli.mjs` | Commandline, zonder scherm: recept valideren, wijzigen en renderen naar WAV. Bedoeld voor Claude Code. |

Beide gebruiken dezelfde kerncode. Die staat in `audiofit.html` in `<script id="audiofit-core">` en bevat geen DOM- of browser-API's. De CLI leest dat scriptblok uit het HTML-bestand en voert het uit. Zo is er één bron van waarheid en geen build-stap.

Uitwisseling tussen beide: het **recept** (JSON, §4.1).

## 3. Ontwerpbeslissingen en aannames

| # | Beslissing | Status |
|---|---|---|
| B1 | Editor: vanilla JavaScript, geen externe bibliotheken, geen netwerkverkeer. Desktopbrowser (Chrome/Edge/Firefox, recent). | Keuze |
| B2 | CLI: Node.js (versie 18 of hoger), alleen ingebouwde modules, geen `npm install`. | Besloten: aanwezig of wordt geïnstalleerd |
| B3 | Tijd aanpassen: eigen WSOLA-implementatie (toonhoogte blijft gelijk). Varispeed als optie per fragment. | Besloten |
| B4 | Export: WAV, 16-bit PCM. | Besloten |
| B5 | Non-destructief: een fragment verwijst naar bron + in/uit + instellingen. | Keuze |
| B6 | Eén commandolaag: elke handeling loopt via `AudioFit.exec(commando)`, in de editor én in de CLI. | Keuze |
| B7 | De AI kiest geen fragmenten en beluistert niets. De AI werkt met wat de gebruiker heeft aangewezen (selecties, markers, fragmenten). | Besloten |
| B8 | MP3 in de CLI vereist `ffmpeg` op het systeempad; WAV werkt zonder. | Besloten: aanwezig of wordt geïnstalleerd, zie §8.3 |
| B9 | Materiaal: muziek en voice-over. Per fragment een preset `music` of `voice` die de WSOLA-parameters bepaalt. | Besloten |
| B10 | Geen batchfunctie; blokken worden één voor één samengesteld. | Besloten |

## 4. Begrippen en datamodel

- **Bron**: een ingeladen audiobestand.
- **Marker**: benoemd tijdpunt in een bron, geplaatst door de gebruiker.
- **Fragment**: uitsnede uit een bron (in, uit) plus instellingen.
- **Blok**: container met vaste doelduur met één of meer fragmenten achter elkaar.
- **Overgang**: hoe fragment i overgaat in fragment i+1 (type + duur).
- **Fit**: berekening die de fragmenten zo rekt/indikt dat het blok exact de doelduur heeft.
- **Ratio**: `brondur / doeldur`. Ratio 1,2 = 20% sneller.
- **Recept**: JSON-beschrijving van het hele project, zonder audiodata.

### 4.1 Recept

```json
{
  "version": 1,
  "sampleRate": 48000,
  "defaults": { "blockDuration": 15.0, "fadeMs": 8,
                "transition": { "type": "crossfade", "ms": 500, "curve": "equal_power" } },
  "sources": [
    { "name": "intro.mp3", "duration": 184.32, "channels": 2,
      "markers": [ { "name": "refrein", "time": 42.118 }, { "name": "slot", "time": 171.6 } ] }
  ],
  "blocks": [
    { "id": "b1", "name": "Blok 1", "targetDuration": 15.0, "fitMode": "proportional",
      "fragments": [
        { "id": "f1", "source": "intro.mp3", "in": 42.118, "out": 48.750,
          "mode": "pitch", "fadeInMs": 8, "fadeOutMs": 8, "gainDb": 0,
          "locked": false, "manualDuration": null },
        { "id": "f2", "source": "regen.wav", "in": 0.000, "out": 9.800,
          "mode": "pitch", "fadeInMs": 8, "fadeOutMs": 8, "gainDb": -3,
          "locked": false, "manualDuration": null }
      ],
      "transitions": [ { "type": "crossfade", "ms": 800, "curve": "equal_power" } ]
    }
  ]
}
```

- Tijden in seconden, 3 decimalen; intern in samples.
- `transitions[i]` hoort bij het paar `fragments[i]` → `fragments[i+1]`.
- Bronnen worden gekoppeld op bestandsnaam.
- `mode`: `"pitch"` (WSOLA, standaard) of `"varispeed"`.
- `preset` (per fragment, niet in het voorbeeld getoond): `"music"` (standaard) of `"voice"`; bepaalt de WSOLA-parameters (§7.1). In de editor een keuzeknop in het detailpaneel van het fragment; als commando-argument bij `add_fragment` en `update_fragment`.
- `sampleRate`: werkfrequentie; de editor vult die met de `AudioContext`-frequentie.

## 5. Architectuur

```
Editor-UI (DOM, canvas)            CLI (argumenten, bestanden)
        │                                   │
        └────────────┬──────────────────────┘
                     ▼
   Commandolaag  AudioFit.exec({cmd, args}) → {ok, result | error}
                     ▼
   Kern (DOM-vrij): Store · Cutter · Stretch · Resampler · Renderer · WavCodec · Validator
```

| Module | Plaats | Verantwoordelijkheid |
|---|---|---|
| `Store` | kern | State = recept + runtime-audiodata; `update()`; undo/redo (max. 50, zonder audiodata). Eén commando = één undo-stap. |
| `Validator` | kern | Controleert recept en commando-argumenten; leesbare foutmeldingen. |
| `Cutter` | kern | Snap naar nuldoorgang; marker-verwijzingen omzetten naar tijden. |
| `Stretch` | kern | WSOLA (§7.1). |
| `Resampler` | kern | Windowed-sinc (§7.2): varispeed en omzetten van afwijkende sample rates. |
| `Renderer` | kern | Fit-berekening en blok samenstellen (§7.3). |
| `WavCodec` | kern | WAV schrijven (§7.4) en lezen (voor de CLI). |
| `Commands` | kern | Commando-register met JSON-schema per commando; `exec`, `execAll`, `describe`. |
| `Loader` | editor | File-input, drag-and-drop, `decodeAudioData`, piekdata. |
| `WaveformView` | editor | Canvas: golfvorm, selectie, markers, zoom, scroll, cursor. |
| `BlockView` | editor | Bloktijdlijn, overlapzones, detailpaneel. |
| `Player` | editor | Afspelen van selectie, fragment (origineel/gefit), overgang of blok. |
| `Worker` | editor | Draait `Stretch`/`Resampler` buiten de UI-thread; worker gestart uit een `Blob` met de kerncode. |
| `AIBridge` | editor | Route A: context kopiëren, commando's plakken en uitvoeren (§8.2). |

## 6. Functionele specificatie (editor)

### 6.1 Inladen
- File-input (`accept="audio/*,.wav,.mp3"`, `multiple`) en drag-and-drop.
- Decodeerfout → melding met bestandsnaam; overige bestanden gaan door.
- Mono blijft mono; in een stereo-blok naar beide kanalen gekopieerd.
- Recept laden met ontbrekende bronnen → lijst "nog in te laden"; koppeling op bestandsnaam zodra het bestand wordt geladen.

### 6.2 Golfvorm en selectie
- Zoomen met Ctrl+scrollwiel rond de muispositie; horizontaal scrollen.
- Slepen maakt een selectie; in- en uit-handgreep los versleepbaar.
- Velden in / uit / duur (`m:ss.mmm`), onderling gekoppeld. Nudge ±1 ms en ±10 ms.
- Optie "alleen laatste 2 s afspelen" om het uit-punt te controleren.

### 6.3 Markers
- Plaatsen: toets `M` op de afspeelcursor (ook tijdens afspelen) of dubbelklik op de tijdas.
- Direct na plaatsen een naamveld; lege naam → automatisch `m1`, `m2`, … Namen zijn uniek per bron.
- Weergave: verticale lijn met label boven de golfvorm. Slepen verplaatst, rechtsklik → hernoemen / verwijderen.
- Markerlijst naast de golfvorm: naam, tijd; klik springt ernaartoe.
- Selectie-handgrepen klikken vast op markers binnen 10 pixels.
- Knop "selecteer tussen markers": kies begin- en eindmarker → selectie.
- Markers gaan mee in het recept en zijn in commando's bruikbaar als `"@naam"` op elke plek waar een tijd verwacht wordt (bijv. `"in": "@refrein"`). De waarde wordt bij uitvoering omgezet naar seconden; latere verplaatsing van de marker verandert bestaande fragmenten niet.

### 6.4 Netjes knippen
- **Snap naar nuldoorgang** (standaard aan): verschuift naar de dichtstbijzijnde tekenwissel in de mono-som binnen ±5 ms; geen gevonden → punt blijft staan.
- **Fades** per fragment: standaard 8 ms in en uit, cosinusvorm, instelbaar 0–5000 ms. Toegepast ná het stretchen.

### 6.5 Blokken en fit
- Nieuw blok: naam + doelduur (seconden, 3 decimalen).
- Fragmenten herordenen (slepen), dupliceren, verwijderen, "open in golfvorm".
- **Fit-modus `manual`** (standaard voor nieuwe blokken, UI-naam "origineel"): zie hieronder.
- **Fit-modus `proportional`** (UI-naam "passend maken"; knop "Pas blok in"): alle niet-vergrendelde fragmenten krijgen dezelfde ratio.

  ```
  T        = targetDuration
  O        = som overlapduren (overgangen met overlap)
  G        = som stiltes (overgangen van type gap)
  L_locked = som brondur vergrendelde fragmenten
  L_free   = som brondur vrije fragmenten
  ratio    = L_free / (T + O − G − L_locked)
  doeldur_i = brondur_i / ratio      (vrij)
  doeldur_i = brondur_i              (vergrendeld)
  ```
  Overlap- en gap-duren gelden in uitvoertijd en schalen niet mee. Noemer ≤ 0 → foutmelding, geen render.
- **Fit-modus `manual`**: eigen doelduur per fragment; het blok toont het verschil. Te kort → stilte aan het eind; te lang → export geblokkeerd.
- **Ratio-indicatie** per fragment (modus `pitch`):

  | Ratio | Indicatie |
  |---|---|
  | 0,90 – 1,10 | groen: nauwelijks hoorbaar |
  | 0,75 – 0,90 en 1,10 – 1,33 | oranje: hoorbaar, meestal bruikbaar |
  | 0,50 – 0,75 en 1,33 – 2,00 | rood: duidelijke artefacten te verwachten |
  | < 0,50 of > 2,00 | geblokkeerd |

  Richtwaarden (HYPOTHESE), als constanten bovenin de kern. In modus `varispeed` toont het fragment ook de verschuiving in halve tonen: `12 × log2(ratio)`.

### 6.6 Overgangen tussen fragmenten

Per paar A → B één overgang. `ms` = overlapduur (bij `gap`: stilteduur).

| Type | Overlap | Gedrag |
|---|---|---|
| `cut` | nee | Harde las; A en B elk een microfade van 8 ms. |
| `gap` | nee | `ms` stilte tussen A en B. |
| `crossfade` | ja | A fadet uit terwijl B infadet. `curve`: `equal_power` (standaard) of `linear`. |
| `fade_out_a` | ja | B start op vol volume; A fadet onder B uit over `ms`. |
| `fade_in_b` | ja | A loopt op vol volume door tot zijn einde (microfade 8 ms); B fadet in over `ms`. |
| `duck` | ja | A zakt in 150 ms naar `duckDb` (standaard −12 dB), blijft daar tijdens de overlap en stopt met een fade van 150 ms; B op vol volume. |
| `mix` | ja | Beide op vol volume opgeteld; alleen microfades. Clipping-waarschuwing actief. |

Regels:
- Overlap maximaal 90% van de doelduur van het kortste van de twee fragmenten; anders begrensd met melding.
- Twee overgangen aan weerszijden van één fragment mogen samen niet langer zijn dan dat fragment; anders beide evenredig ingekort.
- De overgang vervangt aan die kant de fragment-eigen fade.
- In de bloktijdlijn is overlap een gearceerde zone: rand slepen wijzigt `ms`, klikken opent de typekeuze.
- Voorbeluisteren van alleen de overgang: 2 s ervoor tot 2 s erna.

### 6.7 Voorbeluisteren
- Al het stretch-/renderwerk draait in een Worker. Zolang er gerekend wordt: voortgangsbalk met percentage en wat er bezig is ("Blok 1 · fragment 2/3 rekken"), in de statusbalk en op het betreffende blok; knoppen die op het resultaat wachten tonen een bezig-toestand. De UI blijft bedienbaar. Een nieuwe render voor hetzelfde blok annuleert de lopende.
- Fragment: origineel / gefit (A/B). Blok: geheel, met cursor. Overgang: zie §6.6.
- Stretchen bij de eerste keer afspelen; resultaat in cache; voortgang zichtbaar op het fragment.

### 6.8 Exporteren
- Per blok: `<bloknaam>_<doelduur>s.wav`. Alle blokken: losse downloads.
- 16-bit PCM, werkfrequentie, mono of stereo per blok.
- Optie normaliseren per blok (piek naar −1 dBFS), standaard uit.
- Pieken boven 0 dBFS: waarschuwing en begrenzing op ±1,0.
- Recept opslaan/laden als `.json`.

### 6.9 Sneltoetsen
`Spatie` play/stop · `I`/`O` in/uit op cursor · `M` marker · `L` loop · `Enter` selectie → actief blok · `Ctrl+Z`/`Ctrl+Y` undo/redo.

### 6.10 Stemspoor (voice-over)

Een blok heeft twee lagen:
- **Muziekspoor** = `fragments` + `transitions` (ongewijzigd). Vult altijd exact de doelduur; de fit (§6.5) geldt alleen hiervoor.
- **Stemspoor** = `voice`: nul of meer stemfragmenten die bovenop de muziek klinken.

Stemfragment (`voice[i]`):

| Veld | Betekenis |
|---|---|
| `id` | `v1`, `v2`, … |
| `source`, `in`, `out` | zoals bij een muziekfragment; `"@marker"` toegestaan |
| `anchor` | id van het muziekfragment waaraan de stem hangt |
| `offset` | seconden na het **begin van het anker in de uitvoer** (mag negatief, ≥ −begin anker) |
| `targetDuration` | `null` = oorspronkelijke snelheid; getal = gerekt/ingedikt tot deze duur |
| `preset` | standaard `"voice"`; `mode` standaard `"pitch"` |
| `gainDb`, `fadeInMs`, `fadeOutMs` | zoals bij muziek (fades standaard 8 ms) |
| `acceptOverlap` | `false` (standaard); `true` = overlap met een eerder stemfragment bewust geaccepteerd |

Regels:
- Starttijd in het blok = uitvoerstart van het anker + `offset`. Verandert de fit, dan schuift de stem mee.
- Stem moet volledig binnen het blok vallen; anders fout met het aantal seconden te lang (geen render/export).
- Overlappende stemfragmenten: waarschuwing met de betrokken ids en de overlapduur. De gebruiker kan de overlap **accepteren** (`acceptOverlap: true` op het later startende stemfragment); daarna geldt het niet meer als probleem en wordt beide gewoon opgeteld gerenderd. Verandert de overlap later (andere fit, offset), dan blijft de acceptatie staan maar toont de waarschuwing de nieuwe duur.
- Ratio-indicatie zoals §6.5; bij `targetDuration` buiten oranje een waarschuwing ("stem duidelijk hoorbaar versneld/vertraagd").
- Muziekfragment verwijderen dat anker is → fout met de lijst stem-ids (eerst stem verwijderen of verhangen).
- Stem dupliceren/verplaatsen naar ander blok: niet in fase 2.

**Ducking** (per blok): `duckDb` (standaard −6 dB; 0 = uit) en `duckMs` (standaard 150 ms). Tijdens elk stemfragment zakt de muziek naar `duckDb`; de daling begint `duckMs` vóór de stemstart en het herstel eindigt `duckMs` na het stemeinde (begrensd op het blok). Overlappende ducking-zones van opeenvolgende stemfragmenten worden samengevoegd. Curve: cosinus.

Renderen (aanvulling op §7.3): na stap 6 de ducking-omhullende op de muziekbuffer; daarna elk stemfragment (stretch indien `targetDuration`, gain, fades) optellen op zijn starttijd. Invariant ongewijzigd.

Bloktijdlijn (editor): tweede rij onder de muziek met de stemfragmenten op hun positie; ducking-zones gearceerd in de muziekrij.

### 6.11 Voice-over-recorder (fase 5)

Opnemen terwijl het blok speelt, met koptelefoon; de opname wordt direct een stemfragment op de juiste plek.

- **Invoer**: `getUserMedia({audio:{echoCancellation:false, noiseSuppression:false, autoGainControl:false, deviceId}})`; microfoonkeuze (onthouden in `localStorage`, met try/catch). Ruwe PCM opnemen via `AudioWorklet` (module uit `Blob`), terugval `ScriptProcessorNode`. Geen `MediaRecorder` (verliesgevend).
- **Niveaumeter** (piek + RMS, clip-indicator) zichtbaar zodra de microfoon open staat.
- **Kalibratie latency (loopback)**: gebruiker houdt de koptelefoon tegen de microfoon en drukt "Kalibreer". De tool speelt 5 korte klikken (1 kHz-burst van 5 ms, interval 600 ms) en neemt tegelijk op. Per klik: detectie van het begin (eerste sample boven 50% van de piek rond de verwachte positie, venster ±300 ms). Latency = mediaan van (gedetecteerd − verwacht). Spreiding > 5 ms of minder dan 3 klikken gevonden → melding "niet betrouwbaar, harder zetten / dichter bij de microfoon". Resultaat per microfoon+uitvoer onthouden. Handmatige correctie daarna in stappen van 10 ms (bereik ±500 ms); eis gebruiker: 0,1 s precisie volstaat.
- **Opnemen**: gebruiker kiest blok en (optioneel) startpunt; aftellen (instelbaar 0–4 tellen, korte piep alleen in de koptelefoon) → blok speelt vanaf startpunt (gerenderde mix van het blok, ducking uitgeschakeld tijdens opname als optie) en de microfoon neemt op. Stop = knop of Spatie. Opname wordt gecorrigeerd met de latency (begin wegknippen) en als bron `opname-<blok>-<nnn>.wav` toegevoegd (`attachAudio`).
- **Plaatsing**: na stoppen de stemopname automatisch als stemfragment (`add_voice`) met als anker het muziekfragment waarin de opname begon en `offset` = starttijd − ankerstart; in/uit = hele opname (snap uit). Daarna normaal te trimmen.
- **Takes**: elke opname is een nieuwe bron; het vorige stemfragment op die plek wordt niet overschreven maar de nieuwe take komt erbij met `acceptOverlap:false` → bestaande overlap-waarschuwing; knop "vervang vorige take" verwijdert het vorige stemfragment.
- **Opslaan**: elke opname is als WAV te downloaden (en "alle opnames" in één keer); waarschuwing bij niet-opgeslagen opnames (beforeunload), omdat het recept bronnen op bestandsnaam koppelt.
- **Vereisten/browsercheck**: microfoon beschikbaar (`navigator.mediaDevices`), toestemming, AudioWorklet. `file://` is een secure context (FEIT Chromium, getest); Firefox ONBEKEND.

## 7. Algoritmen (kern)

### 7.1 WSOLA (toonhoogte behouden)

Invoer: kanalen van de uitsnede, `outLength`. Uitvoer: exact `outLength` samples per kanaal.

| Parameter | Preset `music` | Preset `voice` | Toelichting |
|---|---|---|---|
| `N` (framelengte) | 2048 (ca. 43–46 ms) | 1024 (ca. 21–23 ms) | Lang frame houdt akkoorden en lage tonen stabiel; kort frame houdt medeklinkers scherp. |
| `Hs` (synthese-hop) | 1024 | 512 | 50% overlap |
| Venster | Hann, periodiek | Hann, periodiek | sommeert bij 50% overlap tot 1 |
| `D` (zoektolerantie) | ±512 (ca. ±11 ms) | ±512 (ca. ±11 ms) | Moet minstens één grondtoonperiode van een lage stem omvatten (ca. 12 ms bij 80 Hz). |
| Correlatie-stap | 2 | 2 | elke 2e sample |

Waarden gelden bij 44,1/48 kHz; bij andere sample rates schalen `N`, `Hs` en `D` mee (afgerond op een even getal). De presetwaarden zijn startpunten (HYPOTHESE) en staan als constanten bovenin de kern, zodat ze na een luistertest zijn bij te stellen.

1. `alpha = outLength / inLength`.
2. Mono-som `m` voor de zoekstap. De gevonden verschuiving geldt voor **alle kanalen**, zodat het stereobeeld intact blijft.
3. Vul de invoer aan met `N` nullen voor en `N + D` nullen na.
4. Frame 0: eerste frame gevensterd naar uitvoerpositie 0. `prevPos = 0`.
5. Voor frame `k = 1, 2, …` zolang `k·Hs < outLength`:
   - `ideal = round(k·Hs / alpha)`
   - `target = prevPos + Hs` (natuurlijk vervolg van het vorige frame)
   - Zoek `δ ∈ [−D, +D]` met maximale kruiscorrelatie tussen `m[ideal+δ … ideal+δ+N)` en `m[target … target+N)` (som van producten, stap 2).
   - `pos = ideal + δ`; per kanaal frame op `pos` × Hann optellen bij de uitvoer vanaf `k·Hs`.
   - `prevPos = pos`.
6. Afsnijden op exact `outLength`.
7. `|alpha − 1| < 0,0005` → WSOLA overslaan, kopiëren en afkorten/aanvullen.
8. Uitsnede korter dan `2N` → terugvallen op varispeed, met zichtbare melding.

Rekenlast: circa `(2D+1) × N/2` vermenigvuldigingen per frame; voor 15 s uitvoer bij 48 kHz ≈ 0,7 miljard. Verwachte rekentijd: enkele seconden per fragment (HYPOTHESE, niet gemeten). Voortgangsmelding per 20 frames.

### 7.2 Resampler (varispeed en sample-rate-conversie)

```
step   = inLength / outLength
cutoff = min(1, 1 / step)
A      = 16
out[i] = Σ in[n] × cutoff × sinc(cutoff × (t − n)) × hann((t − n) / (A / cutoff)),  t = i × step
```
`sinc(0) = 1`; `hann(u) = 0.5 + 0.5·cos(πu)` voor |u| ≤ 1; buiten de invoer geldt `in[n] = 0`.

### 7.3 Blok samenstellen
1. `total = round(targetDuration × sampleRate)`.
2. `outLength_i = round(doeldur_i × sampleRate)`; overlap en gap in samples.
3. Afrondingsverschil corrigeren op het **laatste vrije fragment**: `Σ outLength − Σ overlap + Σ gap === total`.
4. Per fragment: stretchen of resamplen (cache op fragment-id + in + uit + outLength + mode).
5. Gain (`10^(gainDb/20)`), dan de omhullende: fragment-fades aan de buitenranden, overgangscurves aan de binnenranden.
6. Optellen in de uitvoerbuffer: `start_0 = 0`, `start_{i+1} = start_i + outLength_i − overlap_i + gap_i`.
7. Invariant: uitvoerlengte === `total`; anders een fout gooien.

Curves over een overlap van `n` samples, `x = k/n`: `equal_power` uit = `cos(x·π/2)`, in = `sin(x·π/2)`; `linear` uit = `1 − x`, in = `x`.

### 7.4 WAV
- Schrijven: RIFF/WAVE, 44 bytes header, PCM formaat 1, 16 bit little-endian, interleaved. `s = clamp(x, −1, 1)`; `int16 = s < 0 ? s × 32768 : s × 32767`.
- Lezen (CLI): PCM 16/24/32 bit en IEEE float 32 bit; onbekende chunks overslaan; ander formaat → foutmelding met het gevonden formaatnummer.

## 8. Aansturing door Claude

### 8.1 Commando's

`AudioFit.exec({ cmd, args })` → `{ ok: true, result }` of `{ ok: false, error }`.
`AudioFit.execAll([...])` stopt bij de eerste fout en draait de hele reeks terug.

| Commando | Argumenten | Resultaat |
|---|---|---|
| `get_state` | — | volledig recept |
| `list_sources` | — | bronnen met duur, kanalen, markers |
| `add_marker` | `source`, `name`, `time` | — |
| `update_marker` | `source`, `name`, `newName?`, `time?` | — |
| `delete_marker` | `source`, `name` | — |
| `create_block` | `name`, `targetDuration` | blok-id |
| `update_block` | `block`, `name?`, `targetDuration?`, `fitMode?`, `duckDb?`, `duckMs?` | — |
| `delete_block` | `block` | — |
| `add_fragment` | `block`, `source`, `in`, `out`, `position?`, `snap?`, `mode?` | fragment-id, werkelijke in/uit |
| `update_fragment` | `fragment`, `in?`, `out?`, `mode?`, `gainDb?`, `fadeInMs?`, `fadeOutMs?`, `locked?`, `manualDuration?` | — |
| `move_fragment` | `fragment`, `position` (en optioneel `block`) | — |
| `duplicate_fragment` | `fragment` | nieuw fragment-id |
| `delete_fragment` | `fragment` | — |
| `add_voice` | `block`, `source`, `in`, `out`, `anchor`, `offset?`, `targetDuration?`, `snap?`, `preset?`, `mode?` | stem-id, werkelijke in/uit, starttijd in blok |
| `update_voice` | `voice`, `in?`, `out?`, `anchor?`, `offset?`, `targetDuration?`, `acceptOverlap?`, `gainDb?`, `fadeInMs?`, `fadeOutMs?`, `preset?`, `mode?` | — |
| `delete_voice` | `voice` | — |
| `set_transition` | `block`, `index`, `type`, `ms?`, `curve?`, `duckDb?` | — |
| `fit_report` | `block?` (leeg = alle) | per fragment: brondur, doeldur, ratio, status; per blok: klopt of afwijking |
| `render_block` | `block` | duur, piek (dBFS), clipping ja/nee |
| `export_block` | `block?`, `normalize?` | bestandsnaam/-namen |
| `import_recipe` | `recipe` | aantal blokken; ontbrekende bronnen |
| `undo` / `redo` | — | — |

Alleen in de editor: `play`, `stop`.
- `in`, `out` en `time` accepteren seconden of `"@markernaam"`.
- `Commands.describe()` geeft per commando naam, omschrijving en JSON-schema.
- Foutmeldingen zeggen wat er wél mag (bijv. "out 250.0 ligt voorbij het einde van intro.mp3 (184.32 s)").
- Bestanden inladen en bestanden verwijderen zijn geen commando's.

### 8.2 Route A: kopiëren en plakken (editor)
- Knop **Kopieer AI-context**: zet op het klembord (a) een korte instructie met de begrippen uit §4 en de regel "antwoord met één JSON-array van commando's in een codeblok", (b) `Commands.describe()`, (c) het huidige recept inclusief markers.
- Gebruiker plakt dit in een Claude-chat en beschrijft in gewone taal wat er moet gebeuren.
- Knop **Voer commando's uit**: tekstvak waarin het antwoord geplakt wordt. De tool haalt de eerste JSON-array uit de tekst (ook als die in een codeblok staat), toont een voorvertoning als leesbare lijst, en voert na bevestiging `execAll` uit.
- Resultaat: per commando ✓ of de foutmelding; knop "Kopieer resultaat" om fouten terug te plakken in de chat.
- De hele reeks = één undo-stap.

### 8.3 CLI voor Claude Code (`audiofit-cli.mjs`)

Aanroep: `node audiofit-cli.mjs <opdracht> [opties]`. Uitvoer is altijd JSON op stdout; exitcode 0 = gelukt, 1 = fout (met `error` in de JSON).

| Opdracht | Werking |
|---|---|
| `schema` | Drukt `Commands.describe()` en het receptformaat af. |
| `validate <recept.json>` | Controleert structuur, verwijzingen en fit; lijst met problemen. |
| `report <recept.json> [--sources <map>]` | `fit_report` voor alle blokken. |
| `exec <recept.json> --commands <cmds.json>\|--json '<array>' [--out <nieuw.json>]` | Voert commando's uit op het recept en slaat het resultaat op (standaard: overschrijft, met een kopie `<recept>.bak.json`). |
| `render <recept.json> --sources <map> --out <map> [--block <id>] [--normalize]` | Rendert blokken naar WAV. |
| `probe <bestand>` | Duur, kanalen, sample rate van een audiobestand. |
| `doctor` | Controleert de vereisten: Node-versie, `ffmpeg` op het pad, `audiofit.html` met kernblok aanwezig. Per onderdeel ✓ of wat er ontbreekt en hoe het te installeren is. |

Vereisten installeren (eenmalig, door de gebruiker of door Claude Code na toestemming):
- Node.js LTS en ffmpeg. Op Windows kan dat met `winget`; op Linux met de pakketbeheerder van de distributie.
- De exacte pakketnamen niet uit dit document overnemen maar eerst opzoeken (`winget search nodejs`, `winget search ffmpeg`) — CONFABULATIERISICO bij pakket-ID's.
- Na installatie een nieuwe terminal openen en `node audiofit-cli.mjs doctor` draaien.

- De CLI staat in dezelfde map als `audiofit.html` en laadt daaruit het blok `<script id="audiofit-core">`. Ontbreekt het bestand of het blok → duidelijke foutmelding.
- Bronnen: gezocht op bestandsnaam in `--sources`. WAV wordt direct gelezen. MP3 wordt via `ffmpeg -i <bestand> -f wav -acodec pcm_s16le -` omgezet; staat `ffmpeg` niet op het pad → fout met die uitleg.
- Bronnen met een andere sample rate dan `recept.sampleRate` worden omgezet met de `Resampler`.
- Markers plaatsen kan in de CLI via `add_marker` met een expliciete tijd; op het gehoor plaatsen gebeurt in de editor.

Werkwijze: in de editor bronnen laden, markers zetten, fragmenten knippen → recept opslaan in de projectmap naast de bronbestanden → Claude Code past het recept aan met `exec`, controleert met `report` en maakt de WAV's met `render`. Het recept kan daarna weer in de editor geopend worden om te luisteren.

Verschil editor ↔ CLI: de berekening is dezelfde code. De uitvoer is sample-identiek wanneer de bronnen WAV zijn op de werkfrequentie. Bij MP3 of bij sample-rate-conversie decodeert de browser anders dan ffmpeg; de uitvoer is dan gelijkwaardig maar niet bit-identiek (HYPOTHESE: onhoorbaar verschil).

## 9. Schermindeling (editor)

**Vanaf 0.7 blokgericht** (vervangt de schets hieronder):
```
┌ kop: titel · project │ Laden ↶ ↷ Recept AI ●Opname Export▾ ? ┐
├ BLOKKEN │ ACTIEF BLOK: tijdlijn muziek/stem/ducking (groot) │ INSPECTOR ┤
├ BRONNEN-la (inklapbaar): tabs per bron · golfvorm · markers · → muziek / → stem ┤
```
Oorspronkelijke schets (tot 0.6):

```
┌──────────────────────────────────────────────────────────────────┐
│ Audio Fit Tool v0.x   [Laden] [↶] [↷] [Recept ⇅] [AI ⇅] [Export] │
├────────────┬─────────────────────────────────────────┬───────────┤
│ BRONNEN    │ GOLFVORM        ▼refrein        ▼slot   │ MARKERS   │
│ • intro    │ ▁▃▆█▇▅▃▂▁▂▄▆█▇▅▃▁▂▃▅▇█▆▃▁               │ refrein   │
│ • regen    │ [in] [uit] [duur]                       │  0:42.118 │
│            │ [▶] [loop] [snap ☑] [M] [→ blok]        │ slot      │
├────────────┴─────────────────────────────────────────┴───────────┤
│ BLOKKEN                                         [+ nieuw blok]   │
│ Blok 1  15,000 s  proportioneel  ● +8% sneller                   │
│ ┌───────────┬▒▒┬────────┬▒┬──────────┐ [▶] [WAV]                 │
│ │ intro     │▒▒│ regen  │▒│ intro 2  │                           │
│ └───────────┴▒▒┴────────┴▒┴──────────┘                           │
│ Blok 2  15,000 s  ⚠ 1,2 s te kort                                │
└──────────────────────────────────────────────────────────────────┘
```
▒ = overlapzone. `[AI ⇅]` opent "Kopieer AI-context" en "Voer commando's uit".

## 10. Randgevallen

| Situatie | Gedrag |
|---|---|
| Selectie korter dan 20 ms | Toevoegen geweigerd met melding. |
| Fragment korter dan `2N` in modus `pitch` | Varispeed, met melding (§7.1 stap 8). |
| Fades/overgangen samen langer dan het fragment | Evenredig inkorten, met melding. |
| Recept verwijst naar ontbrekende bron | Editor: fragment gemarkeerd, blok niet renderbaar. CLI: fout met de ontbrekende bestandsnamen. |
| Twee bronnen met dezelfde bestandsnaam | Editor: tweede krijgt ` (2)`. CLI: fout. |
| Onbekende markernaam in `"@naam"` | `ok: false` met de lijst bestaande markers van die bron. |
| Marker verwijderd of verplaatst | Bestaande fragmenten veranderen niet (tijden zijn al omgezet). |
| Geplakte AI-tekst bevat geen geldige JSON-array | Melding; niets uitgevoerd. |
| Zeer groot bestand (> 20 min) | Waarschuwing over geheugengebruik; wel inladen. |
| `AudioContext` `suspended` | `resume()` bij de eerste afspeelactie. |

## 11. Acceptatiecriteria

1. WAV en MP3 laden via knop en slepen; golfvorm verschijnt.
2. Selectie van 12,000 s in een blok van 10,000 s → WAV van exact `10 × sampleRate` samples.
3. Modus `pitch`: testsinus 440 Hz blijft bij ratio 1,2 op 440 Hz (±1 Hz).
4. Modus `varispeed`: dezelfde toon → 528 Hz (±1 Hz).
5. Drie fragmenten van samen 17,0 s met twee crossfades van 0,5 s in een blok van 15 s: ratio 17,0 / 16,0 = 1,0625 voor alle drie; export exact 15,000 s.
6. Elk overgangstype geeft de beschreven omhullende (halverwege de overlap: `equal_power` ≈ 0,707 per fragment, `linear` = 0,5).
7. Geen samplesprong > 0,1 op fragmentgrenzen bij `cut` met een testsinus.
8. Marker plaatsen met `M` tijdens afspelen, hernoemen, verslepen; `add_fragment` met `"in": "@a", "out": "@b"` levert het fragment tussen die markers.
9. Elke bewerking in de editor is ook uitvoerbaar via `AudioFit.exec`.
10. Route A: gekopieerde context + een door Claude gegeven commando-array maakt zonder handwerk een kloppend blok; de reeks is met één undo terug te draaien.
11. CLI: `validate`, `exec`, `report` en `render` werken op een in de editor opgeslagen recept met WAV-bronnen; de gerenderde WAV is sample-identiek aan de export uit de editor.
12. CLI met MP3-bron zonder ffmpeg geeft exitcode 1 en een melding die ffmpeg noemt.
13. De editor werkt volledig offline; geen enkele netwerkaanroep.

Zelftest: `window.__selftest()` in de editor en `node audiofit-cli.mjs selftest` controleren criteria 2 t/m 7 met gegenereerde signalen.

## 12. Verworpen en latere alternatieven

| Optie | Reden |
|---|---|
| Ingebouwd AI-paneel met API-sleutel | Niet nodig; route A en de CLI volstaan. Houdt de tool vrij van netwerkverkeer en sleutelbeheer. |
| Audio-analyse voor de AI (inzetten, tempo, stiltes) | De AI kiest geen fragmenten. |
| Signalsmith Stretch (WASM, MIT) | Hogere kwaliteit bij dichte polyfone muziek, maar externe binary. Exacte offline lengte niet geverifieerd (ONBEKEND). |
| Rubber Band als externe engine in de CLI | Sample-exacte offline modus; vereist een geïnstalleerd `rubberband`-programma. Kandidaat voor fase 5 als de WSOLA-kwaliteit tegenvalt: `Stretch` heeft daarvoor een vaste interface. |
| MP3-export | Vereist een externe encoder; WAV volstaat. |

## 13. Fasering

| Fase | Inhoud | Resultaat |
|---|---|---|
| 1 | Kern (`Store`, `Commands`, `Validator`, `Stretch`, `Resampler`, `Renderer`, `WavCodec`) + editor: laden, golfvorm, selectie, snap, markers, één fragment → fit → export | 12 s → 10 s met behoud van toonhoogte |
| 2 | Meerdere fragmenten per blok, fit-modi, alle overgangstypen, ratio-indicatie, A/B, cache, undo/redo; stemspoor met anker, ducking (§6.10); render in Worker | Blokken van 15 s met overlap en voice-over |
| 3 | Recept opslaan/laden, route A (context kopiëren, commando's uitvoeren), zelftest | Aansturing via een Claude-chat |
| 4 | Vereisten controleren/installeren (Node, ffmpeg); `audiofit-cli.mjs`: `doctor`, `schema`, `validate`, `report`, `exec`, `render`, `probe`, `selftest` | Aansturing door Claude Code |
| 5 | Voice-over-recorder (§6.11): opnemen tijdens afspelen, loopback-kalibratie, automatische plaatsing | Stem inspreken in de tool |
| 6 (optioneel) | Externe stretch-engine, ZIP-export, normaliseren per reeks | Uitbreidingen |

Elke fase levert een werkend geheel op.

## 14. Open vragen (beantwoord in 0.5)

1. **Voice-over als laag**: ligt de voice-over bóven de muziek (beide klinken tegelijk gedurende het hele blok), of volgen muziek en stem elkaar op met alleen een overlap bij de overgang?
   - Dit plan ondersteunt het tweede volledig. Het eerste kan nu alleen via een lange `duck`- of `mix`-overgang (maximaal 90% overlap), wat omslachtig is.
   - Als het eerste de bedoeling is, krijgt een blok **twee sporen** (muziek en stem) die elk afzonderlijk op de doelduur worden gefit, met automatische ducking van de muziek onder de stem. Gevolgen: uitbreiding van het recept (`tracks` in plaats van één `fragments`-lijst), van de fit-berekening (per spoor), van de bloktijdlijn (twee rijen) en van de commando's (argument `track`). Dat is een wijziging in fase 2, niet achteraf in te bouwen zonder het receptformaat te breken.
2. **Stem rekken**: mag de voice-over zelf ook in tempo aangepast worden, of blijft die altijd op oorspronkelijke snelheid (`locked`) en wordt alleen de muziek gefit?

**Antwoord (0.5):** muziek klinkt altijd; voice-over ligt over een deel van het blok, over andere muziek dan het deel zonder stem → twee lagen, zie §6.10. Stem mag indien nodig licht versneld of vertraagd worden (optionele `targetDuration`). Recept `version: 2` is een uitbreiding: een recept `version: 1` blijft geldig (`voice: []`, standaard-ducking).

## 15. Instructie voor de bouwende AI

- Lever `audiofit.html` (fase 1–3) en `audiofit-cli.mjs` (fase 4). Geen CDN-links, geen imports, geen `npm`-pakketten.
- Alle kerncode in `<script id="audiofit-core">`, zonder `window`, `document`, `AudioContext` of andere browser-API's. Het blok eindigt met het beschikbaar maken van één object `AudioFit` (in de browser op `window`, in Node als retourwaarde).
- Begin met `Store`, `Validator` en `Commands`; de UI roept uitsluitend commando's aan.
- Constanten uit §6.5, §6.6 en §7 bij elkaar bovenin de kern.
- Implementeer §7 letterlijk, inclusief de lengte-invariant (§7.3 stap 7).
- Bouw per fase en stop na elke fase voor een test.
- Versienummer zichtbaar in de kop en in `schema`; minor +0.1 per wijziging.

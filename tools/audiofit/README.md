# Audio Fit Tool

Knip fragmenten uit muziek en voice-over (WAV/MP3), rek of dik ze in tot een exacte duur (bijv. blokken van 15,000 s) met behoud van toonhoogte, leg voice-over over de muziek en exporteer als WAV.

## Gebruiken

1. Download **`audiofit.html`** — dit ene bestand is de hele tool.
2. Dubbelklik: opent in Chrome of Edge (Firefox: ONBEKEND, niet getest).
3. Niets installeren, werkt offline, geen netwerkverkeer.

Alle uitleg staat in de tool zelf onder **?** (help): werkwijze, sneltoetsen, voice-over en ducking, AI-aansturing, vereisten met browsercheck, zelftest. Knoppen en velden hebben hulpballonnetjes.

### In het kort

1. **Laden** (of slepen): muziek en voice-over.
2. Selecteren in de golfvorm, markers met `M`.
3. **+ nieuw blok** met doelduur.
4. `Enter` = selectie als muziekfragment; `Shift+Enter` = als stemfragment (hangt aan een muziekfragment).
5. Overgangen, ducking en per-fragment instellingen in de bloktijdlijn.
6. **Voorbeluister** en **WAV**. Bewaar je werk met **Recept ⇅** (JSON; audio zelf blijft in je eigen bestanden).

## Vereisten

| Onderdeel | Nodig |
|---|---|
| Editor `audiofit.html` | Alleen een recente browser. MP3 en WAV decodeert de browser zelf. |
| CLI `audiofit-cli.mjs` (alleen voor Claude Code) | Node.js 18+ ([nodejs.org](https://nodejs.org)); voor MP3-bronnen ook ffmpeg ([ffmpeg.org](https://ffmpeg.org)). |

Controle CLI-vereisten:

```bash
node audiofit-cli.mjs doctor
```

Windows: zoek de pakketten met `winget search nodejs` en `winget search ffmpeg` en installeer het gevonden pakket. Een webpagina kan niet zien wat er op je computer geïnstalleerd is; daarom deze `doctor`-opdracht.

## Aansturing door Claude

- **Via een Claude-chat**: **AI ⇅ → Kopieer AI-context**, plak in de chat, beschrijf wat je wilt, plak het antwoord bij **Voer commando's uit**. Eén keer ongedaan maken draait de hele reeks terug.
- **Via Claude Code**: sla het recept op naast de audiobestanden; Claude Code gebruikt de CLI:

```bash
node audiofit-cli.mjs report project.json --sources .
```

```bash
node audiofit-cli.mjs exec project.json --json '[{"cmd":"update_block","args":{"block":"b1","targetDuration":15}}]'
```

```bash
node audiofit-cli.mjs render project.json --sources . --out export
```

Overige opdrachten: `schema`, `validate`, `probe`, `selftest`, `help`.

## Status

| Fase | Inhoud | Status |
|---|---|---|
| 1 | Laden, golfvorm, selectie, snap, markers, fit, export | gereed |
| 2 | Meerdere fragmenten, overgangen, stemspoor + ducking, rekenen op achtergrond met voortgang | gereed |
| 3 | Recept opslaan/laden, AI-aansturing via chat, help, zelftest, normaliseren | gereed |
| 4 | CLI voor Claude Code | gereed |
| 5 | Voice-over opnemen tijdens afspelen, latency-kalibratie | in aanbouw |

## Bekende beperkingen

- WSOLA-kwaliteit en de presets `music`/`voice` zijn nog niet op het gehoor getoetst (HYPOTHESE: groen/oranje ratio's klinken goed).
- Getest in Chromium (headless); Firefox/Safari niet.
- "Alle blokken exporteren" geeft losse downloads; de browser kan om toestemming voor meerdere downloads vragen.

## Voor ontwikkelaars

```
src/core.js          rekenkern (DOM-vrij; editor, Worker en CLI gebruiken dezelfde code)
src/editor.html      editor met placeholder voor de kern
build.mjs            plakt core.js in editor.html → audiofit.html
audiofit-cli.mjs     CLI (laadt de kern uit audiofit.html)
tests/               Node-tests (kern + CLI)
docs/                ontwerpplan en interfacecontract
```

```bash
node build.mjs
```

```bash
node --test tests/*.test.mjs
```

Na elke wijziging in `src/` opnieuw bouwen en `audiofit.html` mee committen.

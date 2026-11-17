# Audio Fit Tool

Knip fragmenten uit WAV/MP3-bestanden en rek of dik ze in tot een exacte duur (bijv. blokken van 15,000 s), met behoud van toonhoogte. Export als WAV.

Versie 0.1.0 — fase 1 van 4 (zie `docs/ontwerpplan-audio-fit-tool.md` §13).

## Gebruiken

1. Download **`audiofit.html`** (dit ene bestand is de hele tool).
2. Dubbelklik erop: het opent in Chrome, Edge of Firefox. Geen installatie, werkt offline, geen netwerkverkeer.

### Werkwijze

1. **Laden** of bestanden in het venster slepen (WAV, MP3; meerdere tegelijk).
2. Klik een bron aan; de golfvorm verschijnt. Zoomen: Ctrl + scrollwiel.
3. **Selecteren**: slepen in de golfvorm, of in/uit typen (`m:ss.mmm`), of `I`/`O` op de afspeelcursor. Nudge-knoppen ±1/±10 ms.
4. **Markers**: `M` (ook tijdens afspelen) of dubbelklik op de tijdas; direct een naam typen. Slepen verplaatst, rechtsklik hernoemt/verwijdert. "Selecteer tussen markers" maakt een selectie tussen twee markers.
5. **Blok**: naam + doelduur invullen → **+ nieuw blok**.
6. **Enter** (of **→ blok**): selectie wordt een fragment in het actieve blok. De tool berekent de ratio (kleur: groen = nauwelijks hoorbaar, oranje = hoorbaar, rood = artefacten).
7. Per fragment: **preset** `music` of `voice`, **modus** `pitch` (toonhoogte gelijk) of `varispeed` (toonhoogte verschuift mee).
8. **Voorbeluister** → controleren. **WAV** → download `<bloknaam>_<duur>s.wav`.

### Sneltoetsen

| Toets | Functie |
|---|---|
| Spatie | afspelen / stoppen |
| I / O | in / uit op cursor |
| M | marker |
| L | loop |
| Enter | selectie → actief blok |
| Ctrl+Z / Ctrl+Y | ongedaan maken / opnieuw |

## Status per fase

| Fase | Inhoud | Status |
|---|---|---|
| 1 | Laden, golfvorm, selectie, snap, markers, fragment → fit → export | **gereed, ter test** |
| 2 | Meerdere fragmenten met overgangen in de UI, herordenen, A/B, undo-UI | rekenkern gereed; UI volgt |
| 3 | Recept opslaan/laden, AI-context kopiëren / commando's plakken, zelftest-knop | gepland |
| 4 | `audiofit-cli.mjs` voor Claude Code (render, validate, report, exec, doctor) | gepland |

## Bekende beperkingen (fase 1)

- WSOLA-kwaliteit bij dichte muziek is nog niet op het gehoor getoetst; presetwaarden zijn startpunten (HYPOTHESE).
- Renderen gebeurt in de UI-thread; bij lange blokken kan de pagina enkele seconden haperen.
- Recept opslaan/laden ontbreekt nog (fase 3): sluiten = werk kwijt.

## Voor ontwikkelaars

```
src/core.js        rekenkern (DOM-vrij; ook gebruikt door de toekomstige CLI)
src/editor.html    editor met placeholder voor de kern
build.mjs          plakt core.js in editor.html → audiofit.html
tests/             Node-tests van de kern
docs/              ontwerpplan en interfacecontract kern ↔ editor
```

Bouwen en testen (Node 18+):

```bash
node build.mjs
```

```bash
node --test tests/*.test.mjs
```

Na elke wijziging aan `src/` opnieuw `node build.mjs` draaien en `audiofit.html` mee committen.

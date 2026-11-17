# Interfacecontract kern ↔ editor (fase 1)

Bron van de eisen: `docs/ontwerpplan-audio-fit-tool.md` (v0.4). Bij conflict wint het ontwerpplan, behalve waar dit contract iets concreter maakt.

## Bestanden

| Bestand | Eigenaar | Inhoud |
|---|---|---|
| `src/core.js` | kern-agent | Alle kerncode. Geen DOM/browser-API's (geen `window`, `document`, `AudioContext`, `Worker`, `Blob`). |
| `src/editor.html` | editor-agent | De editor. Bevat letterlijk de regel `<script id="audiofit-core">/*@@CORE@@*/</script>` vóór het editor-script. |
| `build.mjs` | coördinator | Plakt `src/core.js` in de placeholder → `audiofit.html`. |
| `tests/core.test.mjs` | kern-agent | Node-tests van de kern. |

## Vorm van `src/core.js`

```js
var AudioFit = (function () {
  'use strict';
  // ... CONSTANTS bovenin, daarna modules ...
  return api;
})();
globalThis.AudioFit = AudioFit;
```
Node laadt het met `new Function(code + '\nreturn AudioFit;')()`. Geen `import`/`require`/`export`.

## Runtime-audio (geen commando's, want bestanden laden is geen commando — §8.1)

- `AudioFit.attachAudio(name, { sampleRate, channels: Float32Array[] })`
  Koppelt decodeerde audio aan bron `name`. Bestaat de bron niet in het recept, dan wordt die toegevoegd (`duration`, `channels`, `markers: []`). Tweede bron met dezelfde naam via `attachAudio(..., {rename:true})`-optie → `"naam (2)"`; retourneert de gebruikte naam. Sample rate ≠ recept.sampleRate → kern resamplet met `Resampler`. Geen undo-stap.
- `AudioFit.getAudio(name)` → `{ sampleRate, channels }` of `null`.
- `AudioFit.setSampleRate(sr)` — alleen toegestaan zolang er nog geen audio gekoppeld is.

## Commandolaag

- `AudioFit.exec({cmd, args})` → `{ ok:true, result }` | `{ ok:false, error:string }`. Gooit nooit.
- `AudioFit.execAll([...])` → `{ ok, results:[...], error?, failedIndex? }`; bij fout alles teruggedraaid; geheel = één undo-stap.
- `AudioFit.describe()` → array `{ name, description, schema }`.
- `AudioFit.subscribe(fn)` → `unsubscribe`; `fn(recipe)` na elke state-wijziging (ook undo/redo).
- Alle commando's uit §8.1 behalve `play`/`stop` (die registreert de editor zelf niet in de kern; de editor mag een eigen kleine wrapper hebben).
- Fase 1 moet minstens werken: `get_state`, `list_sources`, `add_marker`, `update_marker`, `delete_marker`, `create_block`, `update_block`, `delete_block`, `add_fragment`, `update_fragment`, `delete_fragment`, `fit_report`, `render_block`, `undo`, `redo`, `import_recipe`. De overige mogen al werken (de kern-agent bouwt Renderer inclusief overgangen volgens §7.3/§6.6 — dat is fase 2-inhoud maar dezelfde code).
- `add_fragment` args: `block, source, in, out, position?, snap? (default true), mode? ("pitch"), preset? ("music")`. `in`/`out`/`time` accepteren getal of `"@marker"`. Resultaat `{ id, in, out }` (werkelijke tijden na snap).
- `render_block` resultaat: `{ duration, samples, peakDb, clipped }`. Buffer ophalen: `AudioFit.getRendered(blockId)` → `{ sampleRate, channels: Float32Array[] }` (laatste render; cache op fragment-id+in+uit+outLength+mode+preset).
- `export_block` in de kern: retourneert `{ files: [{ name, bytes: Uint8Array }] }` (bestandsnaam `<bloknaam>_<doelduur>s.wav`, doelduur met 3 decimalen, bijv. `Blok 1_15.000s.wav`). De editor maakt er downloads van; de CLI schrijft ze weg.

## Hulpfuncties

- `AudioFit.snapToZero(sourceName, timeSec)` → seconden (±5 ms, mono-som, tekenwissel; niets gevonden → ongewijzigd).
- `AudioFit.wav.encode(channels, sampleRate)` → `Uint8Array`; `AudioFit.wav.decode(Uint8Array)` → `{ sampleRate, channels }`.
- `AudioFit.stretch(channels, outLength, {preset, sampleRate})`, `AudioFit.resample(channels, outLength)` — exact `outLength` samples per kanaal.
- `AudioFit.ratioStatus(ratio)` → `"green"|"orange"|"red"|"blocked"` (§6.5).
- `AudioFit.version` = `"0.1.0"`; `AudioFit.CONSTANTS` (alle constanten).
- `AudioFit.selftest()` → `{ ok, checks:[{name, ok, detail}] }` voor criteria 2–7 met gegenereerde signalen (geen audio-bestanden nodig).

## Ids

Blokken `b1, b2, …`, fragmenten `f1, f2, …` (oplopend, nooit hergebruikt binnen een sessie; na `import_recipe` doorgaan vanaf hoogste bestaande nummer).

---

# Aanvulling fase 2 (ontwerp v0.5, §6.10 en §6.7)

## Recept version 2
- `import_recipe` accepteert version 1 (krijgt `voice: []`, `duckDb: -6`, `duckMs: 150`) en 2. `get_state` geeft altijd version 2.
- Per blok: `voice: [...]`, `duckDb`, `duckMs`. Stemfragment-velden: zie ontwerp §6.10 (incl. `acceptOverlap`). Ids `v1, v2, …` (zelfde regels als `f`).

## Nieuwe/gewijzigde commando's
- `add_voice` → `{ id, in, out, start, end }` (start/end = seconden in het blok na fit).
- `update_voice`, `delete_voice`; `update_block` met `duckDb`, `duckMs`.
- `delete_fragment` op een anker → `ok:false` met de stem-ids.
- `move_fragment`, `duplicate_fragment`, `set_transition` moeten volledig werken (fase 2 UI gebruikt ze).
- `fit_report` per blok extra: `voice: [{ id, anchor, start, end, sourceDuration, targetDuration, ratio, status }]`, `duck: [{ start, end }]` (samengevoegde ducking-zones incl. hellingen), `overlaps: [{ a, b, duration, accepted }]`, en per muziekfragment `start` en `end` (uitvoertijd in het blok) zodat de editor de tijdlijn kan tekenen. Per overgang `{ index, type, ms, start, end }` (uitvoertijd van de overlap/gap).
- Niet-geaccepteerde stem-overlap: `fit_report.exportable` blijft `true`, maar `warnings` bevat een regel en `overlaps[].accepted=false`. `render_block`/`export_block` geven bij niet-geaccepteerde overlap `ok:false` met tekst die `acceptOverlap` noemt; de editor vraagt dan "Accepteren?" en roept `update_voice {acceptOverlap:true}` aan.

## Pure render-API voor de Worker
- `AudioFit.renderBlockPure(recipe, audio, blockId, { onProgress, cache })` → `{ sampleRate, channels: Float32Array[], stats: {duration, samples, peakDb, clipped}, warnings }`.
  - `recipe`: zoals `get_state`; `audio`: `{ [bronnaam]: { sampleRate, channels } }` (al op werkfrequentie).
  - `onProgress(fraction 0..1, label)` wordt minstens per ~20 WSOLA-frames aangeroepen; `label` bijv. `"muziek 2/3 rekken"`, `"stem 1/1 rekken"`, `"mixen"`.
  - `cache`: optioneel object (Map-achtig) dat de aanroeper bewaart tussen aanroepen; sleutel per §7.3 stap 4 (+ preset).
  - Gebruikt geen interne state; gooit `Error` met dezelfde Nederlandse tekst als `render_block` zou geven.
- `AudioFit.render_block` in de hoofdthread blijft werken (CLI, tests).

## Worker (editor-zijde)
- Editor maakt de Worker uit een `Blob` met `document.getElementById('audiofit-core').textContent` + een klein worker-script. Protocol: `{type:'audio', name, sampleRate, channels}` (eenmalig per bron, transfer van kopieën), `{type:'render', jobId, recipe, blockId}` → `{type:'progress', jobId, fraction, label}` … `{type:'done', jobId, sampleRate, channels, stats, warnings}` of `{type:'error', jobId, message}`. Annuleren: editor beëindigt de Worker (`terminate`) en start een nieuwe (bronnen opnieuw sturen).
- Fallback als Worker niet kan (bijv. file:// beperking): renderen in de hoofdthread met `renderBlockPure` en `onProgress` via `await`-pauzes; voortgang blijft zichtbaar.

---

# Aanvulling kern 0.3.0 (ontwerp v0.7)
- `create_block` zonder `fitMode` → `"manual"` (constante `CONSTANTS.DEFAULT_FIT_MODE_NEW_BLOCK`). In `manual` betekent `manualDuration: null` de originele lengte, dus een nieuw fragment wordt niet gerekt.
- `import_recipe` zonder `fitMode` per blok → `"proportional"` (ongewijzigd), zodat bestaande recepten hetzelfde blijven.
- Wie een exact gevuld blok wil, geeft `fitMode: "proportional"` mee (of `update_block`), of zet `manualDuration` per fragment.
- `AudioFit.version` = `"0.3.0"`.

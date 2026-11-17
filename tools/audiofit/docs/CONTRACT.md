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

#!/usr/bin/env node
// Audio Fit Tool — commandline (ontwerp §8.3). Node 18+, alleen ingebouwde modules.
// Gebruik: node audiofit-cli.mjs help
// Uitvoer: altijd JSON op stdout; exitcode 0 = gelukt, 1 = fout ({ok:false, error}). Voortgang gaat naar stderr.
import { readFileSync, writeFileSync, existsSync, mkdirSync, readdirSync, copyFileSync, statSync } from 'node:fs';
import { dirname, join, basename, extname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawn, spawnSync } from 'node:child_process';

const CLI_VERSION = '0.1.0';
const HERE = dirname(fileURLToPath(import.meta.url));

class CliError extends Error {}
const fail = (msg) => { throw new CliError(msg); };
const errText = (e) => (e && e.message) ? e.message : String(e);

/* ------------------------------ kern laden ------------------------------ */
let CORE = null; // { AudioFit, origin, file }
function loadCore() {
  if (CORE) return CORE;
  const htmlPath = join(HERE, 'audiofit.html');
  const srcPath = join(HERE, 'src', 'core.js');
  let code, origin, file;
  if (existsSync(htmlPath)) {
    const html = readFileSync(htmlPath, 'utf8');
    const m = /<script id="audiofit-core">([\s\S]*?)<\/script>/.exec(html);
    if (!m) fail('audiofit.html (' + htmlPath + ') bevat geen <script id="audiofit-core">-blok; bouw het bestand opnieuw met "node build.mjs"');
    code = m[1]; origin = 'audiofit.html'; file = htmlPath;
  } else if (existsSync(srcPath)) {
    process.stderr.write('Let op: audiofit.html ontbreekt naast de CLI; src/core.js wordt gebruikt (alleen voor ontwikkeling).\n');
    code = readFileSync(srcPath, 'utf8'); origin = 'src/core.js'; file = srcPath;
  } else {
    fail('audiofit.html niet gevonden in ' + HERE + ' (en ook geen src/core.js). Zet audiofit-cli.mjs in dezelfde map als audiofit.html.');
  }
  let AudioFit;
  try { AudioFit = new Function(code + '\nreturn AudioFit;')(); }
  catch (e) { fail('kerncode uit ' + origin + ' kon niet worden uitgevoerd: ' + errText(e)); }
  if (!AudioFit || typeof AudioFit.exec !== 'function') fail('kerncode uit ' + origin + ' levert geen geldig AudioFit-object');
  CORE = { AudioFit, origin, file };
  return CORE;
}
const AF = () => loadCore().AudioFit;

/* ------------------------------- argumenten ------------------------------ */
const BOOL_FLAGS = new Set(['normalize', 'help', 'version']);
function parseArgs(argv) {
  const pos = [], opt = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a.startsWith('--')) {
      let k = a.slice(2), v;
      const eq = k.indexOf('=');
      if (eq >= 0) { v = k.slice(eq + 1); k = k.slice(0, eq); }
      else if (BOOL_FLAGS.has(k)) v = true;
      else { if (i + 1 >= argv.length) fail('optie --' + k + ' mist een waarde'); v = argv[++i]; }
      opt[k] = v;
    } else pos.push(a);
  }
  return { pos, opt };
}

/* ---------------------------------- IO ---------------------------------- */
function readJsonFile(path, what) {
  if (!existsSync(path)) fail(what + ' niet gevonden: ' + path);
  let text;
  try { text = readFileSync(path, 'utf8'); } catch (e) { fail(what + ' kon niet worden gelezen: ' + errText(e)); }
  try { return JSON.parse(text.replace(/^﻿/, '')); }
  catch (e) { fail(what + ' (' + path + ') is geen geldige JSON: ' + errText(e)); }
}

function isDir(p) { try { return statSync(p).isDirectory(); } catch (_) { return false; } }

function findSourceFile(dir, name) {
  const exact = join(dir, name);
  if (existsSync(exact) && !isDir(exact)) return exact;
  const lower = name.toLowerCase();
  const hit = readdirSync(dir).find((f) => f.toLowerCase() === lower && !isDir(join(dir, f)));
  return hit ? join(dir, hit) : null;
}

/* ------------------------------ audio decoderen -------------------------- */
const FFMPEG_ADVICE = 'ffmpeg is niet gevonden op het pad. Installeer ffmpeg (Windows: zoek eerst het juiste pakket met "winget search ffmpeg" en installeer dat met winget; Linux/macOS: gebruik de pakketbeheerder van je systeem), open daarna een nieuwe terminal en draai "node audiofit-cli.mjs doctor".';

function runFfmpegDecode(file) {
  return new Promise((resolveP, rejectP) => {
    let child;
    try { child = spawn('ffmpeg', ['-v', 'error', '-nostdin', '-i', file, '-f', 'wav', '-acodec', 'pcm_s16le', '-'], { stdio: ['ignore', 'pipe', 'pipe'] }); }
    catch (e) { return rejectP(new CliError('ffmpeg kon niet worden gestart voor ' + basename(file) + '. ' + FFMPEG_ADVICE)); }
    const out = [], err = [];
    child.stdout.on('data', (d) => out.push(d));
    child.stderr.on('data', (d) => err.push(d));
    child.on('error', (e) => {
      if (e && e.code === 'ENOENT') rejectP(new CliError('Kan ' + basename(file) + ' niet decoderen: ' + FFMPEG_ADVICE));
      else rejectP(new CliError('ffmpeg kon niet worden gestart voor ' + basename(file) + ': ' + errText(e)));
    });
    child.on('close', (code) => {
      if (code !== 0) return rejectP(new CliError('ffmpeg kon ' + basename(file) + ' niet decoderen (exitcode ' + code + '): ' + Buffer.concat(err).toString('utf8').trim().slice(0, 400)));
      const buf = Buffer.concat(out);
      if (buf.length < 44) return rejectP(new CliError('ffmpeg gaf geen audio voor ' + basename(file)));
      resolveP(new Uint8Array(buf.buffer, buf.byteOffset, buf.length));
    });
  });
}

// ffmpeg schrijft naar een pipe een WAV-header met onbekende (0 of 0xFFFFFFFF) groottes; herstel die.
function fixPipedWavHeader(u8) {
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  if (u8.length < 12) return u8;
  dv.setUint32(4, u8.length - 8, true);
  let p = 12;
  while (p + 8 <= u8.length) {
    const id = String.fromCharCode(u8[p], u8[p + 1], u8[p + 2], u8[p + 3]);
    const size = dv.getUint32(p + 4, true);
    if (id === 'data') { dv.setUint32(p + 4, u8.length - p - 8, true); break; }
    p += 8 + size + (size & 1);
  }
  return u8;
}

async function decodeFile(file) {
  const ext = extname(file).toLowerCase();
  const raw = readFileSync(file);
  const u8 = new Uint8Array(raw.buffer, raw.byteOffset, raw.length);
  if (ext === '.wav') {
    try { return { ...AF().wav.decode(u8), via: 'wav' }; }
    catch (e) {
      // bijv. 24-bit of float-WAV: probeer ffmpeg als die er is
      try { const d = AF().wav.decode(fixPipedWavHeader(await runFfmpegDecode(file))); return { ...d, via: 'ffmpeg' }; }
      catch (e2) { fail('WAV ' + basename(file) + ' kon niet worden gelezen: ' + errText(e) + (e2 instanceof CliError && /niet gevonden op het pad/.test(e2.message) ? ' (ffmpeg als alternatief is niet beschikbaar)' : '')); }
    }
  }
  const d = AF().wav.decode(fixPipedWavHeader(await runFfmpegDecode(file)));
  return { ...d, via: 'ffmpeg' };
}

/* ----------------------------- recept + bronnen -------------------------- */
function loadRecipe(path) {
  const rec = readJsonFile(path, 'recept');
  const r = AF().exec({ cmd: 'import_recipe', args: { recipe: rec } });
  if (!r.ok) fail(r.error);
  return { raw: rec, imported: r.result };
}
const state = () => { const r = AF().exec({ cmd: 'get_state' }); if (!r.ok) fail(r.error); return r.result; };

function neededSources(rec, blockSel) {
  const set = new Set();
  for (const b of rec.blocks || []) {
    if (blockSel && b.id !== blockSel.id) continue;
    for (const f of b.fragments || []) if (f.source) set.add(f.source);
    for (const v of b.voice || []) if (v.source) set.add(v.source);
  }
  return [...set];
}

function pickBlocks(rec, sel) {
  if (sel === undefined || sel === true) return rec.blocks;
  const b = rec.blocks.find((x) => x.id === sel) || rec.blocks.find((x) => x.name === sel);
  if (!b) fail('blok ' + JSON.stringify(sel) + ' bestaat niet; beschikbaar: ' + (rec.blocks.map((x) => x.id + ' (' + x.name + ')').join(', ') || '(geen blokken)'));
  return [b];
}

async function attachSources(dir, names) {
  if (!existsSync(dir) || !isDir(dir)) fail('--sources: map niet gevonden: ' + dir);
  const found = {}, missing = [];
  for (const n of names) { const f = findSourceFile(dir, n); if (f) found[n] = f; else missing.push(n); }
  if (missing.length) fail('ontbrekende bronbestanden in ' + dir + ': ' + missing.join(', '));
  const info = [];
  for (const n of names) {
    process.stderr.write('bron laden: ' + n + '\n');
    const d = await decodeFile(found[n]);
    AF().attachAudio(n, { sampleRate: d.sampleRate, channels: d.channels });
    info.push({ name: n, sampleRate: d.sampleRate, via: d.via });
  }
  return info;
}

/* --------------------------------- opdrachten ---------------------------- */
const RECIPE_FORMAT = {
  beschrijving: 'Recept = JSON (ontwerp §4.1). Tijden in seconden; "in"/"out"/"time" mogen ook "@markernaam" zijn in commando\'s. transitions[i] hoort bij fragments[i] -> fragments[i+1]. Bronnen worden gekoppeld op bestandsnaam.',
  velden: {
    version: '1 of 2 (versie 1 wordt bij laden naar 2 opgewaardeerd waar de kern dat ondersteunt)',
    sampleRate: 'werkfrequentie in Hz',
    sources: '[{name, duration, channels, markers:[{name,time}]}]',
    blocks: '[{id, name, targetDuration, fitMode, fragments:[{id, source, in, out, mode:"pitch"|"varispeed", preset:"music"|"voice", fadeInMs, fadeOutMs, gainDb, locked, manualDuration}], transitions:[{type, ms, curve}], voice?:[...], duckDb?, duckMs?}]'
  }
};

const HELP = {
  gebruik: 'node audiofit-cli.mjs <opdracht> [opties]',
  opdrachten: {
    schema: 'Commando\'s (Commands.describe()) en receptformaat.',
    'validate <recept.json>': 'Controleert structuur, verwijzingen en fit; lijst met problemen.',
    'report <recept.json> [--sources <map>]': 'fit_report voor alle blokken (met --sources ook controle dat de bronnen bestaan en worden meegeladen).',
    'exec <recept.json> --commands <cmds.json> | --json \'<array>\' [--out <nieuw.json>] [--sources <map>]': 'Voert commando\'s uit; schrijft standaard het recept terug met een kopie <recept>.bak.json. Alles-of-niets. --sources (extra) laat snap-naar-nuldoorgang werken.',
    'render <recept.json> --sources <map> --out <map> [--block <id>] [--normalize]': 'Rendert blokken naar <bloknaam>_<duur>s.wav. Voortgang op stderr.',
    'probe <bestand>': 'Duur, kanalen en sample rate van een audiobestand.',
    doctor: 'Controleert Node, ffmpeg en audiofit.html met kernblok.',
    selftest: 'Zelftest van de kern (criteria 2 t/m 7).',
    'help | --version': 'Deze tekst | versies.'
  },
  uitvoer: 'JSON op stdout; exitcode 0 = gelukt, 1 = fout ({ok:false, error}).'
};

async function cmdSchema() {
  const af = AF();
  return { ok: true, coreVersion: af.version, commands: af.describe(), recipe: RECIPE_FORMAT, emptyRecipe: state() };
}

async function cmdValidate(pos) {
  if (!pos[0]) fail('gebruik: validate <recept.json>');
  const raw = readJsonFile(pos[0], 'recept');
  const v = AF().validateRecipe(raw);
  if (!v.ok) return { ok: false, error: 'recept bevat ' + v.problems.length + ' probleem/problemen', problems: v.problems };
  const imp = AF().exec({ cmd: 'import_recipe', args: { recipe: raw } });
  if (!imp.ok) return { ok: false, error: imp.error, problems: [imp.error] };
  const rep = AF().exec({ cmd: 'fit_report', args: {} });
  if (!rep.ok) return { ok: false, error: rep.error, problems: [rep.error] };
  const warnings = [...(imp.result.warnings || [])];
  const blocks = rep.result.map((b) => {
    (b.warnings || []).forEach((w) => warnings.push(w));
    return { id: b.id, name: b.name, targetDuration: b.targetDuration, exportable: b.exportable, deviation: b.deviation };
  });
  return { ok: true, problems: [], warnings, blocks: state().blocks.length, summary: blocks, missingSources: imp.result.missingSources || [] };
}

async function cmdReport(pos, opt) {
  if (!pos[0]) fail('gebruik: report <recept.json> [--sources <map>]');
  loadRecipe(pos[0]);
  let sources;
  if (opt.sources) sources = await attachSources(resolve(opt.sources), neededSources(state()));
  const rep = AF().exec({ cmd: 'fit_report', args: {} });
  if (!rep.ok) fail(rep.error);
  return { ok: true, sources, report: rep.result };
}

async function cmdExec(pos, opt) {
  if (!pos[0]) fail('gebruik: exec <recept.json> --commands <cmds.json> | --json \'<array>\' [--out <nieuw.json>]');
  const recPath = resolve(pos[0]);
  let cmds;
  if (opt.commands && opt.json) fail('geef --commands of --json, niet allebei');
  if (opt.commands) cmds = readJsonFile(opt.commands, 'commandobestand');
  else if (opt.json) { try { cmds = JSON.parse(opt.json); } catch (e) { fail('--json is geen geldige JSON: ' + errText(e)); } }
  else fail('geef --commands <bestand> of --json \'<array>\'');
  if (cmds && !Array.isArray(cmds) && Array.isArray(cmds.commands)) cmds = cmds.commands;
  if (!Array.isArray(cmds)) fail('de commando\'s moeten een JSON-array van {cmd, args} zijn');
  cmds.forEach((c, i) => { if (!c || typeof c.cmd !== 'string') fail('commando ' + (i + 1) + ' mist het veld "cmd"'); });

  loadRecipe(recPath);
  if (opt.sources) await attachSources(resolve(opt.sources), neededSources(state()));
  const r = AF().execAll(cmds);
  if (!r.ok) return { ok: false, error: r.error, failedIndex: r.failedIndex, results: r.results, written: null };
  const next = state();
  const outPath = opt.out ? resolve(opt.out) : recPath;
  let backup = null;
  if (!opt.out || outPath === recPath) {
    backup = recPath.replace(/\.json$/i, '') + '.bak.json';
    copyFileSync(recPath, backup);
  }
  mkdirSync(dirname(outPath), { recursive: true });
  writeFileSync(outPath, JSON.stringify(next, null, 2) + '\n');
  return { ok: true, results: r.results, written: outPath, backup, blocks: next.blocks.length };
}

function peakOf(chans) { let p = 0; for (const ch of chans) for (let i = 0; i < ch.length; i++) { const a = Math.abs(ch[i]); if (a > p) p = a; } return p; }
const sanitizeName = (s) => s.replace(/[\\/:*?"<>|]/g, '_');

async function cmdRender(pos, opt) {
  if (!pos[0]) fail('gebruik: render <recept.json> --sources <map> --out <map> [--block <id>] [--normalize]');
  if (!opt.sources) fail('--sources <map> is verplicht');
  if (!opt.out) fail('--out <map> is verplicht');
  loadRecipe(pos[0]);
  const rec = state();
  const blocks = pickBlocks(rec, opt.block);
  if (!blocks.length) fail('het recept bevat geen blokken');
  const names = [...new Set(blocks.flatMap((b) => neededSources(rec, b)))];
  await attachSources(resolve(opt.sources), names);
  const outDir = resolve(opt.out);
  mkdirSync(outDir, { recursive: true });
  const af = AF(), files = [], warnings = [];
  for (const b of blocks) {
    process.stderr.write('render blok ' + b.id + ' (' + b.name + ')\n');
    let channels, stats, w = [];
    if (typeof af.renderBlockPure === 'function') {
      const audio = {};
      for (const n of names) audio[n] = af.getAudio(n);
      let last = '';
      const r = af.renderBlockPure(rec, audio, b.id, { onProgress: (fr, label) => {
        const line = '  ' + Math.round(fr * 100) + '% ' + (label || '');
        if (line !== last) { last = line; process.stderr.write(line + '\n'); }
      } });
      channels = r.channels; stats = r.stats; w = r.warnings || [];
    } else {
      const r = af.exec({ cmd: 'render_block', args: { block: b.id } });
      if (!r.ok) fail(r.error);
      const g = af.getRendered(b.id);
      if (!g) fail('render van ' + b.id + ' leverde geen buffer');
      channels = g.channels; stats = r.result;
    }
    w.forEach((x) => warnings.push(x));
    const peak = peakOf(channels);
    let out = channels, peakDb, clipped = false;
    if (opt.normalize && peak > 0) {
      const gn = Math.pow(10, -1 / 20) / peak;
      out = channels.map((ch) => { const o = new Float32Array(ch.length); for (let i = 0; i < ch.length; i++) o[i] = ch[i] * gn; return o; });
      peakDb = -1;
    } else {
      clipped = peak > 1;
      peakDb = peak > 0 ? 20 * Math.log10(peak) : -Infinity;
      if (clipped) warnings.push('blok "' + b.name + '": clipping — pieken begrensd op ±1,0');
    }
    const fname = sanitizeName(b.name + '_' + b.targetDuration.toFixed(3) + 's.wav');
    const target = join(outDir, fname);
    writeFileSync(target, af.wav.encode(out, rec.sampleRate));
    files.push({ block: b.id, name: b.name, file: target, samples: out[0].length, duration: out[0].length / rec.sampleRate,
      channels: out.length, sampleRate: rec.sampleRate, peakDb: Number.isFinite(peakDb) ? Math.round(peakDb * 100) / 100 : null, clipped });
  }
  return { ok: true, files, warnings };
}

async function cmdProbe(pos) {
  if (!pos[0]) fail('gebruik: probe <bestand>');
  const file = resolve(pos[0]);
  if (!existsSync(file)) fail('bestand niet gevonden: ' + file);
  const d = await decodeFile(file);
  const samples = d.channels[0].length;
  return { ok: true, file, duration: samples / d.sampleRate, channels: d.channels.length, sampleRate: d.sampleRate, samples, decodedVia: d.via };
}

async function cmdDoctor() {
  const items = [];
  const major = Number(process.versions.node.split('.')[0]);
  items.push({ name: 'node', ok: major >= 18, required: true, detail: 'Node ' + process.versions.node,
    ...(major >= 18 ? {} : { advice: 'Node 18 of nieuwer is nodig. Windows: zoek met "winget search nodejs"; anders de pakketbeheerder van je systeem of nodejs.org (LTS).' }) });
  const ff = spawnSync('ffmpeg', ['-version'], { encoding: 'utf8' });
  if (ff.error || ff.status !== 0) {
    items.push({ name: 'ffmpeg', ok: false, required: false, detail: 'niet gevonden op het pad', advice: FFMPEG_ADVICE + ' (Alleen nodig voor MP3 en andere niet-WAV-bronnen.)' });
  } else {
    items.push({ name: 'ffmpeg', ok: true, required: false, detail: (ff.stdout.split('\n')[0] || 'ffmpeg').trim() });
  }
  const html = join(HERE, 'audiofit.html');
  if (!existsSync(html)) {
    const fb = existsSync(join(HERE, 'src', 'core.js'));
    items.push({ name: 'audiofit.html', ok: false, required: true, detail: 'niet gevonden in ' + HERE + (fb ? ' (src/core.js is als ontwikkel-terugval beschikbaar)' : ''),
      advice: 'Zet audiofit-cli.mjs in dezelfde map als audiofit.html' + (fb ? ' of draai "node build.mjs" om het te genereren' : '') + '.' });
  } else {
    const has = /<script id="audiofit-core">[\s\S]+?<\/script>/.test(readFileSync(html, 'utf8'));
    let loadOk = false, ver = null;
    if (has) { try { ver = AF().version; loadOk = true; } catch (_) { /* gemeld hieronder */ } }
    items.push({ name: 'audiofit.html', ok: has && loadOk, required: true,
      detail: has ? (loadOk ? 'kernblok aanwezig, kernversie ' + ver : 'kernblok aanwezig maar niet uitvoerbaar') : 'bestand aanwezig maar zonder <script id="audiofit-core">',
      ...(has && loadOk ? {} : { advice: 'Bouw audiofit.html opnieuw ("node build.mjs") of haal een goed exemplaar op.' }) });
  }
  const ok = items.filter((i) => i.required).every((i) => i.ok);
  return { ok, ...(ok ? {} : { error: 'niet aan alle vereisten voldaan: ' + items.filter((i) => i.required && !i.ok).map((i) => i.name).join(', ') }), items };
}

async function cmdSelftest() {
  const r = AF().selftest();
  return { ...r, ...(r.ok ? {} : { error: 'zelftest mislukt: ' + r.checks.filter((c) => !c.ok).map((c) => c.name).join('; ') }) };
}

async function cmdVersion() {
  let coreVersion = null, coreOrigin = null;
  try { coreVersion = AF().version; coreOrigin = loadCore().origin; } catch (_) { /* kern optioneel voor --version */ }
  return { ok: true, cli: CLI_VERSION, core: coreVersion, coreOrigin, node: process.versions.node };
}

/* ----------------------------------- main -------------------------------- */
async function main() {
  const { pos, opt } = parseArgs(process.argv.slice(2));
  const cmd = pos.shift();
  if (opt.version || cmd === '--version' || cmd === 'version') return cmdVersion();
  if (!cmd || cmd === 'help' || opt.help) return { ok: true, ...HELP };
  const table = { schema: cmdSchema, validate: cmdValidate, report: cmdReport, exec: cmdExec, render: cmdRender, probe: cmdProbe, doctor: cmdDoctor, selftest: cmdSelftest };
  if (!table[cmd]) fail('onbekende opdracht ' + JSON.stringify(cmd) + '; kies uit: ' + Object.keys(table).concat(['help']).join(', '));
  if (cmd !== 'doctor' && cmd !== 'probe') loadCore();
  return table[cmd](pos, opt);
}

main().then((res) => {
  process.stdout.write(JSON.stringify(res, null, 2) + '\n');
  process.exitCode = res && res.ok === false ? 1 : 0;
}).catch((e) => {
  process.stdout.write(JSON.stringify({ ok: false, error: errText(e) }, null, 2) + '\n');
  process.exitCode = 1;
});

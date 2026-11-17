// Tests van de CLI (node --test tests/cli.test.mjs)
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, writeFileSync, mkdtempSync, mkdirSync, existsSync, readdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const CLI = join(ROOT, 'audiofit-cli.mjs');

// Zelfde kernkeuze als de CLI: audiofit.html, anders src/core.js
function loadCoreForTests() {
  const html = join(ROOT, 'audiofit.html');
  let code;
  if (existsSync(html)) {
    const m = /<script id="audiofit-core">([\s\S]*?)<\/script>/.exec(readFileSync(html, 'utf8'));
    if (m) code = m[1];
  }
  if (!code) code = readFileSync(join(ROOT, 'src', 'core.js'), 'utf8');
  return new Function(code + '\nreturn AudioFit;')();
}
const AF = loadCoreForTests();
const SR = 48000;

const tmp = () => mkdtempSync(join(tmpdir(), 'afcli-'));
function run(args, env) {
  const r = spawnSync(process.execPath, [CLI, ...args], { encoding: 'utf8', env: env || process.env });
  let json = null;
  try { json = JSON.parse(r.stdout); } catch (_) { /* laat json null */ }
  return { code: r.status, json, stdout: r.stdout, stderr: r.stderr };
}
function sine(freq, dur, sr = SR, amp = 0.5) {
  const n = Math.round(dur * sr), a = new Float32Array(n);
  for (let i = 0; i < n; i++) a[i] = amp * Math.sin(2 * Math.PI * freq * i / sr);
  return a;
}
function writeWav(dir, name, dur, freq = 220, sr = SR) {
  writeFileSync(join(dir, name), AF.wav.encode([sine(freq, dur, sr), sine(freq * 1.5, dur, sr)], sr));
}
// Bouwt een recept met de kern zelf (bronnen: twee WAV's); retourneert { recipe, core }
function buildRecipe(cmds, sources) {
  const A = AF.create();
  A.setSampleRate(SR);
  for (const [n, dur, f] of sources) A.attachAudio(n, { sampleRate: SR, channels: [sine(f, dur), sine(f * 1.5, dur)] });
  const r = A.execAll(cmds);
  assert.ok(r.ok, r.error);
  return { recipe: A.exec({ cmd: 'get_state' }).result, core: A };
}
const baseCmds = [
  { cmd: 'create_block', args: { name: 'Blok 1', targetDuration: 10, fitMode: 'proportional' } },
  { cmd: 'add_marker', args: { source: 'a.wav', name: 'start', time: 1 } },
  { cmd: 'add_fragment', args: { block: 'b1', source: 'a.wav', in: '@start', out: 13, snap: false } }
];

test('schema: commando\'s en receptformaat', () => {
  const r = run(['schema']);
  assert.equal(r.code, 0);
  assert.equal(r.json.ok, true);
  assert.ok(Array.isArray(r.json.commands) && r.json.commands.some((c) => c.name === 'add_fragment'));
  assert.ok(r.json.recipe && r.json.emptyRecipe.sampleRate > 0);
});

test('help en --version', () => {
  assert.equal(run(['help']).json.ok, true);
  const v = run(['--version']);
  assert.equal(v.code, 0);
  assert.match(v.json.cli, /^\d+\.\d+\.\d+$/);
  assert.match(String(v.json.core), /^\d+\.\d+\.\d+$/);
});

test('onbekende opdracht: exit 1 met error', () => {
  const r = run(['bestaatniet']);
  assert.equal(r.code, 1);
  assert.equal(r.json.ok, false);
  assert.match(r.json.error, /onbekende opdracht/);
});

test('validate: goed recept', () => {
  const d = tmp();
  const { recipe } = buildRecipe(baseCmds, [['a.wav', 20, 220]]);
  writeFileSync(join(d, 'r.json'), JSON.stringify(recipe));
  const r = run(['validate', join(d, 'r.json')]);
  assert.equal(r.code, 0, r.stdout);
  assert.equal(r.json.ok, true);
  assert.deepEqual(r.json.problems, []);
});

test('validate: fout recept (onbekend blok-veld/kapotte structuur) en kapotte JSON', () => {
  const d = tmp();
  const { recipe } = buildRecipe(baseCmds, [['a.wav', 20, 220]]);
  recipe.blocks[0].fragments[0].source = 'bestaat-niet.wav';
  recipe.blocks[0].targetDuration = -3;
  writeFileSync(join(d, 'fout.json'), JSON.stringify(recipe));
  const r = run(['validate', join(d, 'fout.json')]);
  assert.equal(r.code, 1);
  assert.equal(r.json.ok, false);
  assert.ok(r.json.problems.length >= 1);
  writeFileSync(join(d, 'kapot.json'), '{ niet json');
  const k = run(['validate', join(d, 'kapot.json')]);
  assert.equal(k.code, 1);
  assert.match(k.json.error, /JSON/);
});

test('exec: marker-verwijzing, overschrijven met .bak.json', () => {
  const d = tmp();
  const { recipe } = buildRecipe([{ cmd: 'create_block', args: { name: 'Blok 1', targetDuration: 10, fitMode: 'proportional' } },
    { cmd: 'add_marker', args: { source: 'a.wav', name: 'start', time: 1 } }], [['a.wav', 20, 220]]);
  const p = join(d, 'recept.json');
  const origText = JSON.stringify(recipe);
  writeFileSync(p, origText);
  const cmds = [{ cmd: 'add_fragment', args: { block: 'b1', source: 'a.wav', in: '@start', out: 5, snap: false } }];
  const r = run(['exec', p, '--json', JSON.stringify(cmds)]);
  assert.equal(r.code, 0, r.stdout);
  assert.equal(r.json.ok, true);
  const nieuw = JSON.parse(readFileSync(p, 'utf8'));
  assert.equal(nieuw.blocks[0].fragments.length, 1);
  assert.equal(nieuw.blocks[0].fragments[0].in, 1);
  assert.equal(nieuw.blocks[0].fragments[0].out, 5);
  assert.equal(readFileSync(join(d, 'recept.bak.json'), 'utf8'), origText);
});

test('exec: --commands bestand met --out laat origineel intact; fout = niets geschreven', () => {
  const d = tmp();
  const { recipe } = buildRecipe([{ cmd: 'create_block', args: { name: 'B', targetDuration: 5, fitMode: 'proportional' } }], [['a.wav', 20, 220]]);
  const p = join(d, 'recept.json');
  writeFileSync(p, JSON.stringify(recipe));
  writeFileSync(join(d, 'c.json'), JSON.stringify([{ cmd: 'update_block', args: { block: 'b1', targetDuration: 7 } }]));
  const out = join(d, 'nieuw.json');
  const r = run(['exec', p, '--commands', join(d, 'c.json'), '--out', out]);
  assert.equal(r.code, 0, r.stdout);
  assert.equal(JSON.parse(readFileSync(out, 'utf8')).blocks[0].targetDuration, 7);
  assert.equal(JSON.parse(readFileSync(p, 'utf8')).blocks[0].targetDuration, 5);
  assert.ok(!existsSync(join(d, 'recept.bak.json')));
  // foute reeks: alles of niets
  const bad = [{ cmd: 'update_block', args: { block: 'b1', targetDuration: 9 } }, { cmd: 'delete_block', args: { block: 'b99' } }];
  const f = run(['exec', p, '--json', JSON.stringify(bad)]);
  assert.equal(f.code, 1);
  assert.equal(f.json.ok, false);
  assert.equal(JSON.parse(readFileSync(p, 'utf8')).blocks[0].targetDuration, 5);
});

test('report: met --sources; ontbrekende bron geeft fout met naam', () => {
  const d = tmp();
  const { recipe } = buildRecipe(baseCmds, [['a.wav', 20, 220]]);
  writeFileSync(join(d, 'r.json'), JSON.stringify(recipe));
  const miss = run(['report', join(d, 'r.json'), '--sources', d]);
  assert.equal(miss.code, 1);
  assert.match(miss.json.error, /a\.wav/);
  writeWav(d, 'a.wav', 20);
  const ok = run(['report', join(d, 'r.json'), '--sources', d]);
  assert.equal(ok.code, 0, ok.stdout);
  assert.ok(Array.isArray(ok.json.report) && ok.json.report.length === 1);
  const zonder = run(['report', join(d, 'r.json')]);
  assert.equal(zonder.code, 0);
});

test('render: WAV-bronnen, exacte lengte en sample-identiek aan export uit de kern', () => {
  const d = tmp(), out = join(d, 'uit');
  const { recipe, core } = buildRecipe(baseCmds, [['a.wav', 20, 220]]);
  writeFileSync(join(d, 'r.json'), JSON.stringify(recipe));
  writeWav(d, 'a.wav', 20);
  const r = run(['render', join(d, 'r.json'), '--sources', d, '--out', out]);
  assert.equal(r.code, 0, r.stdout + r.stderr);
  assert.equal(r.json.files.length, 1);
  const f = r.json.files[0];
  assert.equal(f.samples, 10 * SR);
  assert.equal(readdirSync(out)[0], 'Blok 1_10.000s.wav');
  const dec = AF.wav.decode(new Uint8Array(readFileSync(f.file)));
  assert.equal(dec.channels[0].length, 10 * SR);
  assert.equal(dec.sampleRate, SR);
  // sample-identiek (bronnen zijn de via WAV-16-bit teruggelezen versie van dezelfde signalen)
  const A = AF.create(); A.setSampleRate(SR);
  const src = AF.wav.decode(new Uint8Array(readFileSync(join(d, 'a.wav'))));
  A.attachAudio('a.wav', { sampleRate: SR, channels: src.channels });
  assert.ok(A.exec({ cmd: 'import_recipe', args: { recipe } }).ok);
  const ex = A.exec({ cmd: 'export_block', args: { block: 'b1' } }).result.files[0];
  assert.deepEqual(Buffer.from(ex.bytes), readFileSync(f.file));
  assert.match(r.stderr, /render blok b1/);
  void core;
});

test('render: --block en --normalize', () => {
  const d = tmp(), out = join(d, 'uit');
  const { recipe } = buildRecipe(baseCmds.concat([{ cmd: 'create_block', args: { name: 'Tweede', targetDuration: 4, fitMode: 'proportional' } },
    { cmd: 'add_fragment', args: { block: 'b2', source: 'a.wav', in: 0, out: 6, snap: false } }]), [['a.wav', 20, 220]]);
  writeFileSync(join(d, 'r.json'), JSON.stringify(recipe));
  writeWav(d, 'a.wav', 20);
  const r = run(['render', join(d, 'r.json'), '--sources', d, '--out', out, '--block', 'b2', '--normalize']);
  assert.equal(r.code, 0, r.stdout + r.stderr);
  assert.equal(r.json.files.length, 1);
  assert.equal(r.json.files[0].samples, 4 * SR);
  assert.ok(Math.abs(r.json.files[0].peakDb + 1) < 0.01);
  const x = run(['render', join(d, 'r.json'), '--sources', d, '--out', out, '--block', 'b9']);
  assert.equal(x.code, 1);
  assert.match(x.json.error, /b9/);
});

test('render: afwijkende sample rate van de bron wordt geresampled', () => {
  const d = tmp(), out = join(d, 'uit');
  const { recipe } = buildRecipe(baseCmds, [['a.wav', 20, 220]]);
  writeFileSync(join(d, 'r.json'), JSON.stringify(recipe));
  writeWav(d, 'a.wav', 20, 220, 44100);
  const r = run(['render', join(d, 'r.json'), '--sources', d, '--out', out]);
  assert.equal(r.code, 0, r.stdout + r.stderr);
  assert.equal(r.json.files[0].samples, 10 * SR);
  assert.equal(r.json.files[0].sampleRate, SR);
});

test('render met MP3-bron zonder ffmpeg op PATH: exit 1 en melding noemt ffmpeg', () => {
  const d = tmp(), leeg = join(d, 'leeg');
  mkdirSync(leeg);
  const { recipe } = buildRecipe(baseCmds, [['a.wav', 20, 220]]);
  recipe.sources[0].name = 'a.mp3';
  recipe.blocks[0].fragments[0].source = 'a.mp3';
  writeFileSync(join(d, 'r.json'), JSON.stringify(recipe));
  writeFileSync(join(d, 'a.mp3'), Buffer.from('ID3 geen echte mp3'));
  const r = run(['render', join(d, 'r.json'), '--sources', d, '--out', join(d, 'uit')], { ...process.env, PATH: leeg });
  assert.equal(r.code, 1, r.stdout);
  assert.equal(r.json.ok, false);
  assert.match(r.json.error, /ffmpeg/i);
  assert.match(r.json.error, /winget search ffmpeg/);
});

test('probe: WAV-bestand', () => {
  const d = tmp();
  writeWav(d, 'x.wav', 2.5, 440, 44100);
  const r = run(['probe', join(d, 'x.wav')]);
  assert.equal(r.code, 0, r.stdout);
  assert.equal(r.json.sampleRate, 44100);
  assert.equal(r.json.channels, 2);
  assert.ok(Math.abs(r.json.duration - 2.5) < 1e-6);
  const m = run(['probe', join(d, 'bestaatniet.wav')]);
  assert.equal(m.code, 1);
});

test('probe: niet-WAV via ffmpeg (alleen als ffmpeg aanwezig is)', (t) => {
  const has = spawnSync('ffmpeg', ['-version']);
  if (has.error || has.status !== 0) return t.skip('ffmpeg ontbreekt');
  const d = tmp();
  writeWav(d, 'x.wav', 1.5, 440, 44100);
  const c = spawnSync('ffmpeg', ['-v', 'error', '-y', '-i', join(d, 'x.wav'), join(d, 'x.flac')]);
  if (c.status !== 0) return t.skip('ffmpeg kan geen flac maken');
  const r = run(['probe', join(d, 'x.flac')]);
  assert.equal(r.code, 0, r.stdout);
  assert.equal(r.json.decodedVia, 'ffmpeg');
  assert.ok(Math.abs(r.json.duration - 1.5) < 0.01);
  assert.equal(r.json.sampleRate, 44100);
});

test('doctor: vorm van de JSON', () => {
  const r = run(['doctor']);
  assert.ok(r.json && typeof r.json.ok === 'boolean');
  assert.ok(Array.isArray(r.json.items));
  const names = r.json.items.map((i) => i.name);
  assert.deepEqual(names, ['node', 'ffmpeg', 'audiofit.html']);
  for (const i of r.json.items) {
    assert.equal(typeof i.ok, 'boolean');
    assert.equal(typeof i.detail, 'string');
    if (!i.ok) assert.equal(typeof i.advice, 'string');
  }
  assert.equal(r.json.items[0].ok, true);
  // zonder ffmpeg: advies, maar doctor blijft ok zolang de verplichte onderdelen kloppen
  const leeg = tmp();
  const z = run(['doctor'], { ...process.env, PATH: leeg });
  const ff = z.json.items.find((i) => i.name === 'ffmpeg');
  assert.equal(ff.ok, false);
  assert.match(ff.advice, /winget search ffmpeg/);
});

test('selftest slaagt', () => {
  const r = run(['selftest']);
  assert.equal(r.code, 0, r.stdout);
  assert.equal(r.json.ok, true);
});

test('stemfragment (voice) renderen — alleen als de kern add_voice kent (0.2.0)', (t) => {
  if (!AF.describe().some((c) => c.name === 'add_voice')) return t.skip('kern ' + AF.version + ' heeft nog geen stemspoor');
  const d = tmp(), out = join(d, 'uit');
  const cmds = baseCmds.concat([{ cmd: 'add_voice', args: { block: 'b1', source: 'v.wav', in: 0, out: 2, anchor: 'f1', snap: false } }]);
  const { recipe } = buildRecipe(cmds, [['a.wav', 20, 220], ['v.wav', 5, 330]]);
  writeFileSync(join(d, 'r.json'), JSON.stringify(recipe));
  writeWav(d, 'a.wav', 20);
  writeWav(d, 'v.wav', 5, 330);
  const r = run(['render', join(d, 'r.json'), '--sources', d, '--out', out]);
  assert.equal(r.code, 0, r.stdout + r.stderr);
  assert.equal(r.json.files[0].samples, 10 * SR);
  const v = run(['validate', join(d, 'r.json')]);
  assert.equal(v.code, 0, v.stdout);
});

// Node-tests van de Audio Fit-kern. Draaien: node --test tests/
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const code = readFileSync(join(here, '..', 'src', 'core.js'), 'utf8');
const load = () => new Function(code + '\nreturn AudioFit;')();
const AF = load();

const SR = 48000;
function sine(freq, dur, sr = SR, amp = 0.5, phase = 0) {
  const n = Math.round(dur * sr), o = new Float32Array(n);
  for (let i = 0; i < n; i++) o[i] = amp * Math.sin(2 * Math.PI * freq * i / sr + phase);
  return o;
}
function fresh(sr = SR) { const a = AF.create(); a.setSampleRate(sr); return a; }
function ok(res) { assert.equal(res.ok, true, res.error); return res.result !== undefined ? res.result : res; }

test('vorm en versie', () => {
  assert.equal(AF.version, '0.3.0');
  assert.equal(typeof AF.CONSTANTS, 'object');
  const names = AF.describe().map((d) => d.name);
  for (const n of ['get_state', 'list_sources', 'add_marker', 'update_marker', 'delete_marker', 'create_block', 'update_block',
    'delete_block', 'add_fragment', 'update_fragment', 'move_fragment', 'duplicate_fragment', 'delete_fragment', 'set_transition',
    'fit_report', 'render_block', 'export_block', 'import_recipe', 'undo', 'redo', 'add_voice', 'update_voice', 'delete_voice']) assert.ok(names.includes(n), n);
  assert.equal(typeof AF.renderBlockPure, 'function');
  for (const d of AF.describe()) assert.equal(d.schema.type, 'object');
  assert.ok(!/\b(window|document|require\(|import |AudioContext|Worker|Blob)\b/.test(code), 'geen browser-API of modules in core.js');
});

test('selftest (criteria 2–7 + stemcheck) is ok', () => {
  const r = AF.selftest();
  for (const c of r.checks) console.log((c.ok ? '  ✓ ' : '  ✗ ') + c.name + ' — ' + c.detail);
  console.log('  selftest totaal ' + r.ms + ' ms');
  assert.equal(r.checks.length, 7);
  assert.equal(r.ok, true, JSON.stringify(r.checks.filter((c) => !c.ok)));
});

test('12 s → 10 s levert exact 10 × sampleRate samples', () => {
  const A = fresh();
  A.attachAudio('twaalf.wav', { sampleRate: SR, channels: [sine(220, 12), sine(330, 12)] });
  const b = ok(A.exec({ cmd: 'create_block', args: { name: 'Blok 1', targetDuration: 10, fitMode: 'proportional' } }));
  ok(A.exec({ cmd: 'add_fragment', args: { block: b.id, source: 'twaalf.wav', in: 0, out: 12, snap: false } }));
  const rep = ok(A.exec({ cmd: 'fit_report', args: { block: b.id } }))[0];
  assert.ok(Math.abs(rep.fragments[0].ratio - 1.2) < 1e-12);
  assert.equal(rep.fragments[0].status, 'orange');
  const rr = ok(A.exec({ cmd: 'render_block', args: { block: b.id } }));
  assert.equal(rr.samples, 10 * SR);
  assert.equal(A.getRendered(b.id).channels[0].length, 10 * SR);
  const ex = ok(A.exec({ cmd: 'export_block', args: { block: b.id } }));
  assert.equal(ex.files[0].name, 'Blok 1_10.000s.wav');
  const dec = A.wav.decode(ex.files[0].bytes);
  assert.equal(dec.channels.length, 2);
  assert.equal(dec.channels[0].length, 10 * SR);
  assert.equal(ex.files[0].bytes.length, 44 + 10 * SR * 2 * 2);
});

test('WSOLA 15 s stereo 48 kHz: exacte lengte en rekentijd', () => {
  const n = 18 * SR, L = new Float32Array(n), R = new Float32Array(n);
  for (let i = 0; i < n; i++) { L[i] = 0.4 * Math.sin(i * 0.0288) + 0.2 * Math.sin(i * 0.071); R[i] = 0.4 * Math.sin(i * 0.0431); }
  for (const preset of ['music', 'voice']) {
    const t = performance.now();
    const out = AF.stretch([L, R], 15 * SR, { preset, sampleRate: SR });
    const ms = performance.now() - t;
    console.log(`  WSOLA ${preset}: 18 s → 15 s stereo 48 kHz in ${ms.toFixed(0)} ms`);
    assert.equal(out.length, 2);
    assert.equal(out[0].length, 15 * SR);
    assert.ok(ms < 10000, 'binnen enkele seconden');
  }
  // sample-rate-schaling van de presets
  assert.deepEqual(AF.wsolaParams('music', 48000).N, 2048);
  assert.deepEqual(AF.wsolaParams('music', 44100).N, 2048);
  const p96 = AF.wsolaParams('music', 96000);
  assert.equal(p96.N, 4096); assert.equal(p96.Hs, 2048); assert.equal(p96.D, 1024);
  const p22 = AF.wsolaParams('voice', 22050);
  assert.equal(p22.N % 2, 0); assert.equal(p22.Hs * 2, p22.N); assert.equal(p22.D % 2, 0);
  // stap 7 en 8
  assert.equal(AF.stretchInfo([sine(440, 1)], SR, { sampleRate: SR }).method, 'copy');
  const short = AF.stretchInfo([sine(440, 0.05)], Math.round(0.04 * SR), { sampleRate: SR });
  assert.equal(short.method, 'varispeed');
  assert.match(short.warning, /varispeed/);
});

test('resample levert exact outLength en behoudt een sinus', () => {
  const x = sine(1000, 1, 44100, 0.5);
  const y = AF.resample([x], 48000)[0];
  assert.equal(y.length, 48000);
  // waarde midden in vergelijken met de ideale sinus bij 48 kHz
  let err = 0;
  for (let i = 1000; i < 47000; i++) err = Math.max(err, Math.abs(y[i] - 0.5 * Math.sin(2 * Math.PI * 1000 * i / 48000)));
  assert.ok(err < 2e-3, 'max fout ' + err);
});

test('add_fragment met "@a"/"@b" levert het fragment tussen de markers', () => {
  const A = fresh();
  A.attachAudio('intro.wav', { sampleRate: SR, channels: [sine(100, 8)] });
  ok(A.exec({ cmd: 'add_marker', args: { source: 'intro.wav', name: 'a', time: 1.25 } }));
  ok(A.exec({ cmd: 'add_marker', args: { source: 'intro.wav', name: 'b', time: 3.5 } }));
  const auto = ok(A.exec({ cmd: 'add_marker', args: { source: 'intro.wav', time: 5 } }));
  assert.equal(auto.name, 'm1');
  const b = ok(A.exec({ cmd: 'create_block', args: { name: 'X', targetDuration: 2.25, fitMode: 'proportional' } }));
  const f = ok(A.exec({ cmd: 'add_fragment', args: { block: b.id, source: 'intro.wav', in: '@a', out: '@b', snap: false } }));
  assert.equal(f.in, 1.25); assert.equal(f.out, 3.5); assert.equal(f.id, 'f1');
  // met snap: binnen ±5 ms van de marker, op een nuldoorgang
  const g = ok(A.exec({ cmd: 'add_fragment', args: { block: b.id, source: 'intro.wav', in: '@a', out: '@b' } }));
  assert.ok(Math.abs(g.in - 1.25) <= 0.005 && Math.abs(g.out - 3.5) <= 0.005);
  const sig = A.getAudio('intro.wav').channels[0];
  assert.ok(Math.abs(sig[Math.round(g.in * SR)]) < 0.01, 'snap op nuldoorgang');
  // marker later verplaatsen verandert het fragment niet
  ok(A.exec({ cmd: 'update_marker', args: { source: 'intro.wav', name: 'a', newName: 'begin', time: 2 } }));
  const st = ok(A.exec({ cmd: 'get_state' }));
  assert.equal(st.blocks[0].fragments[0].in, 1.25);
  assert.deepEqual(st.sources[0].markers.map((m) => m.name), ['begin', 'b', 'm1']);
});

test('onbekende marker geeft de lijst bestaande markers', () => {
  const A = fresh();
  A.attachAudio('intro.wav', { sampleRate: SR, channels: [sine(100, 8)] });
  ok(A.exec({ cmd: 'add_marker', args: { source: 'intro.wav', name: 'a', time: 1 } }));
  ok(A.exec({ cmd: 'add_marker', args: { source: 'intro.wav', name: 'b', time: 2 } }));
  const b = ok(A.exec({ cmd: 'create_block', args: { targetDuration: 5, fitMode: 'proportional' } }));
  const r = A.exec({ cmd: 'add_fragment', args: { block: b.id, source: 'intro.wav', in: '@a', out: '@refrein' } });
  assert.equal(r.ok, false);
  assert.match(r.error, /@refrein/);
  assert.match(r.error, /@a \(1\.0 s\)/);
  assert.match(r.error, /@b \(2\.0 s\)/);
  // en buiten-bereik-melding volgens het ontwerp
  const r2 = A.exec({ cmd: 'add_fragment', args: { block: b.id, source: 'intro.wav', in: 0, out: 250.0 } });
  assert.equal(r2.error, 'out 250.0 ligt voorbij het einde van intro.wav (8.0 s)');
  // onbekend argument en onbekend commando
  assert.match(A.exec({ cmd: 'create_block', args: { duur: 3 } }).error, /onbekend argument 'duur'.*toegestaan: name, targetDuration/);
  assert.match(A.exec({ cmd: 'maak_blok' }).error, /onbekend commando "maak_blok"; beschikbaar: /);
  assert.match(A.exec({ cmd: 'add_fragment', args: { block: 'b9', source: 'intro.wav', in: 0, out: 1 } }).error, /blok "b9" bestaat niet; bestaande blokken: b1/);
});

test('execAll: rollback bij fout en het geheel is één undo-stap', () => {
  const A = fresh();
  A.attachAudio('a.wav', { sampleRate: SR, channels: [sine(200, 10)] });
  let calls = 0;
  const unsub = A.subscribe(() => { calls++; });
  const before = JSON.stringify(ok(A.exec({ cmd: 'get_state' })));
  const bad = A.execAll([
    { cmd: 'create_block', args: { name: 'B', targetDuration: 5, fitMode: 'proportional' } },
    { cmd: 'add_fragment', args: { block: 'b1', source: 'a.wav', in: 0, out: 3 } },
    { cmd: 'add_fragment', args: { block: 'b1', source: 'a.wav', in: 4, out: 99 } }
  ]);
  assert.equal(bad.ok, false);
  assert.equal(bad.failedIndex, 2);
  assert.equal(bad.results.length, 2);
  assert.match(bad.error, /commando 3 \(add_fragment\): out 99\.0 ligt voorbij het einde van a\.wav \(10\.0 s\)/);
  assert.equal(JSON.stringify(ok(A.exec({ cmd: 'get_state' }))), before, 'niets gewijzigd');
  assert.equal(calls, 0);
  assert.equal(A.history().undo, 0);

  const good = A.execAll([
    { cmd: 'create_block', args: { name: 'B', targetDuration: 5, fitMode: 'proportional' } },
    { cmd: 'add_fragment', args: { block: 'B', source: 'a.wav', in: 0, out: 3, snap: false } },
    { cmd: 'add_fragment', args: { block: 'B', source: 'a.wav', in: 4, out: 7, snap: false } },
    { cmd: 'set_transition', args: { block: 'B', index: 0, type: 'gap', ms: 250 } },
    { cmd: 'fit_report', args: { block: 'B' } }
  ]);
  assert.equal(good.ok, true, good.error);
  assert.equal(good.results[0].id, 'b2', 'ids worden niet hergebruikt');
  assert.ok(Math.abs(good.results[4][0].ratio - 6 / 4.75) < 1e-12);
  assert.equal(A.history().undo, 1);
  assert.equal(calls, 1);
  ok(A.exec({ cmd: 'undo' }));
  assert.equal(JSON.stringify(ok(A.exec({ cmd: 'get_state' }))), before, 'één undo draait de hele reeks terug');
  ok(A.exec({ cmd: 'redo' }));
  assert.equal(ok(A.exec({ cmd: 'get_state' })).blocks[0].fragments.length, 2);
  assert.equal(A.exec({ cmd: 'redo' }).ok, false);
  assert.match(A.execAll([{ cmd: 'undo' }]).error, /undo kan niet binnen een reeks/);
  unsub();
});

test('undo is begrensd op 50 stappen', () => {
  const A = fresh();
  for (let i = 0; i < 60; i++) ok(A.exec({ cmd: 'create_block', args: { targetDuration: 1 + i } }));
  assert.equal(A.history().undo, 50);
  let n = 0;
  while (A.exec({ cmd: 'undo' }).ok) n++;
  assert.equal(n, 50);
  assert.equal(ok(A.exec({ cmd: 'get_state' })).blocks.length, 10);
});

test('WAV encode/decode roundtrip (16 bit) en lezen van 24 bit / float 32', () => {
  const n = 1000, L = new Float32Array(n), R = new Float32Array(n);
  for (let i = 0; i < n; i++) { L[i] = Math.sin(i * 0.1) * 0.9; R[i] = (i % 7) / 7 - 0.5; }
  L[0] = 1; L[1] = -1; L[2] = 1.7; // begrenzing
  const bytes = AF.wav.encode([L, R], 44100);
  assert.ok(bytes instanceof Uint8Array);
  assert.equal(bytes.length, 44 + n * 4);
  const d = AF.wav.decode(bytes);
  assert.equal(d.sampleRate, 44100);
  assert.equal(d.channels.length, 2);
  for (let i = 3; i < n; i++) {
    assert.ok(Math.abs(d.channels[0][i] - L[i]) <= 2 / 32768);
    assert.ok(Math.abs(d.channels[1][i] - R[i]) <= 2 / 32768);
  }
  assert.equal(d.channels[0][0], 32767 / 32768);
  assert.equal(d.channels[0][1], -1);
  assert.equal(d.channels[0][2], 32767 / 32768);

  // handgemaakte WAV's: extra chunk, 24-bit PCM en IEEE float
  function makeWav(format, bits, samples, extraChunk) {
    const bps = bits / 8, data = samples.length * bps, extra = extraChunk ? 8 + 3 + 1 : 0;
    const buf = new ArrayBuffer(12 + 24 + extra + 8 + data), v = new DataView(buf);
    const w = (o, s) => { for (let i = 0; i < 4; i++) v.setUint8(o + i, s.charCodeAt(i)); };
    w(0, 'RIFF'); v.setUint32(4, buf.byteLength - 8, true); w(8, 'WAVE');
    w(12, 'fmt '); v.setUint32(16, 16, true); v.setUint16(20, format, true); v.setUint16(22, 1, true);
    v.setUint32(24, 22050, true); v.setUint32(28, 22050 * bps, true); v.setUint16(32, bps, true); v.setUint16(34, bits, true);
    let o = 36;
    if (extraChunk) { w(o, 'LIST'); v.setUint32(o + 4, 3, true); o += 8 + 4; }
    w(o, 'data'); v.setUint32(o + 4, data, true); o += 8;
    samples.forEach((s) => {
      if (format === 3) v.setFloat32(o, s, true);
      else { const x = Math.round(s * 8388607); v.setUint8(o, x & 255); v.setUint8(o + 1, (x >> 8) & 255); v.setInt8(o + 2, x >> 16); }
      o += bps;
    });
    return new Uint8Array(buf);
  }
  const f = AF.wav.decode(makeWav(3, 32, [0.25, -0.5, 0.125], true));
  assert.deepEqual(Array.from(f.channels[0]), [0.25, -0.5, 0.125]);
  assert.equal(f.sampleRate, 22050);
  const p24 = AF.wav.decode(makeWav(1, 24, [0.5, -0.25]));
  assert.ok(Math.abs(p24.channels[0][0] - 0.5) < 1e-6 && Math.abs(p24.channels[0][1] + 0.25) < 1e-6);
  const bad = makeWav(1, 24, [0]); new DataView(bad.buffer).setUint16(20, 2, true);
  assert.throws(() => AF.wav.decode(bad), /WAV-formaat 2 /);
});

test('fit-voorbeeld criterium 5: ratio 1.0625 voor alle drie, export exact 15,000 s', () => {
  const A = fresh();
  const n = 20 * SR, x = new Float32Array(n);
  for (let i = 0; i < n; i++) x[i] = 0.3 * Math.sin(2 * Math.PI * 220 * i / SR) + 0.2 * Math.sin(2 * Math.PI * 331 * i / SR);
  A.attachAudio('muziek.wav', { sampleRate: SR, channels: [x] });
  const r = A.execAll([
    { cmd: 'create_block', args: { name: 'Blok 1', targetDuration: 15, fitMode: 'proportional' } },
    { cmd: 'add_fragment', args: { block: 'b1', source: 'muziek.wav', in: 0, out: 5, snap: false } },
    { cmd: 'add_fragment', args: { block: 'b1', source: 'muziek.wav', in: 5, out: 11, snap: false } },
    { cmd: 'add_fragment', args: { block: 'b1', source: 'muziek.wav', in: 12, out: 18, snap: false } },
    { cmd: 'fit_report', args: { block: 'b1' } }
  ]);
  assert.equal(r.ok, true, r.error);
  const rep = r.results[4][0];
  assert.equal(rep.transitions.length, 2);
  assert.ok(rep.transitions.every((t) => t.type === 'crossfade' && t.ms === 500), 'standaard 2 crossfades van 0,5 s');
  for (const f of rep.fragments) {
    assert.ok(Math.abs(f.ratio - 17 / 16) < 1e-12, 'ratio ' + f.ratio);
    assert.equal(f.status, 'green');
  }
  assert.ok(Math.abs(rep.contentDuration - 15) < 1e-9);
  assert.equal(rep.exportable, true);
  const ex = ok(A.exec({ cmd: 'export_block', args: { block: 'b1' } }));
  assert.equal(ex.files[0].name, 'Blok 1_15.000s.wav');
  assert.equal(A.wav.decode(ex.files[0].bytes).channels[0].length, 15 * SR);
});

test('fit-regels: vergrendeld, overlapgrens 90%, noemer ≤ 0, manual', () => {
  const A = fresh();
  A.attachAudio('s.wav', { sampleRate: SR, channels: [sine(300, 30)] });
  ok(A.execAll([
    { cmd: 'create_block', args: { name: 'L', targetDuration: 10, fitMode: 'proportional' } },
    { cmd: 'add_fragment', args: { block: 'b1', source: 's.wav', in: 0, out: 4, snap: false, locked: true } },
    { cmd: 'add_fragment', args: { block: 'b1', source: 's.wav', in: 4, out: 10, snap: false } },
    { cmd: 'set_transition', args: { block: 'b1', index: 0, type: 'cut' } }
  ]));
  let rep = ok(A.exec({ cmd: 'fit_report', args: { block: 'b1' } }))[0];
  assert.equal(rep.fragments[0].ratio, 1);
  assert.ok(Math.abs(rep.fragments[1].ratio - 1) < 1e-12);
  // overlap groter dan 90% van het kortste fragment wordt begrensd
  ok(A.exec({ cmd: 'set_transition', args: { block: 'b1', index: 0, type: 'mix', ms: 5000 } }));
  rep = ok(A.exec({ cmd: 'fit_report', args: { block: 'b1' } }))[0];
  assert.ok(Math.abs(rep.transitions[0].effectiveMs - 3600) < 1e-6, String(rep.transitions[0].effectiveMs));
  assert.ok(rep.warnings.some((w) => /ingekort van 5000 ms naar 3600 ms/.test(w)));
  ok(A.exec({ cmd: 'render_block', args: { block: 'b1' } }));
  assert.equal(A.getRendered('b1').channels[0].length, 10 * SR);
  // noemer ≤ 0
  ok(A.exec({ cmd: 'update_block', args: { block: 'b1', targetDuration: 3 } }));
  ok(A.exec({ cmd: 'set_transition', args: { block: 'b1', index: 0, type: 'gap', ms: 1000 } }));
  const rr = A.exec({ cmd: 'render_block', args: { block: 'b1' } });
  assert.equal(rr.ok, false);
  assert.match(rr.error, /fit onmogelijk/);
  // manual: te kort → stilte, te lang → geblokkeerd
  ok(A.exec({ cmd: 'update_block', args: { block: 'b1', targetDuration: 12, fitMode: 'manual' } }));
  ok(A.exec({ cmd: 'update_fragment', args: { fragment: 'f2', manualDuration: 5 } }));
  rep = ok(A.exec({ cmd: 'fit_report', args: { block: 'b1' } }))[0];
  assert.ok(Math.abs(rep.deviation + 2) < 1e-9);
  const r1 = ok(A.exec({ cmd: 'render_block', args: { block: 'b1' } }));
  assert.equal(r1.samples, 12 * SR);
  ok(A.exec({ cmd: 'update_fragment', args: { fragment: 'f2', manualDuration: 9 } }));
  const r2 = A.exec({ cmd: 'export_block', args: { block: 'b1' } });
  assert.equal(r2.ok, false);
  assert.match(r2.error, /te lang/);
});

test('create_block zonder fitMode: manual, fragment op originele lengte (te kort); recept zonder fitMode blijft proportional', () => {
  const A = fresh();
  A.attachAudio('o.wav', { sampleRate: SR, channels: [sine(300, 20)] });
  const b = ok(A.exec({ cmd: 'create_block', args: { name: 'N', targetDuration: 10 } }));
  assert.equal(ok(A.exec({ cmd: 'get_state' })).blocks[0].fitMode, 'manual');
  ok(A.exec({ cmd: 'add_fragment', args: { block: b.id, source: 'o.wav', in: 0, out: 6, snap: false } }));
  let rep = ok(A.exec({ cmd: 'fit_report', args: { block: b.id } }))[0];
  assert.equal(rep.fragments[0].ratio, 1);
  assert.ok(Math.abs(rep.fragments[0].targetDuration - 6) < 1e-12);
  assert.ok(Math.abs(rep.deviation + 4) < 1e-9, 'blok 4 s te kort');
  assert.equal(rep.exportable, true);
  const r = ok(A.exec({ cmd: 'render_block', args: { block: b.id } }));
  assert.equal(r.samples, 10 * SR);
  const ch = A.getRendered(b.id).channels[0];
  let tail = 0; for (let i = 7 * SR; i < 10 * SR; i++) tail = Math.max(tail, Math.abs(ch[i]));
  assert.equal(tail, 0, 'stilte aan het eind');
  // "Pas blok in" = fitMode proportional → vult exact
  ok(A.exec({ cmd: 'update_block', args: { block: b.id, fitMode: 'proportional' } }));
  rep = ok(A.exec({ cmd: 'fit_report', args: { block: b.id } }))[0];
  assert.ok(Math.abs(rep.fragments[0].ratio - 0.6) < 1e-12);
  assert.ok(Math.abs(rep.deviation) < 1e-9);
  // import zonder fitMode → proportional (bestaande recepten veranderen niet)
  const B = fresh();
  ok(B.exec({ cmd: 'import_recipe', args: { recipe: { version: 2, sampleRate: SR, sources: [],
    blocks: [{ id: 'b1', name: 'X', targetDuration: 5, fragments: [], transitions: [], voice: [] }] } } }));
  assert.equal(ok(B.exec({ cmd: 'get_state' })).blocks[0].fitMode, 'proportional');
});

test('import_recipe, ids doorlopen, attachAudio met hernoemen en resamplen', () => {
  const A = fresh();
  const recipe = {
    version: 1, sampleRate: 48000,
    sources: [{ name: 'intro.mp3', duration: 184.32, channels: 2, markers: [{ name: 'refrein', time: 42.118 }] }],
    blocks: [{ id: 'b7', name: 'Blok 1', targetDuration: 15, fitMode: 'proportional',
      fragments: [{ id: 'f12', source: 'intro.mp3', in: 42.118, out: 48.75 }, { id: 'f3', source: 'intro.mp3', in: 60, out: 70 }],
      transitions: [{ type: 'crossfade', ms: 800, curve: 'equal_power' }] }]
  };
  const r = ok(A.exec({ cmd: 'import_recipe', args: { recipe } }));
  assert.equal(r.blocks, 1);
  assert.deepEqual(r.missingSources, ['intro.mp3']);
  assert.equal(ok(A.exec({ cmd: 'create_block', args: { targetDuration: 2 } })).id, 'b8');
  const err = A.exec({ cmd: 'render_block', args: { block: 'b7' } });
  assert.match(err.error, /ontbrekende bron\(nen\) intro\.mp3/);
  const st = ok(A.exec({ cmd: 'get_state' }));
  assert.equal(st.blocks[0].fragments[0].fadeInMs, 8);
  assert.equal(st.blocks[0].fragments[0].preset, 'music');
  // ongeldig recept
  const bad = A.exec({ cmd: 'import_recipe', args: { recipe: { blocks: [{ id: 'b1', targetDuration: 5, fragments: [{ id: 'f1', source: 'x.wav', in: 0, out: 1 }] }] } } });
  assert.equal(bad.ok, false);
  assert.match(bad.error, /onbekende bron "x\.wav"/);
  // attachAudio: hernoemen en resamplen 44,1 → 48 kHz
  const B = fresh();
  const n1 = B.attachAudio('a.wav', { sampleRate: 44100, channels: [sine(440, 1, 44100)] });
  const n2 = B.attachAudio('a.wav', { sampleRate: 48000, channels: [sine(440, 1)] }, { rename: true });
  assert.equal(n1, 'a.wav'); assert.equal(n2, 'a.wav (2)');
  assert.equal(B.getAudio('a.wav').channels[0].length, 48000);
  assert.equal(B.getAudio('a.wav').sampleRate, 48000);
  assert.throws(() => B.setSampleRate(44100), /al audio gekoppeld/);
  assert.deepEqual(ok(B.exec({ cmd: 'list_sources' })).map((s) => s.name), ['a.wav', 'a.wav (2)']);
  // undo laat geladen bronnen staan
  ok(B.exec({ cmd: 'add_marker', args: { source: 'a.wav', name: 'x', time: 0.5 } }));
  ok(B.exec({ cmd: 'undo' }));
  assert.equal(ok(B.exec({ cmd: 'list_sources' })).length, 2);
});

test('ratioStatus-grenzen', () => {
  assert.equal(AF.ratioStatus(1), 'green');
  assert.equal(AF.ratioStatus(1.1), 'green');
  assert.equal(AF.ratioStatus(0.8), 'orange');
  assert.equal(AF.ratioStatus(1.2), 'orange');
  assert.equal(AF.ratioStatus(1.5), 'red');
  assert.equal(AF.ratioStatus(0.6), 'red');
  assert.equal(AF.ratioStatus(2.1), 'blocked');
  assert.equal(AF.ratioStatus(0.4), 'blocked');
});

/* ===================== Fase 2: stemspoor, ducking, renderBlockPure ===================== */

const dc = (v, dur, sr = SR) => new Float32Array(Math.round(dur * sr)).fill(v);
// Blok met twee muziekfragmenten (dc 0,5) en een cut; ratio 1 → exacte kopie.
function voiceSetup({ T = 4, music = 0.5 } = {}) {
  const A = fresh();
  A.attachAudio('m.wav', { sampleRate: SR, channels: [dc(music, 2)] });
  A.attachAudio('v.wav', { sampleRate: SR, channels: [dc(0.2, 3)] });
  A.attachAudio('nul.wav', { sampleRate: SR, channels: [dc(0, 3)] });
  ok(A.execAll([
    { cmd: 'create_block', args: { name: 'V', targetDuration: T, fitMode: 'proportional' } },
    { cmd: 'add_fragment', args: { block: 'b1', source: 'm.wav', in: 0, out: 2, snap: false } },
    { cmd: 'add_fragment', args: { block: 'b1', source: 'm.wav', in: 0, out: 2, snap: false } },
    { cmd: 'set_transition', args: { block: 'b1', index: 0, type: 'cut' } }
  ]));
  return A;
}

test('recept version 1 importeren → version 2 met voice: [], duckDb −6, duckMs 150; version 2 behoudt stem', () => {
  const A = fresh();
  const v1 = { version: 1, sampleRate: 48000, sources: [{ name: 'a.wav', duration: 30, channels: 2, markers: [] }],
    blocks: [{ id: 'b1', name: 'B', targetDuration: 10, fragments: [{ id: 'f1', source: 'a.wav', in: 0, out: 10 }] }] };
  ok(A.exec({ cmd: 'import_recipe', args: { recipe: v1 } }));
  let st = ok(A.exec({ cmd: 'get_state' }));
  assert.equal(st.version, 2);
  assert.deepEqual(st.blocks[0].voice, []);
  assert.equal(st.blocks[0].duckDb, -6);
  assert.equal(st.blocks[0].duckMs, 150);
  assert.equal(ok(A.exec({ cmd: 'get_state' })).version, 2);
  // version 2 met stem; ids lopen door
  const v2 = JSON.parse(JSON.stringify(v1));
  v2.version = 2; v2.blocks[0].duckDb = -9; v2.blocks[0].duckMs = 200;
  v2.blocks[0].voice = [{ id: 'v7', source: 'a.wav', in: 12, out: 14, anchor: 'f1', offset: 1.5 }];
  ok(A.exec({ cmd: 'import_recipe', args: { recipe: v2 } }));
  st = ok(A.exec({ cmd: 'get_state' }));
  assert.deepEqual(st.blocks[0].voice[0], { id: 'v7', source: 'a.wav', in: 12, out: 14, anchor: 'f1', offset: 1.5, targetDuration: null,
    mode: 'pitch', preset: 'voice', gainDb: 0, fadeInMs: 8, fadeOutMs: 8, acceptOverlap: false });
  assert.equal(st.blocks[0].duckDb, -9);
  A.attachAudio('a.wav', { sampleRate: SR, channels: [sine(200, 30)] });
  assert.equal(ok(A.exec({ cmd: 'add_voice', args: { block: 'b1', source: 'a.wav', in: 20, out: 21, anchor: 'f1', offset: 5 } })).id, 'v8');
  // fouten: onbekende versie, anker uit ander blok
  assert.match(A.exec({ cmd: 'import_recipe', args: { recipe: { version: 3 } } }).error, /version 3 wordt niet ondersteund; verwacht 1 of 2/);
  const bad = JSON.parse(JSON.stringify(v2)); bad.blocks[0].voice[0].anchor = 'f9';
  assert.match(A.exec({ cmd: 'import_recipe', args: { recipe: bad } }).error, /anker "f9" is geen muziekfragment van blok b1; kies uit: f1/);
});

test('stem hangt aan zijn anker en schuift mee met de fit', () => {
  const A = fresh();
  A.attachAudio('m.wav', { sampleRate: SR, channels: [sine(220, 20)] });
  A.attachAudio('v.wav', { sampleRate: SR, channels: [sine(330, 5)] });
  ok(A.execAll([
    { cmd: 'create_block', args: { name: 'B', targetDuration: 10, fitMode: 'proportional' } },
    { cmd: 'add_fragment', args: { block: 'b1', source: 'm.wav', in: 0, out: 5, snap: false } },
    { cmd: 'add_fragment', args: { block: 'b1', source: 'm.wav', in: 6, out: 12, snap: false } }
  ]));
  const add = ok(A.exec({ cmd: 'add_voice', args: { block: 'b1', source: 'v.wav', in: 0, out: 2, anchor: 'f2', offset: 0.5, snap: false } }));
  let rep = ok(A.exec({ cmd: 'fit_report', args: { block: 'b1' } }))[0];
  const f2 = rep.fragments[1];
  assert.equal(add.id, 'v1');
  assert.ok(Math.abs(add.start - (f2.start + 0.5)) < 1e-9 && Math.abs(add.end - add.start - 2) < 1e-9);
  assert.ok(Math.abs(rep.voice[0].start - (f2.start + 0.5)) < 1e-9);
  // vorm van fit_report-uitbreiding
  assert.equal(rep.fragments[0].start, 0);
  assert.ok(Math.abs(rep.fragments[1].end - 10) < 1e-9);
  assert.deepEqual(Object.keys(rep.transitions[0]).slice(0, 5), ['index', 'type', 'ms', 'start', 'end']);
  assert.ok(Math.abs(rep.transitions[0].start - f2.start) < 1e-9 && Math.abs(rep.transitions[0].end - rep.fragments[0].end) < 1e-9);
  assert.deepEqual(Object.keys(rep.voice[0]), ['id', 'anchor', 'start', 'end', 'sourceDuration', 'targetDuration', 'ratio', 'status']);
  assert.equal(rep.voice[0].status, 'green');
  assert.ok(Math.abs(rep.duck[0].start - (rep.voice[0].start - 0.15)) < 1e-9 && Math.abs(rep.duck[0].end - (rep.voice[0].end + 0.15)) < 1e-9);
  assert.deepEqual(rep.overlaps, []);
  // fit verandert → stem schuift mee
  const before = rep.voice[0].start;
  ok(A.exec({ cmd: 'update_block', args: { block: 'b1', targetDuration: 9 } }));
  rep = ok(A.exec({ cmd: 'fit_report', args: { block: 'b1' } }))[0];
  assert.ok(rep.voice[0].start < before - 0.3);
  assert.ok(Math.abs(rep.voice[0].start - (rep.fragments[1].start + 0.5)) < 1e-9);
  // ook na herordenen (move_fragment) volgt de stem het anker
  ok(A.exec({ cmd: 'move_fragment', args: { fragment: 'f2', position: 0 } }));
  rep = ok(A.exec({ cmd: 'fit_report', args: { block: 'b1' } }))[0];
  assert.equal(rep.fragments[0].id, 'f2');
  assert.ok(Math.abs(rep.voice[0].start - 0.5) < 1e-9);
  // targetDuration: ratio en status, en een stem met targetDuration rendert op lengte
  const u = ok(A.exec({ cmd: 'update_voice', args: { voice: 'v1', targetDuration: 1.6 } }));
  assert.ok(Math.abs(u.end - u.start - 1.6) < 1e-9);
  rep = ok(A.exec({ cmd: 'fit_report', args: { block: 'b1' } }))[0];
  assert.ok(Math.abs(rep.voice[0].ratio - 1.25) < 1e-12);
  assert.equal(rep.voice[0].status, 'orange');
  ok(A.exec({ cmd: 'update_voice', args: { voice: 'v1', targetDuration: 1.2 } }));
  rep = ok(A.exec({ cmd: 'fit_report', args: { block: 'b1' } }))[0];
  assert.ok(rep.warnings.some((w) => /stem v1: ratio 1\.667 — stem duidelijk hoorbaar versneld/.test(w)));
  assert.equal(ok(A.exec({ cmd: 'render_block', args: { block: 'b1' } })).samples, 9 * SR);
  // helpende fouten
  assert.match(A.exec({ cmd: 'add_voice', args: { block: 'b1', source: 'v.wav', in: 0, out: 1, anchor: 'f7' } }).error,
    /anker "f7" is geen muziekfragment van blok "B" \(b1\); kies uit: f2, f1/);
  assert.match(A.exec({ cmd: 'update_voice', args: { voice: 'v5', offset: 1 } }).error, /stem "v5" bestaat niet; bestaande stemfragmenten: v1 \(B\)/);
  assert.match(A.exec({ cmd: 'add_voice', args: { block: 'b1', source: 'v.wav', in: 0, out: 1 } }).error, /argument 'anchor' ontbreekt/);
  ok(A.exec({ cmd: 'delete_voice', args: { voice: 'v1' } }));
  assert.deepEqual(ok(A.exec({ cmd: 'get_state' })).blocks[0].voice, []);
});

test('stem buiten het blok: fout met het aantal seconden te lang, geen render', () => {
  const A = voiceSetup();
  const r = ok(A.exec({ cmd: 'add_voice', args: { block: 'b1', source: 'v.wav', in: 0, out: 3, anchor: 'f2', offset: 1.5, snap: false } }));
  assert.ok(Math.abs(r.end - 6.5) < 1e-9);
  assert.match(r.warnings[0], /stem v1 is 2\.5 s te lang/);
  const rep = ok(A.exec({ cmd: 'fit_report', args: { block: 'b1' } }))[0];
  assert.equal(rep.exportable, false);
  assert.ok(rep.warnings.some((w) => /stem v1 is 2\.5 s te lang voor blok "V" \(b1\): eindigt op 6\.5 s, het blok duurt 4\.0 s/.test(w)));
  const rr = A.exec({ cmd: 'render_block', args: { block: 'b1' } });
  assert.equal(rr.ok, false);
  assert.match(rr.error, /2\.5 s te lang/);
  assert.match(A.exec({ cmd: 'export_block', args: { block: 'b1' } }).error, /2\.5 s te lang/);
  // vóór het begin van het blok
  ok(A.exec({ cmd: 'update_voice', args: { voice: 'v1', anchor: 'f1', offset: -0.25, out: 1 } }));
  assert.match(A.exec({ cmd: 'render_block', args: { block: 'b1' } }).error, /stem v1 begint 0\.25 s vóór het begin van blok "V" \(b1\).*offset moet minstens 0\.0 zijn/);
  ok(A.exec({ cmd: 'update_voice', args: { voice: 'v1', offset: 0 } }));
  assert.equal(ok(A.exec({ cmd: 'render_block', args: { block: 'b1' } })).samples, 4 * SR);
});

test('overlappende stemmen: waarschuwing, render geblokkeerd tot acceptOverlap, daarna opgeteld', () => {
  const A = voiceSetup({ music: 0 });
  ok(A.exec({ cmd: 'update_block', args: { block: 'b1', duckDb: 0 } }));
  ok(A.exec({ cmd: 'add_voice', args: { block: 'b1', source: 'v.wav', in: 0, out: 1, anchor: 'f1', offset: 0.5, snap: false, fadeInMs: 0, fadeOutMs: 0 } }));
  ok(A.exec({ cmd: 'add_voice', args: { block: 'b1', source: 'v.wav', in: 0, out: 1, anchor: 'f1', offset: 1.25, snap: false, fadeInMs: 0, fadeOutMs: 0 } }));
  let rep = ok(A.exec({ cmd: 'fit_report', args: { block: 'b1' } }))[0];
  assert.equal(rep.exportable, true);
  assert.equal(rep.overlaps.length, 1);
  assert.equal(rep.overlaps[0].a, 'v1'); assert.equal(rep.overlaps[0].b, 'v2');
  assert.ok(Math.abs(rep.overlaps[0].duration - 0.25) < 1e-9);
  assert.equal(rep.overlaps[0].accepted, false);
  assert.ok(rep.warnings.some((w) => /stemfragmenten v1 en v2 overlappen 0\.25 s/.test(w)));
  const rr = A.exec({ cmd: 'render_block', args: { block: 'b1' } });
  assert.equal(rr.ok, false);
  assert.match(rr.error, /acceptOverlap/);
  assert.match(A.exec({ cmd: 'export_block', args: { block: 'b1' } }).error, /acceptOverlap/);
  ok(A.exec({ cmd: 'update_voice', args: { voice: 'v2', acceptOverlap: true } }));
  rep = ok(A.exec({ cmd: 'fit_report', args: { block: 'b1' } }))[0];
  assert.equal(rep.overlaps[0].accepted, true);
  ok(A.exec({ cmd: 'render_block', args: { block: 'b1' } }));
  const y = A.getRendered('b1').channels[0];
  assert.ok(Math.abs(y[Math.round(0.8 * SR)] - 0.2) < 1e-6, 'alleen v1');
  assert.ok(Math.abs(y[Math.round(1.375 * SR)] - 0.4) < 1e-6, 'v1 + v2 opgeteld');
  assert.ok(Math.abs(y[Math.round(2.0 * SR)] - 0.2) < 1e-6, 'alleen v2');
  // overlap wordt groter → acceptatie blijft, waarschuwing toont nieuwe duur
  ok(A.exec({ cmd: 'update_voice', args: { voice: 'v2', offset: 1.0 } }));
  rep = ok(A.exec({ cmd: 'fit_report', args: { block: 'b1' } }))[0];
  assert.ok(Math.abs(rep.overlaps[0].duration - 0.5) < 1e-9 && rep.overlaps[0].accepted);
  assert.ok(rep.warnings.some((w) => /overlappen 0\.5 s \(geaccepteerd/.test(w)));
});

test('ducking: niveau midden in de zone ≈ 10^(duckDb/20), cosinushellingen, zones samengevoegd', () => {
  const A = voiceSetup();
  ok(A.exec({ cmd: 'update_block', args: { block: 'b1', duckDb: -12, duckMs: 100 } }));
  ok(A.exec({ cmd: 'add_voice', args: { block: 'b1', source: 'nul.wav', in: 0, out: 0.5, anchor: 'f1', offset: 0.5, snap: false } }));
  ok(A.exec({ cmd: 'add_voice', args: { block: 'b1', source: 'nul.wav', in: 0, out: 0.5, anchor: 'f1', offset: 1.1, snap: false } }));
  ok(A.exec({ cmd: 'add_voice', args: { block: 'b1', source: 'nul.wav', in: 0, out: 0.5, anchor: 'f2', offset: 1.0, snap: false } }));
  const rep = ok(A.exec({ cmd: 'fit_report', args: { block: 'b1' } }))[0];
  assert.equal(rep.duck.length, 2, JSON.stringify(rep.duck));
  assert.ok(Math.abs(rep.duck[0].start - 0.4) < 1e-9 && Math.abs(rep.duck[0].end - 1.7) < 1e-9, 'v1+v2 samengevoegd');
  assert.ok(Math.abs(rep.duck[1].start - 2.9) < 1e-9 && Math.abs(rep.duck[1].end - 3.6) < 1e-9);
  ok(A.exec({ cmd: 'render_block', args: { block: 'b1' } }));
  const y = A.getRendered('b1').channels[0], lvl = 10 ** (-12 / 20);
  assert.ok(Math.abs(y[Math.round(0.75 * SR)] / 0.5 - lvl) < 1e-5, 'midden zone 1: ' + y[Math.round(0.75 * SR)] / 0.5);
  assert.ok(Math.abs(y[Math.round(1.05 * SR)] / 0.5 - lvl) < 1e-5, 'tussen v1 en v2 blijft geduckt');
  assert.ok(Math.abs(y[Math.round(3.25 * SR)] / 0.5 - lvl) < 1e-5, 'midden zone 2');
  assert.ok(Math.abs(y[Math.round(0.45 * SR)] / 0.5 - (1 - (1 - lvl) * 0.5)) < 1e-4, 'halverwege de cosinushelling');
  assert.ok(Math.abs(y[Math.round(0.3 * SR)] - 0.5) < 1e-6 && Math.abs(y[Math.round(2.5 * SR)] - 0.5) < 1e-6, 'buiten de zones onveranderd');
  // duckDb 0 = uit; stem bij blokbegin: zone begrensd op 0
  ok(A.exec({ cmd: 'update_voice', args: { voice: 'v1', offset: 0 } }));
  assert.equal(ok(A.exec({ cmd: 'fit_report', args: { block: 'b1' } }))[0].duck[0].start, 0);
  ok(A.exec({ cmd: 'update_block', args: { block: 'b1', duckDb: 0 } }));
  assert.deepEqual(ok(A.exec({ cmd: 'fit_report', args: { block: 'b1' } }))[0].duck, []);
  ok(A.exec({ cmd: 'render_block', args: { block: 'b1' } }));
  assert.ok(Math.abs(A.getRendered('b1').channels[0][Math.round(0.75 * SR)] - 0.5) < 1e-6);
  assert.match(A.exec({ cmd: 'update_block', args: { block: 'b1', duckDb: 3 } }).error, /'duckDb' mag hoogstens 0 zijn/);
});

test('anker verwijderen of naar ander blok verplaatsen is geblokkeerd met de stem-ids', () => {
  const A = voiceSetup();
  ok(A.exec({ cmd: 'add_voice', args: { block: 'b1', source: 'v.wav', in: 0, out: 1, anchor: 'f2', snap: false } }));
  ok(A.exec({ cmd: 'add_voice', args: { block: 'b1', source: 'v.wav', in: 1, out: 1.5, anchor: 'f2', offset: 1.2, snap: false } }));
  const r = A.exec({ cmd: 'delete_fragment', args: { fragment: 'f2' } });
  assert.equal(r.ok, false);
  assert.match(r.error, /fragment f2 is het anker van stem v1, v2; verwijder eerst/);
  ok(A.exec({ cmd: 'create_block', args: { name: 'Ander', targetDuration: 3, fitMode: 'proportional' } }));
  assert.match(A.exec({ cmd: 'move_fragment', args: { fragment: 'f2', position: 0, block: 'b2' } }).error, /anker van stem v1, v2/);
  ok(A.exec({ cmd: 'delete_fragment', args: { fragment: 'f1' } }), 'geen anker: mag');
  ok(A.exec({ cmd: 'update_voice', args: { voice: 'v2', anchor: 'f2' } }));
  ok(A.exec({ cmd: 'delete_voice', args: { voice: 'v1' } }));
  ok(A.exec({ cmd: 'delete_voice', args: { voice: 'v2' } }));
  ok(A.exec({ cmd: 'delete_fragment', args: { fragment: 'f2' } }));
});

test('move_fragment, duplicate_fragment en set_transition', () => {
  const A = fresh();
  A.attachAudio('s.wav', { sampleRate: SR, channels: [sine(300, 30)] });
  ok(A.execAll([
    { cmd: 'create_block', args: { name: 'A', targetDuration: 12, fitMode: 'proportional' } },
    { cmd: 'add_fragment', args: { block: 'b1', source: 's.wav', in: 0, out: 4, snap: false } },
    { cmd: 'add_fragment', args: { block: 'b1', source: 's.wav', in: 5, out: 9, snap: false } },
    { cmd: 'add_fragment', args: { block: 'b1', source: 's.wav', in: 10, out: 14, snap: false } },
    { cmd: 'create_block', args: { name: 'B', targetDuration: 5, fitMode: 'proportional' } }
  ]));
  assert.equal(ok(A.exec({ cmd: 'duplicate_fragment', args: { fragment: 'f2' } })).id, 'f4');
  let st = ok(A.exec({ cmd: 'get_state' }));
  assert.deepEqual(st.blocks[0].fragments.map((f) => f.id), ['f1', 'f2', 'f4', 'f3']);
  assert.equal(st.blocks[0].fragments[2].in, 5);
  assert.equal(st.blocks[0].transitions.length, 3);
  ok(A.exec({ cmd: 'move_fragment', args: { fragment: 'f3', position: 0 } }));
  ok(A.exec({ cmd: 'move_fragment', args: { fragment: 'f1', position: 0, block: 'b2' } }));
  st = ok(A.exec({ cmd: 'get_state' }));
  assert.deepEqual(st.blocks[0].fragments.map((f) => f.id), ['f3', 'f2', 'f4']);
  assert.equal(st.blocks[0].transitions.length, 2);
  assert.deepEqual(st.blocks[1].fragments.map((f) => f.id), ['f1']);
  assert.equal(st.blocks[1].transitions.length, 0);
  assert.match(A.exec({ cmd: 'move_fragment', args: { fragment: 'f2', position: 3 } }).error, /kies 0 t\/m 2/);
  for (const [type, extra] of [['cut', {}], ['gap', { ms: 300 }], ['crossfade', { ms: 400, curve: 'linear' }], ['fade_out_a', { ms: 300 }],
    ['fade_in_b', { ms: 300 }], ['duck', { ms: 600, duckDb: -18 }], ['mix', { ms: 200 }]]) {
    const t = ok(A.exec({ cmd: 'set_transition', args: { block: 'b1', index: 1, type, ...extra } }));
    assert.equal(t.type, type);
    const rep = ok(A.exec({ cmd: 'fit_report', args: { block: 'b1' } }))[0];
    assert.equal(rep.transitions[1].type, type);
    assert.equal(ok(A.exec({ cmd: 'render_block', args: { block: 'b1' } })).samples, 12 * SR, type);
  }
  assert.match(A.exec({ cmd: 'set_transition', args: { block: 'b1', index: 2, type: 'cut' } }).error, /overgangen 0 t\/m 1/);
  assert.match(A.exec({ cmd: 'set_transition', args: { block: 'b1', index: 0, type: 'gap', curve: 'linear' } }).error, /curve geldt alleen/);
});

test('renderBlockPure is sample-identiek aan render_block; voortgang oplopend tot 1', () => {
  const A = fresh();
  const n = 4 * SR, L = new Float32Array(n), R = new Float32Array(n);
  for (let i = 0; i < n; i++) { L[i] = 0.3 * Math.sin(i * 0.031) + 0.1 * Math.sin(i * 0.0071); R[i] = 0.3 * Math.sin(i * 0.027); }
  A.attachAudio('m.wav', { sampleRate: SR, channels: [L, R] });
  A.attachAudio('v.wav', { sampleRate: SR, channels: [sine(180, 3, SR, 0.4)] });
  ok(A.execAll([
    { cmd: 'create_block', args: { name: 'P', targetDuration: 5, fitMode: 'proportional' } },
    { cmd: 'add_fragment', args: { block: 'b1', source: 'm.wav', in: 0, out: 3, snap: false } },
    { cmd: 'add_fragment', args: { block: 'b1', source: 'm.wav', in: 1, out: 3.5, snap: false } },
    { cmd: 'add_voice', args: { block: 'b1', source: 'v.wav', in: 0.2, out: 2.2, anchor: 'f1', offset: 1, targetDuration: 1.8, snap: false } },
    { cmd: 'add_voice', args: { block: 'b1', source: 'v.wav', in: 0, out: 0.8, anchor: 'f2', offset: 0.3, snap: false } }
  ]));
  ok(A.exec({ cmd: 'render_block', args: { block: 'b1' } }));
  const main = A.getRendered('b1');
  const recipe = ok(A.exec({ cmd: 'get_state' }));
  const audio = { 'm.wav': A.getAudio('m.wav'), 'v.wav': A.getAudio('v.wav') };
  const calls = [];
  const cache = new Map();
  const pure = AF.renderBlockPure(recipe, audio, 'b1', { onProgress: (f, label) => calls.push([f, label]), cache });
  assert.equal(pure.sampleRate, SR);
  assert.deepEqual(Object.keys(pure), ['sampleRate', 'channels', 'stats', 'warnings']);
  assert.deepEqual(Object.keys(pure.stats), ['duration', 'samples', 'peakDb', 'clipped']);
  assert.equal(pure.stats.samples, 5 * SR);
  assert.equal(pure.channels.length, main.channels.length);
  for (let c = 0; c < main.channels.length; c++) {
    assert.equal(pure.channels[c].length, main.channels[c].length);
    assert.ok(pure.channels[c].every((x, i) => x === main.channels[c][i]), 'kanaal ' + c + ' sample-identiek');
  }
  assert.ok(calls.length > 10, 'aantal voortgangsmeldingen ' + calls.length);
  for (let i = 1; i < calls.length; i++) assert.ok(calls[i][0] >= calls[i - 1][0], 'oplopend bij ' + i);
  assert.equal(calls[calls.length - 1][0], 1);
  assert.ok(calls.every(([f]) => f >= 0 && f <= 1));
  const labels = new Set(calls.map((c) => c[1]));
  for (const l of ['muziek 1/2 rekken', 'muziek 2/2 rekken', 'stem 1/2 rekken', 'stem 2/2 rekken', 'mixen']) assert.ok(labels.has(l), l);
  // tweede aanroep met dezelfde cache: identiek en sneller (geen WSOLA)
  assert.ok(cache.size >= 3);
  const t = performance.now();
  const again = AF.renderBlockPure(recipe, audio, 'b1', { cache });
  assert.ok(performance.now() - t < 500);
  assert.ok(again.channels[0].every((x, i) => x === pure.channels[0][i]));
  // zelfde Nederlandse fouttekst als render_block
  ok(A.exec({ cmd: 'update_voice', args: { voice: 'v2', anchor: 'f1', offset: 1.5 } }));
  const err = A.exec({ cmd: 'render_block', args: { block: 'b1' } }).error;
  assert.match(err, /acceptOverlap/);
  assert.throws(() => AF.renderBlockPure(ok(A.exec({ cmd: 'get_state' })), audio, 'b1'), (e) => e instanceof Error && e.message === err);
  assert.throws(() => AF.renderBlockPure(recipe, {}, 'b1'), /ontbrekende bron\(nen\) m\.wav, v\.wav/);
  assert.throws(() => AF.renderBlockPure(recipe, audio, 'b9'), /blok "b9" bestaat niet; bestaande blokken: b1 \(P\)/);
});

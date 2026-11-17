var AudioFit = (function () {
  'use strict';

  /* =====================================================================
   * Audio Fit Tool — kern (DOM-vrij)
   * Ontwerp: docs/ontwerpplan-audio-fit-tool.md (v0.5), contract: docs/CONTRACT.md
   * (incl. "Aanvulling fase 2": stemspoor, ducking, renderBlockPure).
   * Geen DOM- of browser-API's; geen modules of afhankelijkheden.
   * ===================================================================== */

  var VERSION = '0.3.0';

  /* ---------------------------------------------------------------------
   * CONSTANTEN (§6.5, §6.6, §7) — richtwaarden (HYPOTHESE) bij elkaar,
   * zodat ze na een luistertest zijn bij te stellen.
   * ------------------------------------------------------------------- */
  var CONSTANTS = {
    RECIPE_VERSION: 2,
    RECIPE_VERSIONS_ACCEPTED: [1, 2],   // version 1 krijgt voice: [], duckDb −6, duckMs 150
    DEFAULT_SAMPLE_RATE: 48000,
    DEFAULT_BLOCK_DURATION: 15.0,
    DEFAULT_FADE_MS: 8,                 // §6.4 fades per fragment, cosinusvorm
    FADE_MAX_MS: 5000,                  // §6.4 instelbaar 0–5000 ms
    DEFAULT_TRANSITION: { type: 'crossfade', ms: 500, curve: 'equal_power' },
    DEFAULT_GAP_MS: 500,
    MIN_FRAGMENT_MS: 20,                // §10 selectie korter dan 20 ms geweigerd
    SNAP_WINDOW_MS: 5,                  // §6.4 snap naar nuldoorgang ±5 ms
    UNDO_MAX: 50,                       // §5 Store
    TIME_DECIMALS: 6,                   // opslagprecisie tijden (< 1 sample bij 48 kHz)

    // §6.5 ratio-indicatie (modus pitch)
    RATIO_BANDS: {
      green: [0.90, 1.10],
      orange: [0.75, 1.33],
      red: [0.50, 2.00]
    },
    FIT_MODES: ['proportional', 'manual'],
    // create_block zonder fitMode → 'manual' (manualDuration null = originele lengte, niets gerekt).
    // import_recipe zonder fitMode blijft 'proportional', zodat bestaande recepten niet veranderen.
    DEFAULT_FIT_MODE_NEW_BLOCK: 'manual',
    FIT_MAX_ITER: 50,

    // §6.6 overgangen
    TRANSITION_TYPES: ['cut', 'gap', 'crossfade', 'fade_out_a', 'fade_in_b', 'duck', 'mix'],
    OVERLAP_TYPES: ['crossfade', 'fade_out_a', 'fade_in_b', 'duck', 'mix'],
    CURVES: ['equal_power', 'linear'],
    MICROFADE_MS: 8,
    DUCK_DEFAULT_DB: -12,
    DUCK_RAMP_MS: 150,
    MAX_OVERLAP_FRACTION: 0.9,

    // §6.10 stemspoor en ducking per blok
    VOICE_DEFAULT_PRESET: 'voice',
    VOICE_DUCK_DEFAULT_DB: -6,          // 0 = ducking uit
    VOICE_DUCK_DEFAULT_MS: 150,
    VOICE_DUCK_MAX_MS: 5000,

    // §7.1 WSOLA
    MODES: ['pitch', 'varispeed'],
    PRESETS: ['music', 'voice'],
    WSOLA_PRESETS: {
      music: { N: 2048, Hs: 1024, D: 512, corrStep: 2 },
      voice: { N: 1024, Hs: 512, D: 512, corrStep: 2 }
    },
    WSOLA_REF_RATE_MIN: 44100,          // presetwaarden gelden bij 44,1/48 kHz
    WSOLA_REF_RATE_MAX: 48000,
    WSOLA_IDENTITY_TOL: 0.0005,         // stap 7
    WSOLA_PROGRESS_FRAMES: 20,

    // §7.2 Resampler
    RESAMPLER_A: 16,

    // §6.8 / §7.4 export
    WAV_HEADER_BYTES: 44,
    NORMALIZE_PEAK_DB: -1,

    RENDER_CACHE_MAX: 64,
    LARGE_FILE_WARN_S: 1200
  };

  /* ---------------------------------------------------------------------
   * Hulpfuncties
   * ------------------------------------------------------------------- */
  function userError(msg) { var e = new Error(msg); e.afUser = true; return e; }
  function fail(msg) { throw userError(msg); }
  function clone(o) { return o === undefined ? undefined : JSON.parse(JSON.stringify(o)); }
  function roundT(x) { var f = Math.pow(10, CONSTANTS.TIME_DECIMALS); return Math.round(x * f) / f; }
  function fmt(x) {
    if (typeof x !== 'number' || !isFinite(x)) return String(x);
    var s = String(Math.round(x * 1000) / 1000);
    if (s.indexOf('.') < 0 && s.indexOf('e') < 0) s += '.0';
    return s;
  }
  function fmtMs(sec) { return String(Math.round(sec * 10000) / 10); }
  function q(s) { return '"' + s + '"'; }
  function has(o, k) { return o != null && Object.prototype.hasOwnProperty.call(o, k); }
  function isOverlapType(t) { return CONSTANTS.OVERLAP_TYPES.indexOf(t) >= 0; }
  function dbToLin(db) { return Math.pow(10, db / 20); }
  function linToDb(x) { return x > 0 ? Math.round(2000 * Math.log(x) / Math.LN10) / 100 : null; }
  function msg(e) { return (e && e.message) ? e.message : String(e); }

  /* =====================================================================
   * Validator — JSON-schema-subset en receptcontrole
   * ===================================================================== */
  var Validator = (function () {
    function typeOf(v) {
      if (v === null) return 'null';
      if (Array.isArray(v)) return 'array';
      if (typeof v === 'number') return isFinite(v) ? (Number.isInteger(v) ? 'integer' : 'number') : 'nan';
      return typeof v;
    }
    var TYPE_NL = { number: 'een getal', integer: 'een geheel getal', string: 'tekst', boolean: 'true of false',
      object: 'een object', array: 'een lijst', 'null': 'null' };
    function typeMatches(v, t) {
      var a = typeOf(v);
      if (t === 'number') return a === 'number' || a === 'integer';
      return a === t;
    }
    function show(v) { try { var s = JSON.stringify(v); return s && s.length > 60 ? s.slice(0, 57) + '...' : s; } catch (e) { return String(v); } }

    // Controleert value tegen schema; gooit een leesbare fout.
    function check(value, schema, path, ctxName) {
      var label = path ? "'" + path + "'" : 'argumenten';
      if (schema.type) {
        var types = Array.isArray(schema.type) ? schema.type : [schema.type];
        var okType = false;
        for (var i = 0; i < types.length; i++) if (typeMatches(value, types[i])) { okType = true; break; }
        if (!okType) {
          fail(label + (ctxName ? ' van ' + ctxName : '') + ' moet ' +
            types.map(function (t) { return TYPE_NL[t] || t; }).join(' of ') +
            (schema.description ? ' (' + schema.description + ')' : '') + ' zijn; kreeg ' + show(value));
        }
      }
      if (schema['enum'] && schema['enum'].indexOf(value) < 0) {
        fail(label + ' moet een van ' + schema['enum'].join(', ') + ' zijn; kreeg ' + show(value));
      }
      if (typeof value === 'number') {
        if (has(schema, 'minimum') && value < schema.minimum) fail(label + ' moet minstens ' + schema.minimum + ' zijn; kreeg ' + value);
        if (has(schema, 'maximum') && value > schema.maximum) fail(label + ' mag hoogstens ' + schema.maximum + ' zijn; kreeg ' + value);
        if (has(schema, 'exclusiveMinimum') && value <= schema.exclusiveMinimum) fail(label + ' moet groter dan ' + schema.exclusiveMinimum + ' zijn; kreeg ' + value);
      }
      if (typeof value === 'string') {
        if (has(schema, 'minLength') && value.length < schema.minLength) fail(label + ' mag niet leeg zijn');
        if (schema.pattern && !(new RegExp(schema.pattern)).test(value)) fail(label + ' heeft een ongeldige vorm (' + (schema.description || schema.pattern) + '); kreeg ' + show(value));
      }
      if (typeOf(value) === 'object' && schema.properties) {
        var props = schema.properties, req = schema.required || [];
        for (var r = 0; r < req.length; r++) {
          if (!has(value, req[r]) || value[req[r]] === undefined) {
            fail("argument '" + req[r] + "' ontbreekt" + (ctxName ? ' voor ' + ctxName : '') + ' (verplicht: ' + req.join(', ') + ')');
          }
        }
        for (var k in value) {
          if (!has(value, k) || value[k] === undefined) continue;
          if (!has(props, k)) {
            if (schema.additionalProperties === false) {
              fail("onbekend argument '" + k + "'" + (ctxName ? ' voor ' + ctxName : '') + '; toegestaan: ' + (Object.keys(props).join(', ') || '(geen)'));
            }
            continue;
          }
          check(value[k], props[k], (path ? path + '.' : '') + k, ctxName);
        }
      }
      if (Array.isArray(value) && schema.items) {
        for (var j = 0; j < value.length; j++) check(value[j], schema.items, (path || '') + '[' + j + ']', ctxName);
      }
    }

    function defaultTransition(defaults, type) {
      var d = (defaults && defaults.transition) || CONSTANTS.DEFAULT_TRANSITION;
      var t = { type: type || d.type };
      t.ms = (t.type === d.type && typeof d.ms === 'number') ? d.ms : (t.type === 'cut' ? 0 : (t.type === 'gap' ? CONSTANTS.DEFAULT_GAP_MS : CONSTANTS.DEFAULT_TRANSITION.ms));
      if (t.type === 'cut') t.ms = 0;
      if (t.type !== 'cut' && t.type !== 'gap') t.curve = (d.curve && CONSTANTS.CURVES.indexOf(d.curve) >= 0) ? d.curve : 'equal_power';
      if (t.type === 'duck') t.duckDb = CONSTANTS.DUCK_DEFAULT_DB;
      return t;
    }

    function newRecipe(sr) {
      return {
        version: CONSTANTS.RECIPE_VERSION,
        sampleRate: sr || CONSTANTS.DEFAULT_SAMPLE_RATE,
        defaults: {
          blockDuration: CONSTANTS.DEFAULT_BLOCK_DURATION,
          fadeMs: CONSTANTS.DEFAULT_FADE_MS,
          transition: clone(CONSTANTS.DEFAULT_TRANSITION)
        },
        sources: [],
        blocks: []
      };
    }

    // Normaliseert een (geïmporteerd) recept: vult standaardwaarden aan en
    // verzamelt problemen. Retourneert { recipe, problems: [string] }.
    function normalizeRecipe(input) {
      var problems = [];
      function p(m) { problems.push(m); }
      if (typeof input === 'string') {
        try { input = JSON.parse(input); } catch (e) { return { recipe: null, problems: ['recept is geen geldige JSON: ' + msg(e)] }; }
      }
      if (typeOf(input) !== 'object') return { recipe: null, problems: ['recept moet een JSON-object zijn'] };
      var r = newRecipe();
      if (has(input, 'version') && CONSTANTS.RECIPE_VERSIONS_ACCEPTED.indexOf(input.version) < 0) p('version ' + show(input.version) + ' wordt niet ondersteund; verwacht ' + CONSTANTS.RECIPE_VERSIONS_ACCEPTED.join(' of '));
      if (has(input, 'sampleRate')) {
        if (typeOf(input.sampleRate) !== 'integer' || input.sampleRate < 8000 || input.sampleRate > 384000) p('sampleRate moet een geheel getal tussen 8000 en 384000 zijn; kreeg ' + show(input.sampleRate));
        else r.sampleRate = input.sampleRate;
      }
      var d = input.defaults;
      if (d !== undefined) {
        if (typeOf(d) !== 'object') p('defaults moet een object zijn');
        else {
          if (has(d, 'blockDuration')) { if (typeof d.blockDuration === 'number' && d.blockDuration > 0) r.defaults.blockDuration = d.blockDuration; else p('defaults.blockDuration moet een getal > 0 zijn'); }
          if (has(d, 'fadeMs')) { if (typeof d.fadeMs === 'number' && d.fadeMs >= 0 && d.fadeMs <= CONSTANTS.FADE_MAX_MS) r.defaults.fadeMs = d.fadeMs; else p('defaults.fadeMs moet tussen 0 en ' + CONSTANTS.FADE_MAX_MS + ' liggen'); }
          if (has(d, 'transition')) {
            var tr = normTransition(d.transition, 'defaults.transition', r.defaults, p);
            if (tr) r.defaults.transition = tr;
          }
        }
      }
      var srcNames = {};
      if (has(input, 'sources')) {
        if (!Array.isArray(input.sources)) p('sources moet een lijst zijn');
        else input.sources.forEach(function (s, i) {
          var where = 'sources[' + i + ']';
          if (typeOf(s) !== 'object') { p(where + ' moet een object zijn'); return; }
          if (typeof s.name !== 'string' || !s.name) { p(where + '.name ontbreekt'); return; }
          if (srcNames[s.name]) { p('bron ' + q(s.name) + ' komt twee keer voor; bronnamen moeten uniek zijn'); return; }
          var src = { name: s.name, duration: 0, channels: 2, markers: [] };
          if (typeof s.duration === 'number' && s.duration > 0) src.duration = s.duration; else p(where + ' (' + s.name + '): duration moet een getal > 0 zijn');
          if (has(s, 'channels')) { if (typeOf(s.channels) === 'integer' && s.channels >= 1) src.channels = s.channels; else p(where + ' (' + s.name + '): channels moet een geheel getal ≥ 1 zijn'); }
          if (has(s, 'markers')) {
            if (!Array.isArray(s.markers)) p(where + '.markers moet een lijst zijn');
            else {
              var mn = {};
              s.markers.forEach(function (m, j) {
                if (typeOf(m) !== 'object' || typeof m.name !== 'string' || !m.name || typeof m.time !== 'number') { p(s.name + ': marker ' + j + ' moet {name, time} zijn'); return; }
                if (mn[m.name]) { p(s.name + ': markernaam ' + q(m.name) + ' komt twee keer voor'); return; }
                mn[m.name] = true;
                if (m.time < 0 || (src.duration && m.time > src.duration + 1e-6)) p(s.name + ': marker ' + q(m.name) + ' (' + fmt(m.time) + ' s) ligt buiten de bron (0–' + fmt(src.duration) + ' s)');
                src.markers.push({ name: m.name, time: m.time });
              });
              src.markers.sort(function (a, b) { return a.time - b.time; });
            }
          }
          srcNames[s.name] = src;
          r.sources.push(src);
        });
      }
      var blockIds = {}, fragIds = {}, voiceIds = {};
      if (has(input, 'blocks')) {
        if (!Array.isArray(input.blocks)) p('blocks moet een lijst zijn');
        else input.blocks.forEach(function (b, i) {
          var where = 'blocks[' + i + ']';
          if (typeOf(b) !== 'object') { p(where + ' moet een object zijn'); return; }
          var blk = { id: b.id, name: b.name, targetDuration: b.targetDuration, fitMode: b.fitMode || 'proportional',
            duckDb: CONSTANTS.VOICE_DUCK_DEFAULT_DB, duckMs: CONSTANTS.VOICE_DUCK_DEFAULT_MS, fragments: [], transitions: [], voice: [] };
          if (typeof blk.id !== 'string' || !blk.id) { p(where + ': id ontbreekt'); return; }
          if (blockIds[blk.id]) { p('blok-id ' + q(blk.id) + ' komt twee keer voor'); return; }
          blockIds[blk.id] = true;
          where = 'blok ' + blk.id;
          if (typeof blk.name !== 'string') blk.name = blk.id;
          if (!(typeof blk.targetDuration === 'number' && blk.targetDuration > 0)) { p(where + ': targetDuration moet een getal > 0 zijn'); blk.targetDuration = r.defaults.blockDuration; }
          if (CONSTANTS.FIT_MODES.indexOf(blk.fitMode) < 0) { p(where + ': fitMode moet ' + CONSTANTS.FIT_MODES.join(' of ') + ' zijn'); blk.fitMode = 'proportional'; }
          if (has(b, 'duckDb') && b.duckDb != null) { if (typeof b.duckDb === 'number' && isFinite(b.duckDb) && b.duckDb <= 0) blk.duckDb = b.duckDb; else p(where + ': duckDb moet een getal ≤ 0 zijn (0 = ducking uit)'); }
          if (has(b, 'duckMs') && b.duckMs != null) { if (typeof b.duckMs === 'number' && b.duckMs >= 0 && b.duckMs <= CONSTANTS.VOICE_DUCK_MAX_MS) blk.duckMs = b.duckMs; else p(where + ': duckMs moet tussen 0 en ' + CONSTANTS.VOICE_DUCK_MAX_MS + ' ms liggen'); }
          var frs = Array.isArray(b.fragments) ? b.fragments : (has(b, 'fragments') ? (p(where + ': fragments moet een lijst zijn'), []) : []);
          frs.forEach(function (f, j) {
            var fw = where + ' fragment ' + j;
            if (typeOf(f) !== 'object') { p(fw + ' moet een object zijn'); return; }
            if (typeof f.id !== 'string' || !f.id) { p(fw + ': id ontbreekt'); return; }
            if (fragIds[f.id]) { p('fragment-id ' + q(f.id) + ' komt twee keer voor'); return; }
            fragIds[f.id] = true;
            fw = 'fragment ' + f.id;
            var fr = {
              id: f.id, source: f.source, 'in': f['in'], out: f.out,
              mode: has(f, 'mode') ? f.mode : 'pitch', preset: has(f, 'preset') ? f.preset : 'music',
              fadeInMs: has(f, 'fadeInMs') ? f.fadeInMs : r.defaults.fadeMs,
              fadeOutMs: has(f, 'fadeOutMs') ? f.fadeOutMs : r.defaults.fadeMs,
              gainDb: has(f, 'gainDb') ? f.gainDb : 0,
              locked: has(f, 'locked') ? f.locked : false,
              manualDuration: has(f, 'manualDuration') ? f.manualDuration : null
            };
            if (typeof fr.source !== 'string' || !srcNames[fr.source]) p(fw + ' verwijst naar onbekende bron ' + q(fr.source) + '; bronnen in het recept: ' + (Object.keys(srcNames).join(', ') || '(geen)'));
            if (typeof fr['in'] !== 'number' || typeof fr.out !== 'number') p(fw + ': in en out moeten getallen (seconden) zijn');
            else {
              if (fr['in'] < 0) p(fw + ': in ' + fmt(fr['in']) + ' ligt vóór het begin van de bron');
              if (fr.out <= fr['in']) p(fw + ': out (' + fmt(fr.out) + ') moet na in (' + fmt(fr['in']) + ') liggen');
              var sd = srcNames[fr.source];
              if (sd && sd.duration && fr.out > sd.duration + 1e-6) p(fw + ': out ' + fmt(fr.out) + ' ligt voorbij het einde van ' + sd.name + ' (' + fmt(sd.duration) + ' s)');
            }
            if (CONSTANTS.MODES.indexOf(fr.mode) < 0) p(fw + ': mode moet ' + CONSTANTS.MODES.join(' of ') + ' zijn');
            if (CONSTANTS.PRESETS.indexOf(fr.preset) < 0) p(fw + ': preset moet ' + CONSTANTS.PRESETS.join(' of ') + ' zijn');
            ['fadeInMs', 'fadeOutMs'].forEach(function (k) {
              if (!(typeof fr[k] === 'number' && fr[k] >= 0 && fr[k] <= CONSTANTS.FADE_MAX_MS)) p(fw + ': ' + k + ' moet tussen 0 en ' + CONSTANTS.FADE_MAX_MS + ' ms liggen');
            });
            if (typeof fr.gainDb !== 'number') p(fw + ': gainDb moet een getal zijn');
            if (typeof fr.locked !== 'boolean') p(fw + ': locked moet true of false zijn');
            if (!(fr.manualDuration === null || (typeof fr.manualDuration === 'number' && fr.manualDuration > 0))) p(fw + ': manualDuration moet null of een getal > 0 zijn');
            blk.fragments.push(fr);
          });
          var need = Math.max(0, blk.fragments.length - 1);
          if (!has(b, 'transitions') || b.transitions == null) {
            for (var t = 0; t < need; t++) blk.transitions.push(defaultTransition(r.defaults));
          } else if (!Array.isArray(b.transitions)) p(where + ': transitions moet een lijst zijn');
          else {
            if (b.transitions.length !== need) p(where + ': ' + blk.fragments.length + ' fragmenten vragen ' + need + ' overgangen; er staan er ' + b.transitions.length + ' in het recept');
            b.transitions.slice(0, need).forEach(function (t, j) {
              var nt = normTransition(t, where + ' overgang ' + j, r.defaults, p);
              blk.transitions.push(nt || defaultTransition(r.defaults));
            });
            while (blk.transitions.length < need) blk.transitions.push(defaultTransition(r.defaults));
          }
          // §6.10 stemspoor
          var vin = has(b, 'voice') && b.voice != null ? b.voice : [];
          if (!Array.isArray(vin)) p(where + ': voice moet een lijst zijn');
          else vin.forEach(function (v, j) {
            var vw = where + ' stem ' + j;
            if (typeOf(v) !== 'object') { p(vw + ' moet een object zijn'); return; }
            if (typeof v.id !== 'string' || !v.id) { p(vw + ': id ontbreekt'); return; }
            if (voiceIds[v.id]) { p('stem-id ' + q(v.id) + ' komt twee keer voor'); return; }
            voiceIds[v.id] = true;
            vw = 'stem ' + v.id;
            var vo = {
              id: v.id, source: v.source, 'in': v['in'], out: v.out, anchor: v.anchor,
              offset: has(v, 'offset') && v.offset != null ? v.offset : 0,
              targetDuration: has(v, 'targetDuration') ? v.targetDuration : null,
              mode: has(v, 'mode') ? v.mode : 'pitch', preset: has(v, 'preset') ? v.preset : CONSTANTS.VOICE_DEFAULT_PRESET,
              gainDb: has(v, 'gainDb') ? v.gainDb : 0,
              fadeInMs: has(v, 'fadeInMs') ? v.fadeInMs : r.defaults.fadeMs,
              fadeOutMs: has(v, 'fadeOutMs') ? v.fadeOutMs : r.defaults.fadeMs,
              acceptOverlap: has(v, 'acceptOverlap') ? v.acceptOverlap : false
            };
            if (typeof vo.source !== 'string' || !srcNames[vo.source]) p(vw + ' verwijst naar onbekende bron ' + q(vo.source) + '; bronnen in het recept: ' + (Object.keys(srcNames).join(', ') || '(geen)'));
            if (typeof vo['in'] !== 'number' || typeof vo.out !== 'number') p(vw + ': in en out moeten getallen (seconden) zijn');
            else {
              if (vo['in'] < 0) p(vw + ': in ' + fmt(vo['in']) + ' ligt vóór het begin van de bron');
              if (vo.out <= vo['in']) p(vw + ': out (' + fmt(vo.out) + ') moet na in (' + fmt(vo['in']) + ') liggen');
              var vsd = srcNames[vo.source];
              if (vsd && vsd.duration && vo.out > vsd.duration + 1e-6) p(vw + ': out ' + fmt(vo.out) + ' ligt voorbij het einde van ' + vsd.name + ' (' + fmt(vsd.duration) + ' s)');
            }
            var fids = blk.fragments.map(function (f) { return f.id; });
            if (fids.indexOf(vo.anchor) < 0) p(vw + ': anker ' + q(vo.anchor) + ' is geen muziekfragment van ' + where + '; ' + (fids.length ? 'kies uit: ' + fids.join(', ') : 'het blok heeft geen muziekfragmenten'));
            if (typeof vo.offset !== 'number' || !isFinite(vo.offset)) p(vw + ': offset moet een getal (seconden) zijn');
            if (!(vo.targetDuration === null || (typeof vo.targetDuration === 'number' && vo.targetDuration > 0))) p(vw + ': targetDuration moet null of een getal > 0 zijn');
            if (CONSTANTS.MODES.indexOf(vo.mode) < 0) p(vw + ': mode moet ' + CONSTANTS.MODES.join(' of ') + ' zijn');
            if (CONSTANTS.PRESETS.indexOf(vo.preset) < 0) p(vw + ': preset moet ' + CONSTANTS.PRESETS.join(' of ') + ' zijn');
            ['fadeInMs', 'fadeOutMs'].forEach(function (k) {
              if (!(typeof vo[k] === 'number' && vo[k] >= 0 && vo[k] <= CONSTANTS.FADE_MAX_MS)) p(vw + ': ' + k + ' moet tussen 0 en ' + CONSTANTS.FADE_MAX_MS + ' ms liggen');
            });
            if (typeof vo.gainDb !== 'number') p(vw + ': gainDb moet een getal zijn');
            if (typeof vo.acceptOverlap !== 'boolean') p(vw + ': acceptOverlap moet true of false zijn');
            blk.voice.push(vo);
          });
          r.blocks.push(blk);
        });
      }
      return { recipe: r, problems: problems };
    }

    function normTransition(t, where, defaults, p) {
      if (typeOf(t) !== 'object') { p(where + ' moet een object zijn'); return null; }
      if (CONSTANTS.TRANSITION_TYPES.indexOf(t.type) < 0) { p(where + ': type moet een van ' + CONSTANTS.TRANSITION_TYPES.join(', ') + ' zijn; kreeg ' + show(t.type)); return null; }
      var nt = defaultTransition(defaults, t.type);
      if (has(t, 'ms') && t.type !== 'cut') { if (typeof t.ms === 'number' && t.ms >= 0) nt.ms = t.ms; else p(where + ': ms moet een getal ≥ 0 zijn'); }
      if (has(t, 'curve') && nt.curve !== undefined) { if (CONSTANTS.CURVES.indexOf(t.curve) >= 0) nt.curve = t.curve; else p(where + ': curve moet ' + CONSTANTS.CURVES.join(' of ') + ' zijn'); }
      if (has(t, 'duckDb') && t.type === 'duck') { if (typeof t.duckDb === 'number' && t.duckDb <= 0) nt.duckDb = t.duckDb; else p(where + ': duckDb moet een getal ≤ 0 zijn'); }
      return nt;
    }

    return { check: check, normalizeRecipe: normalizeRecipe, newRecipe: newRecipe, defaultTransition: defaultTransition, typeOf: typeOf };
  })();

  /* =====================================================================
   * Resampler — windowed-sinc (§7.2)
   *   step = inLength/outLength, cutoff = min(1, 1/step), A = 16
   *   out[i] = Σ in[n]·cutoff·sinc(cutoff(t−n))·hann((t−n)/(A/cutoff)), t = i·step
   * sin/cos via rotatie-recurrentie (zelfde formule, sneller dan Math.sin per tap).
   * ===================================================================== */
  var Resampler = (function () {
    function resample(channels, outLength) {
      outLength = Math.max(0, Math.round(outLength));
      var C = channels.length, inLen = C ? channels[0].length : 0;
      var out = [], c;
      for (c = 0; c < C; c++) out.push(new Float32Array(outLength));
      if (!C || outLength === 0 || inLen === 0) return out;
      if (inLen === outLength) { for (c = 0; c < C; c++) out[c].set(channels[c]); return out; }
      var step = inLen / outLength;
      var cutoff = Math.min(1, 1 / step);
      var W = CONSTANTS.RESAMPLER_A / cutoff;          // halve vensterbreedte in invoersamples
      var maxTaps = Math.ceil(2 * W) + 3;
      var wts = new Float64Array(maxTaps);
      var PI = Math.PI;
      var b = PI * cutoff, cb = Math.cos(b), sb = Math.sin(b);       // stap sinc-hoek
      var h = PI / W, ch = Math.cos(h), sh = Math.sin(h);           // stap hann-hoek
      for (var i = 0; i < outLength; i++) {
        var t = i * step;
        var n0 = Math.ceil(t - W), n1 = Math.floor(t + W);
        if (n0 < 0) n0 = 0;
        if (n1 > inLen - 1) n1 = inLen - 1;
        if (n1 < n0) continue;
        var x = t - n0;                       // daalt met 1 per tap
        var a = PI * cutoff * x, sa = Math.sin(a), ca = Math.cos(a);
        var g = PI * x / W, sg = Math.sin(g), cg = Math.cos(g);
        var m = n1 - n0 + 1;
        for (var j = 0; j < m; j++) {
          var w;
          var u = x / W;
          if (u > 1 || u < -1) w = 0;
          else {
            var y = cutoff * x;
            var sinc = (y > 1e-9 || y < -1e-9) ? sa / (PI * y) : 1;
            w = cutoff * sinc * (0.5 + 0.5 * cg);
          }
          wts[j] = w;
          // hoeken verlagen met b resp. h: sin(a−b) = sa·cb − ca·sb, cos(a−b) = ca·cb + sa·sb
          var nsa = sa * cb - ca * sb; ca = ca * cb + sa * sb; sa = nsa;
          var nsg = sg * ch - cg * sh; cg = cg * ch + sg * sh; sg = nsg;
          x -= 1;
        }
        for (c = 0; c < C; c++) {
          var src = channels[c], s = 0;
          for (var k = 0; k < m; k++) s += src[n0 + k] * wts[k];
          out[c][i] = s;
        }
      }
      return out;
    }
    return { resample: resample };
  })();

  /* =====================================================================
   * Stretch — WSOLA (§7.1)
   * ===================================================================== */
  var Stretch = (function () {
    // Presetwaarden gelden bij 44,1/48 kHz; daarbuiten schalen N, Hs, D mee
    // (afgerond op even getallen; N een viervoud zodat Hs = N/2 ook even is).
    function params(preset, sampleRate) {
      var p = CONSTANTS.WSOLA_PRESETS[preset || 'music'];
      if (!p) fail('onbekende preset ' + q(preset) + '; kies ' + CONSTANTS.PRESETS.join(' of '));
      var sr = sampleRate || CONSTANTS.DEFAULT_SAMPLE_RATE;
      var f = 1;
      if (sr < CONSTANTS.WSOLA_REF_RATE_MIN) f = sr / CONSTANTS.WSOLA_REF_RATE_MIN;
      else if (sr > CONSTANTS.WSOLA_REF_RATE_MAX) f = sr / CONSTANTS.WSOLA_REF_RATE_MAX;
      var N = Math.max(16, 4 * Math.round(p.N * f / 4));
      var D = Math.max(2, 2 * Math.round(p.D * f / 2));
      return { N: N, Hs: N / 2, D: D, corrStep: p.corrStep, scale: f };
    }

    function copyFit(channels, outLength) {
      return channels.map(function (ch) {
        var o = new Float32Array(outLength);
        o.set(ch.length > outLength ? ch.subarray(0, outLength) : ch);
        return o;
      });
    }

    // Retourneert { channels, method: 'wsola'|'copy'|'varispeed', warning? }
    function run(channels, outLength, opts) {
      opts = opts || {};
      outLength = Math.max(0, Math.round(outLength));
      var C = channels.length;
      if (!C) fail('stretch: geen kanalen opgegeven');
      var inLen = channels[0].length;
      for (var c0 = 1; c0 < C; c0++) if (channels[c0].length !== inLen) fail('stretch: alle kanalen moeten even lang zijn');
      var P = params(opts.preset, opts.sampleRate);
      var N = P.N, Hs = P.Hs, D = P.D, step = P.corrStep;
      if (outLength === 0) return { channels: copyFit(channels, 0), method: 'copy' };
      if (inLen === 0) return { channels: copyFit(channels, outLength), method: 'copy' };

      // 1. alpha
      var alpha = outLength / inLen;
      // 7. |alpha − 1| < 0,0005 → kopiëren en afkorten/aanvullen
      if (Math.abs(alpha - 1) < CONSTANTS.WSOLA_IDENTITY_TOL) return { channels: copyFit(channels, outLength), method: 'copy' };
      // 8. uitsnede korter dan 2N → varispeed met melding
      if (inLen < 2 * N) {
        return {
          channels: Resampler.resample(channels, outLength), method: 'varispeed',
          warning: 'uitsnede (' + inLen + ' samples) is korter dan 2N (' + (2 * N) + ' samples); varispeed gebruikt in plaats van WSOLA (toonhoogte verschuift ' + (Math.round(1200 * Math.log(1 / alpha) / Math.LN2) / 100) + ' halve tonen)'
        };
      }

      // Uitlijning (aanvulling op de letterlijke stappen): door de N voorloopnullen
      // (stap 3) en ideal = round(k·Hs/alpha) ligt het framemidden van frame k in de
      // synthese op k·Hs + N/2 en in de invoer op k·Hs/alpha − N/2. Met een voorloop
      // van N·(1+alpha)/2 synthesesamples geldt uitvoer = alpha × invoer voor elk
      // framemidden. Die voorloop wordt na afloop weggeknipt; de frames lopen door
      // tot het hele gebied gevuld is.
      var lead = Math.round(N * (1 + alpha) / 2);
      var synthLen = outLength + lead;

      // 3. aanvullen met N nullen voor en (minstens) N + D nullen na
      var padLen = Math.max(N + inLen + N + D, Math.ceil(synthLen / alpha) + 2 * D + 2 * N + Hs + 8);
      var padded = [], c, i;
      var m = new Float32Array(padLen);
      for (c = 0; c < C; c++) {
        var pc = new Float32Array(padLen);
        pc.set(channels[c], N);
        padded.push(pc);
      }
      // 2. mono-som voor de zoekstap
      var invC = 1 / C;
      for (c = 0; c < C; c++) { var pcc = padded[c]; for (i = 0; i < padLen; i++) m[i] += pcc[i] * invC; }
      // Gedecimeerde kopieën (stap 2) voor contiguë toegang in de correlatielus.
      var cnt = Math.floor(N / step);
      var dec = [];
      for (var ph = 0; ph < step; ph++) {
        var dl = Math.ceil((padLen - ph) / step), dv = new Float32Array(dl);
        for (i = 0; i < dl; i++) dv[i] = m[ph + i * step];
        dec.push(dv);
      }
      var tgt = new Float32Array(cnt);

      // Hann, periodiek
      var win = new Float32Array(N);
      for (i = 0; i < N; i++) win[i] = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / N);

      var out = [];
      for (c = 0; c < C; c++) out.push(new Float32Array(synthLen + N));

      function addFrame(pos, at) {
        for (var cc = 0; cc < C; cc++) {
          var src = padded[cc], dst = out[cc];
          for (var j = 0; j < N; j++) dst[at + j] += src[pos + j] * win[j];
        }
      }

      // 4. frame 0 → uitvoerpositie 0
      addFrame(0, 0);
      var prevPos = 0;
      var totalFrames = Math.ceil(synthLen / Hs);
      var onProgress = typeof opts.onProgress === 'function' ? opts.onProgress : null;
      // 5. frames k = 1, 2, … zolang k·Hs < (synth)lengte
      for (var k = 1; k * Hs < synthLen; k++) {
        var ideal = Math.round(k * Hs / alpha);
        var target = prevPos + Hs;
        for (i = 0; i < cnt; i++) tgt[i] = m[target + i * step];
        var lo = -D; if (ideal + lo < 0) lo = -ideal;
        // Bij gelijke correlatie (bijv. stilte) wint δ = 0: eerst δ = 0 berekenen.
        var best = -Infinity, bestD = 0;
        for (var dd = lo - 1; dd <= D; dd++) {
          var d = dd < lo ? 0 : dd;
          if (dd >= lo && d === 0) continue;
          var base = ideal + d;
          var arr = dec[base % step], off = (base / step) | 0;
          var s = 0;
          for (i = 0; i < cnt; i++) s += arr[off + i] * tgt[i];
          if (s > best) { best = s; bestD = d; }
        }
        var pos = ideal + bestD;
        addFrame(pos, k * Hs);
        prevPos = pos;
        if (onProgress && k % CONSTANTS.WSOLA_PROGRESS_FRAMES === 0) onProgress(Math.min(1, k / totalFrames));
      }
      if (onProgress) onProgress(1);
      // 6. afsnijden op exact outLength
      var res = out.map(function (o) { return o.slice(lead, lead + outLength); });
      return { channels: res, method: 'wsola' };
    }

    return { run: run, params: params };
  })();

  /* =====================================================================
   * WavCodec (§7.4)
   * ===================================================================== */
  var WavCodec = (function () {
    function encode(channels, sampleRate) {
      if (!Array.isArray(channels) || !channels.length) fail('wav.encode: geen kanalen opgegeven');
      var C = channels.length, n = channels[0].length;
      var bytesPerSample = 2, dataLen = n * C * bytesPerSample;
      var buf = new ArrayBuffer(CONSTANTS.WAV_HEADER_BYTES + dataLen);
      var v = new DataView(buf);
      function str(o, s) { for (var i = 0; i < s.length; i++) v.setUint8(o + i, s.charCodeAt(i)); }
      str(0, 'RIFF'); v.setUint32(4, 36 + dataLen, true); str(8, 'WAVE');
      str(12, 'fmt '); v.setUint32(16, 16, true); v.setUint16(20, 1, true); v.setUint16(22, C, true);
      v.setUint32(24, sampleRate, true); v.setUint32(28, sampleRate * C * bytesPerSample, true);
      v.setUint16(32, C * bytesPerSample, true); v.setUint16(34, 16, true);
      str(36, 'data'); v.setUint32(40, dataLen, true);
      var i16 = new Int16Array(n * C);
      for (var c = 0; c < C; c++) {
        var ch = channels[c];
        for (var i = 0; i < n; i++) {
          var s = ch[i];
          if (!(s === s)) s = 0;
          s = s < -1 ? -1 : (s > 1 ? 1 : s);
          i16[i * C + c] = Math.round(s < 0 ? s * 32768 : s * 32767);
        }
      }
      var bytes = new Uint8Array(buf);
      var le = new Uint8Array(new Uint16Array([1]).buffer)[0] === 1;
      if (le) bytes.set(new Uint8Array(i16.buffer), 44);
      else for (var k = 0; k < i16.length; k++) v.setInt16(44 + 2 * k, i16[k], true);
      return bytes;
    }

    function decode(bytes) {
      if (bytes instanceof ArrayBuffer) bytes = new Uint8Array(bytes);
      if (!bytes || typeof bytes.length !== 'number' || !bytes.buffer) fail('wav.decode: verwacht een Uint8Array');
      var v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      var L = bytes.byteLength;
      function str(o) { return String.fromCharCode(v.getUint8(o), v.getUint8(o + 1), v.getUint8(o + 2), v.getUint8(o + 3)); }
      if (L < 12 || str(0) !== 'RIFF' || str(8) !== 'WAVE') fail('geen WAV-bestand (RIFF/WAVE-kop ontbreekt)');
      var pos = 12, fmt = null, dataOff = -1, dataLen = 0;
      while (pos + 8 <= L) {
        var id = str(pos), size = v.getUint32(pos + 4, true), body = pos + 8;
        if (id === 'fmt ') {
          if (size < 16) fail('WAV: fmt-chunk te kort');
          fmt = { format: v.getUint16(body, true), channels: v.getUint16(body + 2, true), sampleRate: v.getUint32(body + 4, true),
            blockAlign: v.getUint16(body + 12, true), bits: v.getUint16(body + 14, true) };
          if (fmt.format === 0xFFFE && size >= 26) fmt.format = v.getUint16(body + 24, true); // WAVE_FORMAT_EXTENSIBLE: subformaat
        } else if (id === 'data') {
          dataOff = body;
          // Pijp-uitvoer (bijv. ffmpeg naar stdout) heeft soms grootte 0 of 0xFFFFFFFF.
          dataLen = (size === 0 || size === 0xFFFFFFFF || body + size > L) ? L - body : size;
          break;
        }
        pos = body + size + (size & 1);          // onbekende chunks overslaan
      }
      if (!fmt) fail('WAV: fmt-chunk ontbreekt');
      if (dataOff < 0) fail('WAV: data-chunk ontbreekt');
      var F = fmt.format, B = fmt.bits, C = fmt.channels;
      var okFmt = (F === 1 && (B === 16 || B === 24 || B === 32)) || (F === 3 && B === 32);
      if (!okFmt) fail('WAV-formaat ' + F + ' met ' + B + ' bit wordt niet ondersteund; ondersteund: PCM (formaat 1) 16/24/32 bit en IEEE float (formaat 3) 32 bit');
      if (C < 1) fail('WAV: aantal kanalen is 0');
      var bps = B / 8, frame = bps * C, n = Math.floor(dataLen / frame);
      var chans = [], c, i;
      for (c = 0; c < C; c++) chans.push(new Float32Array(n));
      var o = dataOff;
      for (i = 0; i < n; i++) {
        for (c = 0; c < C; c++) {
          var s;
          if (F === 3) s = v.getFloat32(o, true);
          else if (B === 16) s = v.getInt16(o, true) / 32768;
          else if (B === 24) { var x = v.getUint8(o) | (v.getUint8(o + 1) << 8) | (v.getInt8(o + 2) << 16); s = x / 8388608; }
          else s = v.getInt32(o, true) / 2147483648;
          chans[c][i] = s;
          o += bps;
        }
      }
      return { sampleRate: fmt.sampleRate, channels: chans };
    }
    return { encode: encode, decode: decode };
  })();

  /* =====================================================================
   * Ratio-indicatie (§6.5)
   * ===================================================================== */
  function ratioStatus(ratio) {
    var B = CONSTANTS.RATIO_BANDS;
    if (typeof ratio !== 'number' || !(ratio > 0) || !isFinite(ratio)) return 'blocked';
    var e = 1e-9;
    if (ratio >= B.green[0] - e && ratio <= B.green[1] + e) return 'green';
    if (ratio >= B.orange[0] - e && ratio <= B.orange[1] + e) return 'orange';
    if (ratio >= B.red[0] - e && ratio <= B.red[1] + e) return 'red';
    return 'blocked';
  }

  /* =====================================================================
   * Fit-berekening (§6.5, regels §6.6) — puur, op het recept
   * ===================================================================== */
  function computeFit(block) { return computeFitFull(block).report; }

  // Retourneert { report (= fit_report-regel), voiceErrors: [string], unaccepted: [overlap] }.
  function computeFitFull(block) {
    var T = block.targetDuration, fr = block.fragments, n = fr.length, tr = block.transitions || [];
    var label = q(block.name) + ' (' + block.id + ')';
    var res = { block: block.id, name: block.name, fitMode: block.fitMode, targetDuration: T, ok: true, error: null,
      ratio: null, fragments: [], transitions: [], voice: [], duck: [], overlaps: [],
      contentDuration: 0, deviation: 0, exportable: false, warnings: [] };
    var full = { report: res, voiceErrors: [], unaccepted: [] };
    if (!n) { res.ok = false; res.error = 'blok ' + label + ' bevat nog geen fragmenten; voeg er een toe met add_fragment'; return full; }
    var src = fr.map(function (f) { return f.out - f['in']; });
    var nominal = [], gaps = [], eff = [], i, j;
    for (i = 0; i < n - 1; i++) {
      var t = tr[i] || { type: 'cut', ms: 0 };
      var ov = isOverlapType(t.type) ? (t.ms || 0) / 1000 : 0;
      nominal.push(ov); eff.push(ov);
      gaps.push(t.type === 'gap' ? (t.ms || 0) / 1000 : 0);
    }
    var G = gaps.reduce(function (a, b) { return a + b; }, 0);
    var Lfree = 0, Llocked = 0, nFree = 0;
    for (i = 0; i < n; i++) { if (fr[i].locked) Llocked += src[i]; else { Lfree += src[i]; nFree++; } }
    var manual = block.fitMode === 'manual';
    var d = new Array(n), ratio = null;
    var reasons = {};
    for (var it = 0; it < CONSTANTS.FIT_MAX_ITER; it++) {
      var O = eff.reduce(function (a, b) { return a + b; }, 0);
      if (manual) {
        for (i = 0; i < n; i++) d[i] = (fr[i].locked || fr[i].manualDuration == null) ? src[i] : fr[i].manualDuration;
      } else if (nFree > 0) {
        var denom = T + O - G - Llocked;
        if (!(denom > 0)) {
          res.ok = false;
          res.error = 'fit onmogelijk voor blok ' + label + ': doelduur ' + fmt(T) + ' s + overlap ' + fmt(O) + ' s − stilte ' + fmt(G) +
            ' s − vergrendeld ' + fmt(Llocked) + ' s = ' + fmt(denom) + ' s (moet > 0 zijn); ontgrendel fragmenten, verkort stiltes of verleng het blok';
          return full;
        }
        ratio = Lfree / denom;
        for (i = 0; i < n; i++) d[i] = fr[i].locked ? src[i] : src[i] / ratio;
      } else {
        for (i = 0; i < n; i++) d[i] = src[i];
      }
      var changed = false;
      // Regel 1: overlap max. 90% van de doelduur van het kortste van de twee.
      for (i = 0; i < n - 1; i++) {
        var lim = CONSTANTS.MAX_OVERLAP_FRACTION * Math.min(d[i], d[i + 1]);
        if (eff[i] > lim + 1e-12) { eff[i] = lim; changed = true; reasons[i] = (reasons[i] || {}); reasons[i].r1 = true; }
      }
      // Regel 2: twee overgangen rond één fragment samen niet langer dan dat fragment.
      for (j = 1; j < n - 1; j++) {
        var l = eff[j - 1], r = eff[j];
        if (l + r > d[j] + 1e-12) {
          var sc = d[j] / (l + r);
          eff[j - 1] = l * sc; eff[j] = r * sc; changed = true;
          reasons[j - 1] = (reasons[j - 1] || {}); reasons[j - 1].r2 = true;
          reasons[j] = (reasons[j] || {}); reasons[j].r2 = true;
        }
      }
      if (!changed) break;
    }
    var Ofin = eff.reduce(function (a, b) { return a + b; }, 0);
    var content = 0;
    for (i = 0; i < n; i++) content += d[i];
    content = content - Ofin + G;
    res.ratio = ratio;
    res.contentDuration = content;
    res.deviation = content - T;
    // Uitvoertijden in het blok (seconden): start_0 = 0, start_{i+1} = start_i + doeldur_i − overlap_i + gap_i.
    var mStart = [], mEnd = [], pos = 0;
    for (i = 0; i < n; i++) {
      mStart.push(pos); mEnd.push(pos + d[i]);
      if (i < n - 1) pos = pos + d[i] - eff[i] + gaps[i];
    }
    var blocked = false;
    for (i = 0; i < n; i++) {
      var ri = src[i] / d[i];
      var st = ratioStatus(ri);
      var fe = { id: fr[i].id, source: fr[i].source, 'in': fr[i]['in'], out: fr[i].out, mode: fr[i].mode, preset: fr[i].preset, locked: fr[i].locked,
        sourceDuration: src[i], targetDuration: d[i], ratio: ri, status: st, start: mStart[i], end: mEnd[i] };
      if (fr[i].mode === 'varispeed') fe.semitones = Math.round(1200 * Math.log(ri) / Math.LN2) / 100;
      if (st === 'blocked' && fr[i].mode === 'pitch') {
        blocked = true;
        res.warnings.push('fragment ' + fr[i].id + ': ratio ' + ri.toFixed(3) + ' valt buiten ' + CONSTANTS.RATIO_BANDS.red[0].toFixed(2) + '–' +
          CONSTANTS.RATIO_BANDS.red[1].toFixed(2) + ' (geblokkeerd); kies andere fragmenten, vergrendel/ontgrendel of pas de doelduur aan');
      }
      res.fragments.push(fe);
    }
    for (i = 0; i < n - 1; i++) {
      var tt = tr[i] || { type: 'cut', ms: 0 };
      var te = { index: i, type: tt.type, ms: tt.ms || 0, start: Math.min(mEnd[i], mStart[i + 1]), end: Math.max(mEnd[i], mStart[i + 1]),
        effectiveMs: isOverlapType(tt.type) ? eff[i] * 1000 : (tt.ms || 0), overlap: eff[i], gap: gaps[i] };
      if (tt.curve) te.curve = tt.curve;
      if (tt.type === 'duck') te.duckDb = tt.duckDb;
      if (eff[i] < nominal[i] - 1e-9) {
        var why = [];
        if (reasons[i] && reasons[i].r1) why.push('max. 90% van het kortste fragment');
        if (reasons[i] && reasons[i].r2) why.push('twee overgangen rond één fragment mogen samen niet langer zijn dan dat fragment');
        res.warnings.push('overgang ' + i + ' (' + fr[i].id + ' → ' + fr[i + 1].id + ', ' + tt.type + ') ingekort van ' + fmtMs(nominal[i]) + ' ms naar ' +
          fmtMs(eff[i]) + ' ms (' + why.join('; ') + ')');
      }
      res.transitions.push(te);
    }
    var tol = 1e-6;
    if (content > T + tol) {
      res.warnings.push('blok ' + label + ' is ' + fmt(content - T) + ' s te lang (' + fmt(content) + ' s i.p.v. ' + fmt(T) + ' s); export geblokkeerd — verkort fragmenten of handmatige duren');
    } else if (content < T - tol) {
      res.warnings.push('blok ' + label + ' is ' + fmt(T - content) + ' s te kort; aan het eind komt stilte');
    }
    computeVoice(block, res, full, mStart, tol);
    res.exportable = res.ok && !blocked && content <= T + tol && !full.voiceErrors.length;
    return full;
  }

  // §6.10 stemspoor: positie (anker + offset), ratio, buiten-blok, overlap, ducking-zones.
  function computeVoice(block, res, full, mStart, tol) {
    var T = block.targetDuration, vs = block.voice || [], label = q(block.name) + ' (' + block.id + ')';
    var ids = block.fragments.map(function (f) { return f.id; });
    var B = CONSTANTS.RATIO_BANDS, placed = [];
    vs.forEach(function (v, idx) {
      var ai = ids.indexOf(v.anchor);
      var sd = v.out - v['in'];
      var td = v.targetDuration == null ? sd : v.targetDuration;
      var ratio = sd / td, st = ratioStatus(ratio);
      var start = ai >= 0 ? mStart[ai] + (v.offset || 0) : null;
      var e = { id: v.id, anchor: v.anchor, start: start, end: start == null ? null : start + td,
        sourceDuration: sd, targetDuration: td, ratio: ratio, status: st };
      if (v.mode === 'varispeed') e.semitones = Math.round(1200 * Math.log(ratio) / Math.LN2) / 100;
      res.voice.push(e);
      function err(m) { full.voiceErrors.push(m); res.warnings.push(m); }
      if (ai < 0) { err('stem ' + v.id + ': anker ' + q(v.anchor) + ' is geen muziekfragment van blok ' + label + '; kies uit: ' + (ids.join(', ') || '(geen)')); return; }
      if (st === 'blocked' && v.mode === 'pitch') {
        err('stem ' + v.id + ': ratio ' + ratio.toFixed(3) + ' valt buiten ' + B.red[0].toFixed(2) + '–' + B.red[1].toFixed(2) +
          ' (geblokkeerd); kies een targetDuration dichter bij de bronduur (' + fmt(sd) + ' s) of null');
      } else if (v.targetDuration != null && st !== 'green' && st !== 'orange') {
        res.warnings.push('stem ' + v.id + ': ratio ' + ratio.toFixed(3) + ' — stem duidelijk hoorbaar ' + (ratio > 1 ? 'versneld' : 'vertraagd'));
      }
      if (e.start < -tol) {
        err('stem ' + v.id + ' begint ' + fmt(-e.start) + ' s vóór het begin van blok ' + label + ' (anker ' + v.anchor + ' start op ' + fmt(mStart[ai]) +
          ' s, offset ' + fmt(v.offset) + ' s); offset moet minstens ' + fmt(-mStart[ai]) + ' zijn');
      }
      if (e.end > T + tol) {
        err('stem ' + v.id + ' is ' + fmt(e.end - T) + ' s te lang voor blok ' + label + ': eindigt op ' + fmt(e.end) + ' s, het blok duurt ' + fmt(T) +
          ' s; verklein offset, verkort de stem of geef een kortere targetDuration');
      }
      placed.push({ v: v, idx: idx, start: e.start, end: e.end });
    });
    // Overlap tussen stemfragmenten (gesorteerd op start; bij gelijke start telt de volgorde in de lijst).
    placed.sort(function (a, b) { return a.start - b.start || a.idx - b.idx; });
    for (var i = 0; i < placed.length; i++) {
      for (var j = i + 1; j < placed.length; j++) {
        var A = placed[i], Bv = placed[j];
        if (Bv.start >= A.end - 1e-9) continue;
        var dur = Math.min(A.end, Bv.end) - Bv.start;
        var ov = { a: A.v.id, b: Bv.v.id, duration: dur, accepted: !!Bv.v.acceptOverlap };
        res.overlaps.push(ov);
        if (ov.accepted) res.warnings.push('stemfragmenten ' + ov.a + ' en ' + ov.b + ' overlappen ' + fmt(dur) + ' s (geaccepteerd; beide worden opgeteld)');
        else {
          full.unaccepted.push(ov);
          res.warnings.push('stemfragmenten ' + ov.a + ' en ' + ov.b + ' overlappen ' + fmt(dur) + ' s; render/export geblokkeerd tot de overlap ' +
            'geaccepteerd is (update_voice {"voice":"' + ov.b + '","acceptOverlap":true}) of een van beide verschoven is');
        }
      }
    }
    res.duck = duckZones(placed, block.duckDb, block.duckMs / 1000, T);
  }

  // Samengevoegde ducking-zones incl. hellingen, begrensd op [0, T]. Werkt in seconden én in samples.
  // Retourneert [{ start, end }] en (intern) per zone de stemgrenzen voor de hellingen.
  function duckGroups(spans, ramp, T) {
    var list = spans.slice().sort(function (a, b) { return a.start - b.start; });
    var groups = [];
    list.forEach(function (sp) {
      var g = groups[groups.length - 1];
      if (g && sp.start - ramp <= g.vEnd + ramp) { if (sp.end > g.vEnd) g.vEnd = sp.end; }
      else groups.push({ vStart: sp.start, vEnd: sp.end });
    });
    groups.forEach(function (g) {
      g.vStart = Math.max(0, g.vStart); g.vEnd = Math.min(T, g.vEnd);
      g.start = Math.max(0, g.vStart - ramp); g.end = Math.min(T, g.vEnd + ramp);
    });
    return groups;
  }
  function duckZones(placed, duckDb, rampSec, T) {
    if (!(duckDb < 0) || !placed.length) return [];
    return duckGroups(placed, rampSec, T).map(function (g) { return { start: g.start, end: g.end }; });
  }

  /* =====================================================================
   * Opzoeken (puur, op een recept)
   * ===================================================================== */
  function findBlock(recipe, ref) {
    for (var i = 0; i < recipe.blocks.length; i++) if (recipe.blocks[i].id === ref) return recipe.blocks[i];
    var byName = recipe.blocks.filter(function (b) { return b.name === ref; });
    if (byName.length === 1) return byName[0];
    var list = recipe.blocks.map(function (b) { return b.id + ' (' + b.name + ')'; });
    fail('blok ' + q(ref) + ' bestaat niet; ' + (list.length ? 'bestaande blokken: ' + list.join(', ') : 'er zijn nog geen blokken; maak er een met create_block'));
  }

  /* =====================================================================
   * Renderer — blok samenstellen (§7.3) + stemspoor en ducking (§6.10).
   * Puur: werkt op (recept, audio, blok, {cache, onProgress}) zonder interne
   * state. Gebruikt door render_block (hoofdthread) én renderBlockPure (achtergrondthread),
   * zodat beide sample-identiek zijn.
   * ===================================================================== */
  var Renderer = (function () {
    function fadeInGain(g, len) { for (var k = 0; k < len; k++) g[k] *= 0.5 - 0.5 * Math.cos(Math.PI * k / len); }
    function fadeOutGain(g, n, len) { for (var k = 0; k < len; k++) g[n - len + k] *= 0.5 + 0.5 * Math.cos(Math.PI * (k + 1) / len); }
    function curveIn(g, len, curve) {
      for (var k = 0; k < len; k++) { var x = k / len; g[k] *= curve === 'linear' ? x : Math.sin(x * Math.PI / 2); }
    }
    function curveOut(g, n, len, curve) {
      for (var k = 0; k < len; k++) { var x = k / len; g[n - len + k] *= curve === 'linear' ? 1 - x : Math.cos(x * Math.PI / 2); }
    }
    function duckOut(g, n, len, level, ramp) {
      for (var k = 0; k < len; k++) {
        var v;
        if (k < ramp) v = 1 - (1 - level) * (0.5 - 0.5 * Math.cos(Math.PI * k / ramp));
        else if (k < len - ramp) v = level;
        else { var kk = k - (len - ramp); v = level * (0.5 + 0.5 * Math.cos(Math.PI * (kk + 1) / ramp)); }
        g[n - len + k] *= v;
      }
    }

    // Stretch/resample van een uitsnede, met cache (sleutel §7.3 stap 4 + preset).
    // kind: 'f' (muziek) of 'v' (stem). progress(fractie 0..1) tijdens WSOLA.
    function rawItem(env, kind, item, outLength, progress) {
      var a = env.audio[item.source], sr = env.sampleRate, cache = env.cache;
      var who = (kind === 'v' ? 'stem ' : 'fragment ') + item.id;
      var key = [kind, item.id, item.source, a.version == null ? '' : a.version, item['in'], item.out, outLength, item.mode, item.preset].join('|');
      if (cache && cache.has(key)) {
        var hit = cache.get(key);
        if (typeof cache['delete'] === 'function') { cache['delete'](key); cache.set(key, hit); }
        if (hit.warning) env.warnings.push(who + ': ' + hit.warning);
        return hit.channels;
      }
      var len = a.channels[0].length;
      var s0 = Math.max(0, Math.min(len, Math.round(item['in'] * sr))), s1 = Math.max(s0, Math.min(len, Math.round(item.out * sr)));
      var slice = a.channels.map(function (ch) { return ch.subarray(s0, s1); });
      var r;
      if (item.mode === 'varispeed') r = { channels: Resampler.resample(slice, outLength), method: 'varispeed' };
      else r = Stretch.run(slice, outLength, { preset: item.preset, sampleRate: sr, onProgress: progress });
      if (r.warning) env.warnings.push(who + ': ' + r.warning);
      if (cache) {
        cache.set(key, { channels: r.channels, warning: r.warning || null });
        if (typeof cache.size === 'number' && typeof cache.keys === 'function' && typeof cache['delete'] === 'function') {
          while (cache.size > CONSTANTS.RENDER_CACHE_MAX) cache['delete'](cache.keys().next().value);
        }
      }
      return r.channels;
    }

    function sourceLen(a, item, sr) {
      var len = a.channels[0].length;
      var s0 = Math.max(0, Math.min(len, Math.round(item['in'] * sr))), s1 = Math.max(s0, Math.min(len, Math.round(item.out * sr)));
      return s1 - s0;
    }

    // Retourneert { sampleRate, channels, stats: {duration, samples, peakDb, clipped}, peak, warnings }.
    function render(recipe, audio, block, opts) {
      opts = opts || {};
      audio = audio || {};
      var full = computeFitFull(block), fit = full.report;
      var bl = q(block.name) + ' (' + block.id + ')';
      if (!fit.ok) fail(fit.error);
      var warnings = fit.warnings.slice();
      var T = block.targetDuration, sr = recipe.sampleRate;
      if (!fit.exportable) {
        if (fit.contentDuration > T + 1e-6) fail('blok ' + bl + ' is ' + fmt(fit.contentDuration - T) + ' s te lang (' + fmt(fit.contentDuration) + ' s i.p.v. ' + fmt(T) + ' s); verkort de handmatige duren of zet fitMode op proportional');
        if (full.voiceErrors.length) fail('blok ' + bl + ' kan niet gerenderd worden: ' + full.voiceErrors.join('; '));
        fail('blok ' + bl + ' kan niet gerenderd worden: ' + fit.warnings.join('; '));
      }
      if (full.unaccepted.length) {
        fail('blok ' + bl + ' kan niet gerenderd worden: ' + full.unaccepted.map(function (o) {
          return 'stemfragmenten ' + o.a + ' en ' + o.b + ' overlappen ' + fmt(o.duration) + ' s';
        }).join('; ') + '. Accepteer de overlap met acceptOverlap: true op het later startende fragment (update_voice {"voice":"' +
          full.unaccepted[0].b + '","acceptOverlap":true}) of verschuif een van beide');
      }
      var fr = block.fragments, n = fr.length, tr = block.transitions, vs = block.voice || [], i, c, k;
      var missing = fr.concat(vs).filter(function (f) { return !audio[f.source]; }).map(function (f) { return f.source; });
      if (missing.length) {
        var uniq = missing.filter(function (x, ix) { return missing.indexOf(x) === ix; });
        fail('blok ' + q(block.name) + ' is niet renderbaar: ontbrekende bron(nen) ' + uniq.join(', ') + '; laad die bestanden eerst');
      }
      fr.concat(vs).forEach(function (f) {
        var a = audio[f.source];
        if (a.sampleRate != null && a.sampleRate !== sr) fail('bron ' + q(f.source) + ' staat op ' + a.sampleRate + ' Hz; verwacht de werkfrequentie ' + sr + ' Hz (eerst resamplen)');
      });
      // 1. total
      var total = Math.round(T * sr);
      // 2. outLength_i, overlap en gap in samples
      var outLen = fit.fragments.map(function (fe) { return Math.round(fe.targetDuration * sr); });
      var ov = fit.transitions.map(function (te) { return Math.round(te.overlap * sr); });
      var gp = fit.transitions.map(function (te) { return Math.round(te.gap * sr); });
      function contentLen() {
        var s = 0; for (var a = 0; a < n; a++) s += outLen[a];
        for (var b = 0; b < n - 1; b++) s += gp[b] - ov[b];
        return s;
      }
      // 3. afrondingsverschil op het laatste vrije fragment
      var lastFree = -1;
      for (i = n - 1; i >= 0; i--) if (!fr[i].locked) { lastFree = i; break; }
      var exact = block.fitMode === 'proportional' && lastFree >= 0;
      var diff = total - contentLen();
      if (exact) outLen[lastFree] += diff;
      else if (diff < 0) {
        if (-diff > n + 1) fail('blok ' + q(block.name) + ' is ' + (-diff) + ' samples te lang');
        outLen[lastFree >= 0 ? lastFree : n - 1] += diff;
      }
      for (i = 0; i < n; i++) if (outLen[i] < 1) fail('fragment ' + fr[i].id + ' krijgt een doelduur van ' + outLen[i] + ' samples; blok ' + q(block.name) + ' is te krap');
      for (i = 0; i < n - 1; i++) ov[i] = Math.min(ov[i], outLen[i], outLen[i + 1]);
      if (exact) {
        var d2 = total - contentLen();
        if (d2 !== 0) outLen[lastFree] += d2;
      }
      // 6. startposities (vooraf, zodat de stem aan zijn anker kan hangen)
      var starts = [], sPos = 0;
      for (i = 0; i < n; i++) { starts.push(sPos); if (i < n - 1) sPos = sPos + outLen[i] - ov[i] + gp[i]; }
      // Stem: lengte en startsample = uitvoerstart anker + offset (begrensd op het blok;
      // de fit heeft al gecontroleerd dat de stem binnen het blok valt, verschil ≤ afronding).
      var fids = fr.map(function (f) { return f.id; });
      var vPlan = vs.map(function (v) {
        var a = audio[v.source];
        var L = v.targetDuration == null ? sourceLen(a, v, sr) : Math.round(v.targetDuration * sr);
        if (L < 1) fail('stem ' + v.id + ' krijgt een duur van ' + L + ' samples; kies een langere uitsnede of targetDuration');
        if (L > total) fail('stem ' + v.id + ' is ' + fmt((L - total) / sr) + ' s te lang voor blok ' + bl);
        var st = starts[fids.indexOf(v.anchor)] + Math.round((v.offset || 0) * sr);
        st = Math.max(0, Math.min(st, total - L));
        return { v: v, start: st, len: L };
      });

      // Voortgang: gewicht per taak = aantal uitvoersamples; mixen ≈ 5%.
      var onProgress = typeof opts.onProgress === 'function' ? opts.onProgress : null;
      var weights = outLen.concat(vPlan.map(function (p) { return p.len; }));
      var W = 0; weights.forEach(function (w) { W += Math.max(1, w); });
      var mixW = Math.max(1, Math.round(W * 0.05)); W += mixW;
      var doneW = 0, lastF = 0;
      function report(frac, label) {
        if (!onProgress) return;
        var f = frac > 1 ? 1 : (frac < lastF ? lastF : frac);
        lastF = f;
        onProgress(f, label);
      }
      function job(w, label) {
        var base = doneW; w = Math.max(1, w);
        report(base / W, label);
        return {
          progress: function (x) { report((base + w * Math.max(0, Math.min(1, x))) / W, label); },
          done: function () { doneW = base + w; report(doneW / W, label); }
        };
      }
      var env = { audio: audio, sampleRate: sr, cache: opts.cache || null, warnings: warnings };

      var C = 1;
      fr.concat(vs).forEach(function (f) { C = Math.max(C, audio[f.source].channels.length); });
      var out = [];
      for (c = 0; c < C; c++) out.push(new Float32Array(total));
      var micro = Math.round(CONSTANTS.MICROFADE_MS / 1000 * sr);
      var duckRamp = Math.round(CONSTANTS.DUCK_RAMP_MS / 1000 * sr);
      var endPos = 0;
      for (i = 0; i < n; i++) {
        var f = fr[i], L = outLen[i], start = starts[i];
        // 4. stretchen of resamplen (cache)
        var jb = job(L, 'muziek ' + (i + 1) + '/' + n + ' rekken');
        var raw = rawItem(env, 'f', f, L, jb.progress);
        jb.done();
        // 5. gain en omhulling
        var g = new Float32Array(L);
        g.fill(dbToLin(f.gainDb || 0));
        var left = null, right = null;
        if (i === 0) left = { kind: 'fade', len: Math.round((f.fadeInMs || 0) / 1000 * sr), own: true };
        else {
          var tl = tr[i - 1];
          if (tl.type === 'crossfade' || tl.type === 'fade_in_b') left = { kind: 'curve', len: ov[i - 1], curve: tl.curve || 'equal_power' };
          else left = { kind: 'fade', len: micro };
        }
        if (i === n - 1) right = { kind: 'fade', len: Math.round((f.fadeOutMs || 0) / 1000 * sr), own: true };
        else {
          var trr = tr[i];
          if (trr.type === 'crossfade' || trr.type === 'fade_out_a') right = { kind: 'curve', len: ov[i], curve: trr.curve || 'equal_power' };
          else if (trr.type === 'duck') right = { kind: 'duck', len: ov[i], level: dbToLin(typeof trr.duckDb === 'number' ? trr.duckDb : CONSTANTS.DUCK_DEFAULT_DB), ramp: Math.min(duckRamp, Math.floor(ov[i] / 2)) };
          else right = { kind: 'fade', len: micro };
        }
        // Fades/overgangen samen langer dan het fragment → fades evenredig inkorten
        // (overgangscurves liggen vast: die bepalen de uitlijning).
        if (left.len + right.len > L) {
          var fixed = (left.kind !== 'fade' ? left.len : 0) + (right.kind !== 'fade' ? right.len : 0);
          var flex = (left.kind === 'fade' ? left.len : 0) + (right.kind === 'fade' ? right.len : 0);
          var room = Math.max(0, L - fixed), sc = flex > 0 ? Math.min(1, room / flex) : 1;
          if (left.kind === 'fade') left.len = Math.floor(left.len * sc);
          if (right.kind === 'fade') right.len = Math.floor(right.len * sc);
          if ((left.own || right.own) && sc < 1) warnings.push('fragment ' + f.id + ': fades samen langer dan het fragment; evenredig ingekort');
        }
        if (left.len > 0) { if (left.kind === 'curve') curveIn(g, left.len, left.curve); else fadeInGain(g, left.len); }
        if (right.len > 0) {
          if (right.kind === 'curve') curveOut(g, L, right.len, right.curve);
          else if (right.kind === 'duck') { if (right.ramp > 0) duckOut(g, L, right.len, right.level, right.ramp); else curveOut(g, L, right.len, 'linear'); }
          else fadeOutGain(g, L, right.len);
        }
        var RC = raw.length;
        for (c = 0; c < C; c++) {
          var rc = raw[c % RC], dst = out[c];
          var lim = Math.min(L, total - start);
          for (k = 0; k < lim; k++) dst[start + k] += rc[k] * g[k];
        }
        endPos = start + L;
      }
      // 7. invariant (muziekspoor)
      if (out[0].length !== total || (exact && endPos !== total) || endPos > total) {
        throw new Error('Interne fout: lengte-invariant geschonden in blok ' + block.id + ' (uitvoer ' + endPos + ' samples, verwacht ' + total + ')');
      }

      // §6.10 stemmen stretchen (alleen met targetDuration; anders kopie op oorspronkelijke snelheid)
      var vRaw = vPlan.map(function (p, j) {
        var jv = job(p.len, 'stem ' + (j + 1) + '/' + vPlan.length + ' rekken');
        var r = rawItem(env, 'v', p.v, p.len, jv.progress);
        jv.done();
        return r;
      });
      report(doneW / W, 'mixen');
      // Ducking-omhullende op de muziek (cosinus, zones samengevoegd, begrensd op het blok)
      if (vPlan.length && block.duckDb < 0) {
        var level = dbToLin(block.duckDb), rampS = Math.round((block.duckMs || 0) / 1000 * sr);
        var groups = duckGroups(vPlan.map(function (p) { return { start: p.start, end: p.start + p.len }; }), rampS, total);
        var envG = new Float32Array(total); envG.fill(1);
        groups.forEach(function (gr) {
          var a0 = gr.start, a1 = gr.vStart, b0 = gr.vEnd, b1 = gr.end, kk;
          for (kk = a0; kk < a1; kk++) envG[kk] = 1 - (1 - level) * (0.5 - 0.5 * Math.cos(Math.PI * (kk - a0) / (a1 - a0)));
          for (kk = a1; kk < b0; kk++) envG[kk] = level;
          for (kk = b0; kk < b1; kk++) envG[kk] = level + (1 - level) * (0.5 - 0.5 * Math.cos(Math.PI * (kk - b0) / (b1 - b0)));
        });
        for (c = 0; c < C; c++) { var oc0 = out[c]; for (k = 0; k < total; k++) oc0[k] *= envG[k]; }
      }
      // Stemmen optellen: gain, eigen fades (cosinus), op hun startsample.
      vPlan.forEach(function (p, j) {
        var v = p.v, L = p.len, raw = vRaw[j];
        var g = new Float32Array(L);
        g.fill(dbToLin(v.gainDb || 0));
        var fi = Math.round((v.fadeInMs || 0) / 1000 * sr), fo = Math.round((v.fadeOutMs || 0) / 1000 * sr);
        if (fi + fo > L) {
          var scv = L / (fi + fo);
          fi = Math.floor(fi * scv); fo = Math.floor(fo * scv);
          warnings.push('stem ' + v.id + ': fades samen langer dan het stemfragment; evenredig ingekort');
        }
        if (fi > 0) fadeInGain(g, fi);
        if (fo > 0) fadeOutGain(g, L, fo);
        var RC = raw.length;
        for (c = 0; c < C; c++) {
          var rc = raw[c % RC], dst = out[c], st = p.start;
          for (k = 0; k < L; k++) dst[st + k] += rc[k] * g[k];
        }
      });
      if (out[0].length !== total) throw new Error('Interne fout: lengte-invariant geschonden in blok ' + block.id + ' na het stemspoor');
      var peak = 0;
      for (c = 0; c < C; c++) { var oc = out[c]; for (k = 0; k < total; k++) { var av = oc[k] < 0 ? -oc[k] : oc[k]; if (av > peak) peak = av; } }
      var clipped = peak > 1;
      if (clipped) warnings.push('blok ' + q(block.name) + ': piek ' + linToDb(peak) + ' dBFS boven 0 dBFS; bij export begrensd op ±1,0 (overweeg gainDb lager of normalize)');
      report(1, 'klaar');
      return { sampleRate: sr, channels: out, stats: { duration: total / sr, samples: total, peakDb: linToDb(peak), clipped: clipped }, peak: peak, warnings: warnings };
    }
    return { render: render };
  })();

  // Pure render-API voor de achtergrondthread (contract fase 2). Geen interne state.
  function renderBlockPure(recipe, audio, blockId, opts) {
    var nr = Validator.normalizeRecipe(recipe);
    if (nr.problems.length) fail('recept bevat ' + nr.problems.length + ' probleem/problemen: ' + nr.problems.join('; '));
    var block = findBlock(nr.recipe, blockId);
    var r = Renderer.render(nr.recipe, audio || {}, block, opts || {});
    return { sampleRate: r.sampleRate, channels: r.channels, stats: r.stats, warnings: r.warnings };
  }

  /* =====================================================================
   * Generator- en meethulpen (selftest)
   * ===================================================================== */
  function mulberry32(a) {
    return function () {
      a |= 0; a = a + 0x6D2B79F5 | 0;
      var t = Math.imul(a ^ a >>> 15, 1 | a);
      t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t;
      return ((t ^ t >>> 14) >>> 0) / 4294967296;
    };
  }
  function genSine(freq, dur, sr, amp, phase) {
    var n = Math.round(dur * sr), o = new Float32Array(n), w = 2 * Math.PI * freq / sr;
    for (var i = 0; i < n; i++) o[i] = amp * Math.sin(w * i + (phase || 0));
    return o;
  }
  // Frequentie via stijgende nuldoorgangen met lineaire interpolatie over het middengedeelte.
  function measureFreq(x, sr, fromFrac, toFrac) {
    var a = Math.floor(x.length * (fromFrac == null ? 0.25 : fromFrac)), b = Math.floor(x.length * (toFrac == null ? 0.75 : toFrac));
    var first = -1, last = -1, count = 0;
    for (var i = a + 1; i < b; i++) {
      if (x[i - 1] < 0 && x[i] >= 0) {
        var tc = (i - 1) + (-x[i - 1]) / (x[i] - x[i - 1]);
        if (first < 0) first = tc;
        last = tc; count++;
      }
    }
    if (count < 2) return 0;
    return (count - 1) * sr / (last - first);
  }

  /* =====================================================================
   * Instantie: Store, Cutter, Renderer, Commands
   * ===================================================================== */
  function createCore() {

    /* ------------------------------ Store ------------------------------ */
    var Store = {
      recipe: Validator.newRecipe(CONSTANTS.DEFAULT_SAMPLE_RATE),
      audio: {},                 // naam → { sampleRate, channels, version }
      audioVersion: 0,
      undo: [], redo: [],
      listeners: [],
      counters: { b: 0, f: 0, v: 0 }
    };
    function audioNames() { return Object.keys(Store.audio); }
    function notify() {
      var snap = clone(Store.recipe);
      Store.listeners.slice().forEach(function (fn) { try { fn(snap); } catch (e) { /* luisteraarsfouten negeren */ } });
    }
    // Runtime-audio is geen onderdeel van undo: na het terugzetten van een
    // snapshot worden geladen bronnen (en de werkfrequentie) weer bijgewerkt.
    function reconcile(recipe) {
      recipe.sampleRate = Store.recipe ? Store.recipe.sampleRate : recipe.sampleRate;
      audioNames().forEach(function (name) {
        var a = Store.audio[name];
        var dur = roundT(a.channels[0].length / a.sampleRate);
        var s = findSourceIn(recipe, name);
        if (!s) recipe.sources.push({ name: name, duration: dur, channels: a.channels.length, markers: [] });
        else { s.duration = dur; s.channels = a.channels.length; }
      });
      return recipe;
    }
    function bumpCounters(recipe) {
      recipe.blocks.forEach(function (b) {
        var mb = /^b(\d+)$/.exec(b.id); if (mb) Store.counters.b = Math.max(Store.counters.b, +mb[1]);
        b.fragments.forEach(function (f) { var mf = /^f(\d+)$/.exec(f.id); if (mf) Store.counters.f = Math.max(Store.counters.f, +mf[1]); });
        (b.voice || []).forEach(function (v) { var mv = /^v(\d+)$/.exec(v.id); if (mv) Store.counters.v = Math.max(Store.counters.v, +mv[1]); });
      });
    }
    function nextId(kind, recipe) {
      bumpCounters(recipe);
      Store.counters[kind]++;
      return kind + Store.counters[kind];
    }
    function commit(beforeJSON, newRecipe) {
      Store.undo.push(beforeJSON);
      if (Store.undo.length > CONSTANTS.UNDO_MAX) Store.undo.shift();
      Store.redo = [];
      Store.recipe = newRecipe;
      notify();
    }

    /* ------------------------------ Cutter ----------------------------- */
    function findSourceIn(recipe, name) {
      for (var i = 0; i < recipe.sources.length; i++) if (recipe.sources[i].name === name) return recipe.sources[i];
      return null;
    }
    function getSource(recipe, name) {
      var s = findSourceIn(recipe, name);
      if (!s) {
        var names = recipe.sources.map(function (x) { return x.name; });
        fail('bron ' + q(name) + ' bestaat niet; ' + (names.length ? 'bekende bronnen: ' + names.join(', ') : 'er zijn nog geen bronnen geladen'));
      }
      return s;
    }
    function markerList(src) {
      if (!src.markers.length) return 'deze bron heeft nog geen markers';
      return 'bestaande markers: ' + src.markers.map(function (m) { return '@' + m.name + ' (' + fmt(m.time) + ' s)'; }).join(', ');
    }
    // Zet een getal of "@marker" om naar seconden en controleert het bereik.
    function resolveTime(src, value, argName) {
      var t;
      if (typeof value === 'string') {
        if (value.charAt(0) !== '@') fail(argName + ' moet een getal (seconden) of "@markernaam" zijn; kreeg ' + q(value));
        var nm = value.slice(1), mk = null;
        for (var i = 0; i < src.markers.length; i++) if (src.markers[i].name === nm) { mk = src.markers[i]; break; }
        if (!mk) fail('marker ' + q(value) + ' bestaat niet in ' + src.name + '; ' + markerList(src));
        t = mk.time;
      } else if (typeof value === 'number' && isFinite(value)) t = value;
      else fail(argName + ' moet een getal (seconden) of "@markernaam" zijn');
      if (t < 0) fail(argName + ' ' + fmt(t) + ' ligt vóór het begin van ' + src.name + ' (0 s)');
      if (t > src.duration + 1e-9) fail(argName + ' ' + fmt(t) + ' ligt voorbij het einde van ' + src.name + ' (' + fmt(src.duration) + ' s)');
      return t;
    }
    function snapToZero(sourceName, timeSec) {
      var a = Store.audio[sourceName];
      if (!a || typeof timeSec !== 'number' || !isFinite(timeSec)) return timeSec;
      var sr = a.sampleRate, ch = a.channels, C = ch.length, len = ch[0].length;
      var n0 = Math.round(timeSec * sr), W = Math.round(CONSTANTS.SNAP_WINDOW_MS / 1000 * sr);
      if (n0 < 0 || n0 > len) return timeSec;
      function mono(n) { var s = 0; for (var c = 0; c < C; c++) s += ch[c][n]; return s; }
      function crossAt(n) { // tekenwissel tussen n-1 en n
        if (n < 1 || n >= len) return -1;
        var x0 = mono(n - 1), x1 = mono(n);
        if ((x0 < 0 && x1 >= 0) || (x0 > 0 && x1 <= 0) || x0 === 0) return Math.abs(x1) < Math.abs(x0) ? n : n - 1;
        return -1;
      }
      for (var dd = 0; dd <= W; dd++) {
        var hit = crossAt(n0 + dd);
        if (hit < 0 && dd > 0) hit = crossAt(n0 - dd + 1);
        if (hit >= 0) return roundT(hit / sr);
      }
      return timeSec;
    }

    /* --------------------------- Opzoekhulpen -------------------------- */
    var getBlock = findBlock;
    function getFragment(recipe, id) {
      for (var i = 0; i < recipe.blocks.length; i++) {
        var b = recipe.blocks[i];
        for (var j = 0; j < b.fragments.length; j++) if (b.fragments[j].id === id) return { block: b, index: j, fragment: b.fragments[j] };
      }
      var list = [];
      recipe.blocks.forEach(function (b) { b.fragments.forEach(function (f) { list.push(f.id + ' (' + b.name + ')'); }); });
      fail('fragment ' + q(id) + ' bestaat niet; ' + (list.length ? 'bestaande fragmenten: ' + list.join(', ') : 'er zijn nog geen fragmenten'));
    }
    function insertFragment(recipe, block, frag, position) {
      var len = block.fragments.length;
      var p = (position == null) ? len : position;
      if (p < 0 || p > len) fail('position ' + p + ' ligt buiten het blok; kies 0 t/m ' + len);
      block.fragments.splice(p, 0, frag);
      if (len >= 1) block.transitions.splice(Math.min(p, len - 1), 0, Validator.defaultTransition(recipe.defaults));
      return p;
    }
    function removeFragmentAt(block, idx) {
      var len = block.fragments.length;
      block.fragments.splice(idx, 1);
      if (len > 1) block.transitions.splice(idx === 0 ? 0 : idx - 1, 1);
    }
    function checkFragmentSpan(src, tin, tout) {
      if (!(tout > tin)) fail('in (' + fmt(tin) + ') moet vóór out (' + fmt(tout) + ') liggen');
      var durMs = (tout - tin) * 1000;
      if (durMs < CONSTANTS.MIN_FRAGMENT_MS - 1e-9) fail('fragment is ' + (Math.round(durMs * 10) / 10) + ' ms lang; minimaal ' + CONSTANTS.MIN_FRAGMENT_MS + ' ms');
    }
    function getVoice(recipe, id) {
      for (var i = 0; i < recipe.blocks.length; i++) {
        var b = recipe.blocks[i], vs = b.voice || [];
        for (var j = 0; j < vs.length; j++) if (vs[j].id === id) return { block: b, index: j, voice: vs[j] };
      }
      var list = [];
      recipe.blocks.forEach(function (b) { (b.voice || []).forEach(function (v) { list.push(v.id + ' (' + b.name + ')'); }); });
      fail('stem ' + q(id) + ' bestaat niet; ' + (list.length ? 'bestaande stemfragmenten: ' + list.join(', ') : 'er zijn nog geen stemfragmenten; voeg er een toe met add_voice'));
    }
    function checkAnchor(block, anchor) {
      var ids = block.fragments.map(function (f) { return f.id; });
      if (ids.indexOf(anchor) < 0) {
        fail('anker ' + q(anchor) + ' is geen muziekfragment van blok ' + q(block.name) + ' (' + block.id + '); ' +
          (ids.length ? 'kies uit: ' + ids.join(', ') : 'voeg eerst een muziekfragment toe met add_fragment'));
      }
    }
    function voicesOnAnchor(block, fragId) {
      return (block.voice || []).filter(function (v) { return v.anchor === fragId; }).map(function (v) { return v.id; });
    }
    // Resultaat van add_voice/update_voice: { id, in, out, start, end } (+ warnings over deze stem, indien aanwezig).
    function voiceResult(block, v) {
      var rep = computeFit(block), e = null;
      rep.voice.forEach(function (x) { if (x.id === v.id) e = x; });
      var res = { id: v.id, 'in': v['in'], out: v.out, start: e ? e.start : null, end: e ? e.end : null };
      var re = new RegExp('\\b' + v.id + '\\b');
      var w = rep.ok ? rep.warnings.filter(function (x) { return re.test(x); }) : [rep.error];
      if (w.length) res.warnings = w;
      return res;
    }

    /* ----------------------------- Renderer ---------------------------- */
    // Zelfde pure Renderer als renderBlockPure; hier met de cache en audio van de instantie.
    var renderCache = new Map();
    var rendered = {};
    function renderBlock(recipe, block, onProgress) {
      var r = Renderer.render(recipe, Store.audio, block, { cache: renderCache, onProgress: onProgress });
      rendered[block.id] = { sampleRate: r.sampleRate, channels: r.channels };
      return { block: block.id, duration: r.stats.duration, samples: r.stats.samples, channels: r.channels.length,
        peakDb: r.stats.peakDb, peak: r.peak, clipped: r.stats.clipped, warnings: r.warnings };
    }

    /* ----------------------------- Commands ---------------------------- */
    var T_TIME = { type: ['number', 'string'], description: 'seconden of "@markernaam"' };
    var T_REF = { type: 'string', minLength: 1 };
    var T_OFFSET = { type: 'number', description: 'seconden na het begin van het anker in de uitvoer (mag negatief)' };
    var T_VTD = { type: ['number', 'null'], exclusiveMinimum: 0, description: 'doelduur van de stem in seconden; null = oorspronkelijke snelheid' };
    var T_DUCKDB = { type: 'number', maximum: 0, description: 'ducking van de muziek onder de stem in dB (≤ 0; 0 = uit)' };
    var T_DUCKMS = { type: 'number', minimum: 0, maximum: CONSTANTS.VOICE_DUCK_MAX_MS, description: 'duur van de ducking-hellingen in ms' };
    function obj(props, required) { return { type: 'object', properties: props, required: required || [], additionalProperties: false }; }

    var commands = [
      { name: 'get_state', description: 'Geeft het volledige recept (zonder audiodata).', schema: obj({}),
        run: function (a, ctx) { return clone(ctx.recipe); } },

      { name: 'list_sources', description: 'Bronnen met duur, kanalen, markers en of de audio geladen is.', schema: obj({}),
        run: function (a, ctx) {
          return ctx.recipe.sources.map(function (s) {
            return { name: s.name, duration: s.duration, channels: s.channels, loaded: !!Store.audio[s.name], markers: clone(s.markers) };
          });
        } },

      { name: 'add_marker', description: 'Plaatst een benoemde marker in een bron. Lege naam → m1, m2, …',
        schema: obj({ source: T_REF, name: { type: 'string', description: 'markernaam (uniek per bron)' }, time: T_TIME }, ['source', 'time']),
        run: function (a, ctx) {
          var s = getSource(ctx.recipe, a.source);
          var t = roundT(resolveTime(s, a.time, 'time'));
          var name = (a.name || '').replace(/^@/, '').trim();
          if (!name) { var k = 1; while (s.markers.some(function (m) { return m.name === 'm' + k; })) k++; name = 'm' + k; }
          if (s.markers.some(function (m) { return m.name === name; })) fail('marker ' + q(name) + ' bestaat al in ' + s.name + '; kies een andere naam of gebruik update_marker; ' + markerList(s));
          s.markers.push({ name: name, time: t });
          s.markers.sort(function (x, y) { return x.time - y.time; });
          return { name: name, time: t };
        } },

      { name: 'update_marker', description: 'Hernoemt en/of verplaatst een marker. Bestaande fragmenten veranderen niet.',
        schema: obj({ source: T_REF, name: T_REF, newName: { type: 'string', minLength: 1 }, time: T_TIME }, ['source', 'name']),
        run: function (a, ctx) {
          var s = getSource(ctx.recipe, a.source);
          var nm = a.name.replace(/^@/, ''), mk = null;
          s.markers.forEach(function (m) { if (m.name === nm) mk = m; });
          if (!mk) fail('marker ' + q('@' + nm) + ' bestaat niet in ' + s.name + '; ' + markerList(s));
          if (a.time !== undefined) mk.time = roundT(resolveTime(s, a.time, 'time'));
          if (a.newName !== undefined) {
            var nn = a.newName.replace(/^@/, '').trim();
            if (!nn) fail('newName mag niet leeg zijn');
            if (nn !== nm && s.markers.some(function (m) { return m.name === nn; })) fail('marker ' + q(nn) + ' bestaat al in ' + s.name + '; ' + markerList(s));
            mk.name = nn;
          }
          s.markers.sort(function (x, y) { return x.time - y.time; });
          return { name: mk.name, time: mk.time };
        } },

      { name: 'delete_marker', description: 'Verwijdert een marker. Bestaande fragmenten veranderen niet.',
        schema: obj({ source: T_REF, name: T_REF }, ['source', 'name']),
        run: function (a, ctx) {
          var s = getSource(ctx.recipe, a.source), nm = a.name.replace(/^@/, '');
          var idx = -1; s.markers.forEach(function (m, i) { if (m.name === nm) idx = i; });
          if (idx < 0) fail('marker ' + q('@' + nm) + ' bestaat niet in ' + s.name + '; ' + markerList(s));
          s.markers.splice(idx, 1);
          return null;
        } },

      { name: 'create_block', description: 'Maakt een nieuw blok met een doelduur in seconden (3 decimalen). Standaard fitMode "manual": fragmenten houden hun originele lengte (manualDuration null) tot je ze rekt; geef fitMode "proportional" om alles automatisch gelijk te rekken tot de doelduur.',
        schema: obj({ name: { type: 'string' }, targetDuration: { type: 'number', exclusiveMinimum: 0, description: 'seconden' },
          fitMode: { type: 'string', 'enum': CONSTANTS.FIT_MODES }, duckDb: T_DUCKDB, duckMs: T_DUCKMS }, []),
        run: function (a, ctx) {
          var id = nextId('b', ctx.recipe);
          var td = a.targetDuration !== undefined ? a.targetDuration : ctx.recipe.defaults.blockDuration;
          var b = { id: id, name: a.name || ('Blok ' + (ctx.recipe.blocks.length + 1)), targetDuration: Math.round(td * 1000) / 1000,
            fitMode: a.fitMode || CONSTANTS.DEFAULT_FIT_MODE_NEW_BLOCK, duckDb: a.duckDb !== undefined ? a.duckDb : CONSTANTS.VOICE_DUCK_DEFAULT_DB,
            duckMs: a.duckMs !== undefined ? a.duckMs : CONSTANTS.VOICE_DUCK_DEFAULT_MS, fragments: [], transitions: [], voice: [] };
          if (!(b.targetDuration > 0)) fail('targetDuration moet groter dan 0 zijn (na afronden op 3 decimalen)');
          ctx.recipe.blocks.push(b);
          return { id: id };
        } },

      { name: 'update_block', description: 'Wijzigt naam, doelduur, fit-modus of ducking (duckDb ≤ 0, 0 = uit; duckMs) van een blok.',
        schema: obj({ block: T_REF, name: { type: 'string' }, targetDuration: { type: 'number', exclusiveMinimum: 0 },
          fitMode: { type: 'string', 'enum': CONSTANTS.FIT_MODES }, duckDb: T_DUCKDB, duckMs: T_DUCKMS }, ['block']),
        run: function (a, ctx) {
          var b = getBlock(ctx.recipe, a.block);
          if (a.name !== undefined) b.name = a.name;
          if (a.targetDuration !== undefined) { var td = Math.round(a.targetDuration * 1000) / 1000; if (!(td > 0)) fail('targetDuration moet groter dan 0 zijn'); b.targetDuration = td; }
          if (a.fitMode !== undefined) b.fitMode = a.fitMode;
          if (a.duckDb !== undefined) b.duckDb = a.duckDb;
          if (a.duckMs !== undefined) b.duckMs = a.duckMs;
          return null;
        } },

      { name: 'delete_block', description: 'Verwijdert een blok met zijn fragmenten en stemfragmenten.', schema: obj({ block: T_REF }, ['block']),
        run: function (a, ctx) {
          var b = getBlock(ctx.recipe, a.block);
          ctx.recipe.blocks.splice(ctx.recipe.blocks.indexOf(b), 1);
          delete rendered[b.id];
          return null;
        } },

      { name: 'add_fragment', description: 'Voegt een fragment (bron + in/uit) toe aan een blok. in/out: seconden of "@marker". Snap naar nuldoorgang standaard aan.',
        schema: obj({ block: T_REF, source: T_REF, 'in': T_TIME, out: T_TIME, position: { type: 'integer', minimum: 0 },
          snap: { type: 'boolean' }, mode: { type: 'string', 'enum': CONSTANTS.MODES }, preset: { type: 'string', 'enum': CONSTANTS.PRESETS },
          gainDb: { type: 'number' }, fadeInMs: { type: 'number', minimum: 0, maximum: CONSTANTS.FADE_MAX_MS },
          fadeOutMs: { type: 'number', minimum: 0, maximum: CONSTANTS.FADE_MAX_MS }, locked: { type: 'boolean' },
          manualDuration: { type: ['number', 'null'], exclusiveMinimum: 0 } }, ['block', 'source', 'in', 'out']),
        run: function (a, ctx) {
          var r = ctx.recipe, b = getBlock(r, a.block), s = getSource(r, a.source);
          var tin = resolveTime(s, a['in'], 'in'), tout = resolveTime(s, a.out, 'out');
          if (a.snap !== false) { tin = snapToZero(s.name, tin); tout = snapToZero(s.name, tout); }
          tin = roundT(tin); tout = roundT(tout);
          checkFragmentSpan(s, tin, tout);
          var f = { id: nextId('f', r), source: s.name, 'in': tin, out: tout, mode: a.mode || 'pitch', preset: a.preset || 'music',
            fadeInMs: a.fadeInMs !== undefined ? a.fadeInMs : r.defaults.fadeMs, fadeOutMs: a.fadeOutMs !== undefined ? a.fadeOutMs : r.defaults.fadeMs,
            gainDb: a.gainDb || 0, locked: !!a.locked, manualDuration: a.manualDuration !== undefined ? a.manualDuration : null };
          insertFragment(r, b, f, a.position);
          return { id: f.id, 'in': tin, out: tout };
        } },

      { name: 'update_fragment', description: 'Wijzigt instellingen van een fragment (in/out met snap, modus, preset, gain, fades, vergrendeling, handmatige duur).',
        schema: obj({ fragment: T_REF, 'in': T_TIME, out: T_TIME, snap: { type: 'boolean' }, mode: { type: 'string', 'enum': CONSTANTS.MODES },
          preset: { type: 'string', 'enum': CONSTANTS.PRESETS }, gainDb: { type: 'number' },
          fadeInMs: { type: 'number', minimum: 0, maximum: CONSTANTS.FADE_MAX_MS }, fadeOutMs: { type: 'number', minimum: 0, maximum: CONSTANTS.FADE_MAX_MS },
          locked: { type: 'boolean' }, manualDuration: { type: ['number', 'null'], exclusiveMinimum: 0 } }, ['fragment']),
        run: function (a, ctx) {
          var loc = getFragment(ctx.recipe, a.fragment), f = loc.fragment, s = getSource(ctx.recipe, f.source);
          var tin = f['in'], tout = f.out;
          if (a['in'] !== undefined) { tin = resolveTime(s, a['in'], 'in'); if (a.snap !== false) tin = snapToZero(s.name, tin); tin = roundT(tin); }
          if (a.out !== undefined) { tout = resolveTime(s, a.out, 'out'); if (a.snap !== false) tout = snapToZero(s.name, tout); tout = roundT(tout); }
          checkFragmentSpan(s, tin, tout);
          f['in'] = tin; f.out = tout;
          ['mode', 'preset', 'gainDb', 'fadeInMs', 'fadeOutMs', 'locked', 'manualDuration'].forEach(function (k) { if (a[k] !== undefined) f[k] = a[k]; });
          return { id: f.id, 'in': f['in'], out: f.out };
        } },

      { name: 'move_fragment', description: 'Verplaatst een fragment naar een andere positie (optioneel in een ander blok).',
        schema: obj({ fragment: T_REF, position: { type: 'integer', minimum: 0 }, block: T_REF }, ['fragment', 'position']),
        run: function (a, ctx) {
          var r = ctx.recipe, loc = getFragment(r, a.fragment);
          var dest = a.block !== undefined ? getBlock(r, a.block) : loc.block;
          var maxPos = dest === loc.block ? dest.fragments.length - 1 : dest.fragments.length;
          if (a.position > maxPos) fail('position ' + a.position + ' ligt buiten het blok; kies 0 t/m ' + maxPos);
          if (dest !== loc.block) {
            var vOn = voicesOnAnchor(loc.block, loc.fragment.id);
            if (vOn.length) fail('fragment ' + loc.fragment.id + ' is het anker van stem ' + vOn.join(', ') + ' en kan niet naar een ander blok; ' +
              'verwijder die stem(men) eerst (delete_voice) of hang ze aan een ander fragment (update_voice met anchor)');
          }
          removeFragmentAt(loc.block, loc.index);
          insertFragment(r, dest, loc.fragment, a.position);
          return null;
        } },

      { name: 'duplicate_fragment', description: 'Dupliceert een fragment direct erachter.', schema: obj({ fragment: T_REF }, ['fragment']),
        run: function (a, ctx) {
          var r = ctx.recipe, loc = getFragment(r, a.fragment);
          var f = clone(loc.fragment); f.id = nextId('f', r);
          insertFragment(r, loc.block, f, loc.index + 1);
          return { id: f.id };
        } },

      { name: 'delete_fragment', description: 'Verwijdert een muziekfragment (en de bijbehorende overgang). Geweigerd als het fragment anker van een stem is.',
        schema: obj({ fragment: T_REF }, ['fragment']),
        run: function (a, ctx) {
          var loc = getFragment(ctx.recipe, a.fragment);
          var vOn = voicesOnAnchor(loc.block, loc.fragment.id);
          if (vOn.length) fail('fragment ' + loc.fragment.id + ' is het anker van stem ' + vOn.join(', ') + '; verwijder eerst die stem(men) (delete_voice) ' +
            'of hang ze aan een ander fragment (update_voice met anchor)');
          removeFragmentAt(loc.block, loc.index);
          return null;
        } },

      { name: 'add_voice', description: 'Voegt een stemfragment (voice-over) toe dat bovenop de muziek klinkt. Start in het blok = uitvoerstart van het anker (muziekfragment-id) + offset (s, mag negatief); schuift mee met de fit. targetDuration null = oorspronkelijke snelheid. Resultaat: id, werkelijke in/uit, start/end in het blok.',
        schema: obj({ block: T_REF, source: T_REF, 'in': T_TIME, out: T_TIME, anchor: { type: 'string', minLength: 1, description: 'id van een muziekfragment in dit blok' },
          offset: T_OFFSET, targetDuration: T_VTD, snap: { type: 'boolean' },
          preset: { type: 'string', 'enum': CONSTANTS.PRESETS }, mode: { type: 'string', 'enum': CONSTANTS.MODES }, gainDb: { type: 'number' },
          fadeInMs: { type: 'number', minimum: 0, maximum: CONSTANTS.FADE_MAX_MS }, fadeOutMs: { type: 'number', minimum: 0, maximum: CONSTANTS.FADE_MAX_MS },
          acceptOverlap: { type: 'boolean' } }, ['block', 'source', 'in', 'out', 'anchor']),
        run: function (a, ctx) {
          var r = ctx.recipe, b = getBlock(r, a.block), s = getSource(r, a.source);
          checkAnchor(b, a.anchor);
          var tin = resolveTime(s, a['in'], 'in'), tout = resolveTime(s, a.out, 'out');
          if (a.snap !== false) { tin = snapToZero(s.name, tin); tout = snapToZero(s.name, tout); }
          tin = roundT(tin); tout = roundT(tout);
          checkFragmentSpan(s, tin, tout);
          var v = { id: nextId('v', r), source: s.name, 'in': tin, out: tout, anchor: a.anchor, offset: roundT(a.offset || 0),
            targetDuration: a.targetDuration != null ? roundT(a.targetDuration) : null,
            mode: a.mode || 'pitch', preset: a.preset || CONSTANTS.VOICE_DEFAULT_PRESET, gainDb: a.gainDb || 0,
            fadeInMs: a.fadeInMs !== undefined ? a.fadeInMs : r.defaults.fadeMs, fadeOutMs: a.fadeOutMs !== undefined ? a.fadeOutMs : r.defaults.fadeMs,
            acceptOverlap: !!a.acceptOverlap };
          if (!b.voice) b.voice = [];
          b.voice.push(v);
          return voiceResult(b, v);
        } },

      { name: 'update_voice', description: 'Wijzigt een stemfragment: in/out (met snap), anchor, offset, targetDuration (null = oorspronkelijke snelheid), acceptOverlap, gain, fades, preset, mode.',
        schema: obj({ voice: T_REF, 'in': T_TIME, out: T_TIME, snap: { type: 'boolean' }, anchor: { type: 'string', minLength: 1 }, offset: T_OFFSET,
          targetDuration: T_VTD, acceptOverlap: { type: 'boolean' }, gainDb: { type: 'number' },
          fadeInMs: { type: 'number', minimum: 0, maximum: CONSTANTS.FADE_MAX_MS }, fadeOutMs: { type: 'number', minimum: 0, maximum: CONSTANTS.FADE_MAX_MS },
          preset: { type: 'string', 'enum': CONSTANTS.PRESETS }, mode: { type: 'string', 'enum': CONSTANTS.MODES } }, ['voice']),
        run: function (a, ctx) {
          var loc = getVoice(ctx.recipe, a.voice), v = loc.voice, s = getSource(ctx.recipe, v.source);
          var tin = v['in'], tout = v.out;
          if (a['in'] !== undefined) { tin = resolveTime(s, a['in'], 'in'); if (a.snap !== false) tin = snapToZero(s.name, tin); tin = roundT(tin); }
          if (a.out !== undefined) { tout = resolveTime(s, a.out, 'out'); if (a.snap !== false) tout = snapToZero(s.name, tout); tout = roundT(tout); }
          checkFragmentSpan(s, tin, tout);
          if (a.anchor !== undefined) checkAnchor(loc.block, a.anchor);
          v['in'] = tin; v.out = tout;
          if (a.anchor !== undefined) v.anchor = a.anchor;
          if (a.offset !== undefined) v.offset = roundT(a.offset);
          if (a.targetDuration !== undefined) v.targetDuration = a.targetDuration === null ? null : roundT(a.targetDuration);
          ['acceptOverlap', 'gainDb', 'fadeInMs', 'fadeOutMs', 'preset', 'mode'].forEach(function (k) { if (a[k] !== undefined) v[k] = a[k]; });
          return voiceResult(loc.block, v);
        } },

      { name: 'delete_voice', description: 'Verwijdert een stemfragment.', schema: obj({ voice: T_REF }, ['voice']),
        run: function (a, ctx) { var loc = getVoice(ctx.recipe, a.voice); loc.block.voice.splice(loc.index, 1); return null; } },

      { name: 'set_transition', description: 'Stelt de overgang in tussen fragment index en index+1: type, ms (overlap of stilte), curve, duckDb.',
        schema: obj({ block: T_REF, index: { type: 'integer', minimum: 0 }, type: { type: 'string', 'enum': CONSTANTS.TRANSITION_TYPES },
          ms: { type: 'number', minimum: 0 }, curve: { type: 'string', 'enum': CONSTANTS.CURVES }, duckDb: { type: 'number', maximum: 0 } }, ['block', 'index', 'type']),
        run: function (a, ctx) {
          var r = ctx.recipe, b = getBlock(r, a.block);
          var nT = b.transitions.length;
          if (!nT) fail('blok ' + q(b.name) + ' heeft ' + b.fragments.length + ' fragment(en) en dus nog geen overgangen');
          if (a.index >= nT) fail('index ' + a.index + ' bestaat niet; blok ' + q(b.name) + ' heeft overgangen 0 t/m ' + (nT - 1));
          var old = b.transitions[a.index];
          var t = Validator.defaultTransition(r.defaults, a.type);
          if (old && old.type === a.type) { t = clone(old); }
          else if (old && a.type !== 'cut' && typeof old.ms === 'number' && old.ms > 0 && (isOverlapType(a.type) === isOverlapType(old.type))) t.ms = old.ms;
          if (a.ms !== undefined) { if (a.type === 'cut' && a.ms !== 0) fail('type cut heeft geen duur; laat ms weg of kies gap/crossfade'); t.ms = a.ms; }
          if (a.curve !== undefined) { if (a.type === 'cut' || a.type === 'gap') fail('curve geldt alleen voor overlappende overgangen, niet voor ' + a.type); t.curve = a.curve; }
          if (a.duckDb !== undefined) { if (a.type !== 'duck') fail('duckDb geldt alleen voor type duck'); t.duckDb = a.duckDb; }
          b.transitions[a.index] = t;
          return clone(t);
        } },

      { name: 'fit_report', description: 'Fit-rapport: per fragment brondur, doeldur, ratio, status en start/end in het blok; per overgang start/end; stemmen (voice), ducking-zones (duck), stem-overlaps; per blok of het klopt of de afwijking.',
        schema: obj({ block: T_REF }, []),
        run: function (a, ctx) {
          var blocks = a.block !== undefined ? [getBlock(ctx.recipe, a.block)] : ctx.recipe.blocks;
          return blocks.map(function (b) {
            var rep = computeFit(b);
            rep.missingSources = b.fragments.concat(b.voice || []).map(function (f) { return f.source; })
              .filter(function (x, i, arr) { return arr.indexOf(x) === i && !Store.audio[x]; });
            return rep;
          });
        } },

      { name: 'render_block', description: 'Rendert een blok (fit + stretch + overgangen + stemspoor met ducking). Resultaat: duur, samples, piek in dBFS, clipping.',
        schema: obj({ block: T_REF }, ['block']),
        run: function (a, ctx) {
          var r = renderBlock(ctx.recipe, getBlock(ctx.recipe, a.block));
          return { duration: r.duration, samples: r.samples, channels: r.channels, peakDb: r.peakDb, clipped: r.clipped, warnings: r.warnings };
        } },

      { name: 'export_block', description: 'Rendert en codeert blok(ken) als 16-bit WAV. Leeg = alle blokken. normalize: piek naar −1 dBFS.',
        schema: obj({ block: T_REF, normalize: { type: 'boolean' } }, []),
        run: function (a, ctx) {
          var blocks = a.block !== undefined ? [getBlock(ctx.recipe, a.block)] : ctx.recipe.blocks;
          if (!blocks.length) fail('er zijn nog geen blokken om te exporteren');
          var files = [], warnings = [];
          blocks.forEach(function (b) {
            var r = renderBlock(ctx.recipe, b);
            var chans = rendered[b.id].channels;
            if (a.normalize && r.peak > 0) {
              var gn = dbToLin(CONSTANTS.NORMALIZE_PEAK_DB) / r.peak;
              chans = chans.map(function (ch) { var o = new Float32Array(ch.length); for (var i = 0; i < ch.length; i++) o[i] = ch[i] * gn; return o; });
            } else if (r.clipped) warnings.push('blok ' + q(b.name) + ': clipping — pieken begrensd op ±1,0');
            r.warnings.forEach(function (w) { warnings.push(w); });
            var name = (b.name + '_' + b.targetDuration.toFixed(3) + 's.wav').replace(/[\\\/:*?"<>|]/g, '_');
            files.push({ name: name, bytes: WavCodec.encode(chans, ctx.recipe.sampleRate), block: b.id, samples: r.samples,
              peakDb: a.normalize && r.peak > 0 ? CONSTANTS.NORMALIZE_PEAK_DB : r.peakDb, clipped: !a.normalize && r.clipped });
          });
          return { files: files, warnings: warnings };
        } },

      { name: 'import_recipe', description: 'Vervangt het project door een recept (object of JSON-tekst). Resultaat: aantal blokken en ontbrekende bronnen.',
        schema: obj({ recipe: { type: ['object', 'string'] } }, ['recipe']),
        run: function (a, ctx) {
          var nr = Validator.normalizeRecipe(a.recipe);
          if (nr.problems.length) fail('recept bevat ' + nr.problems.length + ' probleem/problemen: ' + nr.problems.join('; '));
          var rec = nr.recipe, warnings = [];
          if (audioNames().length && rec.sampleRate !== ctx.recipe.sampleRate) {
            warnings.push('sampleRate van het recept (' + rec.sampleRate + ') wijkt af van de werkfrequentie (' + ctx.recipe.sampleRate + '); werkfrequentie aangehouden');
          }
          if (audioNames().length) rec.sampleRate = ctx.recipe.sampleRate;
          var missing = rec.sources.filter(function (s) { return !Store.audio[s.name]; }).map(function (s) { return s.name; });
          // Afwijkende duur van een al geladen bron melden (koppeling op bestandsnaam).
          rec.sources.forEach(function (s) {
            var au = Store.audio[s.name];
            if (au) { var dd = au.channels[0].length / au.sampleRate; if (Math.abs(dd - s.duration) > 0.01) warnings.push('bron ' + s.name + ': duur in recept ' + fmt(s.duration) + ' s, geladen bestand ' + fmt(dd) + ' s'); }
          });
          reconcile(rec);
          bumpCounters(rec);
          ctx.recipe = rec;
          return { blocks: rec.blocks.length, missingSources: missing, warnings: warnings };
        } },

      { name: 'undo', description: 'Maakt de laatste wijziging ongedaan (max. ' + CONSTANTS.UNDO_MAX + ' stappen).', schema: obj({}), special: true },
      { name: 'redo', description: 'Voert de laatst ongedaan gemaakte wijziging opnieuw uit.', schema: obj({}), special: true }
    ];
    var cmdMap = {};
    commands.forEach(function (c) { cmdMap[c.name] = c; });

    function lookup(c) {
      if (!c || typeof c !== 'object' || Array.isArray(c)) fail('commando moet een object {cmd, args} zijn');
      if (typeof c.cmd !== 'string') fail('commando mist het veld cmd (tekst); beschikbaar: ' + Object.keys(cmdMap).join(', '));
      var def = cmdMap[c.cmd];
      if (!def) {
        var extra = (c.cmd === 'play' || c.cmd === 'stop') ? ' (' + c.cmd + ' bestaat alleen in de editor)' : '';
        fail('onbekend commando ' + q(c.cmd) + extra + '; beschikbaar: ' + Object.keys(cmdMap).join(', '));
      }
      var args = c.args === undefined || c.args === null ? {} : c.args;
      if (Validator.typeOf(args) !== 'object') fail('args van ' + c.cmd + ' moet een object zijn');
      Validator.check(args, def.schema, '', c.cmd);
      return { def: def, args: args };
    }

    function doHistory(kind) {
      var from = kind === 'undo' ? Store.undo : Store.redo, to = kind === 'undo' ? Store.redo : Store.undo;
      if (!from.length) fail(kind === 'undo' ? 'niets om ongedaan te maken' : 'niets om opnieuw uit te voeren');
      to.push(JSON.stringify(Store.recipe));
      if (to.length > CONSTANTS.UNDO_MAX) to.shift();
      Store.recipe = reconcile(JSON.parse(from.pop()));
      bumpCounters(Store.recipe);
      notify();
      return { undo: Store.undo.length, redo: Store.redo.length };
    }

    function exec(c) {
      try {
        var L = lookup(c);
        if (L.def.special) return { ok: true, result: doHistory(L.def.name) };
        var before = JSON.stringify(Store.recipe);
        var ctx = { recipe: JSON.parse(before) };
        var result = L.def.run(L.args, ctx);
        var after = JSON.stringify(ctx.recipe);
        if (after !== before) commit(before, ctx.recipe);
        return { ok: true, result: result === undefined ? null : result };
      } catch (e) {
        return { ok: false, error: e && e.afUser ? e.message : 'Interne fout: ' + msg(e) };
      }
    }

    function execAll(list) {
      var results = [];
      try {
        if (!Array.isArray(list)) return { ok: false, results: results, error: 'execAll verwacht een lijst (array) van commando\'s {cmd, args}', failedIndex: 0 };
        var before = JSON.stringify(Store.recipe);
        var ctx = { recipe: JSON.parse(before) };
        for (var i = 0; i < list.length; i++) {
          try {
            var L = lookup(list[i]);
            if (L.def.special) fail(L.def.name + ' kan niet binnen een reeks (execAll); voer het los uit');
            var r = L.def.run(L.args, ctx);
            results.push(r === undefined ? null : r);
          } catch (e) {
            var cmdName = list[i] && list[i].cmd ? list[i].cmd : '?';
            return { ok: false, results: results, failedIndex: i,
              error: 'commando ' + (i + 1) + ' (' + cmdName + '): ' + (e && e.afUser ? e.message : 'Interne fout: ' + msg(e)) + ' — niets uitgevoerd, alles teruggedraaid' };
          }
        }
        var after = JSON.stringify(ctx.recipe);
        if (after !== before) commit(before, ctx.recipe);
        return { ok: true, results: results };
      } catch (e2) {
        return { ok: false, results: results, error: 'Interne fout: ' + msg(e2), failedIndex: results.length };
      }
    }

    function describe() {
      return commands.map(function (c) { return { name: c.name, description: c.description, schema: clone(c.schema) }; });
    }

    /* --------------------------- Runtime-audio -------------------------- */
    function attachAudio(name, audio, opts) {
      opts = opts || {};
      if (typeof name !== 'string' || !name) throw userError('attachAudio: naam ontbreekt');
      if (!audio || !Array.isArray(audio.channels) || !audio.channels.length) throw userError('attachAudio(' + name + '): channels moet een niet-lege lijst Float32Array zijn');
      if (!(audio.sampleRate > 0)) throw userError('attachAudio(' + name + '): sampleRate ontbreekt');
      var chans = audio.channels.map(function (ch) { return ch instanceof Float32Array ? ch : Float32Array.from(ch); });
      var len = chans[0].length;
      chans.forEach(function (ch) { if (ch.length !== len) throw userError('attachAudio(' + name + '): kanalen zijn niet even lang'); });
      if (!len) throw userError('attachAudio(' + name + '): bestand bevat geen samples');
      var used = name;
      if (opts.rename && Store.audio[name]) { var k = 2; while (Store.audio[name + ' (' + k + ')'] || findSourceIn(Store.recipe, name + ' (' + k + ')')) k++; used = name + ' (' + k + ')'; }
      var sr = Store.recipe.sampleRate;
      if (audio.sampleRate !== sr) chans = Resampler.resample(chans, Math.round(len * sr / audio.sampleRate));
      Store.audio[used] = { sampleRate: sr, channels: chans, version: ++Store.audioVersion };
      reconcile(Store.recipe);
      notify();
      return used;
    }
    function detachAudio(name) { if (Store.audio[name]) { delete Store.audio[name]; notify(); return true; } return false; }
    function getAudio(name) { var a = Store.audio[name]; return a ? { sampleRate: a.sampleRate, channels: a.channels } : null; }
    function setSampleRate(sr) {
      if (!(Number.isInteger(sr) && sr >= 8000 && sr <= 384000)) throw userError('setSampleRate: verwacht een geheel getal tussen 8000 en 384000; kreeg ' + sr);
      if (sr === Store.recipe.sampleRate) return sr;
      if (audioNames().length) throw userError('setSampleRate: kan de werkfrequentie niet wijzigen naar ' + sr + ' Hz; er is al audio gekoppeld op ' + Store.recipe.sampleRate + ' Hz');
      Store.recipe.sampleRate = sr;
      notify();
      return sr;
    }
    function subscribe(fn) {
      Store.listeners.push(fn);
      return function () { var i = Store.listeners.indexOf(fn); if (i >= 0) Store.listeners.splice(i, 1); };
    }
    function getRendered(blockId) { var r = rendered[blockId]; return r ? { sampleRate: r.sampleRate, channels: r.channels } : null; }

    /* ----------------------------- Publiek ----------------------------- */
    var api = {
      version: VERSION,
      CONSTANTS: CONSTANTS,
      exec: exec,
      execAll: execAll,
      describe: describe,
      subscribe: subscribe,
      attachAudio: attachAudio,
      detachAudio: detachAudio,
      getAudio: getAudio,
      setSampleRate: setSampleRate,
      getRendered: getRendered,
      snapToZero: snapToZero,
      history: function () { return { undo: Store.undo.length, redo: Store.redo.length }; },
      validateRecipe: function (r) { var nr = Validator.normalizeRecipe(r); return { ok: !nr.problems.length, problems: nr.problems }; },
      computeFit: function (block) { return computeFit(block); },
      renderBlockPure: renderBlockPure,
      wav: { encode: WavCodec.encode, decode: WavCodec.decode },
      stretch: function (channels, outLength, opts) { return Stretch.run(channels, outLength, opts).channels; },
      stretchInfo: function (channels, outLength, opts) { return Stretch.run(channels, outLength, opts); },
      wsolaParams: Stretch.params,
      resample: Resampler.resample,
      ratioStatus: ratioStatus,
      create: createCore,
      selftest: selftest
    };
    return api;
  }

  /* =====================================================================
   * Zelftest — acceptatiecriteria 2 t/m 7 (§11) met gegenereerde signalen
   * ===================================================================== */
  function selftest(opts) {
    opts = opts || {};
    var sr = opts.sampleRate || 48000;
    var checks = [];
    var t0 = Date.now();
    function check(name, fn) {
      var ts = Date.now();
      try { var r = fn(); checks.push({ name: name, ok: !!r.ok, detail: r.detail + ' [' + (Date.now() - ts) + ' ms]' }); }
      catch (e) { checks.push({ name: name, ok: false, detail: 'fout: ' + msg(e) }); }
    }
    function must(res) { if (!res.ok) throw new Error(res.error); return res; }
    function inst() { var a = createCore(); a.setSampleRate(sr); return a; }
    function musicSignal(dur, seed) {
      var rnd = mulberry32(seed || 1), n = Math.round(dur * sr);
      var L = new Float32Array(n), R = new Float32Array(n);
      for (var i = 0; i < n; i++) {
        var t = i / sr;
        var v = 0.3 * Math.sin(2 * Math.PI * 220 * t) + 0.2 * Math.sin(2 * Math.PI * 330 * t + 0.5) + 0.1 * Math.sin(2 * Math.PI * 55 * t);
        var env = 0.6 + 0.4 * Math.sin(2 * Math.PI * 2 * t);
        L[i] = env * v + 0.02 * (rnd() - 0.5);
        R[i] = env * v * 0.8 + 0.02 * (rnd() - 0.5);
      }
      return [L, R];
    }

    // Criterium 2
    check('2: 12,000 s in blok van 10,000 s → exact 10 × sampleRate samples', function () {
      var A = inst();
      A.attachAudio('st12.wav', { sampleRate: sr, channels: musicSignal(12, 2) });
      must(A.execAll([{ cmd: 'create_block', args: { name: 'B10', targetDuration: 10, fitMode: 'proportional' } },
        { cmd: 'add_fragment', args: { block: 'b1', source: 'st12.wav', 'in': 0, out: 12, snap: false } }]));
      var ex = must(A.exec({ cmd: 'export_block', args: { block: 'b1' } })).result;
      var dec = A.wav.decode(ex.files[0].bytes);
      var ok = dec.channels[0].length === 10 * sr && dec.sampleRate === sr;
      return { ok: ok, detail: dec.channels[0].length + ' samples (verwacht ' + (10 * sr) + '), bestand ' + ex.files[0].name };
    });

    function sineFit(mode) {
      var A = inst();
      A.attachAudio('sin440.wav', { sampleRate: sr, channels: [genSine(440, 3.6, sr, 0.8)] });
      must(A.execAll([{ cmd: 'create_block', args: { name: 'S', targetDuration: 3.0, fitMode: 'proportional' } },
        { cmd: 'add_fragment', args: { block: 'b1', source: 'sin440.wav', 'in': 0, out: 3.6, snap: false, mode: mode } }]));
      var rep = must(A.exec({ cmd: 'fit_report', args: { block: 'b1' } })).result[0];
      must(A.exec({ cmd: 'render_block', args: { block: 'b1' } }));
      var y = A.getRendered('b1').channels[0];
      return { f: measureFreq(y, sr, 0.2, 0.8), ratio: rep.fragments[0].ratio };
    }
    // Criterium 3
    check('3: pitch, 440 Hz bij ratio 1,2 blijft 440 Hz (±1 Hz)', function () {
      var r = sineFit('pitch');
      return { ok: Math.abs(r.f - 440) <= 1 && Math.abs(r.ratio - 1.2) < 1e-9, detail: 'gemeten ' + r.f.toFixed(3) + ' Hz, ratio ' + r.ratio.toFixed(4) };
    });
    // Criterium 4
    check('4: varispeed, 440 Hz bij ratio 1,2 → 528 Hz (±1 Hz)', function () {
      var r = sineFit('varispeed');
      return { ok: Math.abs(r.f - 528) <= 1, detail: 'gemeten ' + r.f.toFixed(3) + ' Hz, ratio ' + r.ratio.toFixed(4) };
    });
    // Criterium 5
    check('5: 3 fragmenten (17,0 s), 2 crossfades 0,5 s, blok 15 s → ratio 1,0625, exact 15,000 s', function () {
      var A = inst();
      A.attachAudio('m20.wav', { sampleRate: sr, channels: musicSignal(20, 5) });
      must(A.execAll([{ cmd: 'create_block', args: { name: 'B15', targetDuration: 15, fitMode: 'proportional' } },
        { cmd: 'add_fragment', args: { block: 'b1', source: 'm20.wav', 'in': 0, out: 6, snap: false } },
        { cmd: 'add_fragment', args: { block: 'b1', source: 'm20.wav', 'in': 6, out: 11.5, snap: false } },
        { cmd: 'add_fragment', args: { block: 'b1', source: 'm20.wav', 'in': 11.5, out: 17, snap: false } },
        { cmd: 'set_transition', args: { block: 'b1', index: 0, type: 'crossfade', ms: 500 } },
        { cmd: 'set_transition', args: { block: 'b1', index: 1, type: 'crossfade', ms: 500 } }]));
      var rep = must(A.exec({ cmd: 'fit_report', args: { block: 'b1' } })).result[0];
      var ratios = rep.fragments.map(function (f) { return f.ratio; });
      var ratioOk = ratios.every(function (x) { return Math.abs(x - 1.0625) < 1e-9; });
      var ex = must(A.exec({ cmd: 'export_block', args: { block: 'b1' } })).result;
      var n = A.wav.decode(ex.files[0].bytes).channels[0].length;
      return { ok: ratioOk && n === 15 * sr, detail: 'ratio\'s ' + ratios.map(function (x) { return x.toFixed(6); }).join(', ') + '; ' + n + ' samples (verwacht ' + (15 * sr) + ')' };
    });
    // Criterium 6
    check('6: omhullende per overgangstype (equal_power ≈ 0,707, linear = 0,5 halverwege)', function () {
      var A = inst();
      var n1 = sr; // 1,0 s
      var one = new Float32Array(2 * sr).fill(1), zero = new Float32Array(2 * sr);
      A.attachAudio('dc.wav', { sampleRate: sr, channels: [one] });
      A.attachAudio('nul.wav', { sampleRate: sr, channels: [zero] });
      var cases = [
        { type: 'crossfade', curve: 'equal_power', a: Math.SQRT1_2, b: Math.SQRT1_2 },
        { type: 'crossfade', curve: 'linear', a: 0.5, b: 0.5 },
        { type: 'fade_out_a', a: Math.SQRT1_2, b: 1 },
        { type: 'fade_in_b', a: 1, b: Math.SQRT1_2 },
        { type: 'duck', a: dbToLin(CONSTANTS.DUCK_DEFAULT_DB), b: 1 },
        { type: 'mix', a: 1, b: 1 }
      ];
      var ovMs = 400, details = [], allOk = true;
      function env(type, curve, first) {
        var srcA = first ? 'dc.wav' : 'nul.wav', srcB = first ? 'nul.wav' : 'dc.wav';
        var T = (type === 'cut') ? 2.0 : (type === 'gap' ? 2.3 : 2.0 - ovMs / 1000);
        var cmds = [{ cmd: 'create_block', args: { name: 'E', targetDuration: T, fitMode: 'proportional' } },
          { cmd: 'add_fragment', args: { block: 'E', source: srcA, 'in': 0, out: 1, snap: false } },
          { cmd: 'add_fragment', args: { block: 'E', source: srcB, 'in': 0, out: 1, snap: false } }];
        var ta = { block: 'E', index: 0, type: type };
        if (type === 'gap') ta.ms = 300; else if (type !== 'cut') ta.ms = ovMs;
        if (curve) ta.curve = curve;
        cmds.push({ cmd: 'set_transition', args: ta });
        must(A.execAll(cmds));
        var blk = A.exec({ cmd: 'get_state' }).result.blocks.filter(function (b) { return b.name === 'E'; })[0];
        must(A.exec({ cmd: 'render_block', args: { block: blk.id } }));
        var y = A.getRendered(blk.id).channels[0];
        must(A.exec({ cmd: 'delete_block', args: { block: blk.id } }));
        return y;
      }
      cases.forEach(function (cs) {
        var ya = env(cs.type, cs.curve, true), yb = env(cs.type, cs.curve, false);
        var ovS = Math.round(ovMs / 1000 * sr), mid = n1 - ovS + ovS / 2;
        var ga = ya[mid], gb = yb[mid];
        var ok = Math.abs(ga - cs.a) < 0.005 && Math.abs(gb - cs.b) < 0.005;
        if (!ok) allOk = false;
        details.push(cs.type + (cs.curve ? '/' + cs.curve : '') + ': A=' + ga.toFixed(3) + ' B=' + gb.toFixed(3) + (ok ? '' : ' (verwacht ' + cs.a.toFixed(3) + '/' + cs.b.toFixed(3) + ')'));
      });
      // cut: microfades aan beide kanten van de las, verder vol volume
      var ca = env('cut', null, true), cb = env('cut', null, false);
      var cutOk = Math.abs(ca[n1 - 1]) < 0.01 && Math.abs(cb[n1]) < 0.01 && Math.abs(ca[n1 / 2] - 1) < 1e-6 && Math.abs(cb[n1 + n1 / 2] - 1) < 1e-6 && ca.length === 2 * sr;
      details.push('cut: A einde=' + ca[n1 - 1].toFixed(4) + ' B begin=' + cb[n1].toFixed(4));
      // gap: 300 ms stilte tussen A en B
      var ga2 = env('gap', null, true), gb2 = env('gap', null, false), gapS = Math.round(0.3 * sr), maxGap = 0;
      for (var k = n1; k < n1 + gapS; k++) maxGap = Math.max(maxGap, Math.abs(ga2[k]) + Math.abs(gb2[k]));
      var gapOk = maxGap < 1e-6 && Math.abs(gb2[n1 + gapS + n1 / 2] - 1) < 1e-6 && ga2.length === Math.round(2.3 * sr);
      details.push('gap: max in stilte=' + maxGap.toExponential(1));
      return { ok: allOk && cutOk && gapOk, detail: details.join('; ') };
    });
    // Criterium 7
    check('7: cut met testsinus → geen samplesprong > 0,1 op de fragmentgrens', function () {
      var A = inst();
      A.attachAudio('sin7.wav', { sampleRate: sr, channels: [genSine(440, 3, sr, 0.9)] });
      // bewust niet op nuldoorgangen (snap uit) zodat de microfades het werk doen
      must(A.execAll([{ cmd: 'create_block', args: { name: 'C', targetDuration: 2.0, fitMode: 'proportional' } },
        { cmd: 'add_fragment', args: { block: 'b1', source: 'sin7.wav', 'in': 0.1003, out: 1.1003, snap: false } },
        { cmd: 'add_fragment', args: { block: 'b1', source: 'sin7.wav', 'in': 1.5517, out: 2.5517, snap: false } },
        { cmd: 'set_transition', args: { block: 'b1', index: 0, type: 'cut' } }]));
      must(A.exec({ cmd: 'render_block', args: { block: 'b1' } }));
      var y = A.getRendered('b1').channels[0], b = sr, w = Math.round(0.02 * sr), mx = 0;
      for (var i = Math.max(1, b - w); i < Math.min(y.length, b + w); i++) mx = Math.max(mx, Math.abs(y[i] - y[i - 1]));
      return { ok: mx <= 0.1, detail: 'grootste samplesprong rond de grens: ' + mx.toFixed(4) };
    });

    // Stemspoor (§6.10): stem op de juiste sample-positie, ducking midden in de zone ≈ 10^(duckDb/20)
    check('§6.10 stem: juiste sample-positie (anker + offset) en ducking-niveau midden in de zone', function () {
      var A = inst();
      var duckDb = CONSTANTS.VOICE_DUCK_DEFAULT_DB, lvl = dbToLin(duckDb);
      A.attachAudio('dc05.wav', { sampleRate: sr, channels: [new Float32Array(2 * sr).fill(0.5)] });
      var vsrc = new Float32Array(sr), spikeAt = Math.round(0.5 * sr);
      vsrc[spikeAt] = 0.25;
      A.attachAudio('stem.wav', { sampleRate: sr, channels: [vsrc] });
      must(A.execAll([{ cmd: 'create_block', args: { name: 'V', targetDuration: 4.0, fitMode: 'proportional' } },
        { cmd: 'add_fragment', args: { block: 'b1', source: 'dc05.wav', 'in': 0, out: 2, snap: false } },
        { cmd: 'add_fragment', args: { block: 'b1', source: 'dc05.wav', 'in': 0, out: 2, snap: false } },
        { cmd: 'set_transition', args: { block: 'b1', index: 0, type: 'cut' } },
        { cmd: 'add_voice', args: { block: 'b1', source: 'stem.wav', 'in': 0, out: 1, anchor: 'f2', offset: 0.3, snap: false } }]));
      var rep = must(A.exec({ cmd: 'fit_report', args: { block: 'b1' } })).result[0];
      must(A.exec({ cmd: 'render_block', args: { block: 'b1' } }));
      var y = A.getRendered('b1').channels[0];
      var expect = 2 * sr + Math.round(0.3 * sr) + spikeAt, found = -1, best = 0;
      for (var i = 1; i < y.length; i++) { var dlt = y[i] - y[i - 1]; if (dlt > best) { best = dlt; found = i; } }
      var z = rep.duck[0], mid = Math.round((z.start + z.end) / 2 * sr) - 50;
      var gain = y[mid] / 0.5;
      var ok = found === expect && Math.abs(best - 0.25) < 1e-6 && Math.abs(gain - lvl) < 1e-4 && Math.abs(y[Math.round(1.0 * sr)] - 0.5) < 1e-6 &&
        Math.abs(rep.voice[0].start - 2.3) < 1e-9 && y.length === 4 * sr;
      return { ok: ok, detail: 'stempiek op sample ' + found + ' (verwacht ' + expect + '); ducking midden in de zone ' + gain.toFixed(4) +
        ' (verwacht ' + lvl.toFixed(4) + ' = 10^(' + duckDb + '/20)); zone ' + z.start.toFixed(3) + '–' + z.end.toFixed(3) + ' s' };
    });

    var ok = checks.every(function (c) { return c.ok; });
    return { ok: ok, checks: checks, ms: Date.now() - t0, sampleRate: sr };
  }

  var api = createCore();
  return api;
})();
globalThis.AudioFit = AudioFit;

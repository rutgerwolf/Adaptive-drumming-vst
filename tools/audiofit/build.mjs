// Plakt src/core.js in de placeholder van src/editor.html → audiofit.html
// Gebruik: node build.mjs
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const dir = dirname(fileURLToPath(import.meta.url));
const PLACEHOLDER = '/*@@CORE@@*/';
const core = readFileSync(join(dir, 'src/core.js'), 'utf8');
const editor = readFileSync(join(dir, 'src/editor.html'), 'utf8');

if (editor.split(PLACEHOLDER).length !== 2) {
  console.error('Placeholder ' + PLACEHOLDER + ' moet precies één keer voorkomen in src/editor.html');
  process.exit(1);
}
if (/<\/script/i.test(core)) {
  console.error('src/core.js bevat "</script" — dat breekt het HTML-blok');
  process.exit(1);
}
writeFileSync(join(dir, 'audiofit.html'), editor.replace(PLACEHOLDER, () => '\n' + core + '\n'));
console.log('audiofit.html geschreven');

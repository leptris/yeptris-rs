import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { chromium } from 'playwright';
import assert from 'node:assert/strict';

const server = createServer(async (req, res) => {
  try {
    if (req.url === '/') { res.setHeader('Content-Type', 'text/html'); res.end('<!doctype html><title>Yeptris browser smoke</title>'); return; }
    const name = req.url === '/yeptris_wasm_smoke_bg.wasm' ? 'yeptris_wasm_smoke_bg.wasm' : 'yeptris_wasm_smoke.js';
    res.setHeader('Content-Type', name.endsWith('.wasm') ? 'application/wasm' : 'text/javascript');
    res.end(await readFile(`wasm-web/${name}`));
  } catch (e) { res.writeHead(500); res.end(String(e)); }
});
server.listen(0, '127.0.0.1');
await new Promise(resolve => server.once('listening', resolve));
const url = `http://127.0.0.1:${server.address().port}`;
let browser;
try {
  browser = await chromium.launch({ headless: true });
  const page = await browser.newPage();
  await page.goto(url);
  const result = await page.evaluate(async () => {
    const module = await import('/yeptris_wasm_smoke.js');
    const wasm = await module.default();
    const before = wasm.memory.buffer.byteLength;
    const status = module.smoke();
    const after = wasm.memory.buffer.byteLength;
    const repeated = module.smoke();
    return { status, repeated, before, after, afterRepeat: wasm.memory.buffer.byteLength };
  });
  assert.equal(result.status, 'ok');
  assert.equal(result.repeated, 'ok');
  assert(result.after > result.before, 'stress consumer must grow WebAssembly memory');
  console.log(`memory grew from ${result.before} to ${result.after} bytes; repeated consumer: ${result.afterRepeat} bytes`);
  console.log(`Chromium ${browser.version()}: vendored parse/emit/allocation/error smoke passed`);
} finally {
  await browser?.close();
  server.close();
}

const fs = require('node:fs');
const assert = require('node:assert/strict');
for (const target of ['node', 'web']) {
  const path = `wasm-${target}/yeptris_wasm_smoke_bg.wasm`;
  const imports = WebAssembly.Module.imports(new WebAssembly.Module(fs.readFileSync(path)));
  console.log(path, JSON.stringify(imports));
  assert(imports.every(x => x.module === 'wbg' || x.module === '__wbindgen_placeholder__' || x.module.startsWith('./yeptris_wasm_smoke')), 'unexpected C runtime or host import');
}

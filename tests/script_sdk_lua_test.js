// script_sdk_lua_test.js -- the script SDK's embedded libraries (src/backend/script/script_sdk_lua.c) are what
// src/sdk/lua gives. The file is generated and checked in; a Lua change that was not regenerated ships the old
// library in the DLL (an API rename once left enemy_placement.lua calling a method that no longer existed).
const { execFileSync } = require('child_process');
const path = require('path');
const gen = path.join(__dirname, '..', 'tools', 'gen_script_sdk.js');
try {
  console.log(execFileSync(process.execPath, [gen, '--check'], { encoding: 'utf8' }).trim());
} catch (e) {
  console.error((e.stderr || e.message || '').toString().trim());
  process.exit(1);
}

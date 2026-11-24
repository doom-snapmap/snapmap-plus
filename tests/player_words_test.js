'use strict';
/* Every sentence the mod system can put in front of a player is free of the internal words (PLATFORM's vocabulary
   rule; SPEC-distribution I11): no salt, hash, unit, composition, manifest, bundle, sidecar, staging, claim, the
   store's setting or file names, and no error number. Builder causes an author reads may name mod.json, the Toybox and
   decls; log lines ("MODS:", "PREFABS:", "CARRIER:", backend_log) are not the player's and are not read.

   The sentences are found in the source, so a new one cannot slip past: every string literal (adjacent literals
   joined) passed to a player sink, every #define whose name says it is a sentence, and every row of the reason table.
   A format directive stands for its argument. Pure ASCII. */
const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..', 'src', 'backend');
/* The calls whose text reaches the player: the dialog and toast notices, the console lines a player reads, the Mods
   and Prefabs tabs' answers, the store's and the serving index's causes the tabs show. */
const SINKS = ['pub_notice', 'pub_print', 'pub_drop', 'pub_single_notice', 'sh_modpublished_notice', 'mu_msg',
    'mu_simple_result', 'mu_file_result', 'pm_msg', 'pm_print', 'pm_toast', 'ms_entry_cause', 'rawmap_tell',
    'mp_menu_notice', 'sh_modmanifest_load_refuse', 'ms_cause', 'ms_set_cause', 'ms_fail', 'ms_rk_fail', 'me_cause',
    'mc_why', 'pub_textf', 'mu_sentence_end'];
const DEFINE = /#define\s+(SH_[A-Z0-9_]*(?:TEXT|SENTENCE|REFUSAL|EARLIER|SCRIPTS_OFF[A-Z_]*)|MODEXPORT_EARLIER|MS_RETIRED_SENTENCE|MP_[A-Z_]*SENTENCE)\b((?:[^\n]*\\\r?\n)*[^\n]*)/g;
const BANNED = [
    [/\bsalts?\b/i, 'salt'], [/\bhash(es)?\b/i, 'hash'], [/\bunits?\b/i, 'unit'], [/\bcompositions?\b/i, 'composition'],
    [/\bmanifests?\b/i, 'manifest'], [/\bbundles?\b/i, 'bundle'], [/\bsidecar/i, 'sidecar'], [/\bstaging\b/i, 'staging'],
    [/\bclaims?\b/i, 'claim'], [/mods\.store/i, 'mods.store'], [/config\.json/i, 'config.json'],
    [/snapmap-plus\.json/i, 'snapmap-plus.json'], [/\.content\b/i, '.content'], [/\berror\s+(%|\d|0x)/i, 'error number'],
    [/system error/i, 'system error'], [/\bthe log\b/i, 'the log'],
    /* DS-WORDS: a refusal or notice names a mod by its name, never by a path under the mods folder (in the C source a
       backslash is written twice) */
    [/\bmods\\\\/i, 'a mods\\ path']];
/* Words a builder cause may use for an author (the builder speaks to the mod's author). */
const LOG_PREFIX = /^(MODS|PREFABS|CARRIER|RAWMAP|LEGACY|STORE|PUBLISH):/;

function literalsIn(text) {
    const out = [];
    const re = /"((?:[^"\\\n]|\\.)*)"/g;
    let m, last = -2, cur = null;
    while ((m = re.exec(text))) {
        const between = text.slice(last, m.index);
        if (cur !== null && last >= 0 && /^\s*$/.test(between)) cur += m[1];
        else { if (cur !== null) out.push(cur); cur = m[1]; }
        last = re.lastIndex;
    }
    if (cur !== null) out.push(cur);
    return out;
}

/* The argument text of each call to `name` (balanced parentheses, strings respected). */
function callsOf(src, name) {
    const out = [];
    const re = new RegExp('\\b' + name + '\\s*\\(', 'g');
    let m;
    while ((m = re.exec(src))) {
        let i = re.lastIndex, depth = 1, inStr = false;
        for (; i < src.length && depth; i++) {
            const c = src[i];
            if (inStr) { if (c === '\\') i++; else if (c === '"') inStr = false; continue; }
            if (c === '"') inStr = true; else if (c === '(') depth++; else if (c === ')') depth--;
        }
        const line = src.slice(0, m.index).split('\n').length;
        out.push({ text: src.slice(re.lastIndex, i - 1), line });
    }
    return out;
}

function lineOf(src, index) { return src.slice(0, index).split('\n').length; }

/* pub_drop(notice, why): the second argument is the log's reason, not the player's. */
const LOG_SECOND = new Set(['pub_drop']);
function firstArg(text) {
    let depth = 0, inStr = false;
    for (let i = 0; i < text.length; i++) {
        const c = text[i];
        if (inStr) { if (c === '\\') i++; else if (c === '"') inStr = false; continue; }
        if (c === '"') inStr = true; else if (c === '(') depth++; else if (c === ')') depth--;
        else if (c === ',' && !depth) return text.slice(0, i);
    }
    return text;
}

const found = [];
let sentences = 0;
function check(file, line, sentence, what) {
    const s = sentence.replace(/\\"/g, '"');
    if (!s.trim() || LOG_PREFIX.test(s.trim())) return;
    sentences++;
    for (const [re, word] of BANNED) if (re.test(s)) found.push(`${file}:${line} (${what}) says "${word}": ${s}`);
}

for (const f of fs.readdirSync(root).filter(n => /\.(c|h)$/.test(n))) {
    const src = fs.readFileSync(path.join(root, f), 'utf8');
    for (const sink of SINKS)
        for (const call of callsOf(src, sink))
            for (const lit of literalsIn(LOG_SECOND.has(sink) ? firstArg(call.text) : call.text)) check(f, call.line, lit, sink);
    let m;
    DEFINE.lastIndex = 0;
    while ((m = DEFINE.exec(src)))
        for (const lit of literalsIn(m[2])) check(f, lineOf(src, m.index), lit, m[1]);
    if (f === 'plain_reason.h') {
        const table = src.match(/SH_REASON_TABLE[\s\S]*?\n\n/);
        if (table) for (const lit of literalsIn(table[0])) check(f, lineOf(src, src.indexOf(table[0])), lit, 'reason table');
    }
}

/* The Snapmap+ window's pages write sentences of their own: none names a path under the mods folder (DS-WORDS). The
   Files view shows a mod's own files to its author, by their paths inside the mod, never by the mods folder. */
const pages = path.join(__dirname, '..', 'src', 'ui', 'webview');
for (const f of fs.readdirSync(pages).filter(n => /\.js$/.test(n))) {
    const src = fs.readFileSync(path.join(pages, f), 'utf8');
    const re = /'((?:[^'\\\n]|\\.)*)'/g;
    let m;
    while ((m = re.exec(src))) {
        sentences++;
        if (/\bmods\\\\/i.test(m[1])) found.push(`${f}:${lineOf(src, m.index)} (page) names a mods\\ path: ${m[1]}`);
    }
}

/* The scanner itself: a banned word in a sink is caught, a log line is not. */
const probe = 'pub_notice("This map uses the salt abc."); pub_print("MODS: hash 1234");';
let caught = 0;
for (const call of callsOf(probe, 'pub_notice')) for (const lit of literalsIn(call.text)) if (BANNED.some(([re]) => re.test(lit))) caught++;
if (caught !== 1) { console.error('player_words_test: the scanner does not catch a banned word'); process.exit(1); }

if (sentences < 200) { console.error(`player_words_test: only ${sentences} sentences found; the scan is not reading the source`); process.exit(1); }
if (found.length) {
    console.error(`player_words_test: ${found.length} player sentence(s) use internal words:`);
    for (const f of found) console.error('  ' + f);
    process.exit(1);
}
console.log(`player_words_test: ${sentences} player sentences, none with an internal word`);

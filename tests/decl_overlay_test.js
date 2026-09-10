/* Tests alignment between the transparent textarea and syntax-highlighted pre.
 * Extract the shipped tokenizer/renderer and assert that stripping markup gives
 * the original text plus the renderer's trailing newline.
 * Run: node tests/decl_overlay_test.js */
'use strict';
const fs = require('fs');
const path = require('path');

const HTML = path.join(__dirname, '..', 'src', 'ui', 'webview', 'mockup.html');
const src = fs.readFileSync(HTML, 'utf8').replace(/\r\n/g, '\n');   // repo files are CRLF

function grab(startRe, endMarker) {
  const i = src.search(startRe);
  if (i < 0) throw new Error('could not find ' + startRe + ' in mockup.html');
  const j = src.indexOf(endMarker, i);
  if (j < 0) throw new Error('could not find end marker after ' + startRe);
  return src.slice(i, j + endMarker.length);
}

// esc() -- one line
const escSrc = grab(/function esc\(s\)/, '\n');
// tokenizeDecl() -- ends at its closing brace + "\n  }\n"
const tokSrc = grab(/function tokenizeDecl\(text\)/, '\n    return toks;\n  }');
// renderHl() -- take the body up to the innerHTML assignment, return h instead
let hlSrc = grab(/function renderHl\(text, toks, diags\)/, "$('declHl').innerHTML = h;");
hlSrc = hlSrc.replace("$('declHl').innerHTML = h;", 'return h;') + '\n}';

const sandbox = {};
new Function('exports', escSrc + '\n' + tokSrc + '\n' + hlSrc +
  '\nexports.esc = esc; exports.tokenizeDecl = tokenizeDecl; exports.renderHl = renderHl;')(sandbox);

const { tokenizeDecl, renderHl } = sandbox;

function stripTags(h) {
  return h.replace(/<span class="[^"]*">/g, '').replace(/<\/span>/g, '')
          .replace(/&quot;/g, '"').replace(/&gt;/g, '>').replace(/&lt;/g, '<')
          .replace(/&amp;/g, '&');
}

const CASES = {
  'plain decl': 'edit = {\n\thealth = 300;\n\taiType = "heavy";\n}',
  'trailing newline': 'edit = {\n\thealth = 300;\n}\n',
  'leading newline': '\nedit = {\n\thealth = 300;\n}',
  'CRLF body': 'edit = {\r\n\thealth = 300;\r\n}\r\n',
  'line comment': '// a note\nedit = {\n\thealth = 300;   // inline\n}',
  'block comment': 'edit = {\n\t/* two\n\t   lines */\n\thealth = 300;\n}',
  'unterminated block comment': 'edit = {\n\t/* never closed\n\thealth = 300;\n}',
  'unterminated string': 'edit = {\n\tmodel = "oops;\n\thealth = 300;\n}',
  'string with escaped quote': 'edit = {\n\tname = "say \\"hi\\"";\n}',
  'backslash at end of line inside string': 'edit = {\n\tname = "trailing\\\n\thealth = 300;\n}',
  'string ending in backslash at EOF': 'edit = {\n\tname = "c:\\\\path\\',
  'list keys': 'edit = {\n\tnum = 2;\n\t"item[0]" = 1;\n\titem[1] = 2;\n}',
  'ampersand + angle brackets': 'edit = {\n\tdesc = "a & b < c > d";\n}',
  'html-ish entity text': 'edit = {\n\tdesc = "&#10; &amp; &lt;";\n}',
  'blank lines': 'edit = {\n\n\n\thealth = 300;\n\n}',
  'lone CR': 'edit = {\r\thealth = 300;\r}',
  'form feed': 'edit = {\n\f\thealth = 300;\n}',
  'U+2028 line separator': 'edit = {\u2028\thealth = 300;\n}',
  'negative + exponent numbers': 'edit = {\n\tx = -1.5e-3;\n\ty = .5;\n}',
  'bad char': 'edit = {\n\t@@@ = 1;\n}',
  'empty': '',
  'only newlines': '\n\n\n',
};

let fails = 0, ran = 0;
for (const [name, text] of Object.entries(CASES)) {
  ran++;
  const toks = tokenizeDecl(text);
  const painted = stripTags(renderHl(text, toks, []));
  const expected = text + '\n';
  if (painted !== expected) {
    fails++;
    console.log('FAIL  ' + name);
    console.log('   textarea lines: ' + expected.split('\n').length +
                '   pre lines: ' + painted.split('\n').length);
    console.log('   textarea: ' + JSON.stringify(expected));
    console.log('   pre     : ' + JSON.stringify(painted));
  } else {
    console.log('ok    ' + name);
  }
  // token ordering invariant: ascending, non-overlapping, text === slice
  let last = 0;
  for (const tk of toks) {
    if (tk.start < last) { console.log('FAIL  ' + name + ': token overlap at ' + tk.start); fails++; break; }
    if (tk.text !== text.slice(tk.start, tk.end)) {
      console.log('FAIL  ' + name + ': token text != source slice at ' + tk.start); fails++; break;
    }
    last = tk.end;
  }
}
console.log('\n' + (ran - fails) + '/' + ran + ' cases clean' + (fails ? '  -- ' + fails + ' FAILURES' : ''));
process.exit(fails ? 1 : 0);

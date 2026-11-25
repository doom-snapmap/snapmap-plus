'use strict';

function lift(html, name) {
  const at = html.indexOf('  function ' + name + '(');
  if (at < 0) throw new Error('no function ' + name);
  let i = html.indexOf('{', at), depth = 0;
  for (; i < html.length; i++) {
    if (html[i] === '{') depth++;
    else if (html[i] === '}' && --depth === 0) break;
  }
  if (i >= html.length) throw new Error('unterminated function ' + name);
  return html.slice(at, i + 1);
}
function between(html, begin, end) {
  const start = html.indexOf(begin);
  const stop = html.indexOf(end, start + begin.length);
  if (start < 0 || stop < 0) throw new Error('missing page markers for ' + begin);
  return html.slice(start, stop);
}

module.exports = { lift, between };

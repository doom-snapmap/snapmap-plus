/* Keyboard routing in the Snapmap+ window (page_keys.js): page-wide shortcuts leave text fields,
 * list boxes, dialogs and menus their keys; a dialog closes on Escape, keeps Tab inside and gives
 * focus back; menus and the tab strip walk with the arrow keys. Run against a small stand-in DOM.
 * Pure ASCII. */
'use strict';
const path = require('path');
const pageKeys = require(path.join(__dirname, '..', 'src', 'ui', 'webview', 'page_keys.js'));

let failures = 0;
function check(ok, message) { if (!ok) { console.error('[FAIL] ' + message); failures++; } }

/* ---- a stand-in DOM: enough of Element for the module ---- */
const doc = {activeElement: null, contains: function (el) { return !el.detached; }};
function el(tag, attrs, children) {
  const e = {tagName: tag, attrs: Object.assign({}, attrs || {}), children: children || [], parent: null, style: {},
             disabled: !!(attrs && attrs.disabled), offsetParent: {}, isContentEditable: false};
  e.children.forEach(function (c) { c.parent = e; });
  e.getAttribute = function (n) { return n in e.attrs ? String(e.attrs[n]) : null; };
  e.setAttribute = function (n, v) { e.attrs[n] = String(v); };
  e.getClientRects = function () { return [1]; };
  e.focus = function () { doc.activeElement = e; };
  e.matches = function (sel) {
    return sel.split(',').some(function (one) {
      one = one.trim();
      const role = one.match(/^\[role="([^"]+)"\]$/);
      if (role) return e.attrs.role === role[1];
      if (one === '[tabindex]') return 'tabindex' in e.attrs;
      if (one === '[href]') return 'href' in e.attrs;
      if (one.charAt(0) === '.') return (' ' + (e.attrs['class'] || '') + ' ').indexOf(' ' + one.slice(1) + ' ') >= 0;
      return e.tagName === one.toUpperCase();
    });
  };
  e.closest = function (sel) { for (let n = e; n; n = n.parent) if (n.matches(sel)) return n; return null; };
  e.querySelectorAll = function (sel) {
    const out = [];
    (function walk(n) { n.children.forEach(function (c) { if (c.matches(sel)) out.push(c); walk(c); }); })(e);
    return out;
  };
  return e;
}

/* ---- a key belongs to the control it was typed in ---- */
{
  const input = el('INPUT'), area = el('TEXTAREA'), select = el('SELECT'), row = el('DIV', {'class': 'entity-item'});
  const inDialog = el('BUTTON'), dialog = el('DIV', {role: 'dialog'}, [inDialog]);
  const item = el('BUTTON', {role: 'menuitem'}), menu = el('DIV', {role: 'menu'}, [item]);
  const option = el('DIV'), listbox = el('DIV', {role: 'listbox'}, [option]);
  const body = el('BODY', {}, [input, area, select, row, dialog, menu, listbox]);
  check(pageKeys.belongsToControl(input) && pageKeys.belongsToControl(area) && pageKeys.belongsToControl(select),
        'text fields and selects keep their keys');
  check(pageKeys.belongsToControl(inDialog) && pageKeys.belongsToControl(item) && pageKeys.belongsToControl(option),
        'dialogs, menus and list boxes keep their keys');
  check(!pageKeys.belongsToControl(row) && !pageKeys.belongsToControl(body), 'a list row or the page takes the shortcuts');
  const editable = el('DIV'); editable.isContentEditable = true;
  check(pageKeys.belongsToControl(editable), 'editable text keeps its keys');
}

/* ---- dialogs: Escape, Tab, focus back ---- */
function key(k, shift) {
  return {key: k, shiftKey: !!shift, defaultPrevented: false, preventDefault: function () { this.defaultPrevented = true; }};
}
{
  const opener = el('BUTTON');
  const a = el('INPUT'), b = el('BUTTON', {disabled: true}), c = el('BUTTON');
  const hidden = el('BUTTON'); hidden.offsetParent = null; hidden.getClientRects = function () { return []; };
  const overlay = el('DIV', {'class': 'modal-overlay'}, [el('DIV', {role: 'dialog'}, [a, b, hidden, c])]);
  el('BODY', {}, [opener, overlay]);
  const dialogs = pageKeys.createDialogs(doc);
  let escaped = 0;
  opener.focus();
  dialogs.open(overlay, {onEscape: function () { escaped++; dialogs.close(overlay); }});
  check(overlay.style.display === 'flex' && overlay.getAttribute('aria-hidden') === 'false', 'open shows the dialog');
  check(doc.activeElement === a, 'focus starts on the first control');
  let e = key('Tab'); dialogs.onKeyDown(e);
  check(e.defaultPrevented && doc.activeElement === c, 'Tab skips disabled and hidden controls');
  e = key('Tab'); dialogs.onKeyDown(e);
  check(doc.activeElement === a, 'Tab wraps to the first control');
  e = key('Tab', true); dialogs.onKeyDown(e);
  check(doc.activeElement === c, 'Shift+Tab wraps to the last control');
  e = key('Escape'); e.defaultPrevented = true; dialogs.onKeyDown(e);
  check(escaped === 0, 'an Escape a control inside already used does not close the dialog');
  dialogs.onKeyDown(key('Escape'));
  check(escaped === 1 && overlay.style.display === 'none' && doc.activeElement === opener && dialogs.count() === 0,
        'Escape closes the dialog and focus goes back to its opener');

  /* Two dialogs: Escape closes the top one, and focus returns inside the first. */
  const inner = el('DIV', {'class': 'modal-overlay'}, [el('DIV', {role: 'dialog'}, [el('BUTTON')])]);
  dialogs.open(overlay, {onEscape: function () { dialogs.close(overlay); }});
  const before = doc.activeElement;
  dialogs.open(inner, {onEscape: function () { dialogs.close(inner); }});
  check(dialogs.top() === inner, 'the newest dialog is on top');
  dialogs.onKeyDown(key('Escape'));
  check(dialogs.top() === overlay && doc.activeElement === before, 'closing the top dialog returns focus to the one below');
  dialogs.close(overlay);
  check(dialogs.count() === 0, 'every dialog closed');
}

/* ---- menus and tabs ---- */
{
  const i1 = el('BUTTON', {role: 'menuitem'}), i2 = el('BUTTON', {role: 'menuitemcheckbox'}), i3 = el('BUTTON', {role: 'menuitemradio'});
  const menu = el('DIV', {role: 'menu'}, [i1, el('DIV', {role: 'separator'}), i2, i3]);
  i1.focus();
  check(pageKeys.menuKey(menu, key('ArrowDown'), doc) && doc.activeElement === i2, 'ArrowDown moves to the next item');
  pageKeys.menuKey(menu, key('End'), doc);
  check(doc.activeElement === i3, 'End moves to the last item');
  pageKeys.menuKey(menu, key('ArrowDown'), doc);
  check(doc.activeElement === i1, 'ArrowDown wraps');
  pageKeys.menuKey(menu, key('ArrowUp'), doc);
  check(doc.activeElement === i3, 'ArrowUp wraps');
  check(!pageKeys.menuKey(menu, key('a'), doc), 'other keys are left alone');

  const tabs = ['assets', 'entities', 'timelines'].map(function (n) { return el('BUTTON', {role: 'tab', id: n}); });
  check(pageKeys.tabKey(tabs, tabs[1], 'ArrowRight') === tabs[2] && pageKeys.tabKey(tabs, tabs[2], 'ArrowRight') === tabs[0],
        'Right moves to the next tab and wraps');
  check(pageKeys.tabKey(tabs, tabs[0], 'ArrowLeft') === tabs[2] && pageKeys.tabKey(tabs, tabs[1], 'Home') === tabs[0] &&
        pageKeys.tabKey(tabs, tabs[0], 'End') === tabs[2], 'Left, Home and End');
  check(pageKeys.tabKey(tabs, tabs[0], 'Enter') === null && pageKeys.tabKey(tabs, el('BUTTON'), 'ArrowRight') === null,
        'other keys, or focus outside the strip, change nothing');
}

if (failures) { console.error(failures + ' keyboard check(s) failed'); process.exit(1); }
console.log('page_keys_test: all checks passed');

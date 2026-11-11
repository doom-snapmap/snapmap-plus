/* Keyboard ownership, dialogs and menus for the Snapmap+ window.
 *
 * A key pressed in a text field, a list box, a dialog or a menu belongs to that
 * control: page-wide shortcuts (arrow keys moving a list's selection, Delete,
 * Escape clearing a selection) never act on it. Every dialog opens through
 * pageKeys.dialogs: Escape closes the top one, Tab stays inside it, and focus
 * returns to where it was when the dialog opened. Context menus are menus of
 * buttons a keyboard can walk. Keep this file ASCII. */
var pageKeys = (function () {
  'use strict';

  var FOCUSABLE = 'button, [href], input, select, textarea, [tabindex]';

  /* True when the key event's target is a control that owns its keys. */
  function belongsToControl(target) {
    if (!target || !target.closest) return false;
    if (target.isContentEditable) return true;
    var tag = target.tagName;
    if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return true;
    return !!target.closest('[role="dialog"], [role="menu"], [role="listbox"], .modal-overlay, .ctxmenu');
  }

  function focusables(root) {
    var all = root.querySelectorAll(FOCUSABLE), out = [];
    for (var i = 0; i < all.length; i++) {
      var el = all[i];
      if (el.disabled || el.getAttribute('tabindex') === '-1' || el.getAttribute('aria-hidden') === 'true') continue;
      if (el.offsetParent === null && el.getClientRects().length === 0) continue;   /* not shown */
      out.push(el);
    }
    return out;
  }

  /* The open dialogs, newest last. open() shows an overlay and remembers what had focus;
     close() hides it and gives focus back. opts.onEscape runs for Escape (the dialog's own
     Cancel or Close), opts.focus names the control to focus first. */
  function createDialogs(doc) {
    var stack = [];
    function find(overlay) {
      for (var i = stack.length - 1; i >= 0; i--) if (stack[i].overlay === overlay) return i;
      return -1;
    }
    function open(overlay, opts) {
      opts = opts || {};
      if (find(overlay) < 0) stack.push({overlay: overlay, onEscape: opts.onEscape || null, restore: doc.activeElement});
      overlay.style.display = 'flex';
      overlay.setAttribute('aria-hidden', 'false');
      var first = opts.focus || focusables(overlay)[0];
      if (first && first.focus) first.focus();
    }
    function close(overlay) {
      var at = find(overlay);
      overlay.style.display = 'none';
      overlay.setAttribute('aria-hidden', 'true');
      if (at < 0) return;
      var entry = stack.splice(at, 1)[0];
      if (at === stack.length && entry.restore && entry.restore.focus && doc.contains(entry.restore)) entry.restore.focus();
    }
    function isOpen(overlay) { return find(overlay) >= 0; }
    function top() { return stack.length ? stack[stack.length - 1].overlay : null; }
    /* Bubble-phase keydown: a control inside the dialog that used the key (a completion list's
       Escape, the editor's Tab) has already called preventDefault. */
    function onKeyDown(e) {
      if (!stack.length || e.defaultPrevented) return;
      var entry = stack[stack.length - 1];
      if (e.key === 'Escape') {
        if (!entry.onEscape) return;
        e.preventDefault();
        entry.onEscape();
        return;
      }
      if (e.key !== 'Tab') return;
      var f = focusables(entry.overlay);
      if (!f.length) { e.preventDefault(); return; }
      var at = f.indexOf(doc.activeElement);
      var next = e.shiftKey ? (at <= 0 ? f.length - 1 : at - 1) : (at < 0 || at === f.length - 1 ? 0 : at + 1);
      e.preventDefault();
      f[next].focus();
    }
    return {open: open, close: close, isOpen: isOpen, top: top, onKeyDown: onKeyDown, count: function () { return stack.length; }};
  }

  /* Arrow keys, Home and End walk a menu's items; returns true when it used the key. */
  function menuKey(menu, e, doc) {
    var items = Array.prototype.slice.call(menu.querySelectorAll('[role="menuitem"], [role="menuitemcheckbox"], [role="menuitemradio"]'))
      .filter(function (el) { return !el.disabled; });
    if (!items.length) return false;
    var at = items.indexOf(doc.activeElement), next = -1;
    if (e.key === 'ArrowDown') next = at < 0 || at === items.length - 1 ? 0 : at + 1;
    else if (e.key === 'ArrowUp') next = at <= 0 ? items.length - 1 : at - 1;
    else if (e.key === 'Home') next = 0;
    else if (e.key === 'End') next = items.length - 1;
    else return false;
    items[next].focus();
    return true;
  }

  /* Tabs in a tablist: Left/Right/Home/End move to and select a tab; returns the tab to select or null. */
  function tabKey(tabs, current, key) {
    var at = tabs.indexOf(current);
    if (at < 0) return null;
    if (key === 'ArrowRight') return tabs[(at + 1) % tabs.length];
    if (key === 'ArrowLeft') return tabs[(at + tabs.length - 1) % tabs.length];
    if (key === 'Home') return tabs[0];
    if (key === 'End') return tabs[tabs.length - 1];
    return null;
  }

  return {belongsToControl: belongsToControl, focusables: focusables, createDialogs: createDialogs,
          menuKey: menuKey, tabKey: tabKey};
})();

if (typeof module !== 'undefined' && module.exports) module.exports = pageKeys;

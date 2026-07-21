/* Snapmap+ site — Community section behavior.
   Renders GitHub Discussions (via the community service) natively on the site:
   1. Index — category tabs + the post list (community.html).
   2. Post view — one discussion: body, reactions, comment thread + comment box, and
      author controls (edit/delete your own post and comments) (community-post.html).
   3. Composer — write or edit a post: markdown, image upload, preview (community-compose.html).
   4. Sign-in — GitHub OAuth via the service; the browser holds only an opaque session id.
   Each feature activates only when its markup is present, same as site.js. */

(function () {
  "use strict";

  var WORKER = "https://snapmap-plus-community.doom-snapmap.workers.dev";
  var REPO_URL = "https://github.com/doom-snapmap/snapmap-plus";
  var DISCUSSIONS_URL = REPO_URL + "/discussions";
  var SESSION_KEY = "smp_session";
  var DRAFT_KEY = "smp_draft";

  function esc(s) {
    return String(s == null ? "" : s)
      .replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  /* ---------- session ---------- */

  function captureSession() {
    if (location.hash.indexOf("#session=") === 0) {
      var sid = location.hash.slice("#session=".length);
      if (/^[0-9a-f]{64}$/i.test(sid)) {
        try { localStorage.setItem(SESSION_KEY, sid); } catch (e) {}
      }
      history.replaceState(null, "", location.pathname + location.search);
    } else if (location.hash === "#login_error") {
      history.replaceState(null, "", location.pathname + location.search);
      window.__smpLoginError = true;
    }
  }

  function sessionId() {
    try { return localStorage.getItem(SESSION_KEY); } catch (e) { return null; }
  }

  function clearSession() {
    try { localStorage.removeItem(SESSION_KEY); } catch (e) {}
  }

  function authHeaders() {
    var sid = sessionId();
    return sid ? { "Authorization": "Bearer " + sid } : {};
  }

  function api(path, opts) {
    opts = opts || {};
    var headers = opts.headers || {};
    var ah = authHeaders();
    Object.keys(ah).forEach(function (k) { headers[k] = ah[k]; });
    if (opts.json) {
      headers["Content-Type"] = "application/json";
      opts.body = JSON.stringify(opts.json);
    }
    return fetch(WORKER + path, {
      method: opts.method || "GET",
      headers: headers,
      body: opts.body || undefined,
    }).then(function (r) {
      if (r.status === 401 && sessionId()) clearSession();   // stale session — drop it
      return r.json().then(function (data) { return { ok: r.ok, status: r.status, data: data }; });
    }).catch(function () { return { ok: false, status: 0, data: null }; });
  }

  var mePromise = null;
  function me() {
    if (!sessionId()) return Promise.resolve(null);
    if (!mePromise) {
      mePromise = api("/auth/me").then(function (r) { return r.ok ? r.data : null; });
    }
    return mePromise;
  }

  /* ---------- shared rendering ---------- */

  function fmtDate(iso) {
    var d = new Date(iso);
    var days = (Date.now() - d.getTime()) / 86400000;
    if (days < 1) {
      var hours = Math.floor(days * 24);
      return hours <= 1 ? "just now" : hours + " hours ago";
    }
    if (days < 30) {
      var n = Math.floor(days);
      return n === 1 ? "yesterday" : n + " days ago";
    }
    return d.toLocaleDateString(undefined, { year: "numeric", month: "long", day: "numeric" });
  }

  var REACTION_EMOJI = {
    THUMBS_UP: "👍", THUMBS_DOWN: "👎", LAUGH: "😄",
    HOORAY: "🎉", CONFUSED: "😕", HEART: "❤️",
    ROCKET: "🚀", EYES: "👀"
  };

  function reactionsHtml(reactions) {
    if (!reactions || !reactions.length) return "";
    var html = '<span class="reactions">';
    reactions.forEach(function (r) {
      var emoji = REACTION_EMOJI[r.content];
      if (!emoji) return;
      html += '<span class="reaction-chip">' + emoji + " " + r.count + "</span>";
    });
    return html + "</span>";
  }

  function reactButton(subjectId) {
    return '<button class="react-btn" type="button" data-react="' + esc(subjectId) +
      '" title="React with 👍">👍<span class="react-plus">+</span></button>';
  }

  function authorHtml(a) {
    var login = esc(a && a.login || "ghost");
    var avatar = "";
    if (a && a.avatarUrl) {
      var sized = a.avatarUrl + (a.avatarUrl.indexOf("?") >= 0 ? "&" : "?") + "s=48";
      avatar = '<img class="avatar" src="' + esc(sized) + '" alt="" loading="lazy">';
    }
    var name = a && a.url
      ? '<a href="' + esc(a.url) + '" rel="noopener">' + login + "</a>"
      : login;
    return avatar + '<span class="author-name">' + name + "</span>";
  }

  /* ---------- video embeds ----------
     GitHub's rendered HTML leaves video links as plain anchors. Recognize known hosts and
     append a responsive iframe AFTER the link. The embed src is rebuilt from the parsed video
     id — never from the raw href — so only allowlisted forms ever reach an iframe. */

  function videoEmbed(href) {
    var m = href.match(/^https?:\/\/(?:www\.)?youtube\.com\/watch\?(?:.*&)?v=([\w-]{6,20})/) ||
            href.match(/^https?:\/\/youtu\.be\/([\w-]{6,20})/);
    if (m) return "https://www.youtube-nocookie.com/embed/" + m[1];
    m = href.match(/^https?:\/\/(?:www\.)?streamable\.com\/([a-z0-9]{4,12})$/i);
    if (m) return "https://streamable.com/e/" + m[1];
    return null;
  }

  function enhanceVideos(container) {
    if (!container) return;
    var anchors = Array.prototype.slice.call(container.querySelectorAll("a[href]"));
    anchors.forEach(function (a) {
      var src = videoEmbed(a.getAttribute("href") || "");
      if (!src) return;
      /* only bare links (the URL as its own text), not prose links */
      if (a.textContent.trim() !== a.getAttribute("href")) return;
      var frame = document.createElement("div");
      frame.className = "video-embed";
      var iframe = document.createElement("iframe");
      iframe.src = src;
      iframe.setAttribute("allowfullscreen", "");
      iframe.setAttribute("loading", "lazy");
      iframe.setAttribute("title", "Embedded video");
      iframe.setAttribute("allow", "encrypted-media; picture-in-picture");
      frame.appendChild(iframe);
      a.parentNode.insertBefore(frame, a.nextSibling);
    });
  }

  function failNote(host, what) {
    host.innerHTML =
      '<div class="changelog-fallback">Could not load ' + what + " right now. " +
      'The community also lives on <a href="' + DISCUSSIONS_URL + '">GitHub Discussions</a>.</div>';
  }

  /* ---------- auth widget ---------- */

  function signInButtonHtml(label) {
    return '<a class="btn btn-ghost btn-signin" href="' + WORKER + '/auth/login" rel="nofollow">' +
      (label || "Sign in with GitHub") + "</a>";
  }

  function initAuthWidget() {
    var host = document.getElementById("community-auth");
    if (!host) return;
    if (window.__smpLoginError) {
      host.innerHTML = '<span class="auth-error">Sign-in didn’t complete — try again.</span> ' + signInButtonHtml();
      return;
    }
    me().then(function (user) {
      if (!user) { host.innerHTML = signInButtonHtml(); return; }
      var sized = user.avatar + (user.avatar.indexOf("?") >= 0 ? "&" : "?") + "s=64";
      host.innerHTML =
        '<span class="auth-chip"><img class="avatar" src="' + esc(sized) + '" alt="">' +
        '<span>' + esc(user.login) + "</span>" +
        '<button class="auth-signout" type="button">Sign out</button></span>';
      host.querySelector(".auth-signout").addEventListener("click", function () {
        api("/auth/logout", { method: "POST" }).then(function () {
          clearSession();
          location.reload();
        });
      });
    });
  }

  /* ---------- 1. index ---------- */

  function initIndex() {
    var list = document.getElementById("community-list");
    if (!list) return;
    var tabs = document.getElementById("community-tabs");
    var more = document.getElementById("community-more");
    var params = new URLSearchParams(location.search);
    var active = params.get("category") || "";

    api("/community/categories").then(function (r) {
      if (!r.ok || !tabs) return;
      var html = '<a class="cat-tab' + (active ? "" : " active") + '" href="community.html">All</a>';
      r.data.categories.forEach(function (c) {
        html += '<a class="cat-tab' + (c.slug === active ? " active" : "") +
          '" href="community.html?category=' + esc(encodeURIComponent(c.slug)) + '">' +
          esc(c.name) + "</a>";
      });
      tabs.innerHTML = html;
    });

    function renderRows(discussions) {
      var html = "";
      discussions.forEach(function (d) {
        html +=
          '<a class="post-card" href="community-post.html?n=' + d.number + '">' +
            '<div class="post-card-main">' +
              '<h2 class="post-title">' + esc(d.title) + "</h2>" +
              '<div class="post-meta">' +
                authorHtml(d.author) +
                '<span class="sep">&middot;</span><time datetime="' + esc(d.createdAt) + '">' + fmtDate(d.createdAt) + "</time>" +
                (d.category ? '<span class="sep">&middot;</span><span class="cat-pill">' + esc(d.category.name) + "</span>" : "") +
              "</div>" +
            "</div>" +
            '<div class="post-card-counts">' +
              '<span class="count-chip" title="Reactions">&#9650; ' + d.reactionCount + "</span>" +
              '<span class="count-chip" title="Comments">&#128172; ' + d.commentCount + "</span>" +
            "</div>" +
          "</a>";
      });
      return html;
    }

    function load(after, append) {
      var q = "/community/discussions";
      var qs = [];
      if (active) qs.push("category=" + encodeURIComponent(active));
      if (after) qs.push("after=" + encodeURIComponent(after));
      if (qs.length) q += "?" + qs.join("&");
      api(q).then(function (r) {
        if (!r.ok) { if (!append) failNote(list, "community posts"); return; }
        var data = r.data;
        if (!data.discussions.length && !append) {
          list.innerHTML =
            '<div class="changelog-fallback">No posts here yet. ' +
            '<a href="community-compose.html">Be the first to write one</a>.</div>';
          if (more) more.innerHTML = "";
          return;
        }
        var html = renderRows(data.discussions);
        if (append) list.insertAdjacentHTML("beforeend", html);
        else list.innerHTML = html;
        if (more) {
          if (data.pageInfo && data.pageInfo.hasNextPage) {
            more.innerHTML = '<button class="btn btn-ghost" type="button">Load more</button>';
            more.querySelector("button").addEventListener("click", function () {
              this.disabled = true;
              load(data.pageInfo.endCursor, true);
            });
          } else {
            more.innerHTML = "";
          }
        }
      });
    }

    load(null, false);
  }

  /* ---------- 2. post view ---------- */

  function initPost() {
    var host = document.getElementById("community-post");
    if (!host) return;
    var params = new URLSearchParams(location.search);
    var n = parseInt(params.get("n") || "", 10);
    if (!n) { location.replace("community.html"); return; }

    var rawBodies = {};   // node id -> raw markdown, for inline comment editing

    function render() {
      Promise.all([api("/community/discussions/" + n), me()]).then(function (results) {
        var r = results[0];
        var user = results[1];
        if (!r.ok) { failNote(host, "this post"); return; }
        var d = r.data;
        document.title = d.title + " — Snapmap+ Community";
        var mine = function (a) { return !!(user && a && a.login === user.login); };

        var head =
          '<div class="section-head"><h1 class="section-title">' + esc(d.title) + "</h1></div>" +
          '<div class="post-meta post-meta-page">' +
            authorHtml(d.author) +
            '<span class="sep">&middot;</span><time datetime="' + esc(d.createdAt) + '">' + fmtDate(d.createdAt) + "</time>" +
            (d.category ? '<span class="sep">&middot;</span><span class="cat-pill">' + esc(d.category.name) + "</span>" : "") +
          "</div>";

        /* bodyHTML arrives already rendered + sanitized by GitHub's own pipeline */
        host.innerHTML =
          head +
          '<div class="post-body">' + d.bodyHTML + "</div>" +
          '<div class="post-foot">' + reactionsHtml(d.reactions) + reactButton(d.id) +
            (mine(d.author)
              ? '<a class="reply-toggle" href="community-compose.html?edit=' + n + '">Edit</a>' +
                '<button class="reply-toggle danger" type="button" data-del-post>Delete</button>'
              : "") +
            '<a class="gh-link" href="' + esc(d.url) + '" rel="noopener">View on GitHub &rarr;</a>' +
          "</div>";
        enhanceVideos(host.querySelector(".post-body"));

        var delPost = host.querySelector("[data-del-post]");
        if (delPost) {
          delPost.addEventListener("click", function () {
            if (!confirm("Delete this post? This cannot be undone.")) return;
            delPost.disabled = true;
            api("/community/discussions/" + n, { method: "DELETE" }).then(function (r2) {
              if (r2.ok) location.href = "community.html";
              else { delPost.disabled = false; alert("Delete failed (" + ((r2.data && r2.data.error) || "error") + ")."); }
            });
          });
        }

        var thread = document.getElementById("community-comments");
        if (!thread) return;

        function commentHtml(c, isReply, topId) {
          rawBodies[c.id] = c.body || "";
          return (
            '<article class="comment' + (isReply ? " comment-reply" : "") + '" data-cid="' + esc(c.id) + '">' +
              '<div class="post-meta">' + authorHtml(c.author) +
                '<span class="sep">&middot;</span><time datetime="' + esc(c.createdAt) + '">' + fmtDate(c.createdAt) + "</time>" +
              "</div>" +
              '<div class="comment-body">' + c.bodyHTML + "</div>" +
              '<div class="comment-foot">' + reactionsHtml(c.reactions) + reactButton(c.id) +
                (!isReply ? '<button class="reply-toggle" type="button" data-reply="' + esc(topId) + '">Reply</button>' : "") +
                (mine(c.author)
                  ? '<button class="reply-toggle" type="button" data-edit-c="' + esc(c.id) + '">Edit</button>' +
                    '<button class="reply-toggle danger" type="button" data-del-c="' + esc(c.id) + '">Delete</button>'
                  : "") +
              "</div>" +
            "</article>"
          );
        }

        var html =
          '<div class="section-head"><h2 class="section-title thread-title">' +
          d.commentCount + (d.commentCount === 1 ? " comment" : " comments") + "</h2></div>";
        d.comments.forEach(function (c) {
          html += commentHtml(c, false, c.id);
          c.replies.forEach(function (rp) { html += commentHtml(rp, true, c.id); });
          html += '<div class="reply-slot" data-slot="' + esc(c.id) + '"></div>';
        });
        html += '<div id="comment-box" class="comment-form-slot"></div>';
        thread.innerHTML = html;
        Array.prototype.slice.call(thread.querySelectorAll(".comment-body")).forEach(enhanceVideos);

        /* the comment box (or a sign-in prompt) */
        var box = document.getElementById("comment-box");
        if (box) {
          if (!user) {
            box.innerHTML = '<div class="changelog-fallback">' + signInButtonHtml("Sign in with GitHub") +
              ' <span class="signin-note">to comment and react.</span></div>';
          } else {
            box.innerHTML =
              '<form class="comment-form"><textarea rows="4" maxlength="60000" ' +
              'placeholder="Write a comment (markdown works)"></textarea>' +
              '<div class="form-row"><button class="btn btn-primary" type="submit">Comment</button></div></form>';
            box.querySelector("form").addEventListener("submit", function (e) {
              e.preventDefault();
              var ta = box.querySelector("textarea");
              var text = ta.value.trim();
              if (!text) return;
              var btn = box.querySelector("button");
              btn.disabled = true; btn.textContent = "Posting…";
              api("/community/discussions/" + n + "/comments", { method: "POST", json: { body: text } })
                .then(function (r2) {
                  if (r2.ok) { render(); }
                  else { btn.disabled = false; btn.textContent = "Comment"; alert("Could not post the comment (" + ((r2.data && r2.data.error) || "error") + ")."); }
                });
            });
          }
        }

        /* thread interactions: replies, inline edit, delete, reactions */
        thread.addEventListener("click", function (e) {
          var t;

          if ((t = e.target.closest("[data-reply]"))) {
            if (!sessionId()) { location.href = WORKER + "/auth/login"; return; }
            var id = t.getAttribute("data-reply");
            var slot = thread.querySelector('[data-slot="' + id + '"]');
            if (!slot || slot.firstChild) return;
            slot.innerHTML =
              '<form class="comment-form comment-form-reply"><textarea rows="3" maxlength="60000" ' +
              'placeholder="Write a reply"></textarea>' +
              '<div class="form-row"><button class="btn btn-primary" type="submit">Reply</button></div></form>';
            slot.querySelector("textarea").focus();
            slot.querySelector("form").addEventListener("submit", function (ev) {
              ev.preventDefault();
              var text = slot.querySelector("textarea").value.trim();
              if (!text) return;
              var btn = slot.querySelector("button");
              btn.disabled = true; btn.textContent = "Posting…";
              api("/community/discussions/" + n + "/comments", { method: "POST", json: { body: text, replyToId: id } })
                .then(function (r2) {
                  if (r2.ok) { render(); }
                  else { btn.disabled = false; btn.textContent = "Reply"; alert("Could not post the reply (" + ((r2.data && r2.data.error) || "error") + ")."); }
                });
            });
            return;
          }

          if ((t = e.target.closest("[data-edit-c]"))) {
            var cid = t.getAttribute("data-edit-c");
            var article = thread.querySelector('[data-cid="' + cid + '"]');
            if (!article || article.querySelector(".comment-edit-form")) return;
            var bodyDiv = article.querySelector(".comment-body");
            bodyDiv.style.display = "none";
            var form = document.createElement("form");
            form.className = "comment-form comment-edit-form";
            form.innerHTML =
              '<textarea rows="4" maxlength="60000"></textarea>' +
              '<div class="form-row"><button class="btn btn-primary" type="submit">Save</button>' +
              '<button class="btn btn-ghost" type="button" data-cancel>Cancel</button></div>';
            form.querySelector("textarea").value = rawBodies[cid] || "";
            bodyDiv.parentNode.insertBefore(form, bodyDiv.nextSibling);
            form.querySelector("[data-cancel]").addEventListener("click", function () {
              form.remove();
              bodyDiv.style.display = "";
            });
            form.addEventListener("submit", function (ev) {
              ev.preventDefault();
              var text = form.querySelector("textarea").value.trim();
              if (!text) return;
              var btn = form.querySelector('button[type="submit"]');
              btn.disabled = true; btn.textContent = "Saving…";
              api("/community/comments/" + encodeURIComponent(cid), { method: "PATCH", json: { body: text } })
                .then(function (r2) {
                  if (r2.ok) { render(); }
                  else { btn.disabled = false; btn.textContent = "Save"; alert("Edit failed (" + ((r2.data && r2.data.error) || "error") + ")."); }
                });
            });
            return;
          }

          if ((t = e.target.closest("[data-del-c]"))) {
            if (!confirm("Delete this comment?")) return;
            t.disabled = true;
            api("/community/comments/" + encodeURIComponent(t.getAttribute("data-del-c")), { method: "DELETE" })
              .then(function (r2) {
                if (r2.ok) { render(); }
                else { t.disabled = false; alert("Delete failed (" + ((r2.data && r2.data.error) || "error") + ")."); }
              });
            return;
          }

          if ((t = e.target.closest("[data-react]"))) {
            if (!sessionId()) { location.href = WORKER + "/auth/login"; return; }
            t.disabled = true;
            api("/community/reactions", { method: "POST", json: { subjectId: t.getAttribute("data-react"), content: "THUMBS_UP" } })
              .then(function (r2) {
                if (r2.ok) render();
                else t.disabled = false;
              });
          }
        });

        /* the post's own react button lives in `host`, not the thread */
        host.addEventListener("click", function (e) {
          var t = e.target.closest("[data-react]");
          if (!t) return;
          if (!sessionId()) { location.href = WORKER + "/auth/login"; return; }
          t.disabled = true;
          api("/community/reactions", { method: "POST", json: { subjectId: t.getAttribute("data-react"), content: "THUMBS_UP" } })
            .then(function (r2) {
              if (r2.ok) render();
              else t.disabled = false;
            });
        });
      });
    }

    render();
  }

  /* ---------- 3. composer (write + edit) ---------- */

  function insertAtCursor(ta, text) {
    var start = ta.selectionStart || 0;
    ta.value = ta.value.slice(0, start) + text + ta.value.slice(ta.selectionEnd || start);
    ta.selectionStart = ta.selectionEnd = start + text.length;
    ta.focus();
  }

  function uploadImage(file, ta, note) {
    if (!file) return;
    note.textContent = "Uploading " + (file.name || "image") + "…";
    fetch(WORKER + "/media/upload", {
      method: "POST",
      headers: authHeaders(),
      body: file,
    }).then(function (r) { return r.json().then(function (d) { return { ok: r.ok, data: d }; }); })
      .then(function (r) {
        if (r.ok && r.data.url) {
          insertAtCursor(ta, "\n![screenshot](" + r.data.url + ")\n");
          note.textContent = "";
        } else {
          note.textContent = "Upload failed: " + ((r.data && r.data.error) || "error");
        }
      })
      .catch(function () { note.textContent = "Upload failed."; });
  }

  function initCompose() {
    var host = document.getElementById("community-compose");
    if (!host) return;
    var params = new URLSearchParams(location.search);
    var editN = parseInt(params.get("edit") || "", 10) || null;

    me().then(function (user) {
      if (!user) {
        host.innerHTML =
          '<div class="changelog-fallback">' + signInButtonHtml() +
          ' <span class="signin-note">to write a post. Your GitHub account is the author — no separate registration.</span></div>';
        return;
      }

      var loads = [api("/community/categories")];
      if (editN) loads.push(api("/community/discussions/" + editN));
      Promise.all(loads).then(function (results) {
        var r = results[0];
        if (!r.ok) { failNote(host, "the composer"); return; }
        var editing = null;
        if (editN) {
          var pr = results[1];
          if (!pr.ok) { failNote(host, "the post to edit"); return; }
          if (pr.data.author.login !== user.login) {
            host.innerHTML = '<div class="changelog-fallback">Only the author can edit this post.</div>';
            return;
          }
          editing = pr.data;
          var titleEl = document.querySelector(".section-title");
          if (titleEl) titleEl.textContent = "Edit post";
          document.title = "Edit post — Snapmap+ Community";
        }

        /* Announcements is maintainer-posted; leave it out of the picker */
        var cats = r.data.categories.filter(function (c) { return c.slug !== "announcements"; });
        var options = cats.map(function (c) {
          var sel = editing && editing.category && editing.category.slug === c.slug ? " selected" : "";
          return '<option value="' + esc(c.id) + '"' + sel + ">" + esc(c.name) + "</option>";
        }).join("");

        host.innerHTML =
          '<form class="compose-form">' +
            '<div class="field"><label for="c-title">Title</label>' +
            '<input id="c-title" type="text" maxlength="200" placeholder="e.g. How to wire a door to a switch" required></div>' +
            '<div class="field"><label for="c-cat">Category</label>' +
            '<select id="c-cat">' + options + "</select></div>" +
            '<div class="field"><label for="c-body">Post (markdown)</label>' +
            '<textarea id="c-body" rows="14" maxlength="60000" required ' +
            'placeholder="Write your guide or tip in markdown.\n\nDrag-drop or paste a screenshot to upload it.\nPaste a YouTube/Streamable link on its own line to embed the video."></textarea></div>' +
            '<div class="form-row">' +
              '<label class="btn btn-ghost btn-file">Add image<input id="c-file" type="file" accept="image/png,image/jpeg,image/gif,image/webp" hidden></label>' +
              '<button id="c-preview" class="btn btn-ghost" type="button">Preview</button>' +
              '<span id="c-note" class="compose-note" aria-live="polite"></span>' +
              '<button class="btn btn-primary compose-submit" type="submit">' + (editing ? "Save changes" : "Publish") + "</button>" +
            "</div>" +
            '<div id="c-preview-box" class="preview-box" hidden></div>' +
          "</form>";

        var form = host.querySelector("form");
        var ta = host.querySelector("#c-body");
        var note = host.querySelector("#c-note");

        if (editing) {
          host.querySelector("#c-title").value = editing.title;
          ta.value = editing.body || "";
        } else {
          /* draft autosave (new posts only — edits always start from the published source) */
          try {
            var draft = JSON.parse(localStorage.getItem(DRAFT_KEY) || "null");
            if (draft) { host.querySelector("#c-title").value = draft.title || ""; ta.value = draft.body || ""; }
          } catch (e) {}
          form.addEventListener("input", function () {
            try {
              localStorage.setItem(DRAFT_KEY, JSON.stringify({
                title: host.querySelector("#c-title").value, body: ta.value,
              }));
            } catch (e) {}
          });
        }

        /* image upload: file picker, drag-drop, paste */
        host.querySelector("#c-file").addEventListener("change", function () {
          uploadImage(this.files[0], ta, note);
          this.value = "";
        });
        ta.addEventListener("dragover", function (e) { e.preventDefault(); });
        ta.addEventListener("drop", function (e) {
          e.preventDefault();
          if (e.dataTransfer.files.length) uploadImage(e.dataTransfer.files[0], ta, note);
        });
        ta.addEventListener("paste", function (e) {
          var files = e.clipboardData && e.clipboardData.files;
          if (files && files.length) { e.preventDefault(); uploadImage(files[0], ta, note); }
        });

        /* preview through GitHub's own renderer, so it matches the published look exactly */
        host.querySelector("#c-preview").addEventListener("click", function () {
          var box = host.querySelector("#c-preview-box");
          if (!box.hidden) { box.hidden = true; this.textContent = "Preview"; return; }
          var self = this;
          api("/community/preview", { method: "POST", json: { text: ta.value } }).then(function (r2) {
            if (!r2.ok) { note.textContent = "Preview failed."; return; }
            box.innerHTML = '<div class="post-body">' + r2.data.html + "</div>";
            enhanceVideos(box);
            box.hidden = false;
            self.textContent = "Hide preview";
          });
        });

        form.addEventListener("submit", function (e) {
          e.preventDefault();
          var btn = host.querySelector(".compose-submit");
          var busy = editing ? "Saving…" : "Publishing…";
          var idle = editing ? "Save changes" : "Publish";
          btn.disabled = true; btn.textContent = busy;
          var payload = {
            title: host.querySelector("#c-title").value.trim(),
            categoryId: host.querySelector("#c-cat").value,
            body: ta.value.trim(),
          };
          var call = editing
            ? api("/community/discussions/" + editN, { method: "PATCH", json: payload })
            : api("/community/discussions", { method: "POST", json: payload });
          call.then(function (r2) {
            if (r2.ok && (editing || r2.data.number)) {
              if (!editing) { try { localStorage.removeItem(DRAFT_KEY); } catch (err) {} }
              location.href = "community-post.html?n=" + (editing ? editN : r2.data.number);
            } else {
              btn.disabled = false; btn.textContent = idle;
              note.textContent = (editing ? "Save" : "Publish") + " failed: " + ((r2.data && r2.data.error) || "error");
            }
          });
        });
      });
    });
  }

  /* ---------- boot ---------- */

  function boot() {
    captureSession();
    initAuthWidget();
    initIndex();
    initPost();
    initCompose();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", boot);
  } else {
    boot();
  }
})();

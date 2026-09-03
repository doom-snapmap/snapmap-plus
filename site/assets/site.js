/* Snapmap+ site — shared behavior for every page.
   Each feature activates only when its markup is present:
   1. Download resolver — points [data-dl] links at the newest release's snapmap-plus.exe.
   2. Lightbox — click any framed screenshot to focus it full-screen.
   3. Guide TOC — builds the sticky sidebar from the rendered guide's headings.
   4. Mobile nav — the hamburger menu in the sticky header.

   The changelog page is rendered statically by Jekyll from CHANGELOG.md
   (see .github/workflows/pages.yml); it needs no script. */

(function () {
  "use strict";

  var REPO = "doom-snapmap/snapmap-plus";
  var API = "https://api.github.com/repos/" + REPO + "/releases";

  /* ---------- 1. download resolver ---------- */

  function resolveDownload() {
    var links = document.querySelectorAll("[data-dl]");
    if (!links.length) return;
    fetchReleases().then(function (rels) {
      if (!rels || !rels.length) return;
      var pick = null;
      for (var i = 0; i < rels.length; i++) {
        if (!rels[i].draft && !rels[i].prerelease) { pick = rels[i]; break; }
      }
      if (!pick) {
        for (var j = 0; j < rels.length; j++) {
          if (!rels[j].draft) { pick = rels[j]; break; }
        }
      }
      if (!pick) return;
      var asset = null;
      for (var k = 0; k < pick.assets.length; k++) {
        if (pick.assets[k].name === "snapmap-plus.exe") { asset = pick.assets[k]; break; }
      }
      if (!asset) return;
      links.forEach(function (a) { a.href = asset.browser_download_url; });
      var ver = document.getElementById("dl-version");
      if (ver) ver.textContent = pick.tag_name;
      var pill = document.getElementById("beta-pill");
      if (pill && pick.prerelease) pill.style.display = "inline-block";
    });
  }

  var releasesPromise = null;
  function fetchReleases() {
    if (!releasesPromise) {
      releasesPromise = fetch(API)
        .then(function (r) { return r.ok ? r.json() : null; })
        .catch(function () { return null; });
    }
    return releasesPromise;
  }

  /* ---------- 2. lightbox ---------- */

  function initLightbox() {
    var images = Array.prototype.slice.call(
      document.querySelectorAll(".shot img, .guide-doc img")
    );
    if (!images.length || typeof HTMLDialogElement === "undefined") return;

    var dialog = document.createElement("dialog");
    dialog.className = "lightbox";
    dialog.setAttribute("aria-label", "Image viewer");
    dialog.innerHTML =
      '<figure><img alt=""><figcaption></figcaption></figure>' +
      '<button class="lightbox-close" type="button" aria-label="Close image viewer">&#10005;</button>';
    document.body.appendChild(dialog);

    var img = dialog.querySelector("img");
    var cap = dialog.querySelector("figcaption");
    var current = -1;

    function captionFor(el) {
      var fig = el.closest("figure");
      var fc = fig && fig.querySelector("figcaption");
      if (fc) return fc.textContent;
      return el.getAttribute("alt") || "";
    }

    function show(i) {
      current = (i + images.length) % images.length;
      var src = images[current];
      img.src = src.currentSrc || src.src;
      img.alt = src.getAttribute("alt") || "";
      cap.textContent = captionFor(src);
    }

    images.forEach(function (el, i) {
      el.setAttribute("tabindex", "0");
      el.setAttribute("role", "button");
      el.setAttribute("aria-label", "Expand image: " + (el.getAttribute("alt") || "screenshot"));
      function open() { show(i); dialog.showModal(); }
      el.addEventListener("click", open);
      el.addEventListener("keydown", function (e) {
        if (e.key === "Enter" || e.key === " ") { e.preventDefault(); open(); }
      });
    });

    dialog.querySelector(".lightbox-close").addEventListener("click", function () {
      dialog.close();
    });
    dialog.addEventListener("click", function (e) {
      if (e.target === dialog) dialog.close(); // backdrop click
    });
    dialog.addEventListener("keydown", function (e) {
      if (e.key === "ArrowRight") show(current + 1);
      if (e.key === "ArrowLeft") show(current - 1);
    });
  }

  /* ---------- 3. guide TOC ---------- */

  function initToc() {
    var toc = document.getElementById("guide-toc-list");
    var doc = document.querySelector(".guide-doc");
    if (!toc || !doc) return;
    var heads = Array.prototype.slice.call(doc.querySelectorAll("h2[id], h3[id]"));
    if (heads.length < 2) {
      var aside = toc.closest(".guide-toc");
      if (aside) aside.style.display = "none";
      return;
    }
    /* collapsed by default on small screens (it renders as a block above the doc there) */
    var details = document.getElementById("guide-toc-details");
    if (details && window.matchMedia("(max-width: 960px)").matches) {
      details.removeAttribute("open");
    }

    var links = {};
    heads.forEach(function (h) {
      var li = document.createElement("li");
      li.className = "toc-" + h.tagName.toLowerCase();
      var a = document.createElement("a");
      a.href = "#" + h.id;
      a.textContent = h.textContent;
      li.appendChild(a);
      toc.appendChild(li);
      links[h.id] = a;
    });

    if ("IntersectionObserver" in window) {
      var activeId = null;
      var observer = new IntersectionObserver(function (entries) {
        entries.forEach(function (entry) {
          if (entry.isIntersecting) {
            if (activeId && links[activeId]) links[activeId].classList.remove("active");
            activeId = entry.target.id;
            if (links[activeId]) links[activeId].classList.add("active");
          }
        });
      }, { rootMargin: "-80px 0px -70% 0px" });
      heads.forEach(function (h) { observer.observe(h); });
    }
  }

  /* ---------- 4. mobile nav ---------- */

  function initNav() {
    var header = document.querySelector(".site-header");
    if (!header) return;
    var toggle = header.querySelector(".nav-toggle");
    var menu = header.querySelector(".nav-menu");
    if (!toggle || !menu) return;

    function setOpen(open) {
      header.classList.toggle("nav-open", open);
      toggle.setAttribute("aria-expanded", open ? "true" : "false");
    }

    toggle.addEventListener("click", function () {
      setOpen(!header.classList.contains("nav-open"));
    });

    // a tap on any menu link dismisses the panel
    menu.addEventListener("click", function (e) {
      if (e.target.closest("a")) setOpen(false);
    });

    // Escape closes and returns focus to the toggle
    document.addEventListener("keydown", function (e) {
      if (e.key === "Escape" && header.classList.contains("nav-open")) {
        setOpen(false);
        toggle.focus();
      }
    });

    // a tap outside the header closes the panel
    document.addEventListener("click", function (e) {
      if (header.classList.contains("nav-open") && !header.contains(e.target)) {
        setOpen(false);
      }
    });

    // widening back to the desktop layout resets state
    var mq = window.matchMedia("(min-width: 761px)");
    var onChange = function (e) { if (e.matches) setOpen(false); };
    if (mq.addEventListener) mq.addEventListener("change", onChange);
    else if (mq.addListener) mq.addListener(onChange);
  }

  /* ---------- boot ---------- */

  function boot() {
    resolveDownload();
    initLightbox();
    initToc();
    initNav();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", boot);
  } else {
    boot();
  }
})();

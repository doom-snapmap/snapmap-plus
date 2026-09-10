/* Draw the decorative Baron silhouette on canvas[data-ghost] with Canvas 2D.
 * Animate only while visible; reduced motion uses one frame, and small screens skip it.
 * Data attributes: opacity (1), scale (1.15), below (0.18), x (0.5), parallax (0.1). */
(function () {
  "use strict";

  var POLY = [[-0.2472, 0.0], [-0.3925, 0.0], [-0.4001, 0.0147], [-0.4001, 0.037], [-0.3862, 0.0691], [-0.3722, 0.0873], [-0.3736, 0.1013], [-0.368, 0.1152], [-0.3436, 0.1494], [-0.3142, 0.1606], [-0.3052, 0.1725], [-0.294, 0.1976], [-0.2912, 0.2311], [-0.2828, 0.2493], [-0.2709, 0.2626], [-0.2598, 0.2654], [-0.2493, 0.2786], [-0.2549, 0.3219], [-0.2353, 0.3666], [-0.213, 0.3876], [-0.192, 0.4686], [-0.1536, 0.5056], [-0.1236, 0.5272], [-0.1068, 0.5552], [-0.1054, 0.5859], [-0.1013, 0.5915], [-0.1061, 0.5964], [-0.1201, 0.595], [-0.1774, 0.6536], [-0.199, 0.6362], [-0.2304, 0.588], [-0.25, 0.5768], [-0.2696, 0.574], [-0.2905, 0.5503], [-0.3115, 0.5363], [-0.3436, 0.5293], [-0.3589, 0.5447], [-0.3876, 0.558], [-0.3932, 0.5719], [-0.3932, 0.5901], [-0.4225, 0.6124], [-0.4323, 0.6376], [-0.4134, 0.6578], [-0.3911, 0.6648], [-0.3862, 0.6404], [-0.3785, 0.6369], [-0.3638, 0.6585], [-0.3568, 0.6865], [-0.3422, 0.7095], [-0.3317, 0.7018], [-0.3219, 0.6837], [-0.3261, 0.6655], [-0.3212, 0.6606], [-0.3017, 0.6844], [-0.2654, 0.6983], [-0.2605, 0.7297], [-0.2451, 0.7647], [-0.2465, 0.7996], [-0.2395, 0.8205], [-0.2207, 0.8408], [-0.1927, 0.8561], [-0.169, 0.8771], [-0.1564, 0.8785], [-0.1425, 0.8897], [-0.1173, 0.8827], [-0.0824, 0.8841], [-0.0726, 0.8743], [-0.0608, 0.8834], [-0.0594, 0.889], [-0.0649, 0.8973], [-0.0894, 0.9106], [-0.1432, 0.9672], [-0.1501, 0.9867], [-0.1466, 1.0], [-0.1131, 0.9972], [-0.0587, 0.9567], [-0.0503, 0.9567], [-0.0293, 0.9693], [-0.014, 0.9707], [0.0335, 0.9693], [0.0587, 0.9539], [0.0796, 0.9581], [0.0943, 0.97], [0.0943, 0.9784], [0.0817, 0.9853], [0.0866, 0.9916], [0.1061, 1.0], [0.1327, 1.0], [0.1446, 0.9909], [0.1473, 0.9811], [0.1362, 0.9448], [0.1103, 0.9134], [0.0635, 0.8792], [0.0538, 0.8457], [0.081, 0.8128], [0.0894, 0.8142], [0.0992, 0.8282], [0.1187, 0.8282], [0.125, 0.8233], [0.1278, 0.808], [0.1459, 0.7814], [0.1613, 0.7465], [0.1627, 0.7353], [0.1529, 0.692], [0.1627, 0.6711], [0.1599, 0.6557], [0.1641, 0.6334], [0.1983, 0.6215], [0.2416, 0.595], [0.257, 0.6131], [0.2919, 0.6313], [0.3534, 0.6313], [0.3624, 0.6222], [0.3589, 0.6131], [0.3492, 0.6159], [0.338, 0.6061], [0.3254, 0.6047], [0.3191, 0.5985], [0.3205, 0.5803], [0.3254, 0.5726], [0.3617, 0.5559], [0.405, 0.5615], [0.4323, 0.5538], [0.4232, 0.5433], [0.4029, 0.5342], [0.3897, 0.5028], [0.3631, 0.493], [0.3394, 0.4986], [0.3254, 0.4846], [0.2961, 0.486], [0.243, 0.5279], [0.2179, 0.5363], [0.1746, 0.5363], [0.1564, 0.5335], [0.1473, 0.5272], [0.1627, 0.4881], [0.2074, 0.4448], [0.2465, 0.3987], [0.2605, 0.3331], [0.2758, 0.2996], [0.2786, 0.2828], [0.2744, 0.2507], [0.2619, 0.2311], [0.2661, 0.2144], [0.2647, 0.1739], [0.2786, 0.1446], [0.3108, 0.1208], [0.3275, 0.0943], [0.3289, 0.0845], [0.3541, 0.058], [0.361, 0.0133], [0.352, 0.0056], [0.2807, 0.0056], [0.2332, 0.0126], [0.1934, 0.0258], [0.1878, 0.0384], [0.192, 0.0468], [0.1781, 0.0705], [0.1906, 0.0985], [0.1865, 0.1222], [0.1655, 0.1627], [0.1446, 0.1837], [0.139, 0.2088], [0.139, 0.2451], [0.1208, 0.2689], [0.1208, 0.2898], [0.1306, 0.3108], [0.0223, 0.4078], [0.0, 0.4134], [-0.0698, 0.3645], [-0.1041, 0.3457], [-0.1041, 0.3387], [-0.0957, 0.3303], [-0.0943, 0.2968], [-0.1397, 0.2486], [-0.1501, 0.2423], [-0.1557, 0.1878], [-0.192, 0.1529], [-0.2116, 0.1208], [-0.2144, 0.0789], [-0.2088, 0.0649], [-0.227, 0.0384], [-0.2311, 0.0147], [-0.2472, 0.0]];
  var CYAN = "74, 201, 240";
  var DEPTH = 0.09;   // hologram plate thickness (figure height = 1)
  var FOCAL = 3.0;

  function mulberry32(a) {
    return function () {
      a |= 0; a = (a + 0x6D2B79F5) | 0;
      var t = Math.imul(a ^ (a >>> 15), 1 | a);
      t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
  }

  function inside(x, y) {
    var c = false;
    for (var i = 0, j = POLY.length - 1; i < POLY.length; j = i++) {
      var xi = POLY[i][0], yi = POLY[i][1], xj = POLY[j][0], yj = POLY[j][1];
      if ((yi > y) !== (yj > y) && x < ((xj - xi) * (y - yi)) / (yj - yi) + xi) c = !c;
    }
    return c;
  }

  var CLOUD = null;
  function cloud() {
    if (!CLOUD) {
      var rng = mulberry32(1337);
      CLOUD = [];
      for (var y = 0; y <= 1; y += 0.016) {
        for (var x = -0.55; x <= 0.55; x += 0.016) {
          var jx = x + (rng() - 0.5) * 0.012, jy = y + (rng() - 0.5) * 0.012;
          if (inside(jx, jy)) CLOUD.push([jx, jy, (rng() - 0.5) * DEPTH]);
        }
      }
    }
    return CLOUD;
  }

  function mount(canvas) {
    var ctx = canvas.getContext("2d");
    if (!ctx) return;
    var reduced = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    var op = parseFloat(canvas.dataset.ghostOpacity || "1");
    var scaleF = parseFloat(canvas.dataset.ghostScale || "1.15");
    var below = parseFloat(canvas.dataset.ghostBelow || "0.18");
    var xF = parseFloat(canvas.dataset.ghostX || "0.5");
    var parallax = parseFloat(canvas.dataset.ghostParallax || "0.1");

    var W = 0, H = 0;
    function resize() {
      var r = canvas.getBoundingClientRect();
      var dpr = Math.min(window.devicePixelRatio || 1, 2);
      W = r.width; H = r.height;
      canvas.width = Math.round(W * dpr);
      canvas.height = Math.round(H * dpr);
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      if (reduced) draw(t0);
    }

    function strokeContour(th, S, cx, cy, z, alpha) {
      ctx.strokeStyle = "rgba(" + CYAN + "," + alpha + ")";
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      var c = Math.cos(th), s = Math.sin(th);
      for (var i = 0; i < POLY.length; i++) {
        var rx = POLY[i][0] * c + z * s;
        var rz = -POLY[i][0] * s + z * c;
        var k = FOCAL / (FOCAL + rz);
        var px = cx + rx * S * k, py = cy - POLY[i][1] * S * k;
        if (i === 0) ctx.moveTo(px, py); else ctx.lineTo(px, py);
      }
      ctx.closePath();
      ctx.stroke();
    }

    var t0 = performance.now();
    function draw(now) {
      var t = (now - t0) / 1000;
      var th = reduced ? -0.3 : (-0.1 + Math.sin(t * 0.18) * 0.32);
      var S = H * scaleF;
      var cx = W * xF;
      var cy = H + below * S;
      if (!reduced && parallax > 0) {
        cy += canvas.getBoundingClientRect().top * -parallax;
      }
      ctx.clearRect(0, 0, W, H);

      strokeContour(th, S, cx, cy, -DEPTH / 2, 0.10 * op);

      var shimmer = reduced ? 1 : (0.85 + 0.15 * Math.sin(t * 2.1));
      ctx.fillStyle = "rgba(" + CYAN + "," + (0.13 * op * shimmer) + ")";
      var pts = cloud(), c = Math.cos(th), s = Math.sin(th);
      for (var i = 0; i < pts.length; i++) {
        var rx = pts[i][0] * c + pts[i][2] * s;
        var rz = -pts[i][0] * s + pts[i][2] * c;
        var k = FOCAL / (FOCAL + rz);
        ctx.fillRect(cx + rx * S * k, cy - pts[i][1] * S * k, 1.6, 1.6);
      }

      strokeContour(th, S, cx, cy, DEPTH / 2, 0.30 * op);

      if (!reduced) {
        var phase = (t * 0.12) % 1.3;
        if (phase <= 1.02) {
          ctx.fillStyle = "rgba(" + CYAN + "," + (0.06 * op) + ")";
          ctx.fillRect(cx - S * 0.55, cy - phase * S, S * 1.1, 1.5);
        }
      }
    }

    var running = false, raf = 0;
    function loop(now) { draw(now); raf = requestAnimationFrame(loop); }
    function start() { if (!running) { running = true; raf = requestAnimationFrame(loop); } }
    function stop() { if (running) { running = false; cancelAnimationFrame(raf); } }

    resize();
    window.addEventListener("resize", resize);
    if (reduced) { draw(t0); return; }
    if ("IntersectionObserver" in window) {
      new IntersectionObserver(function (entries) {
        entries.forEach(function (e) { if (e.isIntersecting) start(); else stop(); });
      }).observe(canvas);
    } else {
      start();
    }
  }

  function boot() {
    if (window.innerWidth < 700) return; // decoration only — skip on small screens
    document.querySelectorAll("canvas[data-ghost]").forEach(mount);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", boot);
  } else {
    boot();
  }
})();

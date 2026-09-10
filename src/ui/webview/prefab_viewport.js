/* Prefab Details interactive 3D viewport. Pure ASCII; embedded by src/ui/build.ps1. */
var prefabViewport = (function () {
  var finite = prefabTransform.finite;
  var vec3 = prefabTransform.vec3;
  var orientationAxes = prefabTransform.orientationAxes;
  var composeMatrix = prefabTransform.composeMatrix;
  var composeBottomAnchoredBox = prefabTransform.composeBottomAnchoredBox;
  var transformPoint = prefabTransform.transformPoint;
  var host = document.getElementById('pcPreview');
  var canvas = document.getElementById('pcCanvas');
  var status = document.getElementById('pcPreviewStatus');
  var gl = null, program = null;
  var loc = {};
  var cube = null, grid = null, wireCube = null, wireCylinder = null;
  var markerMeshes = {};
  var generation = 0;
  var entities = [];
  var requested = {}, resolving = {}, inheritModels = {};
  var pending = 0, failures = 0;
  var meshCache = {}, cacheBytes = 0, cacheClock = 0;
  var CACHE_LIMIT = 24 * 1024 * 1024;
  var TRIANGLE_BUDGET = 250000;
  var framePending = false;
  var yaw = 0.75, pitch = 0.48, distance = 256;
  var target = [0, 0, 0], sceneCenter = [0, 0, 0], sceneRadius = 64;
  var sceneBounds = [-32, -32, -32, 32, 32, 32];
  var cameraTouched = false;
  var lastSceneText = '';
  var dragging = false, lastPointerX = 0, lastPointerY = 0;
  var normalMatrixScratch = new Float32Array(9);

  var MARKER_MODELS = {
    logic:'models/snapmaps/logic/snapedit_logic_hexagon.lwo',
    input:'models/snapmaps/logic/snapedit_logic_circle.lwo',
    output:'models/snapmaps/logic/snapedit_logic_circle.lwo',
    filter:'models/snapmaps/logic/snapedit_logic_diamond.lwo',
    system:'models/snapmaps/logic/snapedit_logic_hexagon.lwo'
  };
  /* The editor renders the semantic hex body at full size and its I/O/filter attachments at half
     scale. The cooked source meshes themselves are similarly sized (the diamond is actually wider),
     so using them all at 1.0 reverses the in-game hierarchy. */
  var MARKER_SCALES = {
    logic:[1,1,1], system:[1,1,1], input:[.5,.5,.5], output:[.5,.5,.5], filter:[.5,.5,.5]
  };
  var MARKER_PROXY_SIZES = {
    logic:[8,43,37.5], system:[8,43,37.5],
    input:[4,21.5,21.5], output:[4,21.5,21.5], filter:[4,28,28]
  };

  function setStatus(text) { if (status) status.textContent = text || ''; }

  function markDirty() {
    if (framePending) return;
    framePending = true;
    requestAnimationFrame(function () {
      framePending = false;
      try { draw(); }
      catch (e) {
        setStatus('3D preview render failed');
        if (window.console && console.error) console.error('Prefab preview draw failed', e);
      }
    });
  }

  function shader(type, source) {
    var s = gl.createShader(type);
    gl.shaderSource(s, source); gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
      var why = gl.getShaderInfoLog(s) || 'unknown shader error';
      gl.deleteShader(s); throw new Error(why);
    }
    return s;
  }

  function makeProgram() {
    var vs = shader(gl.VERTEX_SHADER,
      '#version 300 es\n' +
      'layout(location=0) in vec3 aPosition;\n' +
      'layout(location=1) in vec4 aNormalPacked;\n' +
      'layout(location=2) in float aFade;\n' +
      'uniform mat4 uViewProjection;\n' +
      'uniform mat4 uModel;\n' +
      'uniform mat3 uNormalMatrix;\n' +
      'out float vLight;\n' +
      'out float vFade;\n' +
      'void main(){\n' +
      ' vec3 transformedNormal=uNormalMatrix*(aNormalPacked.xyz*2.0-1.0);\n' +
      ' vec3 normal=dot(transformedNormal,transformedNormal)>0.00000001?normalize(transformedNormal):vec3(0,0,1);\n' +
      ' vec3 lightDir=normalize(vec3(-0.45,0.35,0.82));\n' +
      ' vLight=0.38+0.62*max(dot(normal,lightDir),0.0);\n' +
      ' vFade=aFade;\n' +
      ' gl_Position=uViewProjection*uModel*vec4(aPosition,1.0);\n' +
      '}');
    var fs = shader(gl.FRAGMENT_SHADER,
      '#version 300 es\n' +
      'precision mediump float;\n' +
      'uniform vec3 uColor;\n' +
      'uniform float uUnlit;\n' +
      'uniform float uAlpha;\n' +
      'in float vLight;\n' +
      'in float vFade;\n' +
      'out vec4 outColor;\n' +
      'void main(){ float l=mix(vLight,1.0,uUnlit); outColor=vec4(uColor*l,uAlpha*vFade); }');
    var p = gl.createProgram();
    gl.attachShader(p, vs); gl.attachShader(p, fs); gl.linkProgram(p);
    gl.deleteShader(vs); gl.deleteShader(fs);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) {
      var why = gl.getProgramInfoLog(p) || 'unknown program error';
      gl.deleteProgram(p); throw new Error(why);
    }
    return p;
  }

  function encodedNormal(n) { return Math.max(0, Math.min(255, Math.round((n * 0.5 + 0.5) * 255))); }

  function makeCube() {
    var faces = [
      [[ 1, 0, 0], [[.5,-.5,-.5],[.5,.5,-.5],[.5,.5,.5],[.5,-.5,.5]]],
      [[-1, 0, 0], [[-.5,.5,-.5],[-.5,-.5,-.5],[-.5,-.5,.5],[-.5,.5,.5]]],
      [[ 0, 1, 0], [[.5,.5,-.5],[-.5,.5,-.5],[-.5,.5,.5],[.5,.5,.5]]],
      [[ 0,-1, 0], [[-.5,-.5,-.5],[.5,-.5,-.5],[.5,-.5,.5],[-.5,-.5,.5]]],
      [[ 0, 0, 1], [[-.5,-.5,.5],[.5,-.5,.5],[.5,.5,.5],[-.5,.5,.5]]],
      [[ 0, 0,-1], [[-.5,.5,-.5],[.5,.5,-.5],[.5,-.5,-.5],[-.5,-.5,-.5]]]
    ];
    var bytes = new ArrayBuffer(24 * 16), view = new DataView(bytes), indices = new Uint32Array(36);
    var v = 0, ii = 0;
    for (var f = 0; f < faces.length; f++) {
      var normal = faces[f][0], corners = faces[f][1];
      for (var c = 0; c < 4; c++, v++) {
        var off = v * 16;
        view.setFloat32(off, corners[c][0], true);
        view.setFloat32(off + 4, corners[c][1], true);
        view.setFloat32(off + 8, corners[c][2], true);
        view.setUint8(off + 12, encodedNormal(normal[0]));
        view.setUint8(off + 13, encodedNormal(normal[1]));
        view.setUint8(off + 14, encodedNormal(normal[2]));
        view.setUint8(off + 15, 255);
      }
      var b = f * 4;
      indices[ii++] = b; indices[ii++] = b + 1; indices[ii++] = b + 2;
      indices[ii++] = b; indices[ii++] = b + 2; indices[ii++] = b + 3;
    }
    return uploadMesh(new Uint8Array(bytes), indices, [-.5,-.5,-.5,.5,.5,.5], 'proxy');
  }

  function makeLineMesh(points, indices) {
    var vao = gl.createVertexArray(), vb = gl.createBuffer(), ib = gl.createBuffer();
    gl.bindVertexArray(vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, vb); gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(points), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 12, 0);
    gl.disableVertexAttribArray(1);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ib); gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint32Array(indices), gl.STATIC_DRAW);
    gl.bindVertexArray(null);
    return {vao:vao, vb:vb, ib:ib, count:indices.length};
  }

  function makeWireCube() {
    return makeLineMesh([-.5,-.5,-.5, .5,-.5,-.5, .5,.5,-.5, -.5,.5,-.5,
                         -.5,-.5,.5,  .5,-.5,.5,  .5,.5,.5,  -.5,.5,.5],
      [0,1,1,2,2,3,3,0, 4,5,5,6,6,7,7,4, 0,4,1,5,2,6,3,7]);
  }

  function makeWireCylinder() {
    var segments = 24, points = [], indices = [];
    for (var z = 0; z < 2; z++) for (var i = 0; i < segments; i++) {
      var a = i * Math.PI * 2 / segments;
      points.push(Math.cos(a)*.5, Math.sin(a)*.5, z ? .5 : -.5);
    }
    for (i = 0; i < segments; i++) {
      var next = (i + 1) % segments;
      indices.push(i,next, segments+i,segments+next);
      if ((i & 3) === 0) indices.push(i,segments+i);
    }
    return makeLineMesh(points, indices);
  }

  function makeMarkerPrism(sides, rotation, name) {
    var positions = [], normals = [], indices = [];
    function vertex(x,y,z,nx,ny,nz) {
      positions.push(x,y,z); normals.push(nx,ny,nz); return positions.length/3-1;
    }
    /* Installed SnapMap editor glyphs are thin on local X and occupy the YZ plane. Keep the
       procedural fallback on those same axes so a transport/decoder fallback does not turn it. */
    var frontCenter = vertex(.5,0,0,1,0,0), backCenter = vertex(-.5,0,0,-1,0,0);
    var front = [], back = [];
    for (var i=0;i<sides;i++) {
      var a=rotation+i*Math.PI*2/sides;
      front.push(vertex(.5,Math.cos(a)*.5,Math.sin(a)*.5,1,0,0));
      back.push(vertex(-.5,Math.cos(a)*.5,Math.sin(a)*.5,-1,0,0));
    }
    for (i=0;i<sides;i++) {
      var next=(i+1)%sides;
      indices.push(frontCenter,front[i],front[next], backCenter,back[next],back[i]);
      a=rotation+(i+.5)*Math.PI*2/sides;
      var ny=Math.cos(a),nz=Math.sin(a);
      var s0=vertex(.5,positions[front[i]*3+1],positions[front[i]*3+2],0,ny,nz);
      var s1=vertex(.5,positions[front[next]*3+1],positions[front[next]*3+2],0,ny,nz);
      var s2=vertex(-.5,positions[front[next]*3+1],positions[front[next]*3+2],0,ny,nz);
      var s3=vertex(-.5,positions[front[i]*3+1],positions[front[i]*3+2],0,ny,nz);
      indices.push(s0,s3,s2,s0,s2,s1);
    }
    var bytes=new ArrayBuffer(positions.length/3*16),view=new DataView(bytes);
    for (i=0;i<positions.length/3;i++) {
      var off=i*16;
      view.setFloat32(off,positions[i*3],true); view.setFloat32(off+4,positions[i*3+1],true);
      view.setFloat32(off+8,positions[i*3+2],true);
      view.setUint8(off+12,encodedNormal(normals[i*3]));
      view.setUint8(off+13,encodedNormal(normals[i*3+1]));
      view.setUint8(off+14,encodedNormal(normals[i*3+2])); view.setUint8(off+15,255);
    }
    return uploadMesh(new Uint8Array(bytes),new Uint32Array(indices),[-.5,-.5,-.5,.5,.5,.5],name);
  }

  function uploadMesh(vertices, indices, bounds, name) {
    var vao = gl.createVertexArray(), vb = gl.createBuffer(), ib = gl.createBuffer();
    gl.bindVertexArray(vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, vb); gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 16, 0);
    gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 4, gl.UNSIGNED_BYTE, true, 16, 12);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ib); gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, indices, gl.STATIC_DRAW);
    gl.bindVertexArray(null);
    return {name:name, vao:vao, vb:vb, ib:ib, count:indices.length, bounds:bounds,
            bytes:vertices.byteLength + indices.byteLength, lastUse:++cacheClock};
  }

  function initGl() {
    if (gl) return true;
    if (!canvas) return false;
    try {
      gl = canvas.getContext('webgl2', {alpha:false, antialias:true, depth:true,
                                       powerPreference:'low-power', preserveDrawingBuffer:false});
      if (!gl) throw new Error('WebGL2 is unavailable');
      program = makeProgram();
      loc.viewProjection = gl.getUniformLocation(program, 'uViewProjection');
      loc.model = gl.getUniformLocation(program, 'uModel');
      loc.normalMatrix = gl.getUniformLocation(program, 'uNormalMatrix');
      loc.color = gl.getUniformLocation(program, 'uColor');
      loc.unlit = gl.getUniformLocation(program, 'uUnlit');
      loc.alpha = gl.getUniformLocation(program, 'uAlpha');
      cube = makeCube();
      wireCube = makeWireCube(); wireCylinder = makeWireCylinder();
      markerMeshes.logic = makeMarkerPrism(6, Math.PI/6, 'logic-marker');
      markerMeshes.input = makeMarkerPrism(20, 0, 'input-marker');
      markerMeshes.output = markerMeshes.input;
      markerMeshes.filter = makeMarkerPrism(4, Math.PI/4, 'filter-marker');
      markerMeshes.system = markerMeshes.logic;
      /* Attribute 2 is enabled only by the fading grid VAO. Every mesh sees this constant. */
      gl.vertexAttrib1f(2,1);
      gl.enable(gl.DEPTH_TEST); gl.depthFunc(gl.LEQUAL);
      gl.disable(gl.CULL_FACE);
      return true;
    } catch (e) {
      gl = null; program = null; cube = null;
      setStatus('3D preview unavailable (WebGL2)');
      return false;
    }
  }

  function entityRole(className, inherit, edit) {
    var c=String(className||'').toLowerCase(), h=String(inherit||'').toLowerCase();
    if (c.indexOf('volume_blocking') !== -1 || h === 'snapmaps/volume/blocking') return 'blocker';
    /* Most saved SnapMap entities have isVisible=false, including solid props, pickups, and logic.
       It is editor/runtime state, not a semantic hidden-volume flag. Inheritance distinguishes an
       interactable whose implementation class happens to be idVolume_Trigger_Editable from an
       actual trigger volume. */
    if (c.indexOf('interactable') !== -1 || h.indexOf('snapmaps/interactibles/') === 0) return 'interactable';
    if (c.indexOf('snapmaplistener') !== -1 || h.indexOf('snapmaps/listener/') === 0) return 'output';
    if (c.indexOf('snapmapaction') !== -1 || h.indexOf('snapmaps/action/') === 0) return 'input';
    if (c.indexOf('filter') !== -1 || c.indexOf('compare') !== -1 ||
        h.indexOf('snapmaps/filter/') === 0) return 'filter';
    if (c.indexOf('snapmaplogic') !== -1 || c.indexOf('target_timeline') !== -1 ||
        h.indexOf('snapmaps/logic/') === 0) return 'logic';
    if (c.indexOf('dynamicstamp') !== -1 || h.indexOf('snapmaps/fx/decal') === 0) return 'decal';
    if (c.indexOf('snapmapgameentity_spawner') === -1 &&
        /snapmapgameentity_(?:player|worldtext|light|speaker|codexmessage|message)/.test(c)) return 'system';
    if (c.indexOf('volume_trigger') !== -1 || h.indexOf('snapmaps/triggers/') === 0) return 'trigger';
    return 'prop';
  }

  function roleColor(role, className) {
    if (role === 'trigger') return [0.22,0.72,0.76];
    if (role === 'blocker') return [0.34,0.58,0.78];
    if (role === 'interactable') return [0.86,0.66,0.29];
    if (role === 'logic') return [0.61,0.47,0.82];
    if (role === 'input') return [0.86,0.43,0.31];
    if (role === 'output') return [0.28,0.68,0.79];
    if (role === 'filter') return [0.72,0.50,0.75];
    if (role === 'system') return [0.42,0.58,0.80];
    if (role === 'decal') return [0.70,0.74,0.78];
    var c=String(className||'').toLowerCase();
    if (c.indexOf('ai') !== -1 || c.indexOf('monster') !== -1) return [0.78,0.39,0.34];
    return [0.64,0.67,0.71];
  }

  function renderModelItems(value) {
    var numbered=[];
    if (!value || typeof value !== 'object') return numbered;
    for (var key in value) if (Object.prototype.hasOwnProperty.call(value,key)) {
      var match=/^item\[(\d+)\]$/.exec(key), name=value[key];
      if (match && typeof name === 'string' && name) numbered.push([Number(match[1]),name]);
    }
    numbered.sort(function(a,b){return a[0]-b[0];});
    return numbered.map(function(item){return item[1];});
  }

  function isMeshName(name) {
    return /\.(?:lwo|bmodel|md6)$/i.test(String(name||''));
  }

  function selectEntityModel(role, rmi, edit) {
    if (MARKER_MODELS[role]) return MARKER_MODELS[role];
    var direct=(typeof rmi.model === 'string')?rmi.model:'';
    var listed=renderModelItems(edit.renderModels);
    if (role === 'blocker' && listed.length) {
      /* item[0] is the editor selection/trigger shell. Prefer the visible textured or solid shell. */
      if (rmi.customMaterial && listed[2]) return listed[2];
      return listed[1] || listed[2] || listed[0];
    }
    if (isMeshName(direct)) return direct;
    if (role === 'trigger' && listed.length) return listed[0];
    return listed[0] || '';
  }

  function updateEntityTransform(entity, defaultScale, hasDefaultScale) {
    defaultScale = defaultScale || [1,1,1];
    var scale = vec3(entity.scalePatch, defaultScale);
    for (var i = 0; i < 3; i++) if (Math.abs(scale[i]) < 0.0001) scale[i] = 0.0001;
    var markerScale = MARKER_SCALES[entity.role] || [1,1,1];
    var displayScale = [scale[0]*markerScale[0], scale[1]*markerScale[1],
                        scale[2]*markerScale[2]];
    var inheritedSize = hasDefaultScale &&
      (Math.abs(defaultScale[0]-1)>.0001 || Math.abs(defaultScale[1]-1)>.0001 ||
       Math.abs(defaultScale[2]-1)>.0001);
    var size;
    if (entity.role === 'decal') {
      size=[.5,Math.abs(finite(entity.stampSize.x,12)),Math.abs(finite(entity.stampSize.y,12))];
    } else if (MARKER_PROXY_SIZES[entity.role]) {
      var markerSize=MARKER_PROXY_SIZES[entity.role];
      size=[markerSize[0]*Math.abs(scale[0]),markerSize[1]*Math.abs(scale[1]),
            markerSize[2]*Math.abs(scale[2])];
    } else if (entity.role==='blocker' || entity.role==='trigger' ||
               entity.hasScalePatch || inheritedSize) {
      size=[Math.abs(scale[0]),Math.abs(scale[1]),Math.abs(scale[2])];
    } else if (entity.hasClipSizePatch) {
      size=vec3(entity.clipSizePatch,[32,32,32]).map(Math.abs);
    } else if (entity.model) size=[48,48,48];
    else if (/logic|listener|variable|filter/i.test(entity.className)) size=[12,12,12];
    else size=[32,32,32];
    /* Preserve thin slabs and decals; only expand dimensions that make a singular proxy. */
    for (i = 0; i < 3; i++) if (size[i] < .01) size[i] = .01;
    entity.modelMatrix=composeMatrix(entity.position,entity.axes,displayScale);
    entity.proxyMatrix=(entity.role==='blocker'||entity.role==='trigger')
      ? composeBottomAnchoredBox(entity.position,entity.axes,size)
      : composeMatrix(entity.position,entity.axes,size);
    entity.helperShape=/cylinder/i.test(entity.inherit+entity.model)?'cylinder':'box';
  }

  function entityFromJson(raw) {
    var def = raw && raw.entityDef ? raw.entityDef : {};
    var state = def.state || {}, edit = state.edit || {};
    var rmi = edit.renderModelInfo || {};
    var className = (typeof def.className === 'string') ? def.className : '';
    var inherit = (typeof def.inherit === 'string') ? def.inherit : '';
    var role = entityRole(className, inherit, edit);
    var entity={inherit:inherit, model:selectEntityModel(role,rmi,edit), className:className, role:role,
      resolveModel:role!=='decal'&&!MARKER_MODELS[role], color:roleColor(role,className),
      position:vec3(edit.spawnPosition,[0,0,0]), axes:orientationAxes(edit.spawnOrientation),
      scalePatch:(rmi.scale&&typeof rmi.scale==='object')?rmi.scale:null,
      hasScalePatch:!!(rmi.scale&&typeof rmi.scale==='object'),
      clipSizePatch:(edit.clipModelInfo||{}).size||null,
      hasClipSizePatch:!!(edit.clipModelInfo&&(edit.clipModelInfo.size)), stampSize:edit.size||{},
      modelMatrix:null, proxyMatrix:null, helperShape:'box'};
    updateEntityTransform(entity,[1,1,1],false);
    return entity;
  }

  function extendBounds(bounds, matrix, local) {
    for (var ix = 0; ix < 2; ix++) for (var iy = 0; iy < 2; iy++) for (var iz = 0; iz < 2; iz++) {
      var p = transformPoint(matrix, local[ix ? 3 : 0], local[iy ? 4 : 1], local[iz ? 5 : 2]);
      if (p[0] < bounds[0]) bounds[0] = p[0]; if (p[1] < bounds[1]) bounds[1] = p[1];
      if (p[2] < bounds[2]) bounds[2] = p[2]; if (p[0] > bounds[3]) bounds[3] = p[0];
      if (p[1] > bounds[4]) bounds[4] = p[1]; if (p[2] > bounds[5]) bounds[5] = p[2];
    }
  }

  function rebuildGrid() {
    if (!gl) return;
    var raw = Math.max(sceneRadius / 7, 1);
    var step = Math.pow(2, Math.floor(Math.log(raw) / Math.LN2));
    if (!isFinite(step) || step < 1) step = 1;
    var cx = Math.round(sceneCenter[0] / step) * step;
    var cy = Math.round(sceneCenter[1] / step) * step;
    var z = sceneBounds[2] - step * 0.015, reach = 14, half = step * reach;
    var lines = [];
    function fadeAt(x,y) {
      var radial=Math.sqrt((x-cx)*(x-cx)+(y-cy)*(y-cy))/half;
      if (radial<=.58) return 1;
      return Math.pow(Math.max(0,(1-radial)/.42),1.25);
    }
    function point(x,y) { lines.push(x,y,z,fadeAt(x,y)); }
    /* Subdivide each Cartesian line at cell boundaries so the interpolated alpha follows a circular
       mask instead of fading only from one end of a long line to the other. */
    for (var i=-reach;i<=reach;i++) {
      for (var j=-reach;j<reach;j++) {
        point(cx+j*step,cy+i*step); point(cx+(j+1)*step,cy+i*step);
        point(cx+i*step,cy+j*step); point(cx+i*step,cy+(j+1)*step);
      }
    }
    if (!grid) grid = {vao:gl.createVertexArray(), vb:gl.createBuffer(), count:0};
    gl.bindVertexArray(grid.vao); gl.bindBuffer(gl.ARRAY_BUFFER, grid.vb);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(lines), gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 16, 0);
    gl.disableVertexAttribArray(1); gl.bindVertexArray(null);
    gl.bindVertexArray(grid.vao); gl.enableVertexAttribArray(2);
    gl.vertexAttribPointer(2,1,gl.FLOAT,false,16,12); gl.bindVertexArray(null);
    grid.count = lines.length / 4;
  }

  function refreshBounds(autoFrame) {
    var primary = [Infinity,Infinity,Infinity,-Infinity,-Infinity,-Infinity];
    var all = [Infinity,Infinity,Infinity,-Infinity,-Infinity,-Infinity];
    for (var i = 0; i < entities.length; i++) {
      var e = entities[i], mesh = e.model ? meshCache[e.model] : null;
      var matrix=mesh?e.modelMatrix:e.proxyMatrix;
      var local=mesh?mesh.bounds:[-.5,-.5,-.5,.5,.5,.5];
      extendBounds(all,matrix,local);
      /* Invisible trigger shells may be much larger than the actual prefab. Keep them visible, but
         do not let one 512-unit touch volume reduce the solid scene to a speck when auto-framing. */
      if (e.role !== 'trigger') extendBounds(primary,matrix,local);
    }
    var bounds=isFinite(primary[0])?primary:all;
    if (!isFinite(bounds[0])) bounds = [-32,-32,-32,32,32,32];
    sceneBounds = bounds;
    sceneCenter = [(bounds[0]+bounds[3])*.5, (bounds[1]+bounds[4])*.5, (bounds[2]+bounds[5])*.5];
    var dx = bounds[3]-bounds[0], dy = bounds[4]-bounds[1], dz = bounds[5]-bounds[2];
    sceneRadius = Math.max(Math.sqrt(dx*dx+dy*dy+dz*dz)*.5, 8);
    if (autoFrame) {
      target = sceneCenter.slice();
      distance = Math.max(sceneRadius * 2.25, 32);
    }
    rebuildGrid(); markDirty();
  }

  function requestMesh(name) {
    if (!name || meshCache[name] || requested[name]) return;
    requested[name] = 'pending'; pending++;
    if (typeof post === 'function') post({cmd:'requestPrefabMesh', generation:generation, model:name});
    updateStatus();
  }

  function resolveInherit(inherit) {
    if (!inherit || resolving[inherit]) return;
    if (Object.prototype.hasOwnProperty.call(inheritModels, inherit)) {
      applyResolvedModel(inherit, inheritModels[inherit]); return;
    }
    resolving[inherit] = true; pending++;
    if (typeof post === 'function') post({cmd:'resolvePrefabModel', generation:generation, inherit:inherit});
  }

  function applyResolvedModel(inherit, info) {
    if (typeof info === 'string') info={model:info,scale:null};
    info=info||{model:'',scale:null};
    for (var i = 0; i < entities.length; i++) {
      var entity=entities[i];
      if (!entity.resolveModel || entity.inherit !== inherit) continue;
      if (!entity.model && info.model) entity.model=info.model;
      updateEntityTransform(entity,info.scale||[1,1,1],!!info.scale);
      if (entity.model) requestMesh(entity.model);
    }
  }

  function updateStatus() {
    if (!gl) return;
    if (!entities.length) setStatus('No renderable entities');
    else if (pending > 0) setStatus('Loading installed geometry...');
    else if (failures > 0) setStatus('Interactive preview - simplified where needed');
    else setStatus('Interactive preview');
  }

  function activeModel(name) {
    for (var i = 0; i < entities.length; i++) if (entities[i].model === name) return true;
    return false;
  }

  function evictFor(bytes) {
    while (cacheBytes + bytes > CACHE_LIMIT) {
      var oldest = null;
      for (var name in meshCache) if (Object.prototype.hasOwnProperty.call(meshCache, name)) {
        var candidate = meshCache[name];
        if (!activeModel(name) && (!oldest || candidate.lastUse < oldest.lastUse)) oldest = candidate;
      }
      if (!oldest) return cacheBytes + bytes <= CACHE_LIMIT;
      gl.deleteVertexArray(oldest.vao); gl.deleteBuffer(oldest.vb); gl.deleteBuffer(oldest.ib);
      delete meshCache[oldest.name]; cacheBytes -= oldest.bytes;
    }
    return true;
  }

  function decodeName(bytes) {
    if (window.TextDecoder) return new TextDecoder('utf-8').decode(bytes);
    var s = ''; for (var i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
    try { return decodeURIComponent(escape(s)); } catch (e) { return s; }
  }

  function receiveMeshBuffer(buffer) {
    if (!buffer || buffer.byteLength < 56 || !gl) return;
    var view = new DataView(buffer);
    if (view.getUint32(0, true) !== 0x314d5053 || view.getUint16(4, true) !== 1) return;
    var statusCode = view.getUint16(6, true), gen = view.getUint32(8, true);
    var nameBytes = view.getUint32(12, true), vertexCount = view.getUint32(16, true);
    var indexCount = view.getUint32(20, true), stride = view.getUint32(48, true);
    var namePadded = (nameBytes + 3) & ~3;
    var vertexOffset = 56 + namePadded, vertexBytes = vertexCount * stride;
    var indexOffset = vertexOffset + vertexBytes, indexBytes = indexCount * 4;
    if (gen !== generation || nameBytes >= 512 || stride !== 16 ||
        indexOffset > buffer.byteLength || indexBytes > buffer.byteLength - indexOffset) return;
    var name = decodeName(new Uint8Array(buffer, 56, nameBytes));
    if (requested[name] === 'pending') pending--;
    requested[name] = statusCode === 1 ? 'ready' : 'failed';
    if (statusCode !== 1 || !vertexCount || !indexCount) {
      failures++; updateStatus(); markDirty(); return;
    }
    if (meshCache[name]) { updateStatus(); return; }
    if (!evictFor(vertexBytes + indexBytes)) {
      failures++; requested[name] = 'failed'; updateStatus(); return;
    }
    var bounds = [];
    for (var b = 0; b < 6; b++) bounds.push(view.getFloat32(24 + b*4, true));
    var mesh = uploadMesh(new Uint8Array(buffer, vertexOffset, vertexBytes),
                          new Uint32Array(buffer, indexOffset, indexCount), bounds, name);
    meshCache[name] = mesh; cacheBytes += mesh.bytes;
    refreshBounds(!cameraTouched); updateStatus();
  }

  function modelResolved(d) {
    if (!d || d.generation !== generation || !resolving[d.inherit]) return;
    delete resolving[d.inherit]; pending--;
    var scale=Array.isArray(d.scale)&&d.scale.length>=3
      ? [finite(d.scale[0],1),finite(d.scale[1],1),finite(d.scale[2],1)] : null;
    var info={model:d.model||'',scale:scale};
    inheritModels[d.inherit]=info;
    applyResolvedModel(d.inherit,info);
    refreshBounds(!cameraTouched); updateStatus();
  }

  function meshUnavailable(d) {
    if (!d || d.generation !== generation || requested[d.model] !== 'pending') return;
    requested[d.model] = 'failed'; pending--; failures++;
    updateStatus(); markDirty();
  }

  function mat4Identity() {
    return new Float32Array([1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]);
  }

  function mat4Multiply(a, b) {
    var out = new Float32Array(16);
    for (var c = 0; c < 4; c++) for (var r = 0; r < 4; r++)
      out[c*4+r] = a[r]*b[c*4] + a[4+r]*b[c*4+1] + a[8+r]*b[c*4+2] + a[12+r]*b[c*4+3];
    return out;
  }

  function perspective(fovy, aspect, near, far) {
    var f = 1 / Math.tan(fovy / 2), nf = 1 / (near - far), out = new Float32Array(16);
    out[0] = f/aspect; out[5] = f; out[10] = (far+near)*nf; out[11] = -1;
    out[14] = 2*far*near*nf; return out;
  }

  function normalize(v) {
    var n = Math.sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]) || 1;
    return [v[0]/n,v[1]/n,v[2]/n];
  }

  function cross(a,b) { return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]; }
  function dot(a,b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }

  function lookAt(eye, center) {
    var z = normalize([eye[0]-center[0],eye[1]-center[1],eye[2]-center[2]]);
    var x = normalize(cross([0,0,1], z)), y = cross(z, x);
    var out = mat4Identity();
    out[0]=x[0]; out[1]=y[0]; out[2]=z[0];
    out[4]=x[1]; out[5]=y[1]; out[6]=z[1];
    out[8]=x[2]; out[9]=y[2]; out[10]=z[2];
    out[12]=-dot(x,eye); out[13]=-dot(y,eye); out[14]=-dot(z,eye);
    return out;
  }

  function cssRgb(variable, fallback) {
    var s = getComputedStyle(document.documentElement).getPropertyValue(variable).trim();
    if (/^#[0-9a-f]{6}$/i.test(s)) return [parseInt(s.slice(1,3),16)/255,parseInt(s.slice(3,5),16)/255,parseInt(s.slice(5,7),16)/255];
    return fallback;
  }

  function resizeBuffer() {
    var rect = host.getBoundingClientRect();
    if (rect.width < 2 || rect.height < 2) return false;
    var dpr = Math.min(window.devicePixelRatio || 1, 1.5);
    var w = Math.max(1, Math.round(rect.width*dpr)), h = Math.max(1, Math.round(rect.height*dpr));
    if (canvas.width !== w || canvas.height !== h) { canvas.width=w; canvas.height=h; }
    return true;
  }

  function drawMesh(mesh, matrix, color, alpha, unlit) {
    gl.bindVertexArray(mesh.vao); gl.uniformMatrix4fv(loc.model,false,matrix);
    /* Each model matrix is rotation times diagonal scale. Dividing each basis column by its squared
       length yields the inverse-transpose normal matrix without a per-vertex matrix inverse. */
    var normal=normalMatrixScratch;
    for(var c=0;c<3;c++){
      var o=c*4,n=matrix[o]*matrix[o]+matrix[o+1]*matrix[o+1]+matrix[o+2]*matrix[o+2];
      n=n>.00000001?1/n:1;
      normal[c*3]=matrix[o]*n;normal[c*3+1]=matrix[o+1]*n;normal[c*3+2]=matrix[o+2]*n;
    }
    gl.uniformMatrix3fv(loc.normalMatrix,false,normal);
    gl.uniform3fv(loc.color,color); gl.uniform1f(loc.alpha,alpha);
    gl.uniform1f(loc.unlit,unlit?1:0);
    gl.drawElements(gl.TRIANGLES,mesh.count,gl.UNSIGNED_INT,0);
  }

  function meshBoundsMatrix(model, bounds) {
    var sx=bounds[3]-bounds[0],sy=bounds[4]-bounds[1],sz=bounds[5]-bounds[2];
    var local=new Float32Array([sx,0,0,0, 0,sy,0,0, 0,0,sz,0,
      (bounds[0]+bounds[3])*.5,(bounds[1]+bounds[4])*.5,(bounds[2]+bounds[5])*.5,1]);
    return mat4Multiply(model,local);
  }

  function drawHelperOutline(entity, alpha, mesh) {
    var lines=entity.helperShape==='cylinder'?wireCylinder:wireCube;
    if (!lines) return;
    gl.bindVertexArray(lines.vao); gl.vertexAttrib4f(1,.5,.5,1,1);
    gl.uniformMatrix4fv(loc.model,false,mesh?meshBoundsMatrix(entity.modelMatrix,mesh.bounds):entity.proxyMatrix);
    gl.uniform3fv(loc.color,entity.color); gl.uniform1f(loc.alpha,alpha); gl.uniform1f(loc.unlit,1);
    gl.drawElements(gl.LINES,lines.count,gl.UNSIGNED_INT,0);
  }

  function draw() {
    if (!gl || !host || !resizeBuffer()) return;
    var bg = cssRgb('--field', [0.12,0.12,0.14]);
    gl.viewport(0,0,canvas.width,canvas.height); gl.clearColor(bg[0],bg[1],bg[2],1);
    gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT);
    var cp = Math.cos(pitch), eye = [target[0]+Math.cos(yaw)*cp*distance,
      target[1]+Math.sin(yaw)*cp*distance, target[2]+Math.sin(pitch)*distance];
    var near = Math.max(0.1, distance/2000), far = Math.max(distance + sceneRadius*5, 1000);
    var vp = mat4Multiply(perspective(Math.PI/4, canvas.width/canvas.height, near, far), lookAt(eye,target));
    gl.useProgram(program); gl.uniformMatrix4fv(loc.viewProjection,false,vp);

    if (grid && grid.count) {
      /* A disabled attribute needs all XYZW components. Omitting W throws before any mesh draw. */
      gl.bindVertexArray(grid.vao); gl.vertexAttrib4f(1,.5,.5,1,1);
      gl.uniformMatrix4fv(loc.model,false,mat4Identity());
      var gc = document.documentElement.classList.contains('dark') ? [.29,.30,.33] : [.76,.78,.81];
      gl.uniform3fv(loc.color,gc); gl.uniform1f(loc.alpha,1); gl.uniform1f(loc.unlit,1);
      gl.enable(gl.BLEND); gl.blendFunc(gl.SRC_ALPHA,gl.ONE_MINUS_SRC_ALPHA);
      gl.drawArrays(gl.LINES,0,grid.count);
      gl.disable(gl.BLEND);
    }

    var triangles = 0;
    for (var i=0;i<entities.length;i++) {
      var e=entities[i];
      if (e.role === 'trigger' || e.role === 'decal') continue;
      var mesh=e.model ? meshCache[e.model] : null;
      if (mesh && triangles + mesh.count/3 <= TRIANGLE_BUDGET) {
        triangles += mesh.count/3; mesh.lastUse=++cacheClock;
        drawMesh(mesh,e.modelMatrix,e.color,1,false);
      } else {
        var fallback=markerMeshes[e.role]||cube;
        if (fallback) drawMesh(fallback,e.proxyMatrix,e.color,1,false);
      }
    }

    /* Trigger/helper volumes are editor information, not solid scene geometry. Draw them after the
       opaque scene with a faint fill and a clear wire outline so they cannot masquerade as props. */
    gl.enable(gl.BLEND); gl.blendFunc(gl.SRC_ALPHA,gl.ONE_MINUS_SRC_ALPHA); gl.depthMask(false);
    for (i=0;i<entities.length;i++) {
      e=entities[i]; if (e.role !== 'trigger' && e.role !== 'decal') continue;
      mesh=e.model?meshCache[e.model]:null;
      if (mesh && triangles+mesh.count/3<=TRIANGLE_BUDGET) {
        triangles+=mesh.count/3; mesh.lastUse=++cacheClock;
        drawMesh(mesh,e.modelMatrix,e.color,e.role==='trigger'?.10:.16,true);
      } else if (cube) drawMesh(cube,e.proxyMatrix,e.color,e.role==='trigger'?.07:.12,true);
      drawHelperOutline(e,e.role==='trigger'?.72:.34,mesh);
    }
    gl.depthMask(true); gl.disable(gl.BLEND);
    gl.bindVertexArray(null);
  }

  function newGeneration() {
    generation = (generation + 1) & 0x7fffffff; if (!generation) generation = 1;
    requested = {}; resolving = {}; pending = 0; failures = 0;
    if (typeof post === 'function') post({cmd:'requestPrefabMesh', generation:generation, model:''});
  }

  function load(sceneText) {
    newGeneration(); entities = []; lastSceneText = sceneText || ''; cameraTouched = false;
    if (!initGl()) return;
    var doc;
    try { doc = JSON.parse(lastSceneText); } catch (e) { setStatus('Could not parse prefab scene'); markDirty(); return; }
    var list = doc && Array.isArray(doc.entities) ? doc.entities : [];
    for (var i=0;i<list.length;i++) entities.push(entityFromJson(list[i]));
    for (i=0;i<entities.length;i++) {
      if (entities[i].model) requestMesh(entities[i].model);
      if (entities[i].inherit && entities[i].resolveModel) resolveInherit(entities[i].inherit);
    }
    refreshBounds(true); updateStatus();
  }

  function clear() {
    newGeneration(); entities=[]; lastSceneText='';
    setStatus('Preparing preview...'); markDirty();
  }

  function frameCamera() {
    target=sceneCenter.slice(); distance=Math.max(sceneRadius*2.25,32);
    yaw=.75; pitch=.48; cameraTouched=true; markDirty();
  }

  if (canvas) {
    canvas.addEventListener('pointerdown',function(e){
      if (e.button!==0) return; dragging=true; cameraTouched=true;
      lastPointerX=e.clientX; lastPointerY=e.clientY; canvas.classList.add('dragging');
      canvas.setPointerCapture(e.pointerId); e.preventDefault();
    });
    canvas.addEventListener('pointermove',function(e){
      if (!dragging) return;
      var dx=e.clientX-lastPointerX,dy=e.clientY-lastPointerY; lastPointerX=e.clientX;lastPointerY=e.clientY;
      yaw-=dx*.009; pitch=Math.max(-1.35,Math.min(1.35,pitch+dy*.009)); markDirty();
    });
    function endDrag(e){ dragging=false; canvas.classList.remove('dragging'); if(e&&canvas.hasPointerCapture(e.pointerId))canvas.releasePointerCapture(e.pointerId); }
    canvas.addEventListener('pointerup',endDrag); canvas.addEventListener('pointercancel',endDrag);
    canvas.addEventListener('wheel',function(e){
      cameraTouched=true; distance*=Math.exp(e.deltaY*.0012);
      distance=Math.max(sceneRadius*.12,Math.min(sceneRadius*30,distance)); markDirty(); e.preventDefault();
    },{passive:false});
    canvas.addEventListener('dblclick',function(e){ frameCamera(); e.preventDefault(); });
    canvas.addEventListener('webglcontextlost',function(e){ e.preventDefault(); gl=null; meshCache={};cacheBytes=0;setStatus('Restoring 3D preview...'); });
    canvas.addEventListener('webglcontextrestored',function(){ gl=null;program=null;cube=null;grid=null;load(lastSceneText); });
  }

  if (host && window.ResizeObserver) {
    /* Observe the actual viewport rather than the window. Splitter drags, tab visibility, DPI changes,
       and native window resizes all converge here; RAF coalescing keeps it to one draw per visual frame. */
    new ResizeObserver(function(){ markDirty(); }).observe(host);
  } else {
    /* WebView2 supports ResizeObserver; this keeps direct-browser preview usable on older engines. */
    window.addEventListener('resize', markDirty);
  }
  new MutationObserver(function(){ markDirty(); }).observe(document.documentElement,{attributes:true,attributeFilter:['class']});

  return {load:load, clear:clear, modelResolved:modelResolved,
          meshUnavailable:meshUnavailable, receiveMeshBuffer:receiveMeshBuffer,
          transportUnavailable:function(){ failures++; pending=0; updateStatus(); markDirty(); }};
})();

/* Pure SnapMap prefab transform helpers. Pure ASCII; browser global + CommonJS test export. */
var prefabTransform = (function () {
  function finite(value, fallback) {
    value = Number(value);
    return isFinite(value) ? value : fallback;
  }

  function vec3(value, fallback) {
    value = value || {};
    return [finite(value.x, fallback[0]), finite(value.y, fallback[1]),
            finite(value.z, fallback[2])];
  }

  function orientationAxes(value) {
    var source = value && value.mat ? value.mat : null;
    var identity = [[1,0,0],[0,1,0],[0,0,1]], axes = [];
    for (var i = 0; i < 3; i++) {
      var axis = source ? (Array.isArray(source) ? source[i] : source['mat[' + i + ']']) : null;
      if (!axis) axes.push(identity[i].slice());
      else {
        /* Saved entity state is a sparse patch over idMat3's identity default. In particular, an
           unchanged diagonal component can be absent even when another component keeps this axis
           object present. Falling every missing component back to zero can make the matrix singular. */
        axes.push([finite(axis.x, identity[i][0]), finite(axis.y, identity[i][1]),
                   finite(axis.z, identity[i][2])]);
      }
    }
    return axes;
  }

  function composeMatrix(position, axes, scale) {
    /* idMat3 is the one idTech matrix stored column-major: mat[0..2] are the local X/Y/Z axes.
       WebGL is column-major too, so each saved axis becomes one complete column. Transposing the
       component indices here inverts rotations and can lay props or thin blocks on the wrong plane. */
    return new Float32Array([
      axes[0][0]*scale[0], axes[0][1]*scale[0], axes[0][2]*scale[0], 0,
      axes[1][0]*scale[1], axes[1][1]*scale[1], axes[1][2]*scale[1], 0,
      axes[2][0]*scale[2], axes[2][1]*scale[2], axes[2][2]*scale[2], 0,
      position[0], position[1], position[2], 1
    ]);
  }

  function composeBottomAnchoredBox(position, axes, size) {
    /* Cooked SnapMap block/trigger meshes occupy local z=0..1, while the generic proxy cube is
       centered at z=0. Move the proxy center half a local Z axis so fallback and decoded geometry
       share the exact same origin and dimensions. */
    var half = size[2] * .5;
    var center = [position[0] + axes[2][0] * half,
                  position[1] + axes[2][1] * half,
                  position[2] + axes[2][2] * half];
    return composeMatrix(center, axes, size);
  }

  function determinant(m) {
    return m[0][0]*(m[1][1]*m[2][2]-m[1][2]*m[2][1]) - m[0][1]*(m[1][0]*m[2][2]-m[1][2]*m[2][0]) +
           m[0][2]*(m[1][0]*m[2][1]-m[1][1]*m[2][0]);
  }

  function grabFrame(doc) {
    /* PrefabPopulate stores each entity as world x inverse(grab frame), and the grab frame's axis is the
       hovered entity's own idMat3 (rows = its local X/Y/Z in world, unit vectors, no angles). The prefab
       keeps that axis as grabAxis; PasteInstantiate reapplies it (times a yaw for the change in camera
       heading, which an orbiting preview can ignore). Without it a prefab anchored on a tilted, rolled or
       mirrored entity previews rotated by that entity's inverse. A degenerate axis falls back to identity. */
    var axes = orientationAxes(doc && doc.grabAxis ? doc.grabAxis : null);
    var det = determinant(axes);
    if (!isFinite(det) || Math.abs(Math.abs(det) - 1) > 0.05) return [[1,0,0],[0,1,0],[0,0,1]];
    return axes;
  }

  function applyFrame(position, axes, frame) {
    /* Row-vector idTech composition (FUN_1411afe40): world = local x frame, for the origin and for every
       axis row. The grab point itself is not stored, so the preview's origin is the grab point. */
    function row(v) {
      return [v[0]*frame[0][0] + v[1]*frame[1][0] + v[2]*frame[2][0],
              v[0]*frame[0][1] + v[1]*frame[1][1] + v[2]*frame[2][1],
              v[0]*frame[0][2] + v[1]*frame[1][2] + v[2]*frame[2][2]];
    }
    return {position: row(position), axes: [row(axes[0]), row(axes[1]), row(axes[2])]};
  }

  /* snapmaps/volume/blocking's entityDef size, used for omitted axes when the installed decl cannot be
     resolved; drawing them 1 unit thick would show slabs the paste never makes. */
  var BLOCKING_DEFAULT_SIZE = [16, 128, 64];

  function isBlockingVolume(className, inherit) {
    return String(className||'').toLowerCase().indexOf('volume_blocking') !== -1 ||
           String(inherit||'').toLowerCase() === 'snapmaps/volume/blocking';
  }

  function navigationFlags(className, inherit, edit) {
    /* The AI Navigation tick is the reflected flags.noFlood bit; Block Demons is blockDemons. Both are
       written only when true, so an absent member is false. */
    edit = edit || {};
    var blocking = isBlockingVolume(className, inherit);
    return {marked: blocking && !!(edit.flags && edit.flags.noFlood === true),
            blockDemons: blocking && edit.blockDemons === true};
  }

  function countNavigationBoxes(sceneText) {
    var doc;
    try { doc = typeof sceneText === 'string' ? JSON.parse(sceneText) : sceneText; } catch (e) { return 0; }
    var list = doc && Array.isArray(doc.entities) ? doc.entities : [], count = 0;
    for (var i = 0; i < list.length; i++) {
      var def = list[i] && list[i].entityDef ? list[i].entityDef : {};
      var edit = def.state && def.state.edit ? def.state.edit : {};
      if (navigationFlags(def.className, def.inherit, edit).marked) count++;
    }
    return count;
  }

  function transformPoint(matrix, x, y, z) {
    return [matrix[0]*x + matrix[4]*y + matrix[8]*z + matrix[12],
            matrix[1]*x + matrix[5]*y + matrix[9]*z + matrix[13],
            matrix[2]*x + matrix[6]*y + matrix[10]*z + matrix[14]];
  }

  return {finite:finite, vec3:vec3, orientationAxes:orientationAxes,
          composeMatrix:composeMatrix, composeBottomAnchoredBox:composeBottomAnchoredBox,
          transformPoint:transformPoint, determinant:determinant, grabFrame:grabFrame,
          applyFrame:applyFrame, BLOCKING_DEFAULT_SIZE:BLOCKING_DEFAULT_SIZE,
          isBlockingVolume:isBlockingVolume, navigationFlags:navigationFlags,
          countNavigationBoxes:countNavigationBoxes};
})();

if (typeof module !== 'undefined' && module.exports) module.exports = prefabTransform;

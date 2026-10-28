/* Sparse idMat3 -> WebGL transform regression coverage. */
'use strict';
const path=require('path');
const tx=require(path.join(__dirname,'..','src','ui','webview','prefab_transform.js'));
let failures=0;
function check(value,message){if(!value){console.error('[FAIL] '+message);failures++;}}
function near(a,b){return Math.abs(a-b)<1e-5;}
function vecNear(actual,wanted,message){
  check(actual.length===wanted.length && actual.every((v,i)=>near(v,wanted[i])),
        message+' (got '+Array.from(actual).join(', ')+')');
}

const sparse=tx.orientationAxes({mat:{
  'mat[0]':{x:Math.SQRT1_2,z:-Math.SQRT1_2},
  'mat[1]':{x:0},
  'mat[2]':{x:Math.SQRT1_2,z:Math.SQRT1_2}
}});
vecNear(sparse[1],[0,1,0],'missing diagonal components inherit idMat3 identity');

vecNear(tx.vec3({y:115,z:216},[16,128,64]),[16,115,216],
        'saved scale components override rather than erase inherited entityDef dimensions');

const quarterTurn=tx.orientationAxes({mat:{
  'mat[0]':{x:0,y:1}, 'mat[1]':{x:-1,y:0}
}});
const rotated=tx.composeMatrix([10,20,30],quarterTurn,[2,3,4]);
vecNear(tx.transformPoint(rotated,1,0,0),[10,22,30],
        'saved mat[0] is the complete local-X axis, not a transposed row');
vecNear(tx.transformPoint(rotated,0,1,0),[7,20,30],
        'saved mat[1] is the complete local-Y axis');
vecNear(tx.transformPoint(rotated,0,0,1),[10,20,34],
        'omitted mat[2] retains the local-Z identity axis');

const block=tx.composeBottomAnchoredBox([10,20,30],quarterTurn,[2,6,8]);
vecNear(tx.transformPoint(block,0,0,-.5),[10,20,30],
        'centered proxy minimum shares the cooked block bottom origin');
vecNear(tx.transformPoint(block,0,0,.5),[10,20,38],
        'centered proxy maximum preserves the full block height');

const sideways=tx.orientationAxes({mat:{
  'mat[0]':{x:0,z:-1}, 'mat[1]':{y:1}, 'mat[2]':{x:1,z:0}
}});
const sidewaysBlock=tx.composeBottomAnchoredBox([1,2,3],sideways,[4,6,8]);
vecNear(tx.transformPoint(sidewaysBlock,0,0,-.5),[1,2,3],
        'bottom anchoring follows a rotated local-Z axis');
vecNear(tx.transformPoint(sidewaysBlock,0,0,.5),[9,2,3],
        'rotated block dimensions extend along the saved basis axis');

/* Grab frame. PrefabPopulate stores local = (world - grabPoint) x transpose(grabAxis), with grabAxis the
   hovered entity's idMat3; the preview must undo it with local x grabAxis. */
function rowTimes(v,m){return [0,1,2].map(c=>v[0]*m[0][c]+v[1]*m[1][c]+v[2]*m[2][c]);}
function transpose(m){return [0,1,2].map(r=>[m[0][r],m[1][r],m[2][r]]);}
function populate(world,axes,grabPoint,grabAxis){
  const t=transpose(grabAxis);
  return {position:rowTimes(world.map((v,i)=>v-grabPoint[i]),t), axes:axes.map(a=>rowTimes(a,t))};
}
const c30=Math.cos(Math.PI/6), s30=Math.sin(Math.PI/6);
const tilt=[[1,0,0],[0,c30,s30],[0,-s30,c30]];               /* 30 degrees about X, the R-004 ramp */
const yaw90=[[0,1,0],[-1,0,0],[0,0,1]];
const mirrored=[[-1,0,0],[0,0,1],[0,1,0]];                    /* det -1, a mirrored module copy */
const identity=[[1,0,0],[0,1,0],[0,0,1]];
const grabPoint=[100,-40,12];
const layout=[
  {world:[100,-40,0], axes:tilt},          /* hovered marked ramp */
  {world:[356,-40,0], axes:identity},      /* default-size marked box */
  {world:[100,216,0], axes:yaw90},         /* yawed unmarked box */
  {world:[-28,-40,64], axes:mirrored}];
for (const anchor of [tilt, yaw90, mirrored]) {
  const doc={grabAxis:{mat:anchor.map(r=>({x:r[0],y:r[1],z:r[2]}))}};
  const frame=tx.grabFrame(doc);
  for (const e of layout) {
    const stored=populate(e.world,e.axes,grabPoint,anchor);
    const shown=tx.applyFrame(stored.position,stored.axes,frame);
    vecNear(shown.position,e.world.map((v,i)=>v-grabPoint[i]),
            'preview position is world relative to the grab point (det '+tx.determinant(anchor).toFixed(0)+')');
    for (let r=0;r<3;r++) vecNear(shown.axes[r],e.axes[r],'preview axis '+r+' is the world axis');
  }
}
/* Without the frame (the old preview) the tilted anchor shows the whole group rotated. */
const oldView=populate(layout[1].world,identity,grabPoint,tilt);
check(Math.abs(oldView.axes[2][2]-1)>.1,'an unframed tilted anchor visibly tilts the preview');
vecNear(tx.grabFrame({grabAxis:{mat:[{x:0,y:0,z:0},{x:0,y:0,z:0},{x:0,y:0,z:0}]}})[0],[1,0,0],
        'a degenerate grabAxis falls back to identity');
vecNear(tx.grabFrame({})[2],[0,0,1],'a prefab without grabAxis uses identity');
vecNear(tx.grabFrame({grabAxis:{mat:{'mat[0]':{x:0,y:-1},'mat[1]':{x:1,y:0}}}})[1],[1,0,0],
        'a sparse grabAxis inherits identity components like any idMat3');
vecNear(tx.vec3({z:16},tx.BLOCKING_DEFAULT_SIZE),[16,128,16],
        'omitted Blocking Box dimensions fall back to the blocking entityDef size');

const scene=JSON.stringify({entities:[
  {entityDef:{inherit:'snapmaps/volume/blocking',state:{edit:{flags:{noFlood:true},blockDemons:true}}}},
  {entityDef:{inherit:'snapmaps/volume/blocking',state:{edit:{flags:{noFlood:true}}}}},
  {entityDef:{inherit:'snapmaps/volume/blocking',state:{edit:{}}}},
  {entityDef:{inherit:'snapmaps/triggers/box',state:{edit:{flags:{noFlood:true}}}}}]});
check(tx.countNavigationBoxes(scene)===2,'Prefab Details counts only Blocking Boxes ticked for AI Navigation');
check(tx.countNavigationBoxes('not json')===0,'an unreadable scene counts no navigation boxes');
check(tx.navigationFlags('','snapmaps/volume/blocking',{blockDemons:true}).blockDemons===true &&
      !tx.navigationFlags('','snapmaps/volume/blocking',{blockDemons:true}).marked,
      'Block Demons alone is not the AI Navigation tick');

if(failures){console.error('prefab_transform_test: '+failures+' failure(s)');process.exit(1);}
console.log('prefab_transform_test: OK');

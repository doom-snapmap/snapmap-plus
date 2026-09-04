/* Captionless native-window corner/shadow contract, matched to snapmap-midi. */
'use strict';
const fs=require('fs'), path=require('path');
const root=path.join(__dirname,'..');
const host=fs.readFileSync(path.join(root,'src','ui','webview','snapmap_plus_ui_webview.cpp'),'utf8');
const build=fs.readFileSync(path.join(root,'src','ui','build.ps1'),'utf8');
let failures=0;
function check(value,message){if(!value){console.error('[FAIL] '+message);failures++;}}

const createAt=host.indexOf('CreateWindowExW(');
const extendAt=host.indexOf('DwmExtendFrameIntoClientArea(');
const refreshAt=host.indexOf('SWP_FRAMECHANGED');
check(host.includes('#include <dwmapi.h>'),'host includes the DWM API contract');
check(host.includes('WS_OVERLAPPEDWINDOW'),'window keeps normal managed-window styles');
check(host.includes('case WM_NCCALCSIZE:'),'caption is removed by consuming the non-client area');
check(/MARGINS\s+shadow\s*=\s*\{\s*1,\s*1,\s*1,\s*1\s*\}/.test(host),'one pixel of frame is extended on every side');
check(createAt >= 0 && extendAt > createAt && refreshAt > extendAt,'DWM extension happens after create and before frame refresh');
check(build.includes('"dwmapi.lib"'),'frontend links the DWM import library');

const cameraFrameAt=host.indexOf('if (was_visible && !g_cam_lock) poc_cam_read_send();');
const slowPollAt=host.indexOf('if (was_visible && (frame % 10 == 0)) {');
check(cameraFrameAt >= 0 && slowPollAt > cameraFrameAt,
      'live camera coordinates are sampled per frame before the ten-frame entity poll');
check(!host.includes('if (!g_cam_lock) poc_cam_read_send();'),
      'camera feedback is not nested in the slow entity poll');

if(failures){console.error('window_chrome_contract_test: '+failures+' failure(s)');process.exit(1);}
console.log('window_chrome_contract_test: OK');

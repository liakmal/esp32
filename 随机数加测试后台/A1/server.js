import express from 'express';
import cors from 'cors';
import bodyParser from 'body-parser';

const app = express();
const PORT = 3001;

app.use(cors());
app.use(bodyParser.json());

// ============================================================
//  BLE 扫描数据分享系统
//  完全随机4位识别码，设备绑定，30分钟过期自动清理
// ============================================================
const bleShareStore = new Map(); // code -> { devices, createdAt, expireAt, deviceId }
const deviceCodeMap = new Map();  // deviceId -> code
const BLE_SHARE_TTL = 30 * 60 * 1000; // 30分钟过期

// 生成随机4位识别码（不与现有冲突）
function randomCode() {
  let code;
  let attempts = 0;
  do {
    code = String(Math.floor(Math.random() * 10000)).padStart(4, '0');
    attempts++;
  } while (bleShareStore.has(code) && attempts < 100);
  return code;
}

// 定时清理过期数据
setInterval(() => {
  const t = Date.now();
  for (const [code, data] of bleShareStore.entries()) {
    if (t > data.expireAt) {
      if (data.deviceId && deviceCodeMap.get(data.deviceId) === code) {
        deviceCodeMap.delete(data.deviceId);
      }
      bleShareStore.delete(code);
      console.log(`[BLE-SHARE] 识别码 ${code} 已过期回收`);
    }
  }
}, 60 * 1000); // 每分钟检查

// 上传扫描数据
app.post('/api/ble-share/upload', (req, res) => {
  const { devices, deviceId } = req.body || {};
  if (!Array.isArray(devices) || devices.length === 0) {
    return res.status(400).json({ ok: false, error: '无设备数据' });
  }
  const trimmed = devices.slice(0, 200);
  let code = null;

  // 如果有 deviceId，尝试复用之前的码
  if (deviceId) {
    const existingCode = deviceCodeMap.get(deviceId);
    if (existingCode) {
      const existing = bleShareStore.get(existingCode);
      // 码还在且属于该设备 → 直接更新数据
      if (existing && existing.deviceId === deviceId && Date.now() <= existing.expireAt) {
        code = existingCode;
        bleShareStore.set(code, {
          devices: trimmed,
          count: trimmed.length,
          createdAt: Date.now(),
          expireAt: Date.now() + BLE_SHARE_TTL,
          deviceId,
        });
        console.log(`[BLE-SHARE] 设备 ${deviceId.slice(0,8)}.. 更新数据，复用识别码: ${code}`);
        return res.json({ ok: true, code, expiresIn: BLE_SHARE_TTL / 1000, reused: true });
      }
      // 码已过期或被占用，清理旧映射
      deviceCodeMap.delete(deviceId);
    }
  }

  // 分配新的随机码
  code = randomCode();
  bleShareStore.set(code, {
    devices: trimmed,
    count: trimmed.length,
    createdAt: Date.now(),
    expireAt: Date.now() + BLE_SHARE_TTL,
    deviceId: deviceId || null,
  });
  if (deviceId) deviceCodeMap.set(deviceId, code);
  console.log(`[BLE-SHARE] 上传 ${trimmed.length} 台设备，新识别码: ${code}` + (deviceId ? ` (设备: ${deviceId.slice(0,8)}..)` : ''));
  res.json({ ok: true, code, expiresIn: BLE_SHARE_TTL / 1000, reused: false });
});

// 查询扫描数据
app.get('/api/ble-share/query', (req, res) => {
  const code = String(req.query.code || '').padStart(4, '0');
  const data = bleShareStore.get(code);
  if (!data) return res.json({ ok: false, error: '识别码不存在或已过期' });
  if (Date.now() > data.expireAt) {
    if (data.deviceId && deviceCodeMap.get(data.deviceId) === code) {
      deviceCodeMap.delete(data.deviceId);
    }
    bleShareStore.delete(code);
    return res.json({ ok: false, error: '识别码已过期' });
  }
  const remaining = Math.max(0, Math.floor((data.expireAt - Date.now()) / 1000));
  res.json({ ok: true, code, devices: data.devices, count: data.count, createdAt: data.createdAt, remainingSeconds: remaining });
});

// 查询页面
app.get('/share', (req, res) => {
  const html = `<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>BLE 扫描数据查询</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI','Microsoft YaHei',sans-serif;background:#f0f2f5;min-height:100vh}
.top{background:#fff;padding:20px 16px;text-align:center;border-bottom:1px solid #eee;box-shadow:0 1px 4px rgba(0,0,0,.05)}
.top h1{font-size:20px;color:#222;margin-bottom:12px}
.code-input-wrap{display:flex;justify-content:center;gap:8px;margin-bottom:12px}
.code-input{width:52px;height:56px;text-align:center;font-size:26px;font-weight:700;border:2px solid #ddd;border-radius:12px;outline:none;font-family:monospace;transition:border-color .2s}
.code-input:focus{border-color:#4a90e2}
.btn{padding:10px 28px;background:#4a90e2;color:#fff;border:none;border-radius:10px;font-size:15px;font-weight:600;cursor:pointer}
.btn:active{opacity:.8}
.btn:disabled{background:#ccc}
.info{font-size:12px;color:#999;margin-top:8px}
.error{color:#e05c65;font-size:13px;margin-top:8px;font-weight:600}
.result{padding:12px}
.summary{background:#fff;border-radius:12px;padding:14px 16px;margin-bottom:12px;box-shadow:0 1px 4px rgba(0,0,0,.05);display:flex;justify-content:space-between;align-items:center}
.summary .code-badge{background:#eaf3ff;color:#4a90e2;padding:4px 12px;border-radius:20px;font-size:14px;font-weight:700;font-family:monospace}
.dev-card{background:#fff;border-radius:12px;padding:12px 14px;margin-bottom:8px;box-shadow:0 1px 4px rgba(0,0,0,.05)}
.dev-name{font-size:14px;font-weight:600;color:#222}
.dev-name.unknown{color:#aaa;font-style:italic}
.dev-mac{font-size:12px;font-family:monospace;color:#aaa;margin-top:2px}
.dev-meta{display:flex;gap:12px;margin-top:6px;flex-wrap:wrap}
.dev-meta span{font-size:11px;color:#555;background:#f5f5f5;padding:2px 8px;border-radius:8px}
.dev-uuid{font-size:11px;color:#4a90e2;margin-top:4px}
.dev-raw{font-size:10px;font-family:monospace;color:#888;margin-top:4px;word-break:break-all;line-height:1.5;background:#fafafa;padding:6px 8px;border-radius:6px}
.empty{text-align:center;color:#aaa;padding:40px 20px;font-size:14px}
</style>
</head>
<body>
<div class="top">
  <h1>BLE 扫描数据查询</h1>
  <div class="code-input-wrap">
    <input class="code-input" maxlength="1" id="c0" inputmode="numeric" autocomplete="off"/>
    <input class="code-input" maxlength="1" id="c1" inputmode="numeric" autocomplete="off"/>
    <input class="code-input" maxlength="1" id="c2" inputmode="numeric" autocomplete="off"/>
    <input class="code-input" maxlength="1" id="c3" inputmode="numeric" autocomplete="off"/>
  </div>
  <button class="btn" id="queryBtn" onclick="doQuery()">查 询</button>
  <div id="errMsg" class="error"></div>
  <div class="info">输入 App 上显示的 4 位识别码即可查看扫描结果，数据 30 分钟后自动过期</div>
</div>
<div class="result" id="result"></div>
<script>
const inputs=[document.getElementById('c0'),document.getElementById('c1'),document.getElementById('c2'),document.getElementById('c3')];
inputs.forEach((inp,i)=>{
  inp.addEventListener('input',()=>{
    inp.value=inp.value.replace(/[^0-9]/g,'');
    if(inp.value&&i<3)inputs[i+1].focus();
    if(getCode().length===4)doQuery();
  });
  inp.addEventListener('keydown',(e)=>{
    if(e.key==='Backspace'&&!inp.value&&i>0){inputs[i-1].focus();inputs[i-1].value='';}
  });
  inp.addEventListener('paste',(e)=>{
    e.preventDefault();
    const txt=(e.clipboardData||window.clipboardData).getData('text').replace(/[^0-9]/g,'').slice(0,4);
    for(let j=0;j<4;j++){inputs[j].value=txt[j]||'';}
    if(txt.length===4)doQuery();
  });
});
inputs[0].focus();
function getCode(){return inputs.map(i=>i.value).join('');}
async function doQuery(){
  const code=getCode();
  if(code.length!==4){document.getElementById('errMsg').textContent='请输入完整的4位识别码';return;}
  document.getElementById('errMsg').textContent='';
  document.getElementById('queryBtn').disabled=true;
  try{
    const r=await fetch('/api/ble-share/query?code='+encodeURIComponent(code));
    const d=await r.json();
    if(!d.ok){document.getElementById('errMsg').textContent=d.error||'查询失败';document.getElementById('result').innerHTML='';return;}
    renderResult(d);
  }catch(e){document.getElementById('errMsg').textContent='网络错误';}
  finally{document.getElementById('queryBtn').disabled=false;}
}
function renderResult(d){
  const mins=Math.floor(d.remainingSeconds/60);
  const secs=d.remainingSeconds%60;
  const time=new Date(d.createdAt).toLocaleString('zh-CN',{hour12:false});
  let h='<div class="summary"><div><div style="font-size:14px;font-weight:600;color:#222">共 '+d.count+' 台设备</div><div style="font-size:11px;color:#999;margin-top:2px">上传时间: '+time+' · 剩余 '+mins+'分'+secs+'秒</div></div><div class="code-badge">'+d.code+'</div></div>';
  if(!d.devices||d.devices.length===0){h+='<div class="empty">无设备数据</div>';document.getElementById('result').innerHTML=h;return;}
  d.devices.sort((a,b)=>(b.rssi||(-999))-(a.rssi||(-999)));
  d.devices.forEach(dev=>{
    const name=dev.name||'未知设备';
    const cls=dev.name?'dev-name':'dev-name unknown';
    h+='<div class="dev-card">';
    h+='<div class="'+cls+'">'+esc(name)+'</div>';
    h+='<div class="dev-mac">'+(dev.mac||dev.id||'—')+'</div>';
    h+='<div class="dev-meta">';
    if(dev.rssi!=null)h+='<span>'+dev.rssi+' dBm</span>';
    if(dev.txPower!=null)h+='<span>Tx:'+dev.txPower+'</span>';
    if(dev.connectable!=null)h+='<span>'+(dev.connectable?'可连接':'不可连接')+'</span>';
    if(dev.updateCount>1)h+='<span>×'+dev.updateCount+'</span>';
    h+='</div>';
    if(dev.serviceUuids&&dev.serviceUuids.length)h+='<div class="dev-uuid">UUID: '+dev.serviceUuids.join(', ')+'</div>';
    if(dev.manufacturerData)h+='<div class="dev-raw">MFG: '+esc(dev.manufacturerData)+'</div>';
    if(dev.rawAdvertisement)h+='<div class="dev-raw">RAW: '+esc(dev.rawAdvertisement)+'</div>';
    h+='</div>';
  });
  document.getElementById('result').innerHTML=h;
}
function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}
// URL参数自动查询
const urlCode=new URLSearchParams(location.search).get('code');
if(urlCode&&urlCode.length===4){for(let i=0;i<4;i++)inputs[i].value=urlCode[i]||'';doQuery();}
</script>
</body>
</html>`;
  res.setHeader('Content-Type', 'text/html; charset=utf-8');
  res.send(html);
});

app.listen(PORT, () => {
  console.log(`Server listening on http://0.0.0.0:${PORT}`);
});

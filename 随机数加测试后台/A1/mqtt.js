import express from 'express';
import cors from 'cors';
import bodyParser from 'body-parser';
import mqtt from 'mqtt';
import { createHash } from 'crypto';
import { hostname } from 'os';

const app = express();
const PORT = process.env.PORT || 3003;

app.use(cors());
app.use(bodyParser.json());
app.use(bodyParser.urlencoded({ extended: true }));

// ============================================================
//  设备数据存储
//  结构: deviceData[主机名] = {
//    targets: { MAC -> { mac, rawData, rssi, uuid, updateTime } },
//    lastSeen: timestamp
//  }
// ============================================================
const deviceData = {};

// MQTT 多Broker配置（与O5c固件同步）
const BROKER_PRESETS = {
  emqx:    { label: 'EMQX 国际',   host: 'broker.emqx.io',      tcp: 1883, wss: 8084 },
  hivemq:  { label: 'HiveMQ 国际',  host: 'broker.hivemq.com',   tcp: 1883, wss: 8884 },
};

// 每个broker的连接实例
const brokerClients = {};  // key -> { client, connected, label }

// 已订阅的主机名集合（全局，所有broker共享）
const subscribedHosts = new Set();

// 在线判定阈值（毫秒）：超过此时间没有收到消息视为离线
const ONLINE_THRESHOLD = 5 * 60 * 1000; // 5分钟

// 处理来自任意broker的消息
function handleMqttMessage(brokerKey, topic, message) {
  try {
    // Topic格式: {主机名}/{MAC}  例如 MFAC/F8A7639FF48E
    const idx = topic.indexOf('/');
    if (idx < 0) return;
    const hostName = topic.substring(0, idx);
    const topicMac = topic.substring(idx + 1);

    const messageStr = message.toString();
    let mac = topicMac;
    let rawData = messageStr;
    let rssi = 0;
    let uuid = '';
    let deviceId = '';
    let ts = Date.now();

    // 尝试解析JSON
    try {
      const data = JSON.parse(messageStr);
      if (data.mac) mac = data.mac;
      if (data.advData) rawData = data.advData;
      if (data.rssi) rssi = data.rssi;
      if (data.uuid) uuid = data.uuid;
      if (data.deviceId) deviceId = data.deviceId;
      // ts为秒级时间戳，若值合理（大于2020年）则使用，否则用当前时间
      if (data.ts && data.ts > 1577836800) {
        ts = data.ts * 1000;
      }
    } catch (e) {
      // 非JSON消息，使用原始文本
    }

    // 初始化主机数据
    if (!deviceData[hostName]) {
      deviceData[hostName] = { targets: {}, lastSeen: 0, heartbeat: null, broker: brokerKey };
    }
    deviceData[hostName].lastSeen = Date.now();
    deviceData[hostName].broker = brokerKey;

    // cmd/ack 确认消息
    if (topicMac === 'cmd/ack') {
      try {
        const ack = JSON.parse(messageStr);
        if (!deviceData[hostName].cmdAcks) deviceData[hostName].cmdAcks = [];
        deviceData[hostName].cmdAcks.unshift({ ...ack, receivedAt: Date.now() });
        if (deviceData[hostName].cmdAcks.length > 20) deviceData[hostName].cmdAcks.length = 20;
        console.log(`[MQTT][${brokerKey}][ACK] ${hostName} -> action=${ack.action} status=${ack.status}`);
      } catch (_) {}
      return;
    }

    // config/status 设备实际配置上报
    if (topicMac === 'config/status') {
      try {
        const cs = JSON.parse(messageStr);
        deviceData[hostName].configStatus = {
          scanMode: cs.scanMode || '',
          targets: cs.targets || '',
          deviceId: cs.deviceId || '',
          ts: cs.ts || 0,
          receivedAt: Date.now()
        };
        console.log(`[MQTT][${brokerKey}][CONFIG] ${hostName} -> mode=${cs.scanMode} targets=${cs.targets}`);
      } catch (_) {}
      return;
    }

    // 心跳消息：topic后缀为 "heartbeat" 或 JSON type==heartbeat
    let parsedData = null;
    try { parsedData = JSON.parse(messageStr); } catch (_) {}
    if (topicMac === 'heartbeat' || (parsedData && parsedData.type === 'heartbeat')) {
      const hb = parsedData || {};
      deviceData[hostName].heartbeat = {
        deviceId: hb.deviceId || '',
        powerMode: hb.powerMode || 'unknown',
        batteryLevel: hb.batteryLevel !== undefined ? hb.batteryLevel : null,
        uptime: hb.uptime || 0,
        timestamp: hb.timestamp || Date.now(),
        receivedAt: Date.now(),
        timeSlots: Array.isArray(hb.timeSlots) ? hb.timeSlots : []
      };
      console.log(`[MQTT][${brokerKey}][HB] ${hostName} -> powerMode=${hb.powerMode} battery=${hb.batteryLevel} deviceId=${hb.deviceId}`);
      return;
    }

    // 更新该主机下的目标MAC数据
    deviceData[hostName].targets[topicMac] = {
      mac,
      rawData,
      rssi,
      uuid,
      deviceId,
      updateTime: ts
    };

    console.log(`[MQTT][${brokerKey}] ${hostName}/${topicMac} -> MAC=${mac}, RSSI=${rssi}`);
  } catch (err) {
    // 静默跳过
  }
}

// 初始化所有broker连接
// 生成稳定的clientId（基于机器，避免每次重启换ID导致broker旧session残留）
const machineId = createHash('md5').update(hostname()).digest('hex').substring(0, 6);

for (const [key, cfg] of Object.entries(BROKER_PRESETS)) {
  const url = `mqtt://${cfg.host}:${cfg.tcp}`;
  const stableClientId = `o5q_${key}_${machineId}`;
  const client = mqtt.connect(url, {
    clientId: stableClientId,
    clean: false,       // 持久会话：broker记住订阅，断线期间QoS1消息会排队
    connectTimeout: 10000,
    reconnectPeriod: 5000,
    keepalive: 30,      // 30秒心跳，更快检测断线
  });

  brokerClients[key] = { client, connected: false, label: cfg.label, host: cfg.host };

  client.on('connect', (connack) => {
    console.log(`[MQTT][${key}] 已连接到 ${cfg.host}:${cfg.tcp} (sessionPresent=${connack.sessionPresent}, clientId=${stableClientId})`);
    brokerClients[key].connected = true;
    // 无论是否有旧session，都重新订阅确保一致性
    for (const host of subscribedHosts) {
      client.subscribe(host + '/#', { qos: 1 });
    }
  });

  client.on('message', (topic, message) => {
    handleMqttMessage(key, topic, message);
  });

  client.on('error', (err) => {
    console.error(`[MQTT][${key}] 连接错误:`, err.message);
    brokerClients[key].connected = false;
  });

  client.on('offline', () => {
    console.log(`[MQTT][${key}] 连接断开`);
    brokerClients[key].connected = false;
  });

  client.on('reconnect', () => {
    console.log(`[MQTT][${key}] 正在重连 ${cfg.host}...`);
  });
}

// 定期刷新订阅（防止broker静默丢失订阅）
setInterval(() => {
  if (subscribedHosts.size === 0) return;
  for (const [key, b] of Object.entries(brokerClients)) {
    if (b.connected) {
      for (const host of subscribedHosts) {
        b.client.subscribe(host + '/#', { qos: 1 });
      }
    }
  }
}, 60000); // 每60秒刷新一次

// 订阅指定主机的MQTT主题（在所有已连接的broker上订阅）
function subscribeHost(hostName) {
  const isNew = !subscribedHosts.has(hostName);
  subscribedHosts.add(hostName);
  for (const [key, b] of Object.entries(brokerClients)) {
    if (b.connected) {
      b.client.subscribe(hostName + '/#', { qos: 1 }, (err) => {
        if (err) {
          console.error(`[MQTT][${key}] 订阅 ${hostName}/# 失败:`, err);
        } else if (isNew) {
          console.log(`[MQTT][${key}] 已订阅: ${hostName}/#`);
        }
      });
    }
  }
}

// 判断主机是否在线
function isHostOnline(hostName) {
  const host = deviceData[hostName];
  if (!host || !host.lastSeen) return false;
  return (Date.now() - host.lastSeen) < ONLINE_THRESHOLD;
}

// ============================================================
//  API 接口
// ============================================================

// Broker连接状态API
app.get('/api/brokers', (req, res) => {
  const status = {};
  for (const [key, b] of Object.entries(brokerClients)) {
    status[key] = { label: b.label, host: b.host, connected: b.connected };
  }
  res.json(status);
});

// 查询指定主机的设备信息
app.get('/api/device/:host', (req, res) => {
  const hostName = req.params.host.trim();
  if (!hostName) return res.json({ error: '主机名称不能为空' });

  // 自动订阅该主机
  subscribeHost(hostName);

  const host = deviceData[hostName];
  if (!host || Object.keys(host.targets).length === 0) {
    return res.json({
      hostName,
      online: false,
      targets: [],
      message: '暂无数据，已在所有Broker上监听该主机，请稍后刷新'
    });
  }

  const online = isHostOnline(hostName);
  const brokerKey = host.broker || 'unknown';
  const brokerLabel = brokerClients[brokerKey] ? brokerClients[brokerKey].label : brokerKey;
  const targets = Object.entries(host.targets).map(([key, t]) => ({
    mac: t.mac,
    rawData: t.rawData,
    rssi: t.rssi,
    uuid: t.uuid,
    deviceId: t.deviceId,
    updateTime: new Date(t.updateTime).toLocaleString('zh-CN', { timeZone: 'Asia/Shanghai' })
  }));

  const hb = host.heartbeat;
  const heartbeat = hb ? {
    powerMode: hb.powerMode,
    batteryLevel: hb.batteryLevel !== null ? (typeof hb.batteryLevel === 'number' ? hb.batteryLevel + '%' : String(hb.batteryLevel)) : '-',
    uptime: hb.uptime,
    lastHeartbeat: new Date(hb.receivedAt).toLocaleString('zh-CN', { timeZone: 'Asia/Shanghai' }),
    lastHeartbeatMsAgo: Date.now() - hb.receivedAt,
    timeSlots: hb.timeSlots || []
  } : null;

  res.json({ hostName, online, broker: brokerKey, brokerLabel, targets, heartbeat });
});

// 首页：输入主机名称，跳转到设备详情页
app.get('/', (req, res) => {
  res.send(`<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>设备查询</title>
  <style>
    *{margin:0;padding:0;box-sizing:border-box}
    body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh;display:flex;align-items:center;justify-content:center}
    .container{background:#fff;padding:50px 40px;border-radius:20px;box-shadow:0 20px 60px rgba(0,0,0,.15);max-width:500px;width:90%;text-align:center}
    h1{color:#2c3e50;margin-bottom:10px;font-size:28px}
    .subtitle{color:#888;margin-bottom:35px;font-size:14px}
    .input-wrap{position:relative;margin-bottom:20px}
    input{width:100%;padding:16px 20px;border:2px solid #e1e8ed;border-radius:14px;font-size:18px;text-align:center;letter-spacing:2px;text-transform:uppercase;transition:border-color .3s}
    input:focus{outline:none;border-color:#667eea}
    input::placeholder{text-transform:none;letter-spacing:0;font-size:15px}
    .btn{background:linear-gradient(45deg,#667eea,#764ba2);color:#fff;border:none;padding:16px;border-radius:14px;font-size:16px;cursor:pointer;transition:transform .2s,box-shadow .2s;width:100%}
    .btn:hover{transform:translateY(-2px);box-shadow:0 8px 25px rgba(102,126,234,.4)}
    .btn:active{transform:translateY(0)}
    .broker-status{margin-top:24px;text-align:left}
    .broker-status h3{font-size:13px;color:#999;margin-bottom:10px;text-align:center}
    .broker-item{display:flex;align-items:center;padding:8px 12px;background:#f8f9fa;border-radius:10px;margin-bottom:6px;font-size:13px}
    .broker-dot{width:8px;height:8px;border-radius:50%;margin-right:10px;flex-shrink:0}
    .broker-dot.on{background:#27ae60}
    .broker-dot.off{background:#e74c3c}
    .broker-label{color:#333;font-weight:600;flex:1}
    .broker-host{color:#aaa;font-size:11px;font-family:monospace}
  </style>
</head>
<body>
  <div class="container">
    <h1>设备查询</h1>
    <p class="subtitle">输入设备的主机名称查询在线状态与扫描数据</p>
    
    <div class="input-wrap">
      <input type="text" id="hostInput" placeholder="输入主机名称，例如 MFAC" autofocus />
    </div>
    
    <button class="btn" onclick="goQuery()">查 询</button>
    
    <div class="broker-status">
      <h3>Broker 连接状态</h3>
      <div id="brokerList">加载中...</div>
    </div>
  </div>
  <script>
    function goQuery(){
      const v=document.getElementById('hostInput').value.trim();
      if(!v){alert('请输入主机名称');return;}
      window.location.href='/device/'+encodeURIComponent(v);
    }
    document.getElementById('hostInput').addEventListener('keydown',function(e){
      if(e.key==='Enter') goQuery();
    });
    async function loadBrokers(){
      try{
        const r=await fetch('/api/brokers');
        const d=await r.json();
        let h='';
        for(const[k,b]of Object.entries(d)){
          h+='<div class="broker-item"><span class="broker-dot '+(b.connected?'on':'off')+'"></span><span class="broker-label">'+b.label+'</span><span class="broker-host">'+b.host+'</span></div>';
        }
        document.getElementById('brokerList').innerHTML=h;
      }catch(e){}
    }
    loadBrokers();
    setInterval(loadBrokers,10000);
  </script>
</body>
</html>`);
});

// 设备详情页：显示在线状态、MAC、Raw、更新时间
app.get('/device/:host', (req, res) => {
  const hostName = req.params.host.trim();

  // 自动订阅该主机
  subscribeHost(hostName);

  res.send(`<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>设备查询 - \${hostName}</title>
  <style>
    *{margin:0;padding:0;box-sizing:border-box}
    body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#f0f2f5;min-height:100vh;padding:30px 15px}
    .wrap{max-width:700px;margin:0 auto}
    .back{display:inline-block;color:#667eea;text-decoration:none;margin-bottom:20px;font-size:14px}
    .back:hover{text-decoration:underline}
    .card{background:#fff;border-radius:16px;box-shadow:0 4px 20px rgba(0,0,0,.06);overflow:hidden;margin-bottom:20px}
    .card-header{padding:24px 28px;display:flex;align-items:center;justify-content:space-between;border-bottom:1px solid #f0f0f0}
    .host-name{font-size:24px;font-weight:700;color:#2c3e50}
    .status-badge{padding:6px 16px;border-radius:20px;font-size:13px;font-weight:600}
    .status-online{background:#e8f8f0;color:#27ae60}
    .status-offline{background:#fde8e8;color:#e74c3c}
    .status-waiting{background:#fef9e7;color:#f39c12}
    .card-body{padding:28px}
    .info-row{display:flex;align-items:flex-start;padding:14px 0;border-bottom:1px solid #f8f8f8}
    .info-row:last-child{border-bottom:none}
    .info-label{width:110px;flex-shrink:0;font-weight:600;color:#888;font-size:13px;padding-top:2px}
    .info-value{flex:1;color:#333;word-break:break-all;font-size:15px}
    .info-value.mono{font-family:'SF Mono',Monaco,Consolas,monospace;font-size:13px;background:#f8f9fa;padding:8px 12px;border-radius:8px;line-height:1.6}
    .target-card{background:#f8f9fa;border-radius:12px;padding:20px;margin-bottom:12px}
    .target-card:last-child{margin-bottom:0}
    .target-mac{font-weight:700;color:#2c3e50;font-size:16px;margin-bottom:10px;font-family:'SF Mono',Monaco,Consolas,monospace}
    .refresh-bar{text-align:center;padding:16px}
    .btn-refresh{background:#667eea;color:#fff;border:none;padding:10px 24px;border-radius:10px;font-size:14px;cursor:pointer}
    .btn-refresh:hover{background:#5a6fd6}
    .empty-msg{text-align:center;color:#aaa;padding:40px 20px;font-size:15px}
    .auto-refresh{color:#aaa;font-size:12px;text-align:center;margin-top:8px}
    .broker-tag{display:inline-block;background:#eaf3ff;color:#4a7cc9;padding:3px 10px;border-radius:12px;font-size:11px;font-weight:600;margin-left:10px;vertical-align:middle}
  </style>
</head>
<body>
  <div class="wrap">
    <a class="back" href="/">&larr; 返回查询</a>
    
    <div class="card">
      <div class="card-header">
        <span><span class="host-name" id="hostTitle">${hostName}</span><span class="broker-tag" id="brokerTag" style="display:none"></span></span>
        <span class="status-badge status-waiting" id="statusBadge">检查中...</span>
      </div>
      <div class="card-body" id="deviceBody">
        <div class="empty-msg">正在加载数据...</div>
      </div>
    </div>
    
    <div class="refresh-bar">
      <button class="btn-refresh" onclick="loadDevice()">手动刷新</button>
      <a href="/control/${hostName}" style="display:inline-block;margin-left:12px;background:#e67e22;color:#fff;border:none;padding:10px 24px;border-radius:10px;font-size:14px;cursor:pointer;text-decoration:none">云控</a>
      <div class="auto-refresh">每 10 秒自动刷新</div>
    </div>
  </div>

  <script>
    const HOST = '${hostName}';
    
    async function loadDevice(){
      try{
        const r = await fetch('/api/device/' + encodeURIComponent(HOST));
        const d = await r.json();
        
        const badge = document.getElementById('statusBadge');
        const body = document.getElementById('deviceBody');
        
        if(d.message && (!d.targets || d.targets.length === 0)){
          badge.textContent = '等待数据';
          badge.className = 'status-badge status-waiting';
          body.innerHTML = '<div class="empty-msg">' + d.message + '</div>';
          return;
        }
        
        if(d.online){
          badge.textContent = '在线';
          badge.className = 'status-badge status-online';
        }else{
          badge.textContent = '离线';
          badge.className = 'status-badge status-offline';
        }
        
        // 显示Broker来源
        var btag = document.getElementById('brokerTag');
        if(d.brokerLabel){
          btag.textContent = d.brokerLabel;
          btag.style.display = 'inline-block';
        }
        
        let html = '';
        // 心跳信息卡片
        if(d.heartbeat){
          const hb = d.heartbeat;
          const hbAgo = hb.lastHeartbeatMsAgo < 60000
            ? Math.round(hb.lastHeartbeatMsAgo/1000) + ' 秒前'
            : Math.round(hb.lastHeartbeatMsAgo/60000) + ' 分钟前';
          html += '<div class="target-card" style="background:#f0f4ff;border-left:4px solid #667eea;margin-bottom:16px">';
          html += '<div class="target-mac" style="color:#667eea;margin-bottom:8px">📡 设备状态</div>';
          html += '<div class="info-row"><span class="info-label">电量</span><span class="info-value">' + hb.batteryLevel + '</span></div>';
          html += '<div class="info-row"><span class="info-label">功率模式</span><span class="info-value">' + hb.powerMode + '</span></div>';
          html += '<div class="info-row"><span class="info-label">最后心跳</span><span class="info-value">' + hb.lastHeartbeat + ' (' + hbAgo + ')</span></div>';
          if(hb.timeSlots && hb.timeSlots.length > 0){
            var slotHtml = '';
            hb.timeSlots.forEach(function(s, i){
              var wH = String(s.wakeH !== undefined ? s.wakeH : '-').padStart(2,'0');
              var wM = String(s.wakeM !== undefined ? s.wakeM : '-').padStart(2,'0');
              var sH = String(s.sleepH !== undefined ? s.sleepH : '-').padStart(2,'0');
              var sM = String(s.sleepM !== undefined ? s.sleepM : '-').padStart(2,'0');
              var en = s.enabled !== false;
              slotHtml += '<div style="display:inline-block;background:'+(en?'#e8f8f0':'#f5f5f5')+';padding:4px 12px;border-radius:8px;margin:2px 4px 2px 0;font-size:13px;color:'+(en?'#27ae60':'#aaa')+'">';
              slotHtml += '⏰ ' + wH+':'+wM + ' ~ ' + sH+':'+sM;
              if(!en) slotHtml += ' <span style="font-size:11px">(禁用)</span>';
              slotHtml += '</div>';
            });
            html += '<div class="info-row"><span class="info-label">时间段</span><span class="info-value">' + slotHtml + '</span></div>';
          } else {
            html += '<div class="info-row"><span class="info-label">时间段</span><span class="info-value" style="color:#aaa">全天在线（无时间段）</span></div>';
          }
          html += '</div>';
        }
        d.targets.forEach(function(t){
          html += '<div class="target-card">';
          html += '<div class="target-mac">' + t.mac + '</div>';
          html += '<div class="info-row"><span class="info-label">Raw 数据</span><span class="info-value mono">' + t.rawData + '</span></div>';
          if(t.rssi) html += '<div class="info-row"><span class="info-label">信号强度</span><span class="info-value">' + t.rssi + ' dBm</span></div>';
          if(t.uuid) html += '<div class="info-row"><span class="info-label">UUID</span><span class="info-value">' + t.uuid + '</span></div>';
          html += '<div class="info-row"><span class="info-label">更新时间</span><span class="info-value">' + t.updateTime + '</span></div>';
          html += '</div>';
        });
        
        body.innerHTML = html;
      }catch(e){
        document.getElementById('deviceBody').innerHTML = '<div class="empty-msg">查询失败，请检查网络</div>';
      }
    }
    
    loadDevice();
    setInterval(loadDevice, 10000);
  </script>
</body>
</html>`);
});

// ============================================================
//  云控功能：远程修改设备扫描配置
// ============================================================

// 从心跳或扫描数据中获取deviceId
function getDeviceId(hostName) {
  const data = deviceData[hostName];
  if (!data) return '';
  // 优先从心跳获取
  if (data.heartbeat && data.heartbeat.deviceId) return data.heartbeat.deviceId;
  // 其次从扫描数据获取
  for (const t of Object.values(data.targets || {})) {
    if (t.deviceId) return t.deviceId;
  }
  return '';
}

// 云控指令日志（内存）
const cmdLog = {};  // hostName -> [{ action, scanMode, targets, ts, status, sentAt, ackedAt }]

// 发送云控指令到设备
function publishCmd(hostName, cmdPayload) {
  const topic = hostName + '/cmd';
  const payload = JSON.stringify(cmdPayload);
  let sent = false;
  // 获取设备所在的 broker（优先），否则全部发
  const hostBroker = deviceData[hostName]?.broker;
  for (const [key, b] of Object.entries(brokerClients)) {
    if (b.connected && (!hostBroker || key === hostBroker)) {
      b.client.publish(topic, payload, { qos: 1, retain: true }, (err) => {
        if (err) console.error(`[CMD][${key}] 发送失败:`, err);
        else console.log(`[CMD][${key}] 已发送 ${topic}: ${payload}`);
      });
      sent = true;
    }
  }
  return sent;
}

// 云控API：发送配置指令
app.post('/api/control/:host', (req, res) => {
  const hostName = req.params.host.trim();
  const { scanMode, targets } = req.body || {};

  if (!hostName) return res.json({ ok: false, error: '主机名不能为空' });
  if (!scanMode || !['mac', 'uuid'].includes(scanMode)) {
    return res.json({ ok: false, error: 'scanMode 必须是 mac 或 uuid' });
  }
  if (!targets || typeof targets !== 'string' || targets.trim().length === 0) {
    return res.json({ ok: false, error: '目标列表不能为空' });
  }

  // 从心跳或扫描数据获取 deviceId 作为 token
  const devId = getDeviceId(hostName);
  if (!devId) {
    return res.json({ ok: false, error: '设备尚未上线（无数据记录），无法下发指令' });
  }

  const ts = Math.floor(Date.now() / 1000);
  const cmdPayload = {
    action: 'setScanConfig',
    scanMode,
    targets: targets.trim(),
    token: devId,
    ts
  };

  // 确保已订阅该主机（包括 cmd/ack）
  subscribeHost(hostName);

  const sent = publishCmd(hostName, cmdPayload);
  if (!sent) {
    return res.json({ ok: false, error: '所有Broker均未连接，无法发送' });
  }

  // 记录指令日志
  if (!cmdLog[hostName]) cmdLog[hostName] = [];
  cmdLog[hostName].unshift({ ...cmdPayload, status: 'sent', sentAt: Date.now() });
  if (cmdLog[hostName].length > 50) cmdLog[hostName].length = 50;

  res.json({ ok: true, message: '指令已下发', cmd: cmdPayload });
});

// 云控API：查询指令状态和ack
app.get('/api/control/:host', (req, res) => {
  const hostName = req.params.host.trim();
  const devId = getDeviceId(hostName);
  const acks = deviceData[hostName]?.cmdAcks || [];
  const log = cmdLog[hostName] || [];
  const online = isHostOnline(hostName);
  const brokerKey = deviceData[hostName]?.broker || 'unknown';
  const brokerLabel = brokerClients[brokerKey]?.label || brokerKey;
  const cs = deviceData[hostName]?.configStatus || null;
  res.json({
    hostName,
    online,
    brokerLabel,
    hasHeartbeat: !!devId,
    deviceId: devId,
    configStatus: cs,
    cmdLog: log.slice(0, 20),
    acks: acks.slice(0, 20)
  });
});

// 通过序列号查找主机名API
app.get('/api/lookup/:serial', (req, res) => {
  const serial = req.params.serial.trim().toUpperCase();
  if (!serial) return res.json({ ok: false, error: '序列号不能为空' });

  for (const [hostName, data] of Object.entries(deviceData)) {
    const hb = data.heartbeat;
    if (hb && hb.deviceId && hb.deviceId.toUpperCase() === serial) {
      return res.json({ ok: true, hostName, deviceId: hb.deviceId });
    }
    // 同时检查扫描数据中的deviceId
    for (const t of Object.values(data.targets || {})) {
      if (t.deviceId && t.deviceId.toUpperCase() === serial) {
        return res.json({ ok: true, hostName, deviceId: t.deviceId });
      }
    }
  }
  return res.json({ ok: false, error: '未找到该序列号对应的设备，请确认设备已上线并发送过数据' });
});

// 云控入口页面：输入序列号自动跳转
app.get('/control', (req, res) => {
  res.send(`<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>云控 - 设备查找</title>
  <style>
    *{margin:0;padding:0;box-sizing:border-box}
    body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:linear-gradient(135deg,#e67e22 0%,#d35400 50%,#c0392b 100%);min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}
    .container{background:#fff;padding:50px 40px;border-radius:20px;box-shadow:0 20px 60px rgba(0,0,0,.15);max-width:500px;width:90%;text-align:center}
    h1{color:#2c3e50;margin-bottom:10px;font-size:28px}
    .subtitle{color:#888;margin-bottom:30px;font-size:14px}
    .tabs{display:flex;gap:0;margin-bottom:24px;background:#f0f2f5;border-radius:12px;padding:4px}
    .tab{flex:1;padding:10px;border-radius:10px;font-size:14px;font-weight:600;cursor:pointer;border:none;background:transparent;color:#888;transition:all .2s}
    .tab.active{background:#fff;color:#e67e22;box-shadow:0 2px 8px rgba(0,0,0,.08)}
    .input-wrap{position:relative;margin-bottom:16px}
    input{width:100%;padding:16px 20px;border:2px solid #e1e8ed;border-radius:14px;font-size:16px;text-align:center;letter-spacing:1px;text-transform:uppercase;transition:border-color .3s}
    input:focus{outline:none;border-color:#e67e22}
    input::placeholder{text-transform:none;letter-spacing:0;font-size:14px}
    .btn{background:linear-gradient(45deg,#e67e22,#d35400);color:#fff;border:none;padding:16px;border-radius:14px;font-size:16px;font-weight:600;cursor:pointer;transition:transform .2s,box-shadow .2s;width:100%}
    .btn:hover{transform:translateY(-2px);box-shadow:0 8px 25px rgba(230,126,34,.4)}
    .btn:active{transform:translateY(0)}
    .btn:disabled{opacity:.5;cursor:not-allowed;transform:none;box-shadow:none}
    .msg{margin-top:16px;padding:12px;border-radius:10px;font-size:13px;display:none}
    .msg.err{display:block;background:#fde8e8;color:#e74c3c}
    .msg.loading{display:block;background:#fef9e7;color:#f39c12}
    .hint{font-size:11px;color:#bbb;margin-top:12px}
    .back-link{display:block;margin-top:20px;color:#e67e22;text-decoration:none;font-size:13px}
    .back-link:hover{text-decoration:underline}
  </style>
</head>
<body>
  <div class="container">
    <h1>云控</h1>
    <p class="subtitle">远程修改设备扫描配置</p>

    <div class="tabs">
      <button class="tab active" id="tabSerial" onclick="switchTab('serial')">序列号查找</button>
      <button class="tab" id="tabHost" onclick="switchTab('host')">主机名直达</button>
    </div>

    <div id="serialPanel">
      <div class="input-wrap">
        <input type="text" id="serialInput" placeholder="输入设备芯片序列号" autofocus />
      </div>
      <button class="btn" id="serialBtn" onclick="lookupSerial()">查找并进入</button>
      <div class="hint">序列号可在设备配置页或串口日志中查看</div>
    </div>

    <div id="hostPanel" style="display:none">
      <div class="input-wrap">
        <input type="text" id="hostInput" placeholder="输入主机名称，例如 MFAC" />
      </div>
      <button class="btn" onclick="goHost()">进入云控</button>
    </div>

    <div class="msg" id="msgBox"></div>
    <a class="back-link" href="/">&larr; 返回设备查询</a>
  </div>

  <script>
    function switchTab(tab) {
      document.getElementById('tabSerial').className = 'tab' + (tab === 'serial' ? ' active' : '');
      document.getElementById('tabHost').className = 'tab' + (tab === 'host' ? ' active' : '');
      document.getElementById('serialPanel').style.display = (tab === 'serial') ? '' : 'none';
      document.getElementById('hostPanel').style.display = (tab === 'host') ? '' : 'none';
      document.getElementById('msgBox').className = 'msg';
    }

    function showMsg(text, type) {
      var el = document.getElementById('msgBox');
      el.textContent = text;
      el.className = 'msg ' + type;
    }

    async function lookupSerial() {
      var serial = document.getElementById('serialInput').value.trim();
      if (!serial) { showMsg('请输入序列号', 'err'); return; }

      var btn = document.getElementById('serialBtn');
      btn.disabled = true;
      btn.textContent = '查找中...';
      showMsg('正在查找设备...', 'loading');

      try {
        var r = await fetch('/api/lookup/' + encodeURIComponent(serial));
        var d = await r.json();
        if (d.ok) {
          showMsg('找到设备: ' + d.hostName + '，正在跳转...', 'loading');
          setTimeout(function() {
            window.location.href = '/control/' + encodeURIComponent(d.hostName);
          }, 500);
          return;
        } else {
          showMsg(d.error || '未找到设备', 'err');
        }
      } catch(e) {
        showMsg('网络错误', 'err');
      }
      btn.disabled = false;
      btn.textContent = '查找并进入';
    }

    function goHost() {
      var v = document.getElementById('hostInput').value.trim();
      if (!v) { showMsg('请输入主机名称', 'err'); return; }
      window.location.href = '/control/' + encodeURIComponent(v);
    }

    document.getElementById('serialInput').addEventListener('keydown', function(e) {
      if (e.key === 'Enter') lookupSerial();
    });
    document.getElementById('hostInput').addEventListener('keydown', function(e) {
      if (e.key === 'Enter') goHost();
    });
  </script>
</body>
</html>`);
});

// 云控页面
app.get('/control/:host', (req, res) => {
  const hostName = req.params.host.trim();
  subscribeHost(hostName);

  res.send(`<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>云控 - \${hostName}</title>
  <style>
    *{margin:0;padding:0;box-sizing:border-box}
    body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#f0f2f5;min-height:100vh;padding:30px 15px}
    .wrap{max-width:600px;margin:0 auto}
    .back{display:inline-block;color:#667eea;text-decoration:none;margin-bottom:20px;font-size:14px}
    .back:hover{text-decoration:underline}
    .card{background:#fff;border-radius:16px;box-shadow:0 4px 20px rgba(0,0,0,.06);overflow:hidden;margin-bottom:20px}
    .card-header{padding:20px 24px;display:flex;align-items:center;justify-content:space-between;border-bottom:1px solid #f0f0f0}
    .host-name{font-size:22px;font-weight:700;color:#2c3e50}
    .status-badge{padding:5px 14px;border-radius:20px;font-size:12px;font-weight:600}
    .status-online{background:#e8f8f0;color:#27ae60}
    .status-offline{background:#fde8e8;color:#e74c3c}
    .card-body{padding:24px}
    .form-group{margin-bottom:18px}
    label{display:block;font-size:13px;font-weight:600;color:#555;margin-bottom:6px}
    .radio-group{display:flex;gap:16px}
    .radio-group label{display:flex;align-items:center;gap:6px;font-weight:500;cursor:pointer;padding:10px 20px;background:#f8f9fa;border-radius:10px;border:2px solid transparent;transition:all .2s}
    .radio-group label.active{border-color:#e67e22;background:#fef5ec;color:#e67e22}
    .radio-group input[type=radio]{display:none}
    textarea{width:100%;padding:14px 16px;border:2px solid #e1e8ed;border-radius:12px;font-size:14px;resize:vertical;min-height:80px;font-family:'SF Mono',Monaco,Consolas,monospace;transition:border-color .3s}
    textarea:focus{outline:none;border-color:#e67e22}
    .hint{font-size:11px;color:#aaa;margin-top:4px}
    .btn-send{background:linear-gradient(45deg,#e67e22,#d35400);color:#fff;border:none;padding:14px;border-radius:12px;font-size:15px;font-weight:600;cursor:pointer;transition:transform .2s,box-shadow .2s;width:100%}
    .btn-send:hover{transform:translateY(-2px);box-shadow:0 8px 25px rgba(230,126,34,.4)}
    .btn-send:active{transform:translateY(0)}
    .btn-send:disabled{opacity:.5;cursor:not-allowed;transform:none;box-shadow:none}
    .msg{margin-top:14px;padding:12px 16px;border-radius:10px;font-size:13px;display:none;text-align:center}
    .msg.ok{display:block;background:#e8f8f0;color:#27ae60}
    .msg.err{display:block;background:#fde8e8;color:#e74c3c}
    .log-section{margin-top:20px}
    .log-title{font-size:14px;color:#888;margin-bottom:10px;font-weight:600}
    .log-item{background:#f8f9fa;border-radius:10px;padding:12px 16px;margin-bottom:8px;font-size:12px}
    .log-item .time{color:#aaa;font-size:11px}
    .log-item .action{font-weight:600}
    .log-item.ack{border-left:3px solid #27ae60}
    .log-item.sent{border-left:3px solid #f39c12}
    .no-heartbeat{text-align:center;color:#e74c3c;padding:30px;font-size:14px}
    .broker-tag{display:inline-block;background:#eaf3ff;color:#4a7cc9;padding:2px 8px;border-radius:10px;font-size:11px;font-weight:600;margin-left:8px}
  </style>
</head>
<body>
  <div class="wrap">
    <a class="back" href="/device/${hostName}">&larr; 返回设备详情</a>

    <div class="card">
      <div class="card-header">
        <span class="host-name">${hostName}<span class="broker-tag" id="brokerTag"></span></span>
        <span class="status-badge" id="statusBadge">检查中...</span>
      </div>
      <div class="card-body" id="mainBody">
        <div style="text-align:center;color:#aaa;padding:20px">加载中...</div>
      </div>
    </div>

    <div class="card log-section" id="logCard" style="display:none">
      <div class="card-body">
        <div class="log-title">指令日志</div>
        <div id="logList"></div>
      </div>
    </div>
  </div>

  <script>
    const HOST = '${hostName}';
    let hostData = null;
    let formBuilt = false;

    async function loadStatus() {
      try {
        const r = await fetch('/api/control/' + encodeURIComponent(HOST));
        hostData = await r.json();

        var badge = document.getElementById('statusBadge');
        var btag = document.getElementById('brokerTag');
        if (hostData.online) {
          badge.textContent = '在线';
          badge.className = 'status-badge status-online';
        } else {
          badge.textContent = '离线';
          badge.className = 'status-badge status-offline';
        }
        if (hostData.brokerLabel) btag.textContent = hostData.brokerLabel;

        var body = document.getElementById('mainBody');
        if (!hostData.hasHeartbeat) {
          if (!formBuilt) {
            body.innerHTML = '<div class="no-heartbeat">设备尚未上线（无数据记录）<br>请等待设备发送数据后再使用云控</div>';
          }
          return;
        }

        // 只在首次构建表单，后续刷新不覆盖用户输入
        if (!formBuilt) {
          body.innerHTML = buildForm();
          bindRadio();
          formBuilt = true;
        }
        updateDeviceConfig();
        renderLog();
      } catch(e) {
        if (!formBuilt) {
          document.getElementById('mainBody').innerHTML = '<div class="no-heartbeat">加载失败</div>';
        }
      }
    }

    function buildForm() {
      return '<div id="deviceConfigBox" style="background:#f0f7ff;border-radius:12px;padding:16px 20px;margin-bottom:20px;font-size:13px;display:none">'
        + '<div style="font-weight:700;color:#2c3e50;margin-bottom:8px">设备实际配置</div>'
        + '<div id="deviceConfigContent" style="color:#555">等待设备上报...</div>'
        + '</div>'
        + '<div class="form-group">'
        + '<label>扫描模式</label>'
        + '<div class="radio-group">'
        + '<label id="radioMac" class="active" onclick="pickMode(\\'mac\\')"><input type="radio" name="scanMode" value="mac" checked> MAC 地址</label>'
        + '<label id="radioUuid" onclick="pickMode(\\'uuid\\')"><input type="radio" name="scanMode" value="uuid"> UUID</label>'
        + '</div></div>'
        + '<div class="form-group">'
        + '<label id="targetsLabel">目标 MAC 地址</label>'
        + '<textarea id="targetsInput" placeholder="多个用逗号分隔，如 F8:A7:63:9F:F4:8E,AA:BB:CC:DD:EE:FF"></textarea>'
        + '<div class="hint" id="targetsHint">多个目标用逗号或空格分隔</div>'
        + '</div>'
        + '<button class="btn-send" id="sendBtn" onclick="doSend()">下发配置</button>'
        + '<div class="msg" id="msgBox"></div>';
    }

    function updateDeviceConfig() {
      if (!hostData || !hostData.configStatus) return;
      var cs = hostData.configStatus;
      var box = document.getElementById('deviceConfigBox');
      var content = document.getElementById('deviceConfigContent');
      if (!box || !content) return;
      box.style.display = 'block';
      var modeLabel = cs.scanMode === 'uuid' ? 'UUID' : 'MAC';
      var ago = Math.round((Date.now() - cs.receivedAt) / 1000);
      var agoText = ago < 60 ? ago + '秒前' : Math.round(ago / 60) + '分钟前';
      content.innerHTML = '<span style="display:inline-block;background:#fff;padding:3px 10px;border-radius:6px;margin-right:8px;font-weight:600;color:#2980b9">' + modeLabel + '</span>'
        + '<span style="font-family:monospace;word-break:break-all">' + (cs.targets || '无') + '</span>'
        + '<span style="color:#aaa;margin-left:10px;font-size:11px">(' + agoText + '上报)</span>';
    }

    function pickMode(mode) {
      document.getElementById('radioMac').className = (mode === 'mac') ? 'active' : '';
      document.getElementById('radioUuid').className = (mode === 'uuid') ? 'active' : '';
      document.querySelector('input[value="' + mode + '"]').checked = true;
      if (mode === 'uuid') {
        document.getElementById('targetsLabel').textContent = '目标 UUID';
        document.getElementById('targetsInput').placeholder = '多个用逗号分隔，如 FFF0,181A';
      } else {
        document.getElementById('targetsLabel').textContent = '目标 MAC 地址';
        document.getElementById('targetsInput').placeholder = '多个用逗号分隔，如 F8:A7:63:9F:F4:8E,AA:BB:CC:DD:EE:FF';
      }
    }

    function bindRadio() {
      document.querySelectorAll('.radio-group label').forEach(function(el) {
        el.addEventListener('click', function() {
          var v = el.querySelector('input').value;
          pickMode(v);
        });
      });
    }

    function showMsg(text, type) {
      var el = document.getElementById('msgBox');
      if (!el) return;
      el.textContent = text;
      el.className = 'msg ' + type;
      if (type === 'ok') setTimeout(function(){ el.className = 'msg'; }, 5000);
    }

    async function doSend() {
      var mode = document.querySelector('input[name="scanMode"]:checked').value;
      var targets = document.getElementById('targetsInput').value.trim();
      if (!targets) { showMsg('请输入目标列表', 'err'); return; }

      var btn = document.getElementById('sendBtn');
      btn.disabled = true;
      btn.textContent = '下发中...';

      try {
        var r = await fetch('/api/control/' + encodeURIComponent(HOST), {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ scanMode: mode, targets: targets })
        });
        var d = await r.json();
        if (d.ok) {
          showMsg('指令已下发，等待设备确认...', 'ok');
          setTimeout(loadStatus, 2000);
        } else {
          showMsg(d.error || '下发失败', 'err');
        }
      } catch(e) {
        showMsg('网络错误', 'err');
      }
      btn.disabled = false;
      btn.textContent = '下发配置';
    }

    function renderLog() {
      if (!hostData) return;
      var acks = hostData.acks || [];
      var cmds = hostData.cmdLog || [];
      if (acks.length === 0 && cmds.length === 0) {
        document.getElementById('logCard').style.display = 'none';
        return;
      }
      document.getElementById('logCard').style.display = 'block';
      var list = document.getElementById('logList');

      // 合并 acks 和 cmds，按时间排序
      var items = [];
      cmds.forEach(function(c) {
        items.push({ type: 'sent', time: c.sentAt, mode: c.scanMode, targets: c.targets });
      });
      acks.forEach(function(a) {
        items.push({ type: 'ack', time: a.receivedAt, mode: a.scanMode, targets: a.targets, status: a.status });
      });
      items.sort(function(a, b) { return b.time - a.time; });

      var html = '';
      items.slice(0, 20).forEach(function(it) {
        var timeStr = new Date(it.time).toLocaleString('zh-CN', { timeZone: 'Asia/Shanghai' });
        if (it.type === 'ack') {
          html += '<div class="log-item ack"><span class="time">' + timeStr + '</span> <span class="action">✅ 设备已确认</span> 模式=' + it.mode + ' 目标=' + it.targets + '</div>';
        } else {
          html += '<div class="log-item sent"><span class="time">' + timeStr + '</span> <span class="action">⏳ 已下发</span> 模式=' + it.mode + ' 目标=' + it.targets + '</div>';
        }
      });
      list.innerHTML = html || '<div style="color:#aaa;font-size:13px">暂无记录</div>';
    }

    loadStatus();
    setInterval(loadStatus, 5000);
  </script>
</body>
</html>`);
});

// 模拟发包页面
app.get('/send', (req, res) => {
  res.send(`<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>MQTT 模拟发包</title>
  <script src="https://unpkg.com/mqtt/dist/mqtt.min.js"></script>
  <style>
    *{margin:0;padding:0;box-sizing:border-box}
    body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:linear-gradient(135deg,#1a1a2e 0%,#16213e 50%,#0f3460 100%);min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}
    .container{background:#fff;padding:40px 36px;border-radius:20px;box-shadow:0 20px 60px rgba(0,0,0,.3);max-width:520px;width:100%}
    h1{color:#2c3e50;margin-bottom:6px;font-size:24px;text-align:center}
    .subtitle{color:#888;margin-bottom:30px;font-size:13px;text-align:center}
    .form-group{margin-bottom:18px}
    label{display:block;font-size:13px;font-weight:600;color:#555;margin-bottom:6px}
    input,textarea{width:100%;padding:14px 16px;border:2px solid #e1e8ed;border-radius:12px;font-size:15px;transition:border-color .3s;font-family:inherit}
    input:focus,textarea:focus{outline:none;border-color:#667eea}
    textarea{resize:vertical;min-height:80px;font-family:'SF Mono',Monaco,Consolas,monospace;font-size:13px}
    .btn-send{background:linear-gradient(45deg,#e74c3c,#c0392b);color:#fff;border:none;padding:16px;border-radius:12px;font-size:16px;font-weight:600;cursor:pointer;transition:transform .2s,box-shadow .2s;width:100%;margin-top:6px}
    .btn-send:hover{transform:translateY(-2px);box-shadow:0 8px 25px rgba(231,76,60,.4)}
    .btn-send:active{transform:translateY(0)}
    .btn-send:disabled{opacity:.5;cursor:not-allowed;transform:none;box-shadow:none}
    .status{margin-top:16px;padding:12px 16px;border-radius:10px;font-size:13px;display:none;text-align:center}
    .status.success{display:block;background:#e8f8f0;color:#27ae60}
    .status.error{display:block;background:#fde8e8;color:#e74c3c}
    .status.connecting{display:block;background:#fef9e7;color:#f39c12}
    .mqtt-status{text-align:center;margin-bottom:20px;font-size:12px}
    .mqtt-dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;vertical-align:middle}
    .mqtt-dot.connected{background:#27ae60}
    .mqtt-dot.disconnected{background:#e74c3c}
    .mqtt-dot.connecting{background:#f39c12}
    .history{margin-top:24px}
    .history h3{font-size:14px;color:#888;margin-bottom:10px}
    .history-item{background:#f8f9fa;border-radius:10px;padding:12px 16px;margin-bottom:8px;font-size:12px;font-family:'SF Mono',Monaco,Consolas,monospace;word-break:break-all}
    .history-item .time{color:#aaa;font-size:11px;margin-bottom:4px;font-family:-apple-system,sans-serif}
    .history-item .topic{color:#667eea;font-weight:600}
    .back-link{display:block;text-align:center;margin-top:20px;color:#667eea;text-decoration:none;font-size:13px}
    .back-link:hover{text-decoration:underline}
  </style>
</head>
<body>
  <div class="container">
    <h1>MQTT 模拟发包</h1>
    <p class="subtitle">模拟设备向指定 Broker 发送扫描数据</p>

    <div class="form-group">
      <label>Broker 选择</label>
      <select id="brokerSelect" onchange="switchBroker()" style="width:100%;padding:14px 16px;border:2px solid #e1e8ed;border-radius:12px;font-size:15px;background:#fff;cursor:pointer">
        <option value="emqx">EMQX 国际 (broker.emqx.io)</option>
        <option value="hivemq">HiveMQ 国际 (broker.hivemq.com)</option>
      </select>
    </div>

    <div class="mqtt-status">
      <span class="mqtt-dot connecting" id="mqttDot"></span>
      <span id="mqttStatusText">正在连接 MQTT...</span>
    </div>

    <div class="form-group">
      <label>设备名称（主机名）</label>
      <input type="text" id="hostInput" placeholder="例如 MFAC" />
    </div>

    <div class="form-group">
      <label>MAC 地址</label>
      <input type="text" id="macInput" placeholder="例如 F8:A7:63:9F:F4:8E" />
    </div>

    <div class="form-group">
      <label>Raw 广播数据（advData）</label>
      <textarea id="rawInput" placeholder="例如 0201061AFF4C000215..."></textarea>
    </div>

    <div class="form-group" style="margin-bottom:12px">
      <label style="display:flex;align-items:center;gap:8px;cursor:pointer">
        <input type="checkbox" id="retainCheck" checked style="width:16px;height:16px"> Retain（保留消息，接收端订阅后立即可收到最新一条）
      </label>
    </div>

    <button class="btn-send" id="sendBtn" onclick="doSend()" disabled>发 送</button>

    <div class="status" id="statusMsg"></div>

    <div class="history" id="historyWrap" style="display:none">
      <h3>发送记录</h3>
      <div id="historyList"></div>
    </div>

    <a class="back-link" href="/">← 返回设备查询</a>
  </div>

  <script>
    const BROKERS = {
      emqx:    { label: 'EMQX 国际',  wss: 'wss://broker.emqx.io:8084/mqtt' },
      hivemq:  { label: 'HiveMQ 国际', wss: 'wss://broker.hivemq.com:8884/mqtt' }
    };
    let client = null;
    let connected = false;
    let currentBrokerKey = 'emqx';

    function connectBroker(key) {
      const dot = document.getElementById('mqttDot');
      const text = document.getElementById('mqttStatusText');
      const btn = document.getElementById('sendBtn');
      const cfg = BROKERS[key];
      if (!cfg) return;

      // 断开旧连接
      if (client) {
        try { client.end(true); } catch(e) {}
        client = null;
        connected = false;
      }

      currentBrokerKey = key;
      dot.className = 'mqtt-dot connecting';
      text.textContent = '正在连接 ' + cfg.label + '...';
      btn.disabled = true;

      client = mqtt.connect(cfg.wss, {
        clientId: 'sim_' + Math.random().toString(36).substring(2, 10),
        clean: true,
        connectTimeout: 5000
      });

      client.on('connect', function() {
        connected = true;
        dot.className = 'mqtt-dot connected';
        text.textContent = '已连接 ' + cfg.label;
        btn.disabled = false;
      });

      client.on('error', function(err) {
        connected = false;
        dot.className = 'mqtt-dot disconnected';
        text.textContent = cfg.label + ' 连接失败: ' + err.message;
        btn.disabled = true;
      });

      client.on('offline', function() {
        connected = false;
        dot.className = 'mqtt-dot disconnected';
        text.textContent = cfg.label + ' 连接已断开';
        btn.disabled = true;
      });

      client.on('reconnect', function() {
        dot.className = 'mqtt-dot connecting';
        text.textContent = '正在重连 ' + cfg.label + '...';
      });
    }

    function switchBroker() {
      const sel = document.getElementById('brokerSelect').value;
      connectBroker(sel);
    }

    function showStatus(msg, type) {
      const el = document.getElementById('statusMsg');
      el.textContent = msg;
      el.className = 'status ' + type;
      if (type === 'success') {
        setTimeout(function() { el.className = 'status'; }, 3000);
      }
    }

    function doSend() {
      const host = document.getElementById('hostInput').value.trim();
      const mac = document.getElementById('macInput').value.trim();
      const raw = document.getElementById('rawInput').value.trim();

      if (!host) { showStatus('请输入设备名称', 'error'); return; }
      if (!mac) { showStatus('请输入 MAC 地址', 'error'); return; }
      if (!raw) { showStatus('请输入 Raw 数据', 'error'); return; }
      if (!connected) { showStatus('MQTT 未连接，请稍候', 'error'); return; }

      // Topic: {主机名}/{MAC去掉冒号}
      const topicMac = mac.replace(/[:-]/g, '').toUpperCase();
      const topic = host + '/' + topicMac;

      const payload = JSON.stringify({
        mac: mac,
        advData: raw,
        ts: Math.floor(Date.now() / 1000)
      });

      const retain = document.getElementById('retainCheck').checked;
      const brokerLabel = BROKERS[currentBrokerKey] ? BROKERS[currentBrokerKey].label : currentBrokerKey;
      client.publish(topic, payload, { qos: 1, retain: retain }, function(err) {
        if (err) {
          showStatus('发送失败: ' + err.message, 'error');
        } else {
          showStatus('发送成功! [' + brokerLabel + '] Topic: ' + topic, 'success');
          addHistory(brokerLabel, topic, payload);
        }
      });
    }

    function addHistory(broker, topic, payload) {
      const wrap = document.getElementById('historyWrap');
      const list = document.getElementById('historyList');
      wrap.style.display = 'block';

      const time = new Date().toLocaleString('zh-CN');
      const div = document.createElement('div');
      div.className = 'history-item';
      div.innerHTML = '<div class="time">' + time + ' [' + broker + ']</div>'
        + '<div class="topic">Topic: ' + topic + '</div>'
        + '<div>Payload: ' + payload + '</div>';
      list.insertBefore(div, list.firstChild);

      // 最多保留 20 条
      while (list.children.length > 20) {
        list.removeChild(list.lastChild);
      }
    }

    // 页面加载时连接默认Broker
    connectBroker('emqx');

    // 回车发送
    document.querySelectorAll('input').forEach(function(el) {
      el.addEventListener('keydown', function(e) {
        if (e.key === 'Enter') doSend();
      });
    });
  </script>
</body>
</html>`);
});

app.listen(PORT, () => {
  console.log('============================================');
  console.log('  Device Query Service started');
  console.log('  Home:  http://localhost:' + PORT + '/');
  console.log('  Send:  http://localhost:' + PORT + '/send');
  console.log('  Port:  ' + PORT);
  console.log('  Brokers:');
  for (const [key, cfg] of Object.entries(BROKER_PRESETS)) {
    console.log('    ' + key + ': ' + cfg.host + ':' + cfg.tcp + ' (' + cfg.label + ')');
  }
  console.log('============================================');
});

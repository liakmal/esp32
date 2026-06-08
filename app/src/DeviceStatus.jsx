import { useState, useEffect, useRef } from 'react'

const IP   = '192.168.4.1'
const PORT = 8080
const F    = '-apple-system,BlinkMacSystemFont,"Segoe UI",Arial,sans-serif'
const C    = { bg: '#f0f2f5', white: '#fff', primary: '#5b8dee', danger: '#e05c65',
               text: '#222', sub: '#555', muted: '#aaa', border: '#f0f0f0',
               ok: '#27ae60', warn: '#e67e22' }

const s = {
  wrap:   { fontFamily: F, background: C.bg, minHeight: '100vh', display: 'flex', flexDirection: 'column' },
  bar:    { background: C.white, padding: '10px 14px', display: 'flex', alignItems: 'center', gap: 10,
            borderBottom: `1px solid ${C.border}`, boxShadow: '0 1px 4px rgba(0,0,0,.05)', flexShrink: 0 },
  body:   { flex: 1, overflowY: 'auto', padding: '14px 14px 40px' },
  card:   { background: C.white, borderRadius: 12, boxShadow: '0 1px 6px rgba(0,0,0,.07)', marginBottom: 12, overflow: 'hidden' },
  row:    { display: 'flex', alignItems: 'flex-start', padding: '11px 14px', borderBottom: `1px solid ${C.border}` },
  lbl:    { width: '36%', fontSize: 13, color: '#666', fontWeight: 500, flexShrink: 0, paddingTop: 1 },
  val:    { flex: 1, fontSize: 14, color: C.text, wordBreak: 'break-all', lineHeight: 1.5 },
  sec:    { fontSize: 11, fontWeight: 700, color: '#999', textTransform: 'uppercase', letterSpacing: '.7px',
            padding: '8px 14px 6px', background: '#fafafa', borderBottom: `1px solid ${C.border}` },
  actRow: { display: 'flex', gap: 10, marginTop: 4 },
  btnG:   { flex: 1, padding: '13px 0', background: '#eaeaea', color: '#333', border: 'none',
            borderRadius: 10, fontSize: 15, fontWeight: 600, cursor: 'pointer' },
  btnR:   { flex: 1, padding: '13px 0', background: '#fdecea', color: C.danger, border: 'none',
            borderRadius: 10, fontSize: 15, fontWeight: 600, cursor: 'pointer' },
}

function Row({ label, value }) {
  return (
    <div style={{ ...s.row, borderBottom: 'none' }}>
      <span style={s.lbl}>{label}</span>
      <span style={s.val}>{value}</span>
    </div>
  )
}

function rssiBar(rssi) {
  const bars = rssi >= -50 ? 4 : rssi >= -60 ? 3 : rssi >= -70 ? 2 : 1
  return '▓'.repeat(bars) + '░'.repeat(4 - bars) + ` ${rssi} dBm`
}

function agoText(s) {
  if (s < 0) return '尚未发现'
  if (s < 60) return `${s} 秒前`
  if (s < 3600) return `${Math.floor(s / 60)} 分钟前`
  if (s < 86400) return `${Math.floor(s / 3600)} 小时前`
  return `${Math.floor(s / 86400)} 天前`
}

function uptime(ms) {
  const s = Math.floor(ms / 1000)
  const m = Math.floor(s / 60) % 60
  const h = Math.floor(s / 3600) % 24
  const d = Math.floor(s / 86400)
  if (d > 0) return `${d}天 ${h}时 ${m}分`
  if (h > 0) return `${h}时 ${m}分`
  return `${m}分 ${s % 60}秒`
}

export default function DeviceStatus({ ssid, onBack, onReinit, embedded }) {
  const isC1 = /^C1-/i.test(ssid)

  const [data,     setData]     = useState(null)
  const [loading,  setLoading]  = useState(true)
  const [lastTick, setLastTick] = useState(null)
  const [confirm,  setConfirm]  = useState(null)
  const [acting,   setActing]   = useState(false)
  const resolvedPort = useRef(isC1 ? PORT : PORT)
  const timer = useRef(null)

  // New feature states
  const [logText,     setLogText]     = useState(null)
  const [logOpen,     setLogOpen]     = useState(false)
  const [diagData,    setDiagData]    = useState(null)
  const [diagOpen,    setDiagOpen]    = useState(false)
  const [diagLoading, setDiagLoading] = useState(false)
  const [cfgOpen,     setCfgOpen]     = useState(false)
  const [cfgForm,     setCfgForm]     = useState({})
  const [cfgSaving,   setCfgSaving]   = useState(false)
  const [cfgMsg,      setCfgMsg]      = useState(null)

  const fetchStatus = () => {
    if (isC1) {
      // C1: always port 8080
      fetch(`http://${IP}:${PORT}/api/status.json`, { cache: 'no-store' })
        .then(r => r.json())
        .then(d => { setData(d); setLastTick(new Date()) })
        .catch(() => setData(null))
        .finally(() => setLoading(false))
    } else {
      // O5/O5c: try 8080 (O5c) first, fallback to 80 (O5)
      fetch(`http://${IP}:${PORT}/api/status.json`, { cache: 'no-store' })
        .then(r => { if (!r.ok) throw new Error(); resolvedPort.current = PORT; return r.json() })
        .catch(() =>
          fetch(`http://${IP}:80/api/status.json`, { cache: 'no-store' })
            .then(r => { resolvedPort.current = 80; return r.json() })
        )
        .then(d => { setData(d); setLastTick(new Date()) })
        .catch(() => setData(null))
        .finally(() => setLoading(false))
    }
  }

  useEffect(() => {
    fetchStatus()
    timer.current = setInterval(fetchStatus, 5000)
    return () => clearInterval(timer.current)
  }, [])

  const doAction = async (action) => {
    setActing(true)
    try {
      const endpoint = action === 'silent' ? 'api/silent' : action
      await fetch(`http://${IP}:${resolvedPort.current}/${endpoint}`, { cache: 'no-store' })
      if (action === 'reinit') { setTimeout(onReinit || onBack, 2000) }
      else if (action === 'silent') { setTimeout(onReinit || onBack, 2000) }
      else { setData(null); setLoading(true); setTimeout(fetchStatus, 5000) }
    } catch {}
    setConfirm(null)
    setActing(false)
  }

  return (
    <div style={embedded ? { background: C.bg } : s.wrap}>
      {!embedded && (
        <div style={s.bar}>
          <button onClick={onBack} style={{ background: 'none', border: 'none', color: C.primary, fontSize: 15, fontWeight: 600, cursor: 'pointer', padding: '4px 0' }}>← 返回</button>
          <span style={{ flex: 1, fontSize: 15, fontWeight: 600, color: C.text }}>设备状态</span>
          <button onClick={fetchStatus} style={{ background: 'none', border: 'none', color: C.muted, cursor: 'pointer', fontSize: 16 }}>↻</button>
          {lastTick && <span style={{ fontSize: 11, color: C.muted }}>{lastTick.toLocaleTimeString()}</span>}
        </div>
      )}

      <div style={embedded ? { padding: '14px 14px 40px' } : s.body}>
        {loading && !data && (
          <div style={{ textAlign: 'center', padding: 40, color: C.muted, fontSize: 14 }}>读取状态中...</div>
        )}
        {!loading && !data && (
          <div style={{ textAlign: 'center', padding: 40, color: C.danger, fontSize: 14 }}>无法连接到设备<br /><span style={{ fontSize: 12, color: C.muted }}>请确认已连接 {ssid} 热点</span></div>
        )}

        {data && <>
          {/* 设备信息 */}
          <div style={s.card}>
            <div style={s.sec}>设备信息</div>
            <Row label={isC1 ? '设备名称' : '设备 ID'} value={data.device_name || data.device_id || '—'} />
            <Row label="MAC 地址" value={<span style={{ fontFamily: 'monospace', fontSize: 13 }}>{data.mac}</span>} />
            {!isC1 && <Row label="联网方式" value={data.net_mode === 'ml307r' ? '物联网模块' : data.net_mode === 'ml307' ? 'ML307' : 'WiFi'} />}
            <Row label="运行时间" value={uptime(data.uptime_ms)} />
            <Row label="空闲内存" value={`${(data.free_heap / 1024).toFixed(1)} KB${data.heap_total ? ' / ' + (data.heap_total / 1024).toFixed(0) + ' KB' : ''}`} />
            {data.time_str && <Row label="设备时间" value={data.time_str} />}
          </div>

          {/* WiFi 状态 */}
          <div style={s.card}>
            <div style={s.sec}>WiFi 状态</div>
            <Row label="连接状态" value={
              <span style={{ color: data.wifi_connected ? C.ok : C.danger, fontWeight: 700 }}>
                {data.wifi_connected ? '✅ 已连接' : '❌ 未连接'}
              </span>
            } />
            <Row label="SSID" value={data.wifi_ssid || '—'} />
            {data.wifi_connected && <>
              <Row label="IP 地址" value={<span style={{ fontFamily: 'monospace' }}>{data.wifi_ip}</span>} />
              <Row label="信号强度" value={rssiBar(data.wifi_rssi)} />
            </>}
          </div>

          {/* MQTT 状态（C1 & O5c 共用） */}
          {data.data_mode === 'mqtt' && (
            <div style={s.card}>
              <div style={s.sec}>MQTT</div>
              <Row label="连接状态" value={
                <span style={{ color: data.mqtt_connected ? C.ok : C.danger, fontWeight: 700 }}>
                  {data.mqtt_connected ? '✅ 已连接' : '❌ 未连接'}
                </span>
              } />
              <Row label="Broker" value={<span style={{ fontFamily: 'monospace', fontSize: 13 }}>{data.mqtt_host === 'auto' || data.mqtt_auto_mode ? '自动模式' : `${data.mqtt_host}:${data.mqtt_port}`}</span>} />
              {isC1
                ? <>
                    <Row label="序列号" value={data.mqtt_host_name || '—'} />
                    <Row label="目标 MAC" value={data.mqtt_target_mac || <span style={{ color: C.muted }}>订阅所有</span>} />
                    <Row label="最后消息" value={
                      data.last_msg_ago_s >= 0
                        ? <span style={{ color: C.ok }}>{agoText(data.last_msg_ago_s)}</span>
                        : <span style={{ color: C.muted }}>尚未收到</span>
                    } />
                  </>
                : <Row label="设备序列号" value={data.mqtt_device_name || '—'} />
              }
            </div>
          )}

          {/* 网络诊断 */}
          <div style={s.card}>
            <div style={{ ...s.sec, cursor: 'pointer', display: 'flex', justifyContent: 'space-between' }}
              onClick={() => {
                if (diagOpen) { setDiagOpen(false); return }
                setDiagLoading(true); setDiagOpen(true)
                fetch(`http://${IP}:${resolvedPort.current}/api/diag`, { cache: 'no-store' })
                  .then(r => r.json()).then(d => setDiagData(d))
                  .catch(() => setDiagData({ error: true }))
                  .finally(() => setDiagLoading(false))
              }}>
              <span>网络诊断</span><span>{diagOpen ? '▲' : '▼'}</span>
            </div>
            {diagOpen && (
              diagLoading ? <div style={{ padding: 16, textAlign: 'center', color: C.muted, fontSize: 13 }}>诊断中...</div>
              : diagData?.error ? <div style={{ padding: 16, textAlign: 'center', color: C.danger, fontSize: 13 }}>诊断失败</div>
              : diagData && <>
                <Row label="WiFi/网络" value={<span style={{ color: diagData.wifi_ok ? C.ok : C.danger, fontWeight: 600 }}>{diagData.wifi_ok ? '✅ 正常' : '❌ 异常'}{diagData.is_4g ? ' (4G)' : ''}</span>} />
                {diagData.wifi_ip && <Row label="IP" value={<span style={{ fontFamily: 'monospace', fontSize: 13 }}>{diagData.wifi_ip}</span>} />}
                {diagData.wifi_rssi && <Row label="信号" value={rssiBar(diagData.wifi_rssi)} />}
                <Row label="DNS" value={<span style={{ color: diagData.dns_ok ? C.ok : C.danger }}>{diagData.dns_ok ? '✅' : '❌'} {diagData.dns_target || ''}{diagData.dns_ms !== undefined ? ` (${diagData.dns_ms}ms)` : ''}</span>} />
                <Row label="TCP" value={<span style={{ color: diagData.tcp_ok === true ? C.ok : C.danger }}>{diagData.tcp_ok === true ? '✅' : diagData.tcp_ok === 'inferred' ? 'ℹ️ 推断' : '❌'} {diagData.tcp_target || ''}{diagData.tcp_ms !== undefined ? ` (${diagData.tcp_ms}ms)` : ''}</span>} />
                <Row label="MQTT" value={<span style={{ color: diagData.mqtt_connected ? C.ok : C.danger, fontWeight: 600 }}>{diagData.mqtt_connected ? '✅ 已连接' : '❌ 未连接'}</span>} />
                {diagData.pub_ok !== undefined && <Row label="发布测试" value={<span style={{ color: diagData.pub_ok ? C.ok : C.danger }}>{diagData.pub_ok ? '✅ 成功' : '❌ 失败'}</span>} />}
              </>
            )}
          </div>

          {/* 运行日志 */}
          <div style={s.card}>
            <div style={{ ...s.sec, cursor: 'pointer', display: 'flex', justifyContent: 'space-between' }}
              onClick={() => {
                if (logOpen) { setLogOpen(false); return }
                setLogOpen(true)
                fetch(`http://${IP}:${resolvedPort.current}/api/log`, { cache: 'no-store' })
                  .then(r => r.text()).then(t => setLogText(t))
                  .catch(() => setLogText('无法获取日志'))
              }}>
              <span>运行日志</span><span>{logOpen ? '▲' : '▼'}</span>
            </div>
            {logOpen && <>
              <div style={{ padding: '0 14px 8px', display: 'flex', gap: 8, justifyContent: 'flex-end' }}>
                <button onClick={() => {
                  fetch(`http://${IP}:${resolvedPort.current}/api/log`, { cache: 'no-store' })
                    .then(r => r.text()).then(t => setLogText(t)).catch(() => {})
                }} style={{ padding: '4px 10px', background: '#eaeaea', border: 'none', borderRadius: 6, fontSize: 12, cursor: 'pointer' }}>刷新</button>
                <button onClick={() => {
                  fetch(`http://${IP}:${resolvedPort.current}/api/log/clear`, { cache: 'no-store' })
                    .then(() => setLogText('')).catch(() => {})
                }} style={{ padding: '4px 10px', background: '#fdecea', color: C.danger, border: 'none', borderRadius: 6, fontSize: 12, cursor: 'pointer' }}>清空</button>
              </div>
              <pre style={{ margin: 0, padding: '8px 14px 12px', fontSize: 11, color: '#555', fontFamily: 'monospace', whiteSpace: 'pre-wrap', wordBreak: 'break-all', maxHeight: 300, overflowY: 'auto', background: '#fafafa', borderTop: `1px solid ${C.border}` }}>
                {logText ?? '加载中...'}
              </pre>
            </>}
          </div>

          {/* 热配置修改 */}
          <div style={s.card}>
            <div style={{ ...s.sec, cursor: 'pointer', display: 'flex', justifyContent: 'space-between' }}
              onClick={() => {
                if (cfgOpen) { setCfgOpen(false); return }
                setCfgOpen(true); setCfgMsg(null)
                if (isC1) setCfgForm({ wifi_ssid: data.wifi_ssid || '', wifi_pass: '', mqtt_preset: data.mqtt_preset || '', mqtt_host: '', mqtt_port: '', mqtt_user: '', mqtt_pass: '', mqtt_h_name: '', mqtt_target_mac: '' })
                else setCfgForm({ wifi_ssid: data.wifi_ssid || '', wifi_pass: '', mqtt_preset: data.mqtt_preset || '', mqtt_host: '', mqtt_port: '', mqtt_user: '', mqtt_pass: '', mqtt_dname: '', scan_mode: data.scan_mode || 'mac', scan_target: data.scan_target || '', scan_duration: String(data.scan_duration || ''), scan_interval: String(data.scan_interval || '') })
              }}>
              <span>快速配置修改</span><span>{cfgOpen ? '▲' : '▼'}</span>
            </div>
            {cfgOpen && <div style={{ padding: '8px 14px 14px' }}>
              <div style={{ fontSize: 11, color: C.muted, marginBottom: 10 }}>仅填写需要修改的字段，空值将保持原样</div>
              <CfgField label="WiFi SSID" value={cfgForm.wifi_ssid} onChange={v => setCfgForm(f => ({ ...f, wifi_ssid: v }))} />
              <CfgField label="WiFi 密码" value={cfgForm.wifi_pass} onChange={v => setCfgForm(f => ({ ...f, wifi_pass: v }))} />
              <div style={{ marginBottom: 8 }}>
                <div style={{ fontSize: 11, color: '#888', marginBottom: 3 }}>MQTT Broker</div>
                <select style={{ width: '100%', padding: '8px 10px', border: '1px solid #e5e7eb', borderRadius: 6, fontSize: 13, outline: 'none', boxSizing: 'border-box', background: '#fff' }}
                  value={cfgForm.mqtt_preset || ''} onChange={e => setCfgForm(f => ({ ...f, mqtt_preset: e.target.value }))}>
                  <option value="">不修改</option>
                  <option value="auto">自动（同时监听全部）</option>
                  <option value="emqx">EMQX 国际</option>
                  <option value="hivemq">HiveMQ 国际</option>
                  <option value="custom">自定义</option>
                </select>
              </div>
              {cfgForm.mqtt_preset === 'custom' && <>
                <CfgField label="MQTT Host" value={cfgForm.mqtt_host} onChange={v => setCfgForm(f => ({ ...f, mqtt_host: v }))} />
                <CfgField label="MQTT Port" value={cfgForm.mqtt_port} onChange={v => setCfgForm(f => ({ ...f, mqtt_port: v }))} />
                <CfgField label="MQTT 用户名" value={cfgForm.mqtt_user} onChange={v => setCfgForm(f => ({ ...f, mqtt_user: v }))} />
                <CfgField label="MQTT 密码" value={cfgForm.mqtt_pass} onChange={v => setCfgForm(f => ({ ...f, mqtt_pass: v }))} />
              </>}
              {isC1 && <>
                <CfgField label="序列号" value={cfgForm.mqtt_h_name} onChange={v => setCfgForm(f => ({ ...f, mqtt_h_name: v }))} />
                <CfgField label="目标MAC" value={cfgForm.mqtt_target_mac} onChange={v => setCfgForm(f => ({ ...f, mqtt_target_mac: v }))} />
              </>}
              {!isC1 && <>
                <CfgField label="设备名" value={cfgForm.mqtt_dname} onChange={v => setCfgForm(f => ({ ...f, mqtt_dname: v }))} />
                <div style={{ marginBottom: 8 }}>
                  <div style={{ fontSize: 11, color: '#888', marginBottom: 3 }}>扫描模式</div>
                  <select style={{ width: '100%', padding: '8px 10px', border: '1px solid #e5e7eb', borderRadius: 6, fontSize: 13, outline: 'none', boxSizing: 'border-box', background: '#fff' }}
                    value={cfgForm.scan_mode || 'mac'} onChange={e => setCfgForm(f => ({ ...f, scan_mode: e.target.value }))}>
                    <option value="mac">MAC 地址</option>
                    <option value="uuid">UUID</option>
                  </select>
                </div>
                <CfgField label="扫描目标" value={cfgForm.scan_target} onChange={v => setCfgForm(f => ({ ...f, scan_target: v }))} />
                <CfgField label="扫描时长(秒)" value={cfgForm.scan_duration} onChange={v => setCfgForm(f => ({ ...f, scan_duration: v }))} />
                <CfgField label="扫描间隔(秒)" value={cfgForm.scan_interval} onChange={v => setCfgForm(f => ({ ...f, scan_interval: v }))} />
              </>}
              <button onClick={async () => {
                setCfgSaving(true); setCfgMsg(null)
                const params = new URLSearchParams()
                const presetMap = { auto: ['broker.emqx.io', '1883'], emqx: ['broker.emqx.io', '1883'], hivemq: ['broker.hivemq.com', '1883'] }
                const resolved = { ...cfgForm }
                if (resolved.mqtt_preset && presetMap[resolved.mqtt_preset]) {
                  const [h, p] = presetMap[resolved.mqtt_preset]
                  resolved.mqtt_host = h; resolved.mqtt_port = p
                  resolved.mqtt_user = ''; resolved.mqtt_pass = ''
                }
                delete resolved.mqtt_preset
                Object.entries(resolved).forEach(([k, v]) => { if (v) params.append(k, v) })
                try {
                  const r = await fetch(`http://${IP}:${resolvedPort.current}/api/save-config?${params}`, { cache: 'no-store' })
                  const d = await r.json()
                  setCfgMsg({ ok: true, text: '已保存' + (d.wifi_changed ? ' (WiFi已更新)' : '') + (d.mqtt_changed ? ' (MQTT已更新)' : '') })
                  setTimeout(fetchStatus, 2000)
                } catch { setCfgMsg({ ok: false, text: '保存失败' }) }
                setCfgSaving(false)
              }} disabled={cfgSaving}
                style={{ width: '100%', padding: '11px 0', background: '#5b8dee', color: '#fff', border: 'none', borderRadius: 8, fontSize: 14, fontWeight: 600, cursor: 'pointer', marginTop: 6, opacity: cfgSaving ? .6 : 1 }}>
                {cfgSaving ? '保存中...' : '保存（不重启）'}
              </button>
              {cfgMsg && <div style={{ marginTop: 8, fontSize: 12, fontWeight: 600, textAlign: 'center', color: cfgMsg.ok ? C.ok : C.danger }}>{cfgMsg.text}</div>}
            </div>}
          </div>

          {/* C1: BLE 广播 + 休眠 */}
          {isC1 && (
            <div style={s.card}>
              <div style={s.sec}>其他</div>
              <Row label="BLE 广播" value={
                <span style={{ color: data.ble_advertising ? C.ok : C.danger }}>
                  {data.ble_advertising ? '广播中' : '已停止'}
                </span>
              } />
              <Row label="休眠模式" value={data.never_sleep ? '永不休眠' : '20分钟自动休眠'} />
            </div>
          )}

          {/* O5: BLE 扫描参数 */}
          {!isC1 && (
            <div style={s.card}>
              <div style={s.sec}>BLE 扫描</div>
              <Row label="扫描状态" value={
                <span style={{ color: data.is_scanning ? C.ok : C.muted, fontWeight: 600 }}>
                  {data.is_scanning ? '🔍 扫描中' : '⏸ 待机'}
                </span>
              } />
              <Row label="目标类型" value={data.scan_mode === 'uuid' ? 'UUID 匹配' : 'MAC 匹配'} />
              {data.scan_mode !== 'uuid' && (
                <Row label="目标 MAC" value={data.lc_target_mac || <span style={{ color: C.muted }}>全部设备</span>} />
              )}
              {data.scan_mode === 'uuid' && (
                <Row label="目标 UUID" value={<span style={{ fontFamily: 'monospace', fontSize: 13 }}>{data.target_uuid || <span style={{ color: C.muted }}>—</span>}</span>} />
              )}
              <Row label="扫描时长" value={`${data.scan_duration} 秒`} />
              <Row label="扫描间隔" value={`${data.scan_interval} 秒`} />
              <Row label="上报模式" value={data.data_mode === 'mqtt' ? 'MQTT' : data.data_mode === 'server' ? '自定义服务器' : data.data_mode || '—'} />
              <Row label="最近发现" value={
                data.last_found_ago_s >= 0
                  ? <><span style={{ fontFamily: 'monospace', fontSize: 13 }}>{data.last_found_mac}</span><span style={{ color: C.muted, fontSize: 12, marginLeft: 8 }}>{agoText(data.last_found_ago_s)}</span></>
                  : <span style={{ color: C.muted }}>尚未发现目标设备</span>
              } />
            </div>
          )}

          {/* 操作 */}
          {confirm ? (
            <div style={{ background: C.white, borderRadius: 12, padding: 16, textAlign: 'center' }}>
              <div style={{ fontSize: 14, color: C.text, marginBottom: 14 }}>
                {confirm === 'restart' ? '确定重启设备？配置将保留。'
                  : confirm === 'silent' ? '启用静默模式？设备将重启并关闭 LED 和配置热点。'
                  : '恢复出厂设置？⚠️ 所有配置将被清除！'}
              </div>
              <div style={s.actRow}>
                <button onClick={() => setConfirm(null)} style={s.btnG} disabled={acting}>取消</button>
                <button onClick={() => doAction(confirm)} style={{ ...s.btnR, opacity: acting ? .6 : 1 }} disabled={acting}>
                  {acting ? '处理中...' : '确认'}
                </button>
              </div>
            </div>
          ) : (
            <>
              {!isC1 && (
                <div style={{ ...s.actRow, marginBottom: 8, alignItems: 'center', justifyContent: 'space-between' }}>
                  <span style={{ fontSize: 13, fontWeight: 600, color: '#333' }}>云端控制</span>
                  <div onClick={async () => {
                    if (acting) return
                    setActing(true)
                    try {
                      const r = await fetch(`http://${IP}:${resolvedPort.current}/api/cloud-ctrl`, { cache: 'no-store' })
                      const d = await r.json()
                      if (d.ok) setTimeout(fetchStatus, 1000)
                    } catch {}
                    setActing(false)
                  }} style={{ width: 44, height: 24, borderRadius: 12, background: data?.cloud_ctrl ? '#27ae60' : '#ccc', position: 'relative', cursor: acting ? 'not-allowed' : 'pointer', transition: 'background .3s', opacity: acting ? .6 : 1 }}>
                    <div style={{ width: 20, height: 20, borderRadius: '50%', background: '#fff', position: 'absolute', top: 2, left: data?.cloud_ctrl ? 22 : 2, transition: 'left .3s', boxShadow: '0 1px 3px rgba(0,0,0,.2)' }} />
                  </div>
                </div>
              )}
              <div style={s.actRow}>
                <button onClick={() => setConfirm('restart')} style={s.btnG}>重启</button>
                <button onClick={() => setConfirm('silent')} style={{ ...s.btnG, background: '#fff3e0', color: C.warn }}>静默模式</button>
                <button onClick={() => setConfirm('reinit')}  style={s.btnR}>恢复出厂</button>
              </div>
            </>
          )}
        </>}
      </div>
    </div>
  )
}

function CfgField({ label, value, onChange, ph }) {
  return (
    <div style={{ marginBottom: 8 }}>
      <div style={{ fontSize: 11, color: '#888', marginBottom: 3 }}>{label}</div>
      <input style={{ width: '100%', padding: '8px 10px', border: '1px solid #e5e7eb', borderRadius: 6, fontSize: 13, outline: 'none', fontFamily: 'monospace', boxSizing: 'border-box' }}
        value={value || ''} onChange={e => onChange(e.target.value)} placeholder={ph || ''} />
    </div>
  )
}

import { useState, useEffect } from 'react'

const IP = '192.168.4.1'
const F  = '-apple-system,BlinkMacSystemFont,"Segoe UI",Arial,sans-serif'
const C  = { bg: '#f0f2f5', white: '#fff', primary: '#5b8dee', danger: '#e05c65',
             text: '#222', sub: '#555', muted: '#aaa', border: '#f0f0f0', ok: '#27ae60' }

const s = {
  wrap:   { fontFamily: F, background: C.bg, minHeight: '100vh', display: 'flex', flexDirection: 'column' },
  bar:    { background: C.white, padding: '10px 14px', display: 'flex', alignItems: 'center', gap: 10,
            borderBottom: `1px solid ${C.border}`, boxShadow: '0 1px 4px rgba(0,0,0,.05)', flexShrink: 0 },
  body:   { flex: 1, overflowY: 'auto', padding: '14px 14px 40px' },
  card:   { background: C.white, borderRadius: 12, boxShadow: '0 1px 6px rgba(0,0,0,.07)', padding: '14px 14px 6px', marginBottom: 12 },
  sec:    { fontSize: 12, fontWeight: 700, color: '#888', textTransform: 'uppercase', letterSpacing: '.8px', marginBottom: 10 },
  label:  { display: 'block', fontSize: 14, color: C.sub, marginBottom: 5, marginTop: 12 },
  input:  { width: '100%', padding: '10px 12px', border: '1px solid #e5e7eb', borderRadius: 8,
            fontSize: 15, color: C.text, background: '#fafafa', outline: 'none', boxSizing: 'border-box' },
  select: { width: '100%', padding: '10px 12px', border: '1px solid #e5e7eb', borderRadius: 8,
            fontSize: 15, color: C.text, background: '#fafafa', outline: 'none', boxSizing: 'border-box' },
  small:  { fontSize: 12, color: C.muted, display: 'block', marginTop: 4, lineHeight: 1.5 },
  btn:    { display: 'block', width: '100%', padding: '14px 0', background: C.primary, color: '#fff',
            border: 'none', borderRadius: 10, fontSize: 16, fontWeight: 600, marginTop: 8, cursor: 'pointer' },
  cbRow:  { display: 'flex', alignItems: 'center', gap: 10, padding: '10px 0 6px' },
}

export default function DeviceConfig({ ssid, onBack, embedded }) {
  const isC1 = /^C1-/i.test(ssid)

  const [form, setForm] = useState(isC1 ? {
    device_name: '', ssid: '', pass: '',
    data_mode: 'mqtt', mqtt_preset: 'emqx',
    mqtt_host: '', mqtt_port: '1883', mqtt_user: '', mqtt_pass: '',
    mqtt_host_name: '', mqtt_target_mac: '',
    server: '', never_sleep: false,
  } : {
    ssid: '', pass: '',
    data_mode: 'mqtt', net_mode: 'wifi',
    server: '',
    mqtt_preset: 'auto', mqtt_host: '', mqtt_port: '1883', mqtt_user: '', mqtt_pass: '',
    mqtt_device_name: '', lc_target_mac: '',
    scan_mode: 'mac', target_uuid: '',
    scan_duration: '3', scan_interval: '5',
    cloud_ctrl: true,
  })
  const [loading,    setLoading]    = useState(true)
  const [saving,     setSaving]     = useState(false)
  const [result,     setResult]     = useState(null)
  const [errMsg,     setErrMsg]     = useState('')
  const [wifiList,   setWifiList]   = useState([])
  const [scanning,   setScanning]   = useState(false)
  const [scanMsg,    setScanMsg]    = useState('')
  // isO5c: non-C1 device that uses MQTT (O5c has both MQTT and scan params)
  const [isO5c,      setIsO5c]      = useState(!isC1)
  const [showConfirm, setShowConfirm] = useState(false)
  const [bleList,     setBleList]     = useState([])
  const [bleScanning, setBleScanning] = useState(false)
  const [bleScanMsg,  setBleScanMsg]  = useState('')

  // OTA state: false=collapsed, true=expanded, 'iframe'=fullscreen
  const [otaOpen, setOtaOpen] = useState(false)

  const scanWifi = async () => {
    setScanning(true)
    setScanMsg('扫描中...')
    try {
      const r = await fetch(`http://${IP}/scan`, { cache: 'no-store' })
      const d = await r.json()
      const nets = d.networks || []
      setWifiList(nets)
      setScanMsg(nets.length > 0 ? `找到 ${nets.length} 个网络` : '未找到网络')
    } catch {
      setScanMsg('扫描失败')
    } finally {
      setScanning(false)
      setTimeout(() => setScanMsg(''), 3000)
    }
  }

  // Pre-fill with current device config
  useEffect(() => {
    fetch(`http://${IP}/api/config.json`, { cache: 'no-store' })
      .then(r => r.json())
      .then(d => {
        setForm(f => ({ ...f, ...d, data_mode: 'mqtt', ...(isC1 ? {} : { mqtt_preset: 'auto' }) }))
        if (!isC1) setIsO5c(true)
      })
      .catch(() => {})
      .finally(() => setLoading(false))
  }, [])

  const set = (k, v) => setForm(f => ({ ...f, [k]: v }))

  const scanBle = async () => {
    setBleScanning(true)
    setBleScanMsg('正在扫描附近的蓝牙设备（约10秒）...')
    setBleList([])
    try {
      const r = await fetch(`http://${IP}/blescan`, { cache: 'no-store' })
      const d = await r.json()
      const list = d.devices || []
      setBleList(list)
      setBleScanMsg(list.length > 0 ? `发现 ${list.length} 个设备，点击选择` : '未发现蓝牙设备')
    } catch {
      setBleScanMsg('扫描失败')
    } finally {
      setBleScanning(false)
    }
  }

  const onBleSelect = (d) => {
    const mode = form.scan_mode ?? 'mac'
    if (mode === 'uuid') {
      const uuid = d.uuid || (d.allUuids ? d.allUuids.split(',')[0].trim() : '')
      if (uuid) { set('target_uuid', uuid); setBleScanMsg('已选择 UUID: ' + uuid) }
      else setBleScanMsg('该设备未携带可识别的 UUID')
    } else {
      set('lc_target_mac', d.mac)
      setBleScanMsg('已选择: ' + d.mac + (d.name ? ` (${d.name})` : ''))
    }
  }

  const handleSubmit = (e) => {
    e.preventDefault()
    setShowConfirm(true)
  }

  const doSubmit = async () => {
    setShowConfirm(false)
    setSaving(true)
    setResult(null)
    try {
      const params = new URLSearchParams()
      Object.entries(form).forEach(([k, v]) => {
        if (k === 'never_sleep' || k === 'cloud_ctrl') { if (v) params.append(k, '1') }
        else params.append(k, String(v))
      })
      const res = await fetch(`http://${IP}/save`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: params.toString(),
      })
      if (res.ok) { setResult('ok') }
      else { setResult('err'); setErrMsg(`HTTP ${res.status}`) }
    } catch (err) {
      setResult('err'); setErrMsg(err.message)
    } finally {
      setSaving(false)
    }
  }

  if (result === 'ok') return (
    <div style={embedded ? { background: C.bg, display: 'flex', alignItems: 'center', justifyContent: 'center', padding: '40px 16px', minHeight: 300 } : { ...s.wrap, alignItems: 'center', justifyContent: 'center' }}>
      <div style={{ background: C.white, borderRadius: 14, padding: '32px 24px', maxWidth: 340, textAlign: 'center', margin: 16 }}>
        <div style={{ fontSize: 40, marginBottom: 12 }}>✅</div>
        <div style={{ fontSize: 17, fontWeight: 700, color: C.ok, marginBottom: 8 }}>配置已保存</div>
        <div style={{ fontSize: 13, color: C.muted, marginBottom: 20 }}>设备将在 5 秒后自动重启并连接 WiFi</div>
        <button onClick={onBack} style={{ ...s.btn, marginTop: 0 }}>返回</button>
      </div>
    </div>
  )

  const confirmRows = [
    ['📶 WiFi 网络', form.ssid || '（未填写）'],
    ['🔑 WiFi 密码', form.pass || '（无密码）'],
    isC1  && ['📛 设备名称', form.device_name || '（未填写）'],
    isC1  && form.data_mode === 'mqtt' && ['🔢 订阅序列号', form.mqtt_host_name || '（未填写）'],
    isO5c && ['📛 设备序列号', form.mqtt_device_name || '（未填写）'],
    !isC1 && ['🌐 联网方式', (form.net_mode === 'ml307r' ? '物联网模块' : 'WiFi')],
    !isC1 && (form.scan_mode ?? 'mac') === 'mac' && ['📍 目标 MAC', form.lc_target_mac || '（全部设备）'],
    !isC1 && form.scan_mode === 'uuid' && ['🔷 目标 UUID', form.target_uuid || '（未填写）'],
  ].filter(Boolean)

  return (
    <div style={embedded ? { background: C.bg } : s.wrap}>
      {!embedded && (
        <div style={s.bar}>
          <button onClick={onBack} style={{ background: 'none', border: 'none', color: C.primary, fontSize: 15, fontWeight: 600, cursor: 'pointer', padding: '4px 0' }}>← 返回</button>
          <span style={{ flex: 1, fontSize: 15, fontWeight: 600, color: C.text }}>设备配置</span>
          <span style={{ fontSize: 12, color: C.muted }}>{ssid}</span>
        </div>
      )}

      {loading ? (
        <div style={{ flex: 1, display: 'flex', alignItems: 'center', justifyContent: 'center', color: C.muted, padding: 40 }}>读取配置中...</div>
      ) : (
        <form onSubmit={handleSubmit} style={embedded ? { padding: '14px 14px 40px' } : s.body}>
          {/* ── 基本信息（C1 + O5 共有）── */}
          <div style={s.card}>
            <div style={s.sec}>基本信息</div>
            {isC1 && <>
              <label style={s.label}>设备名称</label>
              <input style={s.input} value={form.device_name || ''} onChange={e => set('device_name', e.target.value)} placeholder="例如：V999999" maxLength={32} required />
            </>}

            {!isC1 && <>
              <label style={s.label}>联网方式</label>
              <select style={s.select} value={form.net_mode || 'wifi'} onChange={e => set('net_mode', e.target.value)}>
                <option value="wifi">WiFi</option>
                <option value="ml307r">物联网模块(ML307R)</option>
              </select>
            </>}

            {(isC1 || form.net_mode !== 'ml307r') && <>
              <label style={s.label}>目标 WiFi <span style={{ color: C.danger, fontSize: 12 }}>*</span></label>
              <div style={{ display: 'flex', gap: 8, alignItems: 'center' }}>
                {wifiList.length > 0
                  ? <select style={{ ...s.select, flex: 1 }} value={form.ssid || ''} onChange={e => set('ssid', e.target.value)} required>
                      {!form.ssid && <option value="">-- 选择网络 --</option>}
                      {wifiList.map(n => (
                        <option key={n.ssid} value={n.ssid}>
                          {n.ssid}（{n.rssi >= -50 ? '极强' : n.rssi >= -60 ? '强' : n.rssi >= -70 ? '中' : '弱'} {n.rssi}dBm）
                        </option>
                      ))}
                    </select>
                  : <input style={{ ...s.input, flex: 1 }} value={form.ssid || ''} onChange={e => set('ssid', e.target.value)} placeholder="输入或扫描选择 WiFi" required />
                }
                <button type="button" onClick={scanWifi} disabled={scanning}
                  style={{ padding: '10px 14px', background: '#f0f0f0', color: '#444', border: 'none', borderRadius: 8, fontSize: 14, cursor: 'pointer', flexShrink: 0, opacity: scanning ? .6 : 1 }}>
                  {scanning ? '...' : '扫描'}
                </button>
              </div>
              {scanMsg && <small style={{ ...s.small, color: scanMsg.includes('失败') ? C.danger : C.ok }}>{scanMsg}</small>}

              <label style={s.label}>WiFi 密码</label>
              <input style={s.input} value={form.pass || ''} onChange={e => set('pass', e.target.value)} placeholder="留空表示无密码" />
            </>}
          </div>

          {/* ── C1：MQTT 配置 ── */}
          {isC1 && (
            <div style={s.card}>
              <div style={s.sec}>数据传输 (MQTT)</div>
              {<>
                <label style={s.label}>Broker 选择</label>
                <select style={s.select} value={form.mqtt_preset} onChange={e => set('mqtt_preset', e.target.value)}>
                  <option value="auto">自动（同时监听全部）</option>
                  <option value="emqx">EMQX 国际</option>
                  <option value="hivemq">HiveMQ 国际</option>
                  <option value="custom">自定义</option>
                </select>
                {form.mqtt_preset === 'custom' && <>
                  <label style={s.label}>Broker Host</label>
                  <input style={s.input} value={form.mqtt_host || ''} onChange={e => set('mqtt_host', e.target.value)} placeholder="broker.hivemq.com" />
                  <label style={s.label}>Broker Port</label>
                  <input style={s.input} type="number" value={form.mqtt_port || ''} onChange={e => set('mqtt_port', e.target.value)} placeholder="1883" />
                  <label style={s.label}>用户名（可选）</label>
                  <input style={s.input} value={form.mqtt_user || ''} onChange={e => set('mqtt_user', e.target.value)} placeholder="MQTT Username" />
                  <label style={s.label}>密码（可选）</label>
                  <input style={s.input} value={form.mqtt_pass || ''} onChange={e => set('mqtt_pass', e.target.value)} placeholder="MQTT Password" />
                </>}
                <label style={s.label}>序列号 <span style={{ color: C.danger, fontSize: 12 }}>*必填</span></label>
                <input style={s.input} value={form.mqtt_host_name || ''} onChange={e => set('mqtt_host_name', e.target.value)} placeholder="如: ASXD" required />
                <small style={s.small}>与主机相同的序列号</small>
                <label style={s.label}>目标 MAC <span style={{ color: C.muted, fontSize: 11 }}>可选</span></label>
                <input style={s.input} value={form.mqtt_target_mac || ''} onChange={e => set('mqtt_target_mac', e.target.value)} placeholder="XX:XX:XX:XX:XX:XX 或留空" />
                <small style={s.small}>留空则订阅主机下所有数据</small>
              </>}
            </div>
          )}

          {/* ── C1：永不休眠 ── */}
          {isC1 && (
            <div style={s.card}>
              <div style={s.cbRow}>
                <input type="checkbox" id="ns" checked={!!form.never_sleep} onChange={e => set('never_sleep', e.target.checked)} style={{ width: 18, height: 18, accentColor: C.primary }} />
                <label htmlFor="ns" style={{ fontSize: 14, color: C.sub, margin: 0 }}>永不休眠（持续运行）</label>
              </div>
            </div>
          )}

          {/* ── O5/O5c：联网与数据 ── */}
          {!isC1 && (
            <div style={s.card}>
              <div style={s.sec}>数据传输 (MQTT)</div>
              {<>
                <label style={s.label}>Broker 选择</label>
                <select style={s.select} value={form.mqtt_preset || 'auto'} onChange={e => set('mqtt_preset', e.target.value)}>
                  <option value="auto">自动（同时监听全部）</option>
                  <option value="emqx">EMQX 国际</option>
                  <option value="hivemq">HiveMQ 国际</option>
                  <option value="custom">自定义</option>
                </select>
                {form.mqtt_preset === 'custom' && <>
                  <label style={s.label}>Broker Host</label>
                  <input style={s.input} value={form.mqtt_host || ''} onChange={e => set('mqtt_host', e.target.value)} placeholder="broker.hivemq.com" />
                  <label style={s.label}>Broker Port</label>
                  <input style={s.input} type="number" value={form.mqtt_port || '1883'} onChange={e => set('mqtt_port', e.target.value)} placeholder="1883" />
                  <label style={s.label}>用户名（可选）</label>
                  <input style={s.input} value={form.mqtt_user || ''} onChange={e => set('mqtt_user', e.target.value)} placeholder="MQTT Username" />
                  <label style={s.label}>密码（可选）</label>
                  <input style={s.input} value={form.mqtt_pass || ''} onChange={e => set('mqtt_pass', e.target.value)} placeholder="MQTT Password" />
                </>}
                <label style={s.label}>设备序列号 <span style={{ color: C.danger, fontSize: 12 }}>*</span></label>
                <input style={s.input} value={form.mqtt_device_name || ''} onChange={e => set('mqtt_device_name', e.target.value)} placeholder="如: ASXD" required />
                <small style={s.small}>MQTT topic 前缀（主机序列号）</small>
              </>}

              <div style={s.cbRow}>
                <input type="checkbox" id="cc" checked={!!form.cloud_ctrl} onChange={e => set('cloud_ctrl', e.target.checked)} style={{ width: 18, height: 18, accentColor: C.primary }} />
                <label htmlFor="cc" style={{ fontSize: 14, color: C.sub, margin: 0 }}>启用云控</label>
              </div>
              <small style={s.small}>开启后可通过 MQTT 远程下发指令控制设备</small>
            </div>
          )}

          {/* ── O5：BLE 扫描参数 ── */}
          {!isC1 && (
            <div style={s.card}>
              <div style={s.sec}>BLE 扫描</div>

              <label style={s.label}>扫描目标类型</label>
              <select style={s.select} value={form.scan_mode ?? 'mac'}
                onChange={e => { set('scan_mode', e.target.value); setBleList([]); setBleScanMsg('') }}>
                <option value="mac">按 MAC</option>
                <option value="uuid">按 UUID</option>
              </select>

              {/* MAC 模式 */}
              {(form.scan_mode ?? 'mac') === 'mac' && (
                <>
                  <label style={s.label}>目标 MAC <span style={{ color: C.muted, fontSize: 11 }}>可选</span></label>
                  <div style={{ display: 'flex', gap: 8 }}>
                    <input style={{ ...s.input, flex: 1 }} value={form.lc_target_mac ?? ''}
                      onChange={e => set('lc_target_mac', e.target.value)}
                      placeholder="XX:XX:XX:XX:XX:XX 或留空" />
                    <button type="button" onClick={scanBle} disabled={bleScanning}
                      style={{ padding: '10px 12px', background: '#f0f4ff', color: C.primary, border: `1px solid ${C.primary}`, borderRadius: 8, fontSize: 13, fontWeight: 600, cursor: 'pointer', flexShrink: 0, opacity: bleScanning ? .6 : 1 }}>
                      {bleScanning ? '扫描中...' : '🔍 搜索'}
                    </button>
                  </div>
                  <small style={s.small}>留空则上报所有扫描到的设备</small>
                </>
              )}

              {/* UUID 模式 */}
              {(form.scan_mode ?? 'mac') === 'uuid' && (
                <>
                  <label style={s.label}>目标 UUID 片段 <span style={{ color: C.danger, fontSize: 12 }}>*</span></label>
                  <div style={{ display: 'flex', gap: 8 }}>
                    <input style={{ ...s.input, flex: 1 }} value={form.target_uuid ?? ''}
                      onChange={e => set('target_uuid', e.target.value)}
                      placeholder="如: 180D 或 FFE0，可空格/逗号分隔" />
                    <button type="button" onClick={scanBle} disabled={bleScanning}
                      style={{ padding: '10px 12px', background: '#f0f4ff', color: C.primary, border: `1px solid ${C.primary}`, borderRadius: 8, fontSize: 13, fontWeight: 600, cursor: 'pointer', flexShrink: 0, opacity: bleScanning ? .6 : 1 }}>
                      {bleScanning ? '扫描中...' : '🔍 搜索'}
                    </button>
                  </div>
                  <small style={s.small}>BLE 广播包中的 UUID 片段</small>
                </>
              )}

              {/* BLE 扫描状态 */}
              {bleScanMsg && (
                <small style={{ ...s.small, color: bleScanMsg.startsWith('已选择') || bleScanMsg.startsWith('发现') ? C.ok : bleScanMsg.includes('失败') || bleScanMsg.includes('未发现') || bleScanMsg.includes('未携带') ? C.danger : C.muted }}>
                  {bleScanMsg}
                </small>
              )}

              {/* BLE 扫描结果列表 */}
              {bleList.length > 0 && (
                <div style={{ marginTop: 8, maxHeight: 240, overflowY: 'auto', border: `1px solid ${C.border}`, borderRadius: 10, padding: 6, background: '#fafafa' }}>
                  {bleList.map((d, i) => (
                    <div key={i} onClick={() => onBleSelect(d)}
                      style={{ padding: '10px 10px', cursor: 'pointer', borderRadius: 8, marginBottom: 4, background: C.white, border: `1px solid ${C.border}` }}>
                      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 3 }}>
                        <span style={{ fontSize: 13, fontWeight: 600, color: d.name ? C.text : C.muted, fontStyle: d.name ? 'normal' : 'italic' }}>
                          {d.name || '未知设备'}
                        </span>
                        <span style={{ fontSize: 12, color: C.muted }}>{d.rssi} dBm</span>
                      </div>
                      <div style={{ fontSize: 12, color: '#555', fontFamily: 'monospace', marginBottom: 2 }}>{d.mac}</div>
                      {(d.allUuids || d.uuid) && (
                        <div style={{ fontSize: 11, color: '#2196F3', wordBreak: 'break-all' }}>
                          UUID: {d.allUuids || d.uuid}
                        </div>
                      )}
                    </div>
                  ))}
                </div>
              )}

              <label style={{ ...s.label, marginTop: 14 }}>扫描持续时间（秒）</label>
              <input style={s.input} type="number" value={form.scan_duration ?? '3'} onChange={e => set('scan_duration', e.target.value)} min="1" max="60" />
              <label style={s.label}>扫描间隔（秒）</label>
              <input style={s.input} type="number" value={form.scan_interval ?? '5'} onChange={e => set('scan_interval', e.target.value)} min="1" max="3600" />
            </div>
          )}

          {result === 'err' && <div style={{ color: C.danger, fontSize: 13, textAlign: 'center', marginBottom: 8 }}>发送失败：{errMsg}</div>}
          <button type="submit" style={{ ...s.btn, opacity: saving ? .6 : 1 }} disabled={saving}>
            {saving ? '发送中...' : '保存配置'}
          </button>

          {/* ── OTA 固件升级 ── */}
          <div style={{ ...s.card, marginTop: 12 }}>
            <div style={{ ...s.sec, cursor: 'pointer', display: 'flex', justifyContent: 'space-between', marginBottom: 0 }}
              onClick={() => setOtaOpen(v => v === true ? false : v === false ? true : v)}>
              <span>固件升级 (OTA)</span><span>{otaOpen === true ? '▲' : '▼'}</span>
            </div>
            {otaOpen === true && <div style={{ paddingTop: 10 }}>
              <div style={{ fontSize: 12, color: C.muted, marginBottom: 10, lineHeight: 1.6 }}>
                点击下方按钮打开设备内置升级页面，在页面中选择 <b>.bin</b> 或 <b>.bin.enc</b> 固件文件上传。<br />升级期间请勿断电。
              </div>
              <button type="button" onClick={() => setOtaOpen('iframe')}
                style={{ width: '100%', padding: '12px 0', background: C.primary, color: '#fff', border: 'none', borderRadius: 10, fontSize: 15, fontWeight: 700, cursor: 'pointer' }}>
                打开固件升级页面
              </button>
            </div>}
          </div>
          {otaOpen === 'iframe' && (
            <div style={{ position: 'fixed', top: 0, left: 0, right: 0, bottom: 0, background: '#fff', zIndex: 9999, display: 'flex', flexDirection: 'column' }}>
              <div style={{ padding: '10px 14px', display: 'flex', alignItems: 'center', gap: 10, borderBottom: '1px solid #e8e8e8', flexShrink: 0 }}>
                <button type="button" onClick={() => setOtaOpen(true)}
                  style={{ background: 'none', border: 'none', color: C.primary, fontSize: 15, fontWeight: 600, cursor: 'pointer' }}>← 返回</button>
                <span style={{ flex: 1, fontSize: 14, fontWeight: 600, color: C.text }}>固件升级</span>
              </div>
              <iframe src={`http://${IP}/update`}
                style={{ flex: 1, border: 'none', width: '100%' }}
                allow="microphone; camera" />
            </div>
          )}
        </form>
      )}

      {/* ── 确认弹窗 ── */}
      {showConfirm && (
        <div style={{ position: 'fixed', top: 0, right: 0, bottom: 0, left: 0, background: 'rgba(0,0,0,.45)', display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 999, padding: '0 16px' }}
          onClick={() => setShowConfirm(false)}>
          <div style={{ background: C.white, borderRadius: 18, padding: '24px 20px 28px', width: '100%', maxWidth: 440 }}
            onClick={e => e.stopPropagation()}>
            <div style={{ width: 38, height: 4, background: '#ddd', borderRadius: 2, margin: '0 auto 18px' }} />
            <div style={{ fontSize: 16, fontWeight: 700, color: C.text, marginBottom: 4 }}>确认配置信息</div>
            <div style={{ fontSize: 12, color: C.muted, marginBottom: 16 }}>请核对以下信息，确认无误后保存</div>
            {confirmRows.map(([label, value], i) => (
              <div key={i} style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '11px 0', borderBottom: i < confirmRows.length - 1 ? `1px solid ${C.border}` : 'none' }}>
                <span style={{ fontSize: 14, color: C.sub }}>{label}</span>
                <span style={{ fontSize: 14, fontWeight: 600, color: C.text, maxWidth: '58%', textAlign: 'right', wordBreak: 'break-all', fontFamily: 'monospace' }}>{value}</span>
              </div>
            ))}
            <div style={{ display: 'flex', gap: 10, marginTop: 22 }}>
              <button onClick={() => setShowConfirm(false)}
                style={{ flex: 1, padding: '13px 0', background: '#f0f0f0', color: C.text, border: 'none', borderRadius: 10, fontSize: 15, fontWeight: 600, cursor: 'pointer' }}>
                取消
              </button>
              <button onClick={doSubmit} disabled={saving}
                style={{ flex: 2, padding: '13px 0', background: C.primary, color: '#fff', border: 'none', borderRadius: 10, fontSize: 15, fontWeight: 600, cursor: 'pointer', opacity: saving ? .6 : 1 }}>
                确认保存
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  )
}

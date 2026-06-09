import { useState, useEffect, useRef, useCallback } from 'react'
import mqtt from 'mqtt'

// ── 颜色/样式常量 ──────────────────────────────────────────────────────────────
const C = { bg: '#f0f2f5', white: '#fff', primary: '#4a90e2', green: '#27ae60', red: '#c0392b', orange: '#e67e22', border: '#f0f0f0', sub: '#888', muted: '#bbb', text: '#222' }
const card = { background: C.white, borderRadius: 12, boxShadow: '0 1px 6px rgba(0,0,0,.07)', marginBottom: 12, overflow: 'hidden' }

// ── Broker 预设 ──────────────────────────────────────────────────────────────
const BROKERS = {
  emqx:    { label: 'EMQX 国际',  wss: 'wss://broker.emqx.io:8084/mqtt' },
  hivemq:  { label: 'HiveMQ 国际', wss: 'wss://broker.hivemq.com:8884/mqtt' },
}

const ONLINE_THRESHOLD = 5 * 60 * 1000 // 5分钟

// ── MQTT Hook ──────────────────────────────────────────────────────────────────
function useMqtt() {
  const clientsRef = useRef({}) // key -> { client, connected }
  const [status, setStatus] = useState({}) // key -> boolean
  const [deviceData, setDeviceData] = useState({}) // hostName -> { targets, lastSeen, heartbeat, broker, cmdAcks, configStatus }
  const subscribedRef = useRef(new Set())

  useEffect(() => {
    const clients = {}
    for (const [key, cfg] of Object.entries(BROKERS)) {
      const client = mqtt.connect(cfg.wss, {
        clientId: 'app_' + key + '_' + Math.random().toString(36).substring(2, 8),
        clean: true,
        connectTimeout: 8000,
        reconnectPeriod: 5000,
      })
      clients[key] = { client, connected: false }

      client.on('connect', () => {
        clients[key].connected = true
        setStatus(s => ({ ...s, [key]: true }))
        // 重新订阅
        for (const h of subscribedRef.current) {
          client.subscribe(h + '/#', { qos: 1 })
        }
      })
      client.on('offline', () => {
        clients[key].connected = false
        setStatus(s => ({ ...s, [key]: false }))
      })
      client.on('error', () => {
        clients[key].connected = false
        setStatus(s => ({ ...s, [key]: false }))
      })
      client.on('message', (topic, message) => {
        handleMsg(key, topic, message)
      })
    }
    clientsRef.current = clients
    return () => {
      for (const b of Object.values(clients)) {
        try { b.client.end(true) } catch (_) {}
      }
    }
  }, [])

  const handleMsg = useCallback((brokerKey, topic, message) => {
    const idx = topic.indexOf('/')
    if (idx < 0) return
    const hostName = topic.substring(0, idx)
    const topicSuffix = topic.substring(idx + 1)
    const msgStr = message.toString()

    setDeviceData(prev => {
      const copy = { ...prev }
      if (!copy[hostName]) copy[hostName] = { targets: {}, lastSeen: 0, heartbeat: null, broker: brokerKey, cmdAcks: [], configStatus: null }
      const host = { ...copy[hostName] }
      host.lastSeen = Date.now()
      host.broker = brokerKey

      // cmd/ack
      if (topicSuffix === 'cmd/ack') {
        try {
          const ack = JSON.parse(msgStr)
          host.cmdAcks = [{ ...ack, receivedAt: Date.now() }, ...(host.cmdAcks || [])].slice(0, 20)
        } catch (_) {}
        copy[hostName] = host
        return copy
      }

      // config/status
      if (topicSuffix === 'config/status') {
        try {
          const cs = JSON.parse(msgStr)
          host.configStatus = { scanMode: cs.scanMode || '', targets: cs.targets || '', deviceId: cs.deviceId || '', receivedAt: Date.now() }
        } catch (_) {}
        copy[hostName] = host
        return copy
      }

      // heartbeat
      let parsed = null
      try { parsed = JSON.parse(msgStr) } catch (_) {}
      if (topicSuffix === 'heartbeat' || (parsed && parsed.type === 'heartbeat')) {
        const hb = parsed || {}
        host.heartbeat = {
          deviceId: hb.deviceId || '',
          powerMode: hb.powerMode || 'unknown',
          batteryLevel: hb.batteryLevel !== undefined ? hb.batteryLevel : null,
          uptime: hb.uptime || 0,
          receivedAt: Date.now(),
          timeSlots: Array.isArray(hb.timeSlots) ? hb.timeSlots : [],
        }
        copy[hostName] = host
        return copy
      }

      // 普通扫描数据
      let mac = topicSuffix, rawData = msgStr, rssi = 0, uuid = '', deviceId = '', ts = Date.now()
      if (parsed) {
        if (parsed.mac) mac = parsed.mac
        if (parsed.advData) rawData = parsed.advData
        if (parsed.rssi) rssi = parsed.rssi
        if (parsed.uuid) uuid = parsed.uuid
        if (parsed.deviceId) deviceId = parsed.deviceId
        if (parsed.ts && parsed.ts > 1577836800) ts = parsed.ts * 1000
      }
      host.targets = { ...host.targets, [topicSuffix]: { mac, rawData, rssi, uuid, deviceId, updateTime: ts } }
      copy[hostName] = host
      return copy
    })
  }, [])

  const subscribe = useCallback((hostName) => {
    subscribedRef.current.add(hostName)
    for (const b of Object.values(clientsRef.current)) {
      if (b.connected) b.client.subscribe(hostName + '/#', { qos: 1 })
    }
  }, [])

  const publish = useCallback((topic, payload, opts = {}) => {
    let sent = false
    for (const b of Object.values(clientsRef.current)) {
      if (b.connected) {
        b.client.publish(topic, typeof payload === 'string' ? payload : JSON.stringify(payload), { qos: 1, ...opts })
        sent = true
      }
    }
    return sent
  }, [])

  const publishTo = useCallback((brokerKey, topic, payload, opts = {}) => {
    const b = clientsRef.current[brokerKey]
    if (b && b.connected) {
      b.client.publish(topic, typeof payload === 'string' ? payload : JSON.stringify(payload), { qos: 1, ...opts })
      return true
    }
    return false
  }, [])

  return { status, deviceData, subscribe, publish, publishTo }
}

// ── 查询子页 ──────────────────────────────────────────────────────────────────
function QueryTab({ mqttCtx }) {
  const { status, deviceData, subscribe } = mqttCtx
  const [hostInput, setHostInput] = useState('')
  const [activeHost, setActiveHost] = useState(null)

  const doQuery = () => {
    const h = hostInput.trim().toUpperCase()
    if (!h) return
    subscribe(h)
    setActiveHost(h)
  }

  const host = activeHost ? deviceData[activeHost] : null
  const online = host && (Date.now() - host.lastSeen) < ONLINE_THRESHOLD
  const targets = host ? Object.values(host.targets) : []

  return (
    <div style={{ padding: '16px 16px 80px' }}>
      <div style={{ ...card, padding: 16 }}>
        <div style={{ fontSize: 13, color: C.sub, marginBottom: 8 }}>输入设备主机名称</div>
        <div style={{ display: 'flex', gap: 8 }}>
          <input value={hostInput} onChange={e => setHostInput(e.target.value)} onKeyDown={e => e.key === 'Enter' && doQuery()}
            placeholder="例如 MFAC" style={{ flex: 1, padding: '10px 12px', border: '1px solid #ddd', borderRadius: 8, fontSize: 14, outline: 'none', fontFamily: 'monospace', textTransform: 'uppercase' }} />
          <button onClick={doQuery} style={{ padding: '10px 20px', background: C.primary, color: '#fff', border: 'none', borderRadius: 8, fontSize: 14, fontWeight: 600, cursor: 'pointer' }}>查询</button>
        </div>
      </div>

      {/* Broker 状态 */}
      <div style={{ ...card, padding: 12 }}>
        <div style={{ fontSize: 11, color: '#999', fontWeight: 600, marginBottom: 8 }}>Broker 连接状态</div>
        {Object.entries(BROKERS).map(([k, cfg]) => (
          <div key={k} style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '4px 0', fontSize: 12 }}>
            <span style={{ width: 8, height: 8, borderRadius: '50%', background: status[k] ? C.green : C.red, flexShrink: 0 }} />
            <span style={{ fontWeight: 600, color: C.text }}>{cfg.label}</span>
            <span style={{ color: C.muted, fontSize: 11 }}>{status[k] ? '已连接' : '未连接'}</span>
          </div>
        ))}
      </div>

      {activeHost && (
        <>
          <div style={{ ...card, padding: 16 }}>
            <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 12 }}>
              <span style={{ fontSize: 18, fontWeight: 700, color: C.text, fontFamily: 'monospace' }}>{activeHost}</span>
              <span style={{ padding: '4px 12px', borderRadius: 20, fontSize: 12, fontWeight: 600, background: online ? '#e8f8f0' : '#fde8e8', color: online ? C.green : C.red }}>
                {host ? (online ? '在线' : '离线') : '等待数据'}
              </span>
            </div>

            {/* 心跳 */}
            {host?.heartbeat && (
              <div style={{ background: '#f0f4ff', borderRadius: 10, padding: 12, marginBottom: 12, borderLeft: '3px solid #667eea' }}>
                <div style={{ fontSize: 13, fontWeight: 700, color: '#667eea', marginBottom: 6 }}>设备状态</div>
                <Row label="电量" value={host.heartbeat.batteryLevel !== null ? host.heartbeat.batteryLevel + '%' : '-'} />
                <Row label="功率模式" value={host.heartbeat.powerMode} />
                <Row label="最后心跳" value={timeAgo(host.heartbeat.receivedAt)} />
                {host.heartbeat.timeSlots?.length > 0 && (
                  <Row label="时间段" value={host.heartbeat.timeSlots.map((s, i) =>
                    `${String(s.wakeH ?? '-').padStart(2, '0')}:${String(s.wakeM ?? '-').padStart(2, '0')}~${String(s.sleepH ?? '-').padStart(2, '0')}:${String(s.sleepM ?? '-').padStart(2, '0')}${s.enabled === false ? '(禁用)' : ''}`
                  ).join(', ')} />
                )}
              </div>
            )}

            {/* configStatus */}
            {host?.configStatus && (
              <div style={{ background: '#f0f7ff', borderRadius: 10, padding: 12, marginBottom: 12, fontSize: 12 }}>
                <span style={{ fontWeight: 700, color: C.text }}>实际配置: </span>
                <span style={{ background: '#fff', padding: '2px 8px', borderRadius: 6, fontWeight: 600, color: '#2980b9', marginRight: 6 }}>{host.configStatus.scanMode === 'uuid' ? 'UUID' : 'MAC'}</span>
                <span style={{ fontFamily: 'monospace', wordBreak: 'break-all' }}>{host.configStatus.targets || '无'}</span>
              </div>
            )}

            {targets.length === 0 && (
              <div style={{ textAlign: 'center', color: C.muted, padding: 20, fontSize: 13 }}>暂无扫描数据，已开始监听…</div>
            )}

            {targets.sort((a, b) => (b.updateTime || 0) - (a.updateTime || 0)).map((t, i) => (
              <div key={i} style={{ background: '#f8f9fa', borderRadius: 10, padding: 12, marginBottom: 8 }}>
                <div style={{ fontWeight: 700, fontFamily: 'monospace', fontSize: 14, color: C.text, marginBottom: 6 }}>{t.mac}</div>
                {t.rssi !== 0 && <Row label="RSSI" value={t.rssi + ' dBm'} />}
                {t.uuid && <Row label="UUID" value={t.uuid} />}
                <Row label="Raw" value={t.rawData} mono />
                <Row label="更新" value={new Date(t.updateTime).toLocaleString('zh-CN')} />
              </div>
            ))}
          </div>
          <div style={{ textAlign: 'center', fontSize: 11, color: C.muted }}>数据实时更新</div>
        </>
      )}
    </div>
  )
}

function Row({ label, value, mono }) {
  return (
    <div style={{ display: 'flex', padding: '3px 0', fontSize: 12 }}>
      <span style={{ width: 70, flexShrink: 0, color: C.sub, fontWeight: 600 }}>{label}</span>
      <span style={{ flex: 1, color: C.text, wordBreak: 'break-all', fontFamily: mono ? 'monospace' : 'inherit', fontSize: mono ? 11 : 12, background: mono ? '#f0f0f0' : 'none', padding: mono ? '2px 6px' : 0, borderRadius: 4, lineHeight: 1.5 }}>{value}</span>
    </div>
  )
}

function timeAgo(ts) {
  if (!ts) return '-'
  const diff = Date.now() - ts
  if (diff < 60000) return Math.round(diff / 1000) + ' 秒前'
  if (diff < 3600000) return Math.round(diff / 60000) + ' 分钟前'
  return new Date(ts).toLocaleString('zh-CN')
}

// ── 云控子页 ──────────────────────────────────────────────────────────────────
function ControlTab({ mqttCtx }) {
  const { deviceData, subscribe, publish } = mqttCtx
  const [hostInput, setHostInput] = useState('')
  const [activeHost, setActiveHost] = useState(null)
  const [scanMode, setScanMode] = useState('mac')
  const [targets, setTargets] = useState('')
  const [msg, setMsg] = useState(null) // { text, type }
  const [sending, setSending] = useState(false)

  const doEnter = () => {
    const h = hostInput.trim().toUpperCase()
    if (!h) return
    subscribe(h)
    setActiveHost(h)
  }

  const host = activeHost ? deviceData[activeHost] : null
  const online = host && (Date.now() - host.lastSeen) < ONLINE_THRESHOLD
  // 获取 deviceId（作为 token）
  const devId = host?.heartbeat?.deviceId || (() => {
    for (const t of Object.values(host?.targets || {})) { if (t.deviceId) return t.deviceId }
    return ''
  })()

  const doSend = () => {
    if (!activeHost) return
    if (!targets.trim()) { setMsg({ text: '请输入目标列表', type: 'err' }); return }
    if (!devId) { setMsg({ text: '设备尚未上线（无 deviceId），无法下发', type: 'err' }); return }

    setSending(true)
    const cmdPayload = {
      action: 'setScanConfig',
      scanMode,
      targets: targets.trim(),
      token: devId,
      ts: Math.floor(Date.now() / 1000),
    }
    const topic = activeHost + '/cmd'
    const sent = publish(topic, cmdPayload, { retain: true })
    setSending(false)
    if (sent) {
      setMsg({ text: '指令已下发，等待设备确认', type: 'ok' })
      setTimeout(() => setMsg(null), 4000)
    } else {
      setMsg({ text: '所有 Broker 均未连接', type: 'err' })
    }
  }

  return (
    <div style={{ padding: '16px 16px 80px' }}>
      <div style={{ ...card, padding: 16 }}>
        <div style={{ fontSize: 13, color: C.sub, marginBottom: 8 }}>输入设备主机名称</div>
        <div style={{ display: 'flex', gap: 8 }}>
          <input value={hostInput} onChange={e => setHostInput(e.target.value)} onKeyDown={e => e.key === 'Enter' && doEnter()}
            placeholder="例如 MFAC" style={{ flex: 1, padding: '10px 12px', border: '1px solid #ddd', borderRadius: 8, fontSize: 14, outline: 'none', fontFamily: 'monospace', textTransform: 'uppercase' }} />
          <button onClick={doEnter} style={{ padding: '10px 20px', background: C.orange, color: '#fff', border: 'none', borderRadius: 8, fontSize: 14, fontWeight: 600, cursor: 'pointer' }}>进入</button>
        </div>
      </div>

      {activeHost && (
        <div style={{ ...card, padding: 16 }}>
          <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 14 }}>
            <span style={{ fontSize: 18, fontWeight: 700, color: C.text, fontFamily: 'monospace' }}>{activeHost}</span>
            <span style={{ padding: '4px 12px', borderRadius: 20, fontSize: 12, fontWeight: 600, background: online ? '#e8f8f0' : '#fde8e8', color: online ? C.green : C.red }}>
              {online ? '在线' : '离线'}
            </span>
          </div>

          {/* 设备实际配置 */}
          {host?.configStatus && (
            <div style={{ background: '#f0f7ff', borderRadius: 10, padding: 12, marginBottom: 14, fontSize: 12 }}>
              <div style={{ fontWeight: 700, color: C.text, marginBottom: 4 }}>设备实际配置</div>
              <span style={{ background: '#fff', padding: '2px 8px', borderRadius: 6, fontWeight: 600, color: '#2980b9', marginRight: 6 }}>{host.configStatus.scanMode === 'uuid' ? 'UUID' : 'MAC'}</span>
              <span style={{ fontFamily: 'monospace', wordBreak: 'break-all' }}>{host.configStatus.targets || '无'}</span>
            </div>
          )}

          {!devId && (
            <div style={{ textAlign: 'center', color: C.red, padding: 20, fontSize: 13 }}>设备尚未上线（无数据记录），请等待设备发送数据后再使用云控</div>
          )}

          {devId && (
            <>
              <div style={{ marginBottom: 14 }}>
                <div style={{ fontSize: 13, fontWeight: 600, color: '#555', marginBottom: 6 }}>扫描模式</div>
                <div style={{ display: 'flex', gap: 10 }}>
                  {['mac', 'uuid'].map(m => (
                    <button key={m} onClick={() => setScanMode(m)}
                      style={{ flex: 1, padding: '10px 0', borderRadius: 10, border: scanMode === m ? '2px solid ' + C.orange : '2px solid transparent', background: scanMode === m ? '#fef5ec' : '#f8f9fa', color: scanMode === m ? C.orange : C.sub, fontWeight: 600, fontSize: 14, cursor: 'pointer' }}>
                      {m === 'mac' ? 'MAC 地址' : 'UUID'}
                    </button>
                  ))}
                </div>
              </div>

              <div style={{ marginBottom: 14 }}>
                <div style={{ fontSize: 13, fontWeight: 600, color: '#555', marginBottom: 6 }}>目标{scanMode === 'mac' ? ' MAC 地址' : ' UUID'}</div>
                <textarea value={targets} onChange={e => setTargets(e.target.value)}
                  placeholder={scanMode === 'mac' ? '多个用逗号分隔，如 F8:A7:63:9F:F4:8E,AA:BB:CC:DD:EE:FF' : '多个用逗号分隔，如 FFF0,181A'}
                  style={{ width: '100%', padding: '10px 12px', border: '1px solid #ddd', borderRadius: 8, fontSize: 13, outline: 'none', fontFamily: 'monospace', minHeight: 70, resize: 'vertical' }} />
              </div>

              <button onClick={doSend} disabled={sending}
                style={{ width: '100%', padding: '12px 0', background: C.orange, color: '#fff', border: 'none', borderRadius: 10, fontSize: 15, fontWeight: 600, cursor: 'pointer', opacity: sending ? 0.6 : 1 }}>
                {sending ? '下发中...' : '下发配置'}
              </button>

              {msg && (
                <div style={{ marginTop: 10, padding: 10, borderRadius: 8, textAlign: 'center', fontSize: 13, fontWeight: 600, background: msg.type === 'ok' ? '#e8f8f0' : '#fde8e8', color: msg.type === 'ok' ? C.green : C.red }}>{msg.text}</div>
              )}
            </>
          )}

          {/* ACK 日志 */}
          {host?.cmdAcks?.length > 0 && (
            <div style={{ marginTop: 16 }}>
              <div style={{ fontSize: 13, fontWeight: 600, color: C.sub, marginBottom: 8 }}>指令确认日志</div>
              {host.cmdAcks.slice(0, 10).map((a, i) => (
                <div key={i} style={{ background: '#f8f9fa', borderRadius: 8, padding: '8px 12px', marginBottom: 6, fontSize: 12, borderLeft: '3px solid ' + C.green }}>
                  <span style={{ color: C.muted, fontSize: 11 }}>{new Date(a.receivedAt).toLocaleString('zh-CN')}</span>
                  <span style={{ marginLeft: 8, fontWeight: 600, color: C.green }}>✅ {a.action || '确认'}</span>
                  {a.scanMode && <span style={{ marginLeft: 6 }}>模式={a.scanMode}</span>}
                </div>
              ))}
            </div>
          )}
        </div>
      )}
    </div>
  )
}

// ── 模拟发送子页 ──────────────────────────────────────────────────────────────
function SendTab({ mqttCtx }) {
  const { status, publishTo } = mqttCtx
  const [broker, setBroker] = useState('emqx')
  const [host, setHost] = useState('')
  const [mac, setMac] = useState('')
  const [raw, setRaw] = useState('')
  const [retain, setRetain] = useState(true)
  const [msg, setMsg] = useState(null)
  const [history, setHistory] = useState([])

  const doSend = () => {
    if (!host.trim()) { setMsg({ text: '请输入设备名称', type: 'err' }); return }
    if (!mac.trim()) { setMsg({ text: '请输入 MAC 地址', type: 'err' }); return }
    if (!raw.trim()) { setMsg({ text: '请输入 Raw 数据', type: 'err' }); return }
    if (!status[broker]) { setMsg({ text: 'Broker 未连接', type: 'err' }); return }

    const topicMac = mac.replace(/[:-]/g, '').toUpperCase()
    const topic = host.trim().toUpperCase() + '/' + topicMac
    const payload = JSON.stringify({ mac: mac.trim(), advData: raw.trim(), ts: Math.floor(Date.now() / 1000) })

    const sent = publishTo(broker, topic, payload, { retain })
    if (sent) {
      setMsg({ text: '发送成功! Topic: ' + topic, type: 'ok' })
      setHistory(prev => [{ time: new Date().toLocaleString('zh-CN'), broker: BROKERS[broker].label, topic, payload }, ...prev].slice(0, 20))
      setTimeout(() => setMsg(null), 3000)
    } else {
      setMsg({ text: '发送失败', type: 'err' })
    }
  }

  return (
    <div style={{ padding: '16px 16px 80px' }}>
      <div style={{ ...card, padding: 16 }}>
        <div style={{ fontSize: 15, fontWeight: 700, color: C.text, marginBottom: 12 }}>MQTT 模拟发包</div>

        <div style={{ marginBottom: 12 }}>
          <div style={{ fontSize: 12, color: C.sub, marginBottom: 4 }}>Broker</div>
          <select value={broker} onChange={e => setBroker(e.target.value)}
            style={{ width: '100%', padding: '10px 12px', border: '1px solid #ddd', borderRadius: 8, fontSize: 13, outline: 'none', background: '#fff' }}>
            {Object.entries(BROKERS).map(([k, cfg]) => (
              <option key={k} value={k}>{cfg.label} {status[k] ? '✓' : '✗'}</option>
            ))}
          </select>
        </div>

        <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginBottom: 12, fontSize: 12 }}>
          <span style={{ width: 8, height: 8, borderRadius: '50%', background: status[broker] ? C.green : C.red }} />
          <span style={{ color: status[broker] ? C.green : C.red, fontWeight: 600 }}>{status[broker] ? '已连接' : '未连接'}</span>
        </div>

        <Field label="设备名称（主机名）" value={host} onChange={setHost} placeholder="例如 MFAC" upper />
        <Field label="MAC 地址" value={mac} onChange={setMac} placeholder="例如 F8:A7:63:9F:F4:8E" />
        <div style={{ marginBottom: 12 }}>
          <div style={{ fontSize: 12, color: C.sub, marginBottom: 4 }}>Raw 广播数据（advData）</div>
          <textarea value={raw} onChange={e => setRaw(e.target.value)} placeholder="例如 0201061AFF4C000215..."
            style={{ width: '100%', padding: '10px 12px', border: '1px solid #ddd', borderRadius: 8, fontSize: 12, outline: 'none', fontFamily: 'monospace', minHeight: 60, resize: 'vertical' }} />
        </div>

        <label style={{ display: 'flex', alignItems: 'center', gap: 6, fontSize: 12, color: C.sub, marginBottom: 14, cursor: 'pointer' }}>
          <input type="checkbox" checked={retain} onChange={e => setRetain(e.target.checked)} /> Retain（保留消息）
        </label>

        <button onClick={doSend}
          style={{ width: '100%', padding: '12px 0', background: '#e74c3c', color: '#fff', border: 'none', borderRadius: 10, fontSize: 15, fontWeight: 600, cursor: 'pointer' }}>
          发 送
        </button>

        {msg && (
          <div style={{ marginTop: 10, padding: 10, borderRadius: 8, textAlign: 'center', fontSize: 13, fontWeight: 600, background: msg.type === 'ok' ? '#e8f8f0' : '#fde8e8', color: msg.type === 'ok' ? C.green : C.red }}>{msg.text}</div>
        )}
      </div>

      {history.length > 0 && (
        <div style={{ ...card, padding: 16 }}>
          <div style={{ fontSize: 13, fontWeight: 600, color: C.sub, marginBottom: 8 }}>发送记录</div>
          {history.map((h, i) => (
            <div key={i} style={{ background: '#f8f9fa', borderRadius: 8, padding: '8px 12px', marginBottom: 6, fontSize: 11, fontFamily: 'monospace', wordBreak: 'break-all' }}>
              <div style={{ fontSize: 10, color: C.muted, fontFamily: 'inherit', marginBottom: 2 }}>{h.time} [{h.broker}]</div>
              <div style={{ color: '#667eea', fontWeight: 600 }}>Topic: {h.topic}</div>
              <div>{h.payload}</div>
            </div>
          ))}
        </div>
      )}
    </div>
  )
}

function Field({ label, value, onChange, placeholder, upper }) {
  return (
    <div style={{ marginBottom: 12 }}>
      <div style={{ fontSize: 12, color: C.sub, marginBottom: 4 }}>{label}</div>
      <input value={value} onChange={e => onChange(e.target.value)} placeholder={placeholder}
        style={{ width: '100%', padding: '10px 12px', border: '1px solid #ddd', borderRadius: 8, fontSize: 13, outline: 'none', fontFamily: 'monospace', textTransform: upper ? 'uppercase' : 'none' }} />
    </div>
  )
}

// ── 主组件 ──────────────────────────────────────────────────────────────────────
const subTabs = [
  { id: 'query', label: '查询' },
  { id: 'control', label: '云控' },
  { id: 'send', label: '模拟发送' },
]

export default function MqttPanel() {
  const [subTab, setSubTab] = useState('query')
  const mqttCtx = useMqtt()

  return (
    <div style={{ background: C.bg, minHeight: '100vh', paddingBottom: 60 }}>
      {/* 子 Tab 栏 */}
      <div style={{ display: 'flex', background: '#fff', borderBottom: '1px solid #e8e8e8', position: 'sticky', top: 44, zIndex: 700 }}>
        {subTabs.map(t => (
          <button key={t.id} onClick={() => setSubTab(t.id)}
            style={{ flex: 1, padding: '10px 0', border: 'none', borderBottom: subTab === t.id ? '2px solid ' + C.primary : '2px solid transparent', background: 'none', fontSize: 13, fontWeight: subTab === t.id ? 700 : 500, color: subTab === t.id ? C.primary : '#999', cursor: 'pointer' }}>
            {t.label}
          </button>
        ))}
      </div>

      {subTab === 'query' && <QueryTab mqttCtx={mqttCtx} />}
      {subTab === 'control' && <ControlTab mqttCtx={mqttCtx} />}
      {subTab === 'send' && <SendTab mqttCtx={mqttCtx} />}
    </div>
  )
}

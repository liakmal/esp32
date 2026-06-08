import { useState, useEffect, useRef, useCallback } from 'react'
import { Capacitor } from '@capacitor/core'
import { WifiPlugin } from './wifi-capacitor'

const F = '-apple-system,BlinkMacSystemFont,"Segoe UI",Arial,sans-serif'
const C = {
  bg: '#f0f2f5', white: '#fff', primary: '#4a90e2', green: '#27ae60',
  red: '#e05c65', orange: '#e67e22', text: '#222', sub: '#555',
  muted: '#aaa', border: '#f0f0f0',
}

const SCAN_DURATION = 10 // 单次扫描秒数

// ── UUID 简化（128-bit → 短 UUID） ─────────────────────────
function shortUuid(uuid) {
  if (!uuid) return uuid
  // 标准 BLE base UUID: 0000xxxx-0000-1000-8000-00805f9b34fb
  const m = uuid.match(/^0000([0-9a-f]{4})-0000-1000-8000-00805f9b34fb$/i)
  if (m) return '0x' + m[1].toUpperCase()
  // 已经是短 UUID (4 hex chars)
  if (/^[0-9a-f]{4}$/i.test(uuid)) return '0x' + uuid.toUpperCase()
  // 其他情况保留前8位
  if (uuid.length > 8) return uuid.substring(0, 8).toUpperCase() + '…'
  return uuid.toUpperCase()
}

// ── Raw 数据格式化 ───────────────────────────────────────────
function formatRaw(raw) {
  if (!raw) return raw
  // 去掉空格和已有的 0x 前缀，去掉尾部连续的 00，加 0x 前缀
  let hex = raw.replace(/\s+/g, '').replace(/^0x/i, '').replace(/(00)+$/i, '')
  if (!hex) return '0x00'
  return '0x' + hex.toUpperCase()
}

// ── 信号强度条 ─────────────────────────────────────────────
function RssiBar({ rssi }) {
  const pct = Math.max(0, Math.min(100, (rssi + 100) * 2))
  const bars = pct >= 75 ? 4 : pct >= 50 ? 3 : pct >= 25 ? 2 : 1
  const col = pct >= 60 ? C.green : pct >= 35 ? C.orange : C.red
  return (
    <span style={{ display: 'inline-flex', alignItems: 'flex-end', gap: 2 }}>
      {[1, 2, 3, 4].map(i => (
        <span key={i} style={{ width: 4, height: 4 + i * 3, borderRadius: 1, background: i <= bars ? col : '#ddd' }} />
      ))}
    </span>
  )
}

// ── 时间格式化 ─────────────────────────────────────────────
function timeStr(ts) {
  if (!ts) return ''
  const d = new Date(ts)
  return d.toLocaleTimeString('zh-CN', { hour12: false })
}

// ── 设备详情弹窗 ──────────────────────────────────────────
function DeviceDetail({ device, onClose }) {
  if (!device) return null
  const rows = [
    ['设备名称', device.name || '未知设备'],
    ['MAC 地址', device.mac || device.id || '—'],
    ['RSSI', `${device.rssi} dBm`],
    ['发现时间', timeStr(device.firstSeen)],
    ['最后更新', timeStr(device.lastSeen)],
    ['更新次数', device.updateCount || 1],
  ]
  if (device.txPower != null) rows.push(['Tx Power', `${device.txPower} dBm`])
  if (device.connectable != null) rows.push(['可连接', device.connectable ? '是' : '否'])

  const services = device.serviceUuids || []
  const mfgData = device.manufacturerData || null
  const rawAdv = device.rawAdvertisement || null

  return (
    <div style={{ position: 'fixed', top: 0, left: 0, right: 0, bottom: 0, width: '100%', height: '100%', background: C.bg, zIndex: 9999, display: 'flex', flexDirection: 'column' }}>
      {/* Header with back button */}
      <div style={{ background: C.white, padding: '12px 16px', borderBottom: `1px solid ${C.border}`, display: 'flex', alignItems: 'center', gap: 12, flexShrink: 0 }}>
        <button onClick={onClose} style={{ width: 36, height: 36, borderRadius: 10, border: 'none', background: '#f0f0f0', cursor: 'pointer', fontSize: 18, display: 'flex', alignItems: 'center', justifyContent: 'center', flexShrink: 0 }}>←</button>
        <div style={{ flex: 1, minWidth: 0 }}>
          <div style={{ fontSize: 17, fontWeight: 700, color: C.text, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{device.name || '未知设备'}</div>
          <div style={{ fontSize: 12, fontFamily: 'monospace', color: C.muted, marginTop: 1 }}>{device.mac || device.id || ''}</div>
        </div>
      </div>

      {/* Scrollable content */}
      <div style={{ flex: 1, overflowY: 'auto', padding: '16px 16px 32px' }}>
        <div style={{ background: C.white, borderRadius: 12, overflow: 'hidden', marginBottom: 12, boxShadow: '0 1px 4px rgba(0,0,0,.05)' }}>
          {rows.map(([label, value], i) => (
            <div key={i} style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '11px 14px', borderBottom: i < rows.length - 1 ? `1px solid ${C.border}` : 'none' }}>
              <span style={{ fontSize: 13, color: C.sub }}>{label}</span>
              <span style={{ fontSize: 13, fontWeight: 600, color: C.text, fontFamily: 'monospace', maxWidth: '60%', textAlign: 'right', wordBreak: 'break-all' }}>{value}</span>
            </div>
          ))}
        </div>

        {services.length > 0 && (
          <div style={{ marginBottom: 12 }}>
            <div style={{ fontSize: 12, fontWeight: 700, color: '#888', marginBottom: 6, letterSpacing: '.5px', paddingLeft: 2 }}>SERVICE UUIDS</div>
            <div style={{ background: C.white, borderRadius: 12, padding: '10px 14px', boxShadow: '0 1px 4px rgba(0,0,0,.05)' }}>
              {services.map((uuid, i) => (
                <div key={i} style={{ fontSize: 13, fontFamily: 'monospace', color: C.primary, padding: '4px 0', wordBreak: 'break-all' }}>{shortUuid(uuid)}</div>
              ))}
            </div>
          </div>
        )}

        {mfgData && (
          <div style={{ marginBottom: 12 }}>
            <div style={{ fontSize: 12, fontWeight: 700, color: '#888', marginBottom: 6, letterSpacing: '.5px', paddingLeft: 2 }}>MANUFACTURER DATA</div>
            <div style={{ background: C.white, borderRadius: 12, padding: '10px 14px', fontSize: 12, fontFamily: 'monospace', color: C.sub, wordBreak: 'break-all', lineHeight: 1.7, boxShadow: '0 1px 4px rgba(0,0,0,.05)' }}>{mfgData}</div>
          </div>
        )}

        {rawAdv && (
          <div style={{ marginBottom: 12 }}>
            <div style={{ fontSize: 12, fontWeight: 700, color: '#888', marginBottom: 6, letterSpacing: '.5px', paddingLeft: 2 }}>RAW ADVERTISEMENT</div>
            <div style={{ background: C.white, borderRadius: 12, padding: '10px 14px', fontSize: 11, fontFamily: 'monospace', color: C.sub, wordBreak: 'break-all', lineHeight: 1.7, boxShadow: '0 1px 4px rgba(0,0,0,.05)' }}>{formatRaw(rawAdv)}</div>
          </div>
        )}
      </div>
    </div>
  )
}

// ── 主组件 ────────────────────────────────────────────────
export default function BleScanner() {
  const [scanning, setScanning] = useState(false)
  const [devices, setDevices] = useState({})  // keyed by mac/id
  const [selected, setSelected] = useState(null)

  // Expose a way for parent to close detail via back button
  useEffect(() => {
    window.__bleDetailOpen = !!selected
    window.__bleDetailClose = () => setSelected(null)
    return () => { window.__bleDetailOpen = false }
  }, [selected])
  const [filter, setFilter] = useState('')
  const [sortBy, setSortBy] = useState('rssi') // rssi | name | recent
  const [showNameOnly, setShowNameOnly] = useState(false)
  const [scanCount, setScanCount] = useState(0)
  const [scanTime, setScanTime] = useState(0)
  const [error, setError] = useState('')
  const [shareCode, setShareCode] = useState(null) // 4-digit share code
  const [sharing, setSharing] = useState(false)
  const scanRef = useRef(false)
  const timerRef = useRef(null)
  const devicesRef = useRef({})
  const isNative = Capacitor.isNativePlatform()

  // Keep ref in sync
  useEffect(() => { devicesRef.current = devices }, [devices])

  const startScan = useCallback(async () => {
    setError('')
    setScanning(true)
    scanRef.current = true
    setScanTime(0)
    const startTs = Date.now()
    timerRef.current = setInterval(() => {
      const elapsed = Math.floor((Date.now() - startTs) / 1000)
      setScanTime(elapsed)
      if (elapsed >= SCAN_DURATION) stopScan()
    }, 1000)

    if (isNative) {
      // 主动请求蓝牙权限
      try {
        await WifiPlugin.requestPermissions()
      } catch {}
      try {
        await nativeScan()
      } catch (e) {
        const msg = e.message || '扫描失败'
        if (msg.includes('未开启')) {
          setError('请先开启蓝牙')
        } else if (msg.includes('权限')) {
          setError('请在系统设置中授予蓝牙权限')
        } else {
          setError(msg)
        }
        stopScan()
      }
    } else {
      // Web: use Web Bluetooth API if available, otherwise mock
      if (navigator.bluetooth) {
        try {
          await webBluetoothScan()
        } catch (e) {
          // Web Bluetooth may not support scanning, use mock
          await mockScan()
        }
      } else {
        await mockScan()
      }
    }
  }, [isNative])

  const stopScan = useCallback(() => {
    scanRef.current = false
    setScanning(false)
    if (timerRef.current) { clearInterval(timerRef.current); timerRef.current = null }
    if (isNative) WifiPlugin.stopBleScan().catch(() => {})
  }, [isNative])

  const clearDevices = useCallback(() => {
    setDevices({})
    devicesRef.current = {}
    setScanCount(0)
  }, [])

  // Add/update device in the map
  const addDevice = useCallback((dev) => {
    const key = dev.mac || dev.id || `unknown-${Math.random()}`
    setDevices(prev => {
      const existing = prev[key]
      const now = Date.now()
      return {
        ...prev,
        [key]: {
          ...dev,
          id: key,
          firstSeen: existing?.firstSeen || now,
          lastSeen: now,
          updateCount: (existing?.updateCount || 0) + 1,
          rssiHistory: [...(existing?.rssiHistory || []).slice(-19), dev.rssi],
        }
      }
    })
    setScanCount(c => c + 1)
  }, [])

  // Mock scan for browser testing
  const mockScan = async () => {
    const mockDevices = [
      { name: 'Mi Band 7', mac: 'AA:BB:CC:DD:EE:01', rssi: -45, txPower: -8, connectable: true, serviceUuids: ['FEE0', 'FEE1'] },
      { name: 'AirPods Pro', mac: 'AA:BB:CC:DD:EE:02', rssi: -62, connectable: true, manufacturerData: '4C00 0719 01 0E 20 ...' },
      { name: null, mac: 'AA:BB:CC:DD:EE:03', rssi: -78, connectable: false },
      { name: 'O5-A1B2C3', mac: 'AA:BB:CC:DD:EE:04', rssi: -55, connectable: true, serviceUuids: ['FFE0'] },
      { name: 'C1-D4E5F6', mac: 'AA:BB:CC:DD:EE:05', rssi: -70, connectable: true, serviceUuids: ['FFE0'] },
      { name: 'ThermoSensor', mac: 'AA:BB:CC:DD:EE:06', rssi: -88, txPower: -12, connectable: false, serviceUuids: ['181A', '180F'] },
      { name: 'Heart Rate Band', mac: 'AA:BB:CC:DD:EE:07', rssi: -52, connectable: true, serviceUuids: ['180D'] },
      { name: null, mac: 'AA:BB:CC:DD:EE:08', rssi: -91, connectable: false },
      { name: 'Tile Tracker', mac: 'AA:BB:CC:DD:EE:09', rssi: -67, connectable: true, manufacturerData: 'FEED 0102 03 ...' },
      { name: 'ESP32-BLE', mac: 'AA:BB:CC:DD:EE:0A', rssi: -41, connectable: true, serviceUuids: ['12345678-1234-1234-1234-123456789ABC'] },
    ]
    for (let round = 0; scanRef.current; round++) {
      await new Promise(r => setTimeout(r, 800 + Math.random() * 400))
      if (!scanRef.current) break
      // Randomly add 1-3 devices per round
      const count = 1 + Math.floor(Math.random() * 3)
      for (let i = 0; i < count; i++) {
        const dev = mockDevices[Math.floor(Math.random() * mockDevices.length)]
        addDevice({
          ...dev,
          rssi: dev.rssi + Math.floor(Math.random() * 10 - 5), // jitter
        })
      }
    }
  }

  // Web Bluetooth scan (limited API)
  const webBluetoothScan = async () => {
    // requestLEScan is experimental and not widely supported
    if (!navigator.bluetooth.requestLEScan) throw new Error('unsupported')
    const scan = await navigator.bluetooth.requestLEScan({ acceptAllAdvertisements: true })
    navigator.bluetooth.addEventListener('advertisementreceived', (event) => {
      addDevice({
        name: event.device.name || null,
        mac: event.device.id,
        rssi: event.rssi,
        txPower: event.txPower,
        serviceUuids: event.uuids?.map(u => u.toString()) || [],
      })
    })
    // Stop when scanRef becomes false
    const check = setInterval(() => {
      if (!scanRef.current) { scan.stop(); clearInterval(check) }
    }, 500)
  }

  // Native Capacitor BLE scan via WifiPlugin
  const nativeScan = async () => {
    const res = await WifiPlugin.startBleScan()
    if (!res.ok) throw new Error(res.error || 'BLE scan failed')
    // Poll for results while scanning
    while (scanRef.current) {
      await new Promise(r => setTimeout(r, 1000))
      if (!scanRef.current) break
      try {
        const r = await WifiPlugin.getBleScanResults()
        const list = r.devices || []
        list.forEach(dev => {
          addDevice({
            name: dev.name || null,
            mac: dev.mac,
            rssi: dev.rssi || -80,
            txPower: dev.txPower,
            connectable: dev.connectable,
            serviceUuids: dev.serviceUuids || [],
            manufacturerData: dev.manufacturerData || null,
            rawAdvertisement: dev.rawAdvertisement || null,
          })
        })
      } catch {}
    }
  }

  // Cleanup on unmount
  useEffect(() => () => {
    scanRef.current = false
    if (timerRef.current) clearInterval(timerRef.current)
    if (isNative) WifiPlugin.stopBleScan().catch(() => {})
  }, [])

  // ── Sorted & filtered device list ──
  const deviceList = Object.values(devices)
    .filter(d => {
      if (showNameOnly && !d.name) return false
      if (!filter) return true
      const q = filter.toLowerCase()
      return (d.name && d.name.toLowerCase().includes(q)) ||
             (d.mac && d.mac.toLowerCase().includes(q)) ||
             (d.serviceUuids || []).some(u => u.toLowerCase().includes(q))
    })
    .sort((a, b) => {
      if (sortBy === 'rssi') return b.rssi - a.rssi
      if (sortBy === 'name') return (a.name || 'zzz').localeCompare(b.name || 'zzz')
      if (sortBy === 'recent') return (b.lastSeen || 0) - (a.lastSeen || 0)
      return 0
    })

  return (
    <div style={{ fontFamily: F, background: C.bg, minHeight: '100vh', display: 'flex', flexDirection: 'column', paddingBottom: 60 }}>
      <style>{`
        @keyframes pulse { 0%,100%{opacity:1} 50%{opacity:.4} }
        @keyframes spin { to{transform:rotate(360deg)} }
      `}</style>

      {/* Header */}
      <div style={{ background: C.white, padding: '14px 16px', borderBottom: `1px solid ${C.border}`, boxShadow: '0 1px 4px rgba(0,0,0,.05)' }}>
        <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 10 }}>
          <div>
            <div style={{ fontSize: 18, fontWeight: 700, color: C.text }}>BLE 扫描器</div>
            <div style={{ fontSize: 12, color: C.muted, marginTop: 2 }}>
              {scanning
                ? <span style={{ animation: 'pulse 1.5s ease-in-out infinite' }}>扫描中... {scanTime}s</span>
                : `${deviceList.length} 台设备`}
              {scanCount > 0 && <span style={{ marginLeft: 8, color: C.sub }}>({scanCount} 次广播)</span>}
            </div>
          </div>
          <div style={{ display: 'flex', gap: 8 }}>
            {Object.keys(devices).length > 0 && (
              <button onClick={async () => {
                if (sharing) return
                setSharing(true)
                try {
                  const devList = Object.values(devices).map(d => ({
                    name: d.name || null, mac: d.mac || d.id, rssi: d.rssi,
                    txPower: d.txPower, connectable: d.connectable,
                    serviceUuids: d.serviceUuids || [], manufacturerData: d.manufacturerData || null,
                    rawAdvertisement: d.rawAdvertisement ? formatRaw(d.rawAdvertisement) : null, updateCount: d.updateCount || 1,
                  }))
                  let did = localStorage.getItem('ble_device_id')
                  if (!did) { did = 'xxxx-xxxx-xxxx'.replace(/x/g, () => Math.floor(Math.random()*16).toString(16)); localStorage.setItem('ble_device_id', did) }
                  const server = localStorage.getItem('ble_server') || 'http://ble.qyan10.store'
                  const r = await fetch(server + '/api/ble-share/upload', {
                    method: 'POST', headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ devices: devList, deviceId: did }),
                  })
                  const d = await r.json()
                  if (d.ok) setShareCode(d.code)
                  else setError(d.error || '发送失败')
                } catch (e) { setError('网络错误: ' + (e.message || '')) }
                finally { setSharing(false) }
              }} style={{ padding: '8px 12px', background: '#eaf3ff', color: C.primary, border: 'none', borderRadius: 8, fontSize: 13, fontWeight: 600, cursor: 'pointer', opacity: sharing ? 0.6 : 1 }}>
                {sharing ? '发送中...' : '发送'}
              </button>
            )}
            {Object.keys(devices).length > 0 && (
              <button onClick={clearDevices} style={{ padding: '8px 12px', background: '#fdecea', color: C.red, border: 'none', borderRadius: 8, fontSize: 13, fontWeight: 600, cursor: 'pointer' }}>清空</button>
            )}
            <button onClick={scanning ? stopScan : startScan}
              style={{ padding: '8px 16px', background: scanning ? C.red : C.primary, color: '#fff', border: 'none', borderRadius: 8, fontSize: 13, fontWeight: 600, cursor: 'pointer', minWidth: 72 }}>
              {scanning ? '停止' : '扫描'}
            </button>
          </div>
        </div>

        {/* Filter & Sort */}
        <div style={{ display: 'flex', gap: 8, alignItems: 'center' }}>
          <input
            style={{ flex: 1, padding: '8px 12px', border: '1px solid #e5e7eb', borderRadius: 8, fontSize: 13, outline: 'none', background: '#fafafa' }}
            placeholder="搜索名称、MAC、UUID..."
            value={filter}
            onChange={e => setFilter(e.target.value)}
          />
          <select
            style={{ padding: '8px 10px', border: '1px solid #e5e7eb', borderRadius: 8, fontSize: 12, outline: 'none', background: '#fafafa', color: C.sub }}
            value={sortBy}
            onChange={e => setSortBy(e.target.value)}
          >
            <option value="rssi">信号排序</option>
            <option value="name">名称排序</option>
            <option value="recent">最近发现</option>
          </select>
        </div>

        {/* Quick filters */}
        <div style={{ display: 'flex', gap: 8, marginTop: 8, alignItems: 'center' }}>
          <button
            onClick={() => setShowNameOnly(v => !v)}
            style={{ padding: '4px 10px', borderRadius: 20, fontSize: 11, fontWeight: 600, border: `1px solid ${showNameOnly ? C.primary : '#ddd'}`, background: showNameOnly ? '#eaf3ff' : C.white, color: showNameOnly ? C.primary : C.muted, cursor: 'pointer' }}
          >
            仅显示有名称
          </button>
          <span style={{ fontSize: 11, color: C.muted }}>
            共 {Object.keys(devices).length} 台 · 显示 {deviceList.length} 台
          </span>
        </div>
      </div>

      {error && (
        <div style={{ margin: '12px 16px', padding: '10px 14px', background: '#fdecea', borderRadius: 10, fontSize: 13, color: C.red }}>{error}</div>
      )}

      {/* Device List */}
      <div style={{ flex: 1, overflowY: 'auto', padding: '8px 12px' }}>
        {deviceList.length === 0 && !scanning && (
          <div style={{ textAlign: 'center', padding: '48px 20px' }}>
            <div style={{ fontSize: 48, marginBottom: 16 }}>📡</div>
            <div style={{ fontSize: 16, fontWeight: 600, color: C.text, marginBottom: 8 }}>等待扫描</div>
            <div style={{ fontSize: 13, color: C.muted, lineHeight: 1.7 }}>点击上方"扫描"按钮开始<br />发现附近的蓝牙 BLE 设备</div>
          </div>
        )}

        {deviceList.length === 0 && scanning && (
          <div style={{ textAlign: 'center', padding: '48px 20px' }}>
            <div style={{ width: 32, height: 32, border: '3px solid #e8e8e8', borderTopColor: C.primary, borderRadius: '50%', animation: 'spin .7s linear infinite', margin: '0 auto 16px' }} />
            <div style={{ fontSize: 14, color: C.sub }}>正在搜索附近的蓝牙设备...</div>
          </div>
        )}

        {deviceList.map(dev => {
          const pct = Math.max(0, Math.min(100, (dev.rssi + 100) * 2))
          const isRecent = dev.lastSeen && (Date.now() - dev.lastSeen) < 3000
          const isDevice = /^(O5|C1)-/i.test(dev.name)
          return (
            <div key={dev.id}
              onClick={() => setSelected(dev)}
              style={{
                background: C.white,
                borderRadius: 12,
                padding: '12px 14px',
                marginBottom: 8,
                boxShadow: '0 1px 4px rgba(0,0,0,.05)',
                cursor: 'pointer',
                borderLeft: isDevice ? `3px solid ${C.primary}` : '3px solid transparent',
                opacity: isRecent || !scanning ? 1 : 0.6,
                transition: 'opacity .3s',
              }}
            >
              <div style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
                {/* Signal indicator */}
                <div style={{
                  width: 36, height: 36, borderRadius: 8,
                  background: pct >= 60 ? '#edfbf3' : pct >= 35 ? '#fff8ed' : '#fdecea',
                  display: 'flex', alignItems: 'center', justifyContent: 'center', flexShrink: 0,
                }}>
                  <RssiBar rssi={dev.rssi} />
                </div>

                {/* Info */}
                <div style={{ flex: 1, minWidth: 0 }}>
                  <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                    <span style={{
                      fontSize: 14, fontWeight: 600,
                      color: dev.name ? C.text : C.muted,
                      fontStyle: dev.name ? 'normal' : 'italic',
                      overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
                    }}>
                      {dev.name || '未知设备'}
                    </span>
                    {isDevice && (
                      <span style={{ padding: '1px 6px', borderRadius: 10, fontSize: 10, fontWeight: 700, background: '#eaf3ff', color: C.primary }}>
                        {dev.name.split('-')[0]}
                      </span>
                    )}
                    {dev.connectable && (
                      <span style={{ fontSize: 10, color: C.green }}>●</span>
                    )}
                  </div>
                  <div style={{ fontSize: 12, fontFamily: 'monospace', color: C.muted, marginTop: 2 }}>{dev.mac || dev.id}</div>
                  {dev.serviceUuids && dev.serviceUuids.length > 0 && (
                    <div style={{ fontSize: 11, color: C.primary, marginTop: 2, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                      UUID: {dev.serviceUuids.map(u => shortUuid(u)).join(', ')}
                    </div>
                  )}
                </div>

                {/* RSSI value */}
                <div style={{ textAlign: 'right', flexShrink: 0 }}>
                  <div style={{ fontSize: 15, fontWeight: 700, color: pct >= 60 ? C.green : pct >= 35 ? C.orange : C.red, fontFamily: 'monospace' }}>{dev.rssi}</div>
                  <div style={{ fontSize: 10, color: C.muted }}>dBm</div>
                  {dev.updateCount > 1 && (
                    <div style={{ fontSize: 10, color: C.muted, marginTop: 2 }}>×{dev.updateCount}</div>
                  )}
                </div>
              </div>

              {/* Mini RSSI history bar */}
              {dev.rssiHistory && dev.rssiHistory.length > 1 && (
                <div style={{ display: 'flex', alignItems: 'flex-end', gap: 1, marginTop: 6, height: 14, overflow: 'hidden' }}>
                  {dev.rssiHistory.slice(-30).map((r, i) => {
                    const h = Math.min(14, Math.max(2, ((r + 100) / 50) * 14))
                    const c = r >= -50 ? C.green : r >= -70 ? C.orange : C.red
                    return <div key={i} style={{ width: 2, height: h, borderRadius: 1, background: c, opacity: 0.4 + (i / Math.min(30, dev.rssiHistory.length)) * 0.6 }} />
                  })}
                </div>
              )}
            </div>
          )
        })}
      </div>

      {/* Device detail sheet */}
      {selected && <DeviceDetail device={selected} onClose={() => setSelected(null)} />}

      {/* Share code dialog */}
      {shareCode != null && (
        <div style={{ position: 'fixed', top: 0, left: 0, width: '100%', height: '100%', background: 'rgba(0,0,0,.45)', zIndex: 10000, display: 'flex', alignItems: 'center', justifyContent: 'center' }} onClick={() => setShareCode(null)}>
          <div style={{ background: C.white, borderRadius: 18, padding: '28px 24px', width: '80%', maxWidth: 320, textAlign: 'center' }} onClick={e => e.stopPropagation()}>
            <div style={{ fontSize: 15, color: C.sub, marginBottom: 12 }}>数据已发送，识别码：</div>
            <div style={{ display: 'flex', justifyContent: 'center', gap: 10, marginBottom: 16 }}>
              {shareCode.split('').map((ch, i) => (
                <div key={i} style={{ width: 48, height: 56, background: '#f0f2f5', borderRadius: 12, display: 'flex', alignItems: 'center', justifyContent: 'center', fontSize: 28, fontWeight: 800, fontFamily: 'monospace', color: C.primary }}>{ch}</div>
              ))}
            </div>
            <div style={{ fontSize: 13, color: C.sub, marginBottom: 16 }}>请将识别码发送给客服</div>
            <div style={{ fontSize: 11, color: C.muted, marginBottom: 16 }}>有效期 30 分钟</div>
            <button onClick={() => setShareCode(null)} style={{ width: '100%', padding: '12px 0', background: C.primary, color: '#fff', border: 'none', borderRadius: 10, fontSize: 15, fontWeight: 600, cursor: 'pointer' }}>知道了</button>
          </div>
        </div>
      )}
    </div>
  )
}

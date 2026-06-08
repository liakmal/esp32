import { useState, useEffect, useCallback, useRef } from 'react'
import { Capacitor } from '@capacitor/core'
import { App as CapApp } from '@capacitor/app'
import { WifiPlugin } from './wifi-capacitor'
import DeviceConfig from './DeviceConfig'
import DeviceStatus from './DeviceStatus'
import BleScanner from './BleScanner'
import MqttPanel from './MqttPanel'

const DEVICE_IP    = '192.168.4.1'

const isElectron  = typeof window !== 'undefined' && !!window.deviceAPI
const isCapacitor = Capacitor.isNativePlatform()

// ── In-App Browser（Electron: webview DOM / Android: iframe）─────────────────
function InAppBrowser({ url, onBack }) {
  const containerRef = useRef(null)
  const wvRef        = useRef(null)
  const iframeRef    = useRef(null)
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    if (isElectron) {
      if (!containerRef.current) return
      const wv = document.createElement('webview')
      wv.src = url
      wv.style.cssText = 'width:100%;height:100%;'
      wv.addEventListener('did-start-loading', () => setLoading(true))
      wv.addEventListener('did-stop-loading',  () => setLoading(false))
      containerRef.current.appendChild(wv)
      wvRef.current = wv
      return () => {
        if (containerRef.current && wv.parentNode === containerRef.current)
          containerRef.current.removeChild(wv)
      }
    } else {
      setLoading(false)
    }
  }, [url])

  const handleReload = () => {
    if (isElectron) wvRef.current?.reload()
    else if (iframeRef.current) iframeRef.current.src = url
  }

  const toolbar = (
    <div style={{ background: 'white', padding: '8px 14px', display: 'flex', alignItems: 'center', gap: 10, borderBottom: '1px solid #f0f0f0', flexShrink: 0, boxShadow: '0 1px 4px rgba(0,0,0,.05)', WebkitAppRegion: 'no-drag' }}>
      <button onClick={onBack} style={{ background: 'none', border: 'none', color: '#4a90e2', cursor: 'pointer', fontSize: 15, fontWeight: 600, padding: '4px 0', flexShrink: 0 }}>← 返回</button>
      <span style={{ flex: 1, fontSize: 12, color: '#888', fontFamily: 'monospace', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{url}</span>
      {loading && <span style={{ fontSize: 11, color: '#bbb' }}>加载中...</span>}
      <button onClick={handleReload} style={{ background: 'none', border: 'none', color: '#888', cursor: 'pointer', fontSize: 14, padding: '4px 0' }}>↻</button>
    </div>
  )

  if (isElectron) {
    return (
      <div style={{ display: 'flex', flexDirection: 'column', height: '100vh' }}>
        {toolbar}
        <div ref={containerRef} style={{ flex: 1, overflow: 'hidden' }} />
      </div>
    )
  }

  // Android / browser: use <iframe>
  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100vh' }}>
      {toolbar}
      <iframe
        ref={iframeRef}
        src={url}
        style={{ flex: 1, width: '100%', border: 'none' }}
        onLoad={() => setLoading(false)}
      />
    </div>
  )
}

// ── Helpers ───────────────────────────────────────────────────────────────────
function typeInfo(ssid) {
  if (/^C1-/i.test(ssid)) return { id: 'c1', label: 'C1', color: '#7b5ea7', bg: '#f3eeff', desc: 'BLE 广播器' }
  if (/^O5-/i.test(ssid)) return { id: 'o5', label: 'O5', color: '#2271c3', bg: '#eaf3ff', desc: 'BLE 扫描器' }
  return { id: '?', label: '?', color: '#888', bg: '#f5f5f5', desc: '未知' }
}

// ── Style helpers ─────────────────────────────────────────────────────────────
const F = '-apple-system,BlinkMacSystemFont,"Segoe UI",Arial,sans-serif'
const C = { bg: '#f0f2f5', white: '#fff', primary: '#4a90e2', green: '#27ae60', red: '#c0392b', border: '#f0f0f0', sub: '#888', muted: '#bbb', text: '#222' }
const card  = { background: C.white, borderRadius: 12, boxShadow: '0 1px 6px rgba(0,0,0,.07)', marginBottom: 12, overflow: 'hidden' }
const sec   = { fontSize: 11, color: '#999', padding: '7px 14px', fontWeight: 600, letterSpacing: .5, background: '#fafafa', borderBottom: `1px solid ${C.border}` }
const row   = { display: 'flex', alignItems: 'center', padding: '12px 14px', borderBottom: `1px solid ${C.border}` }
const bigBtn = (bg, dis, textColor) => ({ display: 'block', width: '100%', padding: '14px 0', textAlign: 'center', borderRadius: 10, fontSize: 16, fontWeight: 700, border: 'none', cursor: dis ? 'not-allowed' : 'pointer', background: dis ? '#b8d4f0' : bg, color: textColor || '#fff', marginBottom: 12 })

// ── Bottom Tab Bar ────────────────────────────────────────────────────────────
const TAB_BAR_H = 56
const tabs = [
  { id: 'device', label: '设备' },
  { id: 'scanner', label: '扫描' },
  { id: 'mqtt', label: '查询' },
]

function TabBar({ active, onChange }) {
  return (
    <div style={{
      position: 'fixed', bottom: 0, left: 0, right: 0, height: TAB_BAR_H,
      background: '#fff', borderTop: '1px solid #e8e8e8',
      display: 'flex', alignItems: 'center', justifyContent: 'center',
      zIndex: 900, boxShadow: '0 -1px 6px rgba(0,0,0,.06)',
    }}>
      {tabs.map(t => {
        const isActive = t.id === active
        return (
          <button key={t.id} onClick={() => onChange(t.id)}
            style={{
              flex: 1, display: 'flex', flexDirection: 'column', alignItems: 'center',
              justifyContent: 'center', gap: 2, border: 'none', background: 'none',
              cursor: 'pointer', padding: '6px 0',
              color: isActive ? C.primary : '#999',
              transition: 'color .2s',
            }}>
            <span style={{ fontSize: 14, fontWeight: isActive ? 700 : 500 }}>{t.label}</span>
          </button>
        )
      })}
    </div>
  )
}

// ── Settings Page ─────────────────────────────────────────────────────────────
const APP_VERSION = '2.1.0'
const APP_AUTHOR = '@北晨科技'
const DEFAULT_SERVER = 'http://ble.qyan10.store'

function SettingsPage({ onBack }) {
  const isCustom = () => { const s = localStorage.getItem('ble_server'); return s && s !== DEFAULT_SERVER }
  const [server, setServer] = useState(() => isCustom() ? localStorage.getItem('ble_server') : '')
  const [saved, setSaved] = useState(false)
  const [checking, setChecking] = useState(false)

  const saveServer = () => {
    const v = server.trim().replace(/\/+$/, '')
    localStorage.setItem('ble_server', v || DEFAULT_SERVER)
    if (!v || v === DEFAULT_SERVER) setServer('')
    else setServer(v)
    setSaved(true)
    setTimeout(() => setSaved(false), 1500)
  }

  const checkUpdate = async () => {
    setChecking(true)
    await new Promise(r => setTimeout(r, 1500))
    setChecking(false)
    alert('当前已是最新版本 v' + APP_VERSION)
  }

  const sCard = { ...card, marginBottom: 12 }
  const sRow = { display: 'flex', alignItems: 'center', padding: '14px 16px', borderBottom: `1px solid ${C.border}` }
  const sLabel = { fontSize: 13, color: C.sub, width: 80, flexShrink: 0 }
  const sVal = { fontSize: 14, fontWeight: 600, color: C.text }

  return (
    <div style={{ fontFamily: F, background: C.bg, minHeight: '100vh' }}>
      <div style={{ background: '#fff', padding: '14px 16px', display: 'flex', alignItems: 'center', borderBottom: '1px solid #e8e8e8' }}>
        <button onClick={onBack} style={{ background: 'none', border: 'none', color: C.primary, fontSize: 15, fontWeight: 600, cursor: 'pointer', padding: 0, marginRight: 12 }}>← 返回</button>
        <span style={{ fontSize: 17, fontWeight: 700, color: C.text }}>设置</span>
      </div>
      <div style={{ maxWidth: 400, margin: '0 auto', padding: '16px 16px 80px' }}>
        <div style={sCard}>
          <div style={{ ...sRow, borderBottom: `1px solid ${C.border}` }}>
            <span style={sLabel}>版本号</span>
            <span style={sVal}>v{APP_VERSION}</span>
          </div>
          <div style={{ ...sRow, borderBottom: `1px solid ${C.border}` }}>
            <span style={sLabel}>作者</span>
            <span style={sVal}>{APP_AUTHOR}</span>
          </div>
          <div style={{ ...sRow, borderBottom: 'none', cursor: 'pointer' }} onClick={checkUpdate}>
            <span style={sLabel}>检查更新</span>
            <span style={{ fontSize: 14, fontWeight: 600, color: C.primary }}>{checking ? '检查中...' : '点击检查'}</span>
          </div>
        </div>

        <div style={{ fontSize: 11, color: '#999', fontWeight: 600, padding: '7px 4px', letterSpacing: .5 }}>工具</div>
        <div style={sCard}>
          <div style={{ ...sRow, borderBottom: 'none', cursor: 'pointer' }} onClick={() => {
            if (isCapacitor) {
              try { Capacitor.Plugins.GpsPlugin.openGps() } catch(e) { alert('GPS 功能仅在 Android 端可用') }
            } else { alert('GPS 功能仅在 Android 端可用') }
          }}>
            <span style={sLabel}>📍 GPS</span>
            <span style={{ fontSize: 14, fontWeight: 600, color: C.primary }}>虚拟定位 →</span>
          </div>
        </div>

        <div style={{ fontSize: 11, color: '#999', fontWeight: 600, padding: '7px 4px', letterSpacing: .5 }}>BLE 分享服务器</div>
        <div style={sCard}>
          <div style={{ padding: '14px 16px' }}>
            <div style={{ fontSize: 12, color: C.sub, marginBottom: 8 }}>App 发送 BLE 扫描数据的目标服务器地址</div>
            <input
              style={{ width: '100%', padding: '10px 12px', border: '1px solid #ddd', borderRadius: 8, fontSize: 13, outline: 'none', fontFamily: 'monospace', marginBottom: 10 }}
              value={server}
              onChange={e => setServer(e.target.value)}
              placeholder="留空使用默认服务器"
            />
            <div style={{ display: 'flex', gap: 8 }}>
              <button onClick={saveServer} style={{ flex: 1, padding: '10px 0', background: C.primary, color: '#fff', border: 'none', borderRadius: 8, fontSize: 14, fontWeight: 600, cursor: 'pointer' }}>
                {saved ? '✓ 已保存' : '保存'}
              </button>
              <button onClick={() => { setServer(''); localStorage.setItem('ble_server', DEFAULT_SERVER); setSaved(true); setTimeout(() => setSaved(false), 1500) }}
                style={{ padding: '10px 14px', background: '#f0f2f5', color: C.sub, border: 'none', borderRadius: 8, fontSize: 13, fontWeight: 600, cursor: 'pointer' }}>
                重置默认
              </button>
            </div>
          </div>
        </div>
      </div>
    </div>
  )
}

// ── Main App ──────────────────────────────────────────────────────────────────
export default function App() {
  const [activeTab, setActiveTab] = useState('device')
  const [showSettings, setShowSettings] = useState(false)

  // 全屏页面（config/status/webview）不显示底栏
  const [fullscreen, setFullscreen] = useState(false)

  // Global Android back button handler
  const activeTabRef = useRef(activeTab)
  const showSettingsRef = useRef(showSettings)
  const fullscreenRef = useRef(fullscreen)
  useEffect(() => { activeTabRef.current = activeTab }, [activeTab])
  useEffect(() => { showSettingsRef.current = showSettings }, [showSettings])
  useEffect(() => { fullscreenRef.current = fullscreen }, [fullscreen])

  useEffect(() => {
    if (!isCapacitor) return
    const listener = CapApp.addListener('backButton', () => {
      // BLE detail page open → close it
      if (window.__bleDetailOpen && typeof window.__bleDetailClose === 'function') {
        window.__bleDetailClose()
        return
      }
      // Settings page → close
      if (showSettingsRef.current) {
        setShowSettings(false)
        return
      }
      // Fullscreen manage view → go home (handled by DeviceTab)
      if (fullscreenRef.current) {
        window.__deviceTabGoHome && window.__deviceTabGoHome()
        return
      }
      // Non-device tab → switch to device tab
      if (activeTabRef.current !== 'device') {
        setActiveTab('device')
        return
      }
      // Already on home → minimize app
      CapApp.minimizeApp()
    })
    return () => { listener.then(h => h.remove()) }
  }, [])

  if (showSettings) return <SettingsPage onBack={() => setShowSettings(false)} />

  return (
    <div style={{ fontFamily: F }}>
      {!fullscreen && (
        <div style={{ background: '#fff', padding: '12px 16px', display: 'flex', alignItems: 'center', justifyContent: 'space-between', borderBottom: '1px solid #e8e8e8', position: 'sticky', top: 0, zIndex: 800 }}>
          <span style={{ fontSize: 17, fontWeight: 700, color: C.text }}>设备配置助手</span>
          <button onClick={() => setShowSettings(true)} style={{ background: 'none', border: 'none', cursor: 'pointer', padding: 4, fontSize: 20, color: '#999', lineHeight: 1 }}>⚙️</button>
        </div>
      )}
      <div style={{ display: activeTab === 'device' ? 'block' : 'none' }}>
        <DeviceTab onFullscreen={setFullscreen} />
      </div>
      <div style={{ display: activeTab === 'scanner' ? 'block' : 'none' }}>
        <BleScanner />
      </div>
      <div style={{ display: activeTab === 'mqtt' ? 'block' : 'none' }}>
        <MqttPanel />
      </div>
      {!fullscreen && <TabBar active={activeTab} onChange={setActiveTab} />}
    </div>
  )
}

// ── Device Tab (auto-detect current WiFi) ───────────────────────────────────
function DeviceTab({ onFullscreen }) {
  // view: home | manage
  const [view,        setView]        = useState('home')
  const [currentWifi, setCurrentWifi] = useState('')
  const [checking,    setChecking]    = useState(true)
  const [manageTab,   setManageTab]   = useState('status')
  const pollRef = useRef(null)

  // Notify parent about fullscreen views (hide tab bar)
  useEffect(() => {
    onFullscreen(view === 'manage')
  }, [view, onFullscreen])

  // Expose go-home for global back button
  useEffect(() => {
    window.__deviceTabGoHome = () => { setView('home'); setManageTab('status') }
    return () => { window.__deviceTabGoHome = null }
  }, [])

  // Check if device is first-time init → show config tab
  const enterManage = useCallback(async () => {
    setView('manage')
    try {
      const r = await fetch(`http://${DEVICE_IP}/api/config.json`, { cache: 'no-store' })
      const cfg = await r.json()
      if (!cfg.wifi_ssid) setManageTab('config')
      else setManageTab('status')
    } catch {
      setManageTab('status')
    }
  }, [])

  // Request WiFi permissions on Android startup
  useEffect(() => {
    if (isCapacitor) {
      WifiPlugin.requestPermissions().catch(() => {})
    }
  }, [])

  // Poll current WiFi SSID
  const checkWifi = useCallback(async () => {
    let ssid = ''
    if (isElectron) {
      ssid = (await window.deviceAPI.getCurrentWifi()) || ''
    } else if (isCapacitor) {
      const r = await WifiPlugin.getCurrentSsid()
      ssid = r.ssid || ''
    }
    setCurrentWifi(ssid)
    setChecking(false)
    return ssid
  }, [])

  // On mount + periodic check (every 3s when on home)
  useEffect(() => {
    checkWifi()
    pollRef.current = setInterval(() => {
      if (view === 'home') checkWifi()
    }, 3000)
    return () => clearInterval(pollRef.current)
  }, [view, checkWifi])

  // ── Render ────────────────────────────────────────────────────────────────
  const wrap = { maxWidth: 400, margin: '0 auto', padding: '20px 16px', paddingBottom: 72 }

  if (view === 'manage') {
    const ssid = currentWifi
    const isTab = (t) => manageTab === t
    const tabSt = (t) => ({ flex: 1, padding: '11px 0', border: 'none', borderBottom: isTab(t) ? `2px solid ${C.primary}` : '2px solid transparent', background: 'none', fontSize: 14, fontWeight: isTab(t) ? 700 : 500, color: isTab(t) ? C.primary : '#999', cursor: 'pointer', transition: 'all .2s' })
    return (
      <div style={{ fontFamily: F, background: C.bg, minHeight: '100vh', display: 'flex', flexDirection: 'column' }}>
        <style>{`*{box-sizing:border-box}`}</style>
        <div style={{ background: '#fff', padding: '10px 14px', display: 'flex', alignItems: 'center', gap: 10, borderBottom: `1px solid ${C.border}`, boxShadow: '0 1px 4px rgba(0,0,0,.05)', flexShrink: 0 }}>
          <button onClick={() => { setView('home'); setManageTab('status') }} style={{ background: 'none', border: 'none', color: C.primary, fontSize: 15, fontWeight: 600, cursor: 'pointer', padding: '4px 0' }}>← 首页</button>
          <span style={{ flex: 1, fontSize: 15, fontWeight: 600, color: C.text }}>{ssid}</span>
          <span style={{ fontSize: 12, color: C.sub, fontFamily: 'monospace' }}>{DEVICE_IP}</span>
        </div>
        <div style={{ display: 'flex', background: '#fff', borderBottom: '1px solid #e8e8e8' }}>
          <button onClick={() => setManageTab('status')} style={tabSt('status')}>📊 状态</button>
          <button onClick={() => setManageTab('config')} style={tabSt('config')}>⚙️ 配置</button>
        </div>
        <div style={{ flex: 1, overflowY: 'auto' }}>
          {manageTab === 'status'
            ? <DeviceStatus ssid={ssid} onBack={() => setView('home')} onReinit={() => setView('home')} embedded />
            : <DeviceConfig ssid={ssid} onBack={() => setManageTab('status')} embedded />
          }
        </div>
      </div>
    )
  }

  // Home view: show current WiFi, prompt user to connect manually
  const isDevice = /^(O5|C1)-/i.test(currentWifi)
  const curInfo = currentWifi ? typeInfo(currentWifi) : null
  return (
    <div style={{ fontFamily: F, background: C.bg, minHeight: '100vh' }}>
      <style>{`@keyframes spin{to{transform:rotate(360deg)}} *{box-sizing:border-box}`}</style>
      <div style={wrap}>
        <div style={{ marginBottom: 22 }}>
          <div style={{ fontSize: 20, fontWeight: 700, color: C.text }}>设备配置助手</div>
          <div style={{ fontSize: 13, color: C.sub, marginTop: 3 }}>C1 广播器 · O5 扫描器</div>
        </div>

        {/* Current WiFi status */}
        <div style={{ ...card, marginBottom: 14 }}>
          <div style={sec}>当前 WiFi 连接</div>
          <div style={{ ...row, borderBottom: 'none' }}>
            {checking ? (
              <span style={{ fontSize: 13, color: C.sub }}>检测中...</span>
            ) : currentWifi ? (
              <span style={{ display: 'flex', alignItems: 'center', gap: 8, width: '100%' }}>
                <span style={{ fontSize: 14, fontWeight: 600, color: C.text, fontFamily: 'monospace' }}>{currentWifi}</span>
                {curInfo && isDevice && (
                  <span style={{ padding: '2px 7px', borderRadius: 20, fontSize: 11, fontWeight: 700, background: curInfo.bg, color: curInfo.color }}>{curInfo.label}</span>
                )}
                {!isDevice && <span style={{ fontSize: 12, color: '#e67e22', fontWeight: 600 }}>非设备热点</span>}
              </span>
            ) : (
              <span style={{ fontSize: 13, color: C.muted }}>未连接 WiFi</span>
            )}
          </div>
        </div>

        {/* Action: if connected to device, show device card; otherwise refresh */}
        {isDevice ? (
          <div style={{ ...card, padding: '20px 16px', textAlign: 'center', marginBottom: 14 }}>
            <div style={{ display: 'inline-flex', alignItems: 'center', justifyContent: 'center', width: 52, height: 52, borderRadius: '50%', background: curInfo.bg, marginBottom: 10 }}>
              <span style={{ fontSize: 22, fontWeight: 800, color: curInfo.color }}>{curInfo.label}</span>
            </div>
            <div style={{ fontSize: 15, fontWeight: 700, color: C.green, marginBottom: 4 }}>已连接{curInfo.id === 'o5' ? '主机' : '子机'}（{curInfo.label}）</div>
            <div style={{ fontSize: 13, color: C.sub, marginBottom: 14 }}>{curInfo.desc} · {currentWifi}</div>
            <button style={{ width: '100%', padding: '13px 0', background: C.green, color: '#fff', border: 'none', borderRadius: 10, fontSize: 16, fontWeight: 700, cursor: 'pointer' }}
              onClick={enterManage}>
              点击进入管理页面
            </button>
          </div>
        ) : (
          <button style={bigBtn(C.primary, false)} onClick={() => { setChecking(true); checkWifi() }}>
            {checking ? '检测中...' : '刷新连接状态'}
          </button>
        )}

        {/* Guide */}
        <div style={card}>
          <div style={sec}>使用流程</div>
          {[
            ['1', '打开手机 WiFi 设置',   '手动搜索并连接设备热点（O5-XXXX 或 C1-XXXX）'],
            ['2', '密码默认 12345678',    '输入热点密码完成连接'],
            ['3', '返回本 App',           'App 自动识别已连接的设备类型'],
            ['4', '自动跳转管理页面',      '根据 O5 或 C1 展示对应的状态和配置'],
          ].map(([n, title, desc], idx, arr) => (
            <div key={n} style={{ ...row, gap: 12, borderBottom: idx < arr.length - 1 ? `1px solid ${C.border}` : 'none' }}>
              <div style={{ width: 26, height: 26, borderRadius: '50%', background: '#eaf3ff', color: C.primary, display: 'flex', alignItems: 'center', justifyContent: 'center', fontWeight: 700, fontSize: 13, flexShrink: 0 }}>{n}</div>
              <div>
                <div style={{ fontSize: 13, fontWeight: 600, color: C.text }}>{title}</div>
                <div style={{ fontSize: 12, color: C.sub, marginTop: 2 }}>{desc}</div>
              </div>
            </div>
          ))}
        </div>

        <div style={{ textAlign: 'center', fontSize: 12, color: C.muted, marginTop: 8, paddingBottom: 20 }}>@北晨科技 · 设备配置助手 v2.1</div>
      </div>
    </div>
  )
}

const { app, BrowserWindow, ipcMain, shell } = require('electron')
const { exec } = require('child_process')
const http = require('http')
const fs = require('fs')
const path = require('path')
const os = require('os')

const isDev = !app.isPackaged

function createWindow() {
  const win = new BrowserWindow({
    width: 420,
    height: 780,
    minWidth: 360,
    minHeight: 600,
    resizable: true,
    show: false,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      nodeIntegration: false,
      contextIsolation: true,
      webviewTag: true,
    },
    title: '设备配置助手',
    backgroundColor: '#f0f2f5',
  })

  win.once('ready-to-show', () => win.show())

  win.webContents.on('did-fail-load', (event, code, desc, url) => {
    console.error('[Electron] 页面加载失败:', code, desc, url)
    // 开发模式下若 Vite 未就绪则 1 秒后重试
    if (isDev) setTimeout(() => win.loadURL('http://localhost:3000'), 1000)
  })

  if (isDev) {
    win.loadURL('http://localhost:3000')
  } else {
    win.loadFile(path.join(__dirname, '../dist/index.html'))
  }
}

app.whenReady().then(createWindow)
app.on('window-all-closed', () => { if (process.platform !== 'darwin') app.quit() })
app.on('activate', () => { if (BrowserWindow.getAllWindows().length === 0) createWindow() })

// ── WiFi: scan available networks ─────────────────────────────────────────────
function parseNetshNetworks(output) {
  const map = {}
  const lines = output.split(/\r?\n/)
  let cur = null

  for (const line of lines) {
    const ssidM = line.match(/^SSID\s+\d+\s*:\s*(.+)$/)
    if (ssidM) {
      const ssid = ssidM[1].trim()
      if (!map[ssid]) map[ssid] = { ssid, signal: 0 }
      cur = map[ssid]
      continue
    }
    if (!cur) continue
    const sigM = line.match(/Signal\s*:\s*(\d+)%/)
    if (sigM) {
      const s = parseInt(sigM[1])
      if (s > cur.signal) cur.signal = s
    }
  }
  return Object.values(map)
}

ipcMain.handle('wifi:scan', async () => {
  return new Promise((resolve) => {
    exec('netsh wlan show networks mode=bssid', { encoding: 'utf8' }, (err, stdout) => {
      if (err) {
        resolve({ ok: false, networks: [], all: 0, error: err.message })
        return
      }
      const all = parseNetshNetworks(stdout)
      const devices = all.filter(n => /^(O5|C1)-/i.test(n.ssid))
      resolve({ ok: true, networks: devices, all: all.length })
    })
  })
})

// ── WiFi: get current connected SSID ─────────────────────────────────────────
ipcMain.handle('wifi:current', async () => {
  return new Promise((resolve) => {
    exec('netsh wlan show interfaces', { encoding: 'utf8' }, (err, stdout) => {
      if (err) { resolve(null); return }
      for (const line of stdout.split(/\r?\n/)) {
        const m = line.match(/^\s+SSID\s*:\s*(.+)$/)
        if (m) { resolve(m[1].trim()); return }
      }
      resolve(null)
    })
  })
})

// ── WiFi: connect to network ──────────────────────────────────────────────────
function makeProfileXml(ssid, password) {
  const e = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;')
  return `<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
  <name>${e(ssid)}</name>
  <SSIDConfig>
    <SSID><name>${e(ssid)}</name></SSID>
  </SSIDConfig>
  <connectionType>ESS</connectionType>
  <connectionMode>manual</connectionMode>
  <MSM>
    <security>
      <authEncryption>
        <authentication>WPA2PSK</authentication>
        <encryption>AES</encryption>
        <useOneX>false</useOneX>
      </authEncryption>
      <sharedKey>
        <keyType>passPhrase</keyType>
        <protected>false</protected>
        <keyMaterial>${e(password)}</keyMaterial>
      </sharedKey>
    </security>
  </MSM>
</WLANProfile>`
}

ipcMain.handle('wifi:connect', async (_, { ssid, password }) => {
  return new Promise((resolve) => {
    const xml = makeProfileXml(ssid, password)
    const tmpFile = path.join(os.tmpdir(), `devmgr_${Date.now()}.xml`)

    try { fs.writeFileSync(tmpFile, xml, 'utf8') }
    catch (e) { resolve({ ok: false, error: '写入配置文件失败: ' + e.message }); return }

    exec(`netsh wlan add profile filename="${tmpFile}"`, { encoding: 'utf8' }, (err1) => {
      try { fs.unlinkSync(tmpFile) } catch {}

      exec(`netsh wlan connect name="${ssid}" ssid="${ssid}"`, { encoding: 'utf8' }, (err2, stdout2) => {
        if (err2) { resolve({ ok: false, error: err2.message }); return }
        const ok = !/error|fail/i.test(stdout2)
        resolve({ ok, output: stdout2.trim() })
      })
    })
  })
})

// ── Probe device HTTP reachability ────────────────────────────────────────────
ipcMain.handle('wifi:probe', async (_, ip) => {
  return new Promise((resolve) => {
    const req = http.get(
      { hostname: ip, port: 80, path: '/', timeout: 2500 },
      (res) => { resolve({ ok: true, status: res.statusCode }); res.destroy() }
    )
    req.on('error', () => resolve({ ok: false }))
    req.on('timeout', () => { req.destroy(); resolve({ ok: false }) })
  })
})

// ── Open URL in default browser ───────────────────────────────────────────────
ipcMain.handle('shell:open', async (_, url) => {
  shell.openExternal(url)
})

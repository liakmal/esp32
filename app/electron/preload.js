const { contextBridge, ipcRenderer } = require('electron')

contextBridge.exposeInMainWorld('deviceAPI', {
  scanWifi:      ()            => ipcRenderer.invoke('wifi:scan'),
  connectWifi:   (ssid, pass)  => ipcRenderer.invoke('wifi:connect', { ssid, password: pass }),
  getCurrentWifi:()            => ipcRenderer.invoke('wifi:current'),
  probeDevice:   (ip)          => ipcRenderer.invoke('wifi:probe', ip),
  openUrl:       (url)         => ipcRenderer.invoke('shell:open', url),
})

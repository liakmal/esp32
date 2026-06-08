import { registerPlugin } from '@capacitor/core'

// Register the native WifiPlugin. On Android it maps to WifiPlugin.java.
// On web/browser it falls back to a stub that returns empty results.
const WifiPlugin = registerPlugin('WifiPlugin', {
  web: {
    async scan() {
      return { ok: false, networks: [], all: 0, error: 'Only available on Android' }
    },
    async connect() {
      return { ok: false, error: 'Only available on Android' }
    },
    async getCurrentSsid() {
      return { ssid: null }
    },
    async probe({ ip }) {
      try {
        await fetch(`http://${ip}/`, { mode: 'no-cors', cache: 'no-store' })
        return { ok: true }
      } catch {
        return { ok: false }
      }
    },
    async startBleScan() {
      return { ok: false, error: 'BLE scan only available on Android' }
    },
    async getBleScanResults() {
      return { devices: [], scanning: false }
    },
    async stopBleScan() {
      return { ok: true }
    },
  },
})

export { WifiPlugin }

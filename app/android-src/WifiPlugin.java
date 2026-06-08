package com.bctech.devicemanager;

import android.Manifest;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothManager;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanRecord;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.wifi.ScanResult;
import android.net.wifi.WifiConfiguration;
import android.net.wifi.WifiManager;
import android.net.wifi.WifiNetworkSpecifier;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.util.SparseArray;

import com.getcapacitor.JSArray;
import com.getcapacitor.JSObject;
import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.PluginMethod;
import com.getcapacitor.annotation.CapacitorPlugin;
import com.getcapacitor.annotation.Permission;
import com.getcapacitor.annotation.PermissionCallback;

import java.net.HttpURLConnection;
import java.net.URL;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

@CapacitorPlugin(
    name = "WifiPlugin",
    permissions = {
        @Permission(strings = {
            Manifest.permission.ACCESS_WIFI_STATE,
            Manifest.permission.CHANGE_WIFI_STATE,
            Manifest.permission.ACCESS_FINE_LOCATION,
            Manifest.permission.ACCESS_COARSE_LOCATION,
            Manifest.permission.CHANGE_NETWORK_STATE,
            Manifest.permission.ACCESS_NETWORK_STATE
        }, alias = "wifi"),
        @Permission(strings = {
            Manifest.permission.BLUETOOTH_SCAN,
            Manifest.permission.BLUETOOTH_CONNECT
        }, alias = "ble")
    }
)
public class WifiPlugin extends Plugin {

    private ConnectivityManager.NetworkCallback networkCallback;
    private Network boundNetwork;

    // BLE scanning state
    private BluetoothLeScanner bleScanner;
    private ScanCallback bleScanCallback;
    private final ConcurrentHashMap<String, JSObject> bleDevices = new ConcurrentHashMap<>();
    private volatile boolean bleScanning = false;

    // ── scan ──────────────────────────────────────────────────────────────────
    @PluginMethod
    public void scan(PluginCall call) {
        if (!hasRequiredPermissions()) {
            requestAllPermissions(call, "scanAfterPermission");
            return;
        }
        doScan(call);
    }

    @PermissionCallback
    private void scanAfterPermission(PluginCall call) {
        doScan(call);
    }

    private void doScan(PluginCall call) {
        WifiManager wm = (WifiManager) getContext().getApplicationContext()
                .getSystemService(Context.WIFI_SERVICE);
        if (wm == null || !wm.isWifiEnabled()) {
            JSObject r = new JSObject();
            r.put("ok", false); r.put("networks", new JSArray()); r.put("all", 0);
            r.put("error", "WiFi 未开启");
            call.resolve(r);
            return;
        }

        wm.startScan();
        List<ScanResult> results = wm.getScanResults();
        JSArray devices = new JSArray();
        int total = 0;
        for (ScanResult sr : results) {
            if (sr.SSID == null || sr.SSID.isEmpty()) continue;
            total++;
            if (sr.SSID.matches("(?i)(O5|C1)-.*")) {
                JSObject net = new JSObject();
                net.put("ssid", sr.SSID);
                net.put("signal", WifiManager.calculateSignalLevel(sr.level, 100));
                devices.put(net);
            }
        }
        JSObject r = new JSObject();
        r.put("ok", true); r.put("networks", devices); r.put("all", total);
        call.resolve(r);
    }

    // ── getCurrentSsid ────────────────────────────────────────────────────────
    @PluginMethod
    public void getCurrentSsid(PluginCall call) {
        WifiManager wm = (WifiManager) getContext().getApplicationContext()
                .getSystemService(Context.WIFI_SERVICE);
        String ssid = null;
        if (wm != null && wm.getConnectionInfo() != null) {
            ssid = wm.getConnectionInfo().getSSID();
            if (ssid != null) ssid = ssid.replace("\"", "");
            if ("<unknown ssid>".equals(ssid)) ssid = null;
        }
        call.resolve(new JSObject().put("ssid", ssid));
    }

    // ── connect ───────────────────────────────────────────────────────────────
    @PluginMethod
    public void connect(PluginCall call) {
        String ssid     = call.getString("ssid");
        String password = call.getString("password", "");
        if (ssid == null) { call.reject("ssid required"); return; }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            connectAndroid10(ssid, password, call);
        } else {
            connectLegacy(ssid, password, call);
        }
    }

    // Android 10+ — WifiNetworkSpecifier (process-bound, shows system dialog once)
    private void connectAndroid10(String ssid, String password, PluginCall call) {
        WifiNetworkSpecifier.Builder sb = new WifiNetworkSpecifier.Builder().setSsid(ssid);
        if (password != null && !password.isEmpty()) sb.setWpa2Passphrase(password);

        NetworkRequest req = new NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .setNetworkSpecifier(sb.build())
            .build();

        ConnectivityManager cm = (ConnectivityManager) getContext()
                .getSystemService(Context.CONNECTIVITY_SERVICE);
        if (networkCallback != null) {
            try { cm.unregisterNetworkCallback(networkCallback); } catch (Exception ignored) {}
        }

        networkCallback = new ConnectivityManager.NetworkCallback() {
            @Override public void onAvailable(Network network) {
                cm.bindProcessToNetwork(network);
                boundNetwork = network;
                call.resolve(new JSObject().put("ok", true));
            }
            @Override public void onUnavailable() {
                call.resolve(new JSObject().put("ok", false).put("error", "系统拒绝连接"));
            }
        };
        cm.requestNetwork(req, networkCallback, new Handler(Looper.getMainLooper()), 15000);
    }

    // Android 9 and below — WifiConfiguration (deprecated in API 29)
    @SuppressWarnings("deprecation")
    private void connectLegacy(String ssid, String password, PluginCall call) {
        WifiManager wm = (WifiManager) getContext().getApplicationContext()
                .getSystemService(Context.WIFI_SERVICE);
        WifiConfiguration cfg = new WifiConfiguration();
        cfg.SSID = "\"" + ssid + "\"";
        cfg.preSharedKey = "\"" + password + "\"";
        int netId = wm.addNetwork(cfg);
        if (netId == -1) {
            call.resolve(new JSObject().put("ok", false).put("error", "添加网络配置失败"));
            return;
        }
        wm.disconnect();
        wm.enableNetwork(netId, true);
        wm.reconnect();
        boundNetwork = null; // set boundNetwork to null for Android 9 and below
        call.resolve(new JSObject().put("ok", true));
    }

    // ── BLE: startBleScan ──────────────────────────────────────────────────────
    @PluginMethod
    public void startBleScan(PluginCall call) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            // Android 12+ needs BLUETOOTH_SCAN
            if (getActivity().checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) != android.content.pm.PackageManager.PERMISSION_GRANTED) {
                requestPermissionForAlias("ble", call, "blePermCallback");
                return;
            }
        } else {
            if (!hasRequiredPermissions()) {
                requestAllPermissions(call, "blePermCallback");
                return;
            }
        }
        doStartBleScan(call);
    }

    @PermissionCallback
    private void blePermCallback(PluginCall call) {
        doStartBleScan(call);
    }

    private void doStartBleScan(PluginCall call) {
        BluetoothManager bm = (BluetoothManager) getContext().getSystemService(Context.BLUETOOTH_SERVICE);
        if (bm == null) {
            call.resolve(new JSObject().put("ok", false).put("error", "设备不支持蓝牙"));
            return;
        }
        BluetoothAdapter adapter = bm.getAdapter();
        if (adapter == null || !adapter.isEnabled()) {
            call.resolve(new JSObject().put("ok", false).put("error", "蓝牙未开启"));
            return;
        }

        // Stop any existing scan
        stopBleScanInternal();
        bleDevices.clear();

        bleScanner = adapter.getBluetoothLeScanner();
        if (bleScanner == null) {
            call.resolve(new JSObject().put("ok", false).put("error", "无法获取 BLE 扫描器"));
            return;
        }

        bleScanCallback = new ScanCallback() {
            @Override
            public void onScanResult(int callbackType, android.bluetooth.le.ScanResult result) {
                try {
                    String mac = result.getDevice().getAddress();
                    String name = result.getDevice().getName();
                    int rssi = result.getRssi();
                    ScanRecord record = result.getScanRecord();

                    JSObject dev = new JSObject();
                    dev.put("mac", mac);
                    dev.put("rssi", rssi);
                    if (name != null) dev.put("name", name);
                    dev.put("connectable", result.isConnectable());
                    if (record != null) {
                        int txPower = record.getTxPowerLevel();
                        if (txPower != Integer.MIN_VALUE) dev.put("txPower", txPower);

                        // Service UUIDs
                        List<ParcelUuid> uuids = record.getServiceUuids();
                        if (uuids != null && !uuids.isEmpty()) {
                            JSArray uuidArr = new JSArray();
                            for (ParcelUuid pu : uuids) {
                                String full = pu.getUuid().toString();
                                // Shorten standard BLE base UUID: 0000xxxx-0000-1000-8000-00805f9b34fb
                                if (full.matches("(?i)0000[0-9a-f]{4}-0000-1000-8000-00805f9b34fb")) {
                                    uuidArr.put("0x" + full.substring(4, 8).toUpperCase());
                                } else {
                                    uuidArr.put(full);
                                }
                            }
                            dev.put("serviceUuids", uuidArr);
                        }

                        // Manufacturer data
                        SparseArray<byte[]> mfg = record.getManufacturerSpecificData();
                        if (mfg != null && mfg.size() > 0) {
                            StringBuilder sb = new StringBuilder();
                            for (int i = 0; i < mfg.size(); i++) {
                                int key = mfg.keyAt(i);
                                byte[] val = mfg.valueAt(i);
                                sb.append(String.format("0x%04X: ", key));
                                for (byte b : val) sb.append(String.format("%02X ", b));
                                if (i < mfg.size() - 1) sb.append("| ");
                            }
                            dev.put("manufacturerData", sb.toString().trim());
                        }

                        // Raw advertisement bytes
                        byte[] rawBytes = record.getBytes();
                        if (rawBytes != null) {
                            StringBuilder hex = new StringBuilder("0x");
                            for (byte b : rawBytes) hex.append(String.format("%02X", b));
                            dev.put("rawAdvertisement", hex.toString());
                        }
                    }
                    dev.put("timestamp", System.currentTimeMillis());
                    bleDevices.put(mac, dev);
                } catch (SecurityException ignored) {}
            }

            @Override
            public void onScanFailed(int errorCode) {
                bleScanning = false;
            }
        };

        try {
            ScanSettings settings = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                .build();
            bleScanner.startScan(null, settings, bleScanCallback);
            bleScanning = true;
            call.resolve(new JSObject().put("ok", true));
        } catch (SecurityException e) {
            call.resolve(new JSObject().put("ok", false).put("error", "权限不足: " + e.getMessage()));
        }
    }

    // ── BLE: getBleScanResults ─────────────────────────────────────────────────
    @PluginMethod
    public void getBleScanResults(PluginCall call) {
        JSArray arr = new JSArray();
        for (JSObject dev : bleDevices.values()) {
            arr.put(dev);
        }
        JSObject r = new JSObject();
        r.put("devices", arr);
        r.put("scanning", bleScanning);
        call.resolve(r);
    }

    // ── BLE: stopBleScan ──────────────────────────────────────────────────────
    @PluginMethod
    public void stopBleScan(PluginCall call) {
        stopBleScanInternal();
        call.resolve(new JSObject().put("ok", true));
    }

    private void stopBleScanInternal() {
        if (bleScanner != null && bleScanCallback != null) {
            try {
                bleScanner.stopScan(bleScanCallback);
            } catch (Exception ignored) {}
        }
        bleScanCallback = null;
        bleScanning = false;
    }

    // ── probe ───────────────────────────────────────────────────────────────────────────
    @PluginMethod
    public void probe(PluginCall call) {
        String ip = call.getString("ip", "192.168.4.1");
        Network net = boundNetwork; // use bound network for Android 10+
        new Thread(() -> {
            // Try port 80 first (config AP), then 8080 (status AP / C1 status mode)
            for (int port : new int[]{80, 8080}) {
                try {
                    URL url = new URL("http://" + ip + (port == 80 ? "/" : ":" + port + "/"));
                    HttpURLConnection conn = (HttpURLConnection)
                        (net != null ? net.openConnection(url) : url.openConnection());
                    conn.setConnectTimeout(2000);
                    conn.setReadTimeout(2000);
                    conn.connect();
                    int code = conn.getResponseCode();
                    conn.disconnect();
                    call.resolve(new JSObject().put("ok", true).put("status", code).put("port", port));
                    return;
                } catch (Exception ignored) {}
            }
            call.resolve(new JSObject().put("ok", false));
        }).start();
    }
}

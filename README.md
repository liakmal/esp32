# ESP32 设备管理系统

C1 广播器 / O5c 扫描器的整套方案，包含 **ESP32 固件**、**桌面 / 安卓配置工具** 和 **云端后台服务**。

## 项目结构

| 目录 | 说明 |
|------|------|
| [`app/`](app/) | 设备配置助手：扫描并连接 `C1-XXXX` / `O5-XXXX` 热点，进入设备配置 / 状态页（Electron + React + Capacitor，支持 Windows 桌面与 Android） |
| [`程固/`](程固/) | ESP32 固件源码与编译产物：`C1`（BLE 广播器）、`O5c`（BLE 扫描器），含 `.bin` 烧录文件与刷机工具 |
| [`随数测后/A1/`](随数测后/A1/) | CentOS 7.6 云端后台（Node.js）：BLE 云端、设备编码生成器、O5c 数据查询与 MQTT 云控 |

## 三个组成部分

### 1. app — 设备配置助手
- 调用 `netsh wlan show networks` 扫描附近的 `O5-XXXX` / `C1-XXXX` 热点
- 一键连接热点（默认密码 `12345678`），连接后轮询 `192.168.4.1` 探测设备就绪
- 快速跳转设备配置页（`:80`）或状态页（`:8080`）
- 技术栈：React 18、Vite 5、Electron 29、Capacitor 6、mqtt
- 开发运行：

  ```bash
  cd app
  npm install
  npm run dev      # 同时启动 Vite + Electron
  npm run android  # 构建并打开 Android 工程
  ```

  详见 [`app/README.md`](app/README.md)。

### 2. 程固 — ESP32 固件
- `C1/C1.ino`：BLE 广播器固件
- `O5c/O5c.ino`：BLE 扫描器固件
- 使用 BLE、WiFi、MQTT（PubSubClient）、AsyncWebServer、OTA（Update）、mbedtls AES 等
- 目录内附带已编译的 `.bin` 文件与刷机工具压缩包

### 3. 随数测后/A1 — 云端后台
三个 Node.js 服务，通过 systemd 托管、Nginx 反向代理：

| 服务 | 端口 | 域名 | 文件 |
|------|------|------|------|
| BLE 云端 | 3001 | `ble.qyan10.store` | `server.js` |
| 设备编码生成器 | 3002 | `code.qyan10.store` | `app.js` |
| O5c 数据查询 & MQTT 云控 | 3003 | `mqtt.qyan10.store` | `mqtt.js` |

- 支持多 MQTT Broker（emqx / hivemq），通过 MQTT 远程修改 O5c 设备的扫描配置
- 一键部署：

  ```bash
  cd /root/A1
  sed -i 's/\r$//' deploy.sh
  chmod +x deploy.sh
  ./deploy.sh      # 安装依赖 + 配置 Nginx + 注册 systemd 自启
  ```

  详见 [`随数测后/A1/README.md`](随数测后/A1/README.md)。

## 整体数据流

```
ESP32 设备(C1/O5c) ──BLE/WiFi──► app 配置工具 ──配置──► 设备联网
        │
        └──MQTT──► 云端后台(A1) ──Web/API──► 浏览器查看与远程云控
```

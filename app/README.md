# 设备配置助手

C1 广播器 / O5 扫描器 的 **Windows 桌面管理工具**（Electron + React）。

## 核心功能

- **扫描 WiFi 列表**：调用 `netsh wlan show networks` 搜索附近所有 `O5-XXXX` / `C1-XXXX` 热点
- **一键连接**：点击设备名称，自动使用统一密码（默认 `12345678`）连接热点
- **自动探测就绪**：连接后轮询 `192.168.4.1`，设备响应后立即提示
- **快速跳转**：进入配置页（`:80`）或状态页（`:8080`），在系统默认浏览器打开
- **密码自定义**：在首页折叠区域修改连接密码

## 使用流程

1. 打开 App，点击 **扫描设备 WiFi**
2. 列表中出现 `O5-XXXX` 或 `C1-XXXX`，点击目标设备
3. App 自动连接热点（密码 `12345678`），等待设备就绪
4. 点击 **进入配置** 或 **查看状态**

## 开发运行

```bash
cd app
npm install        # 首次安装（含 Electron，约 100MB）
npm run dev        # 同时启动 Vite + Electron 窗口
```

如需单独启动 Vite 预览（不打开 Electron 窗口）：
```bash
npm run vite       # 浏览器打开 http://localhost:3000
```

## 打包为可执行文件

```bash
npm run dist       # 生成 dist/ 后由 electron-builder 打包 .exe
```

## 文件结构

```
app/
├── electron/
│   ├── main.js       ← 主进程：WiFi扫描/连接/探测（netsh）
│   └── preload.js    ← IPC 桥：暴露 window.deviceAPI
├── src/
│   ├── main.jsx
│   └── App.jsx       ← React UI：扫描→连接→就绪流程
├── public/
│   └── manifest.json
├── index.html
├── vite.config.js
└── package.json
```

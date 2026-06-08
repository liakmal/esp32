# CentOS 7.6 部署说明

服务器：`38.76.204.187` | 域名：`qyan10.store`

| 服务 | 端口 | 域名 | 文件 |
|------|------|------|------|
| BLE 云后端 | 3001 | `ble.qyan10.store` | `server.js` |
| 设备编号生成器 | 3002 | `code.qyan10.store` | `app.js` |
| O5c 设备查询 & 云控 | 3003 | `mqtt.qyan10.store` | `mqtt.js` |

---

## 访问地址

| 页面 | 地址 | 说明 |
|------|------|------|
| BLE 设备管理 | http://ble.qyan10.store/admin | BLE 扫描数据共享 |
| 编号生成（前台） | http://code.qyan10.store/ | 设备编号查询 |
| 编号管理（后台） | http://code.qyan10.store/admin | 编号管理（需登录） |
| O5c 设备查询 | http://mqtt.qyan10.store/ | 输入主机名查看实时数据 |
| O5c 模拟发包 | http://mqtt.qyan10.store/send | 手动模拟 MQTT 发送 |
| O5c 云控入口 | http://mqtt.qyan10.store/control | 输入序列号/主机名进入云控 |
| O5c 云控 | http://mqtt.qyan10.store/control/{主机名} | 远程修改扫描配置 |

---

## 文件说明

| 文件 | 说明 |
|------|------|
| `server.js` | BLE 云后端主程序（端口 3001） |
| `app.js` | 设备编号生成器主程序（端口 3002） |
| `mqtt.js` | O5c 设备查询 + 模拟发包 + 云控主程序（端口 3003） |
| `package.json` | 统一依赖配置（express, mqtt, exceljs 等） |
| `deploy.sh` | **一键部署脚本**（依赖安装 + Nginx + 自启动，全自动） |
| `start.sh` | Linux 手动启动脚本 |
| `start.bat` | Windows 本地调试启动脚本 |
| `codes.json` | 编号数据文件（运行后自动生成） |

---

## mqtt.js 功能详情

### 多 Broker 支持

同时连接三个 MQTT Broker，与 O5c 固件预设同步：

| Key | 服务器 | TCP | WSS（浏览器） |
|-----|--------|-----|---------------|
| `emqx` | `broker.emqx.io` | 1883 | 8084 |
| `hivemq` | `broker.hivemq.com` | 1883 | 8884 |

- 使用 `clean: false` 持久会话 + QoS 1，防止断线期间消息丢失
- 稳定 clientId（基于机器哈希），重启后 broker 可关联旧 session
- 每 60 秒自动刷新订阅

### API 接口

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/brokers` | 查询所有 Broker 连接状态 |
| GET | `/api/device/:host` | 查询指定主机的设备数据（自动订阅） |
| GET | `/api/lookup/:serial` | 通过芯片序列号查找主机名 |
| GET | `/api/control/:host` | 查询云控状态和指令日志 |
| POST | `/api/control/:host` | 下发云控指令（修改扫描模式/目标） |

### 云控功能

通过 MQTT 远程修改 O5c 设备的扫描配置：

- **扫描模式切换**：MAC 地址 / UUID
- **目标列表修改**：设置目标 MAC 或 UUID（多个用逗号分隔）
- **安全校验**：指令中携带 `token`（设备芯片序列号），设备端校验后才执行
- **去重机制**：通过 `ts` 时间戳防止重复执行（retain 消息）
- **执行确认**：设备执行后发布 ack，页面实时显示确认状态
- **配置持久化**：写入 ESP32 Preferences，断电重启后仍生效

指令 Topic：`{设备名}/cmd`（下行） / `{设备名}/cmd/ack`（上行确认）

---

## 首次部署

### 1. 安装 Node.js 和 Nginx

```bash
yum install -y epel-release
yum install -y nginx
systemctl enable nginx
```

> Node.js 未安装？运行 `start.sh` 会自动从国内镜像下载 Node.js 16（兼容 CentOS 7.6）

### 2. 域名解析

在 Namecheap **Advanced DNS** 页面，添加三条 A 记录：

| Type | Host | Value | TTL |
|------|------|-------|-----|
| A Record | `ble` | `38.76.204.187` | 30 min |
| A Record | `code` | `38.76.204.187` | 30 min |
| A Record | `mqtt` | `38.76.204.187` | 30 min |

### 3. 上传并一键部署

将 `A1` 文件夹上传到服务器 `/root/A1/`，然后执行：

```bash
cd /root/A1
sed -i 's/\r$//' deploy.sh
chmod +x deploy.sh
./deploy.sh
```

脚本自动完成：修复换行符 → 安装依赖 → 配置 Nginx 反向代理 → 注册 systemd 自启服务 → 启动。

---

## 日常管理（systemd）

部署完成后，三个服务由 systemd 管理，**开机自启、崩溃自动重启**。

```bash
# 查看状态
systemctl status ble-backend
systemctl status code-generator
systemctl status mqtt-viewer

# 重启服务
systemctl restart ble-backend
systemctl restart code-generator
systemctl restart mqtt-viewer

# 停止服务
systemctl stop ble-backend
systemctl stop code-generator
systemctl stop mqtt-viewer

# 查看日志
journalctl -u ble-backend -f
journalctl -u code-generator -f
journalctl -u mqtt-viewer -f
```

---

## 更新代码

重新上传 A1 文件夹后：

```bash
cd /root/A1
sed -i 's/\r$//' deploy.sh
chmod +x deploy.sh
./deploy.sh
```

---

## （可选）HTTPS 证书

```bash
yum install -y certbot python2-certbot-nginx
certbot --nginx -d ble.qyan10.store -d code.qyan10.store -d mqtt.qyan10.store
```

证书自动续期，配置完成后可通过 `https://` 访问。

---

## 数据备份

编号数据保存在 `codes.json` 文件中：

```bash
cp /root/A1/codes.json /root/A1/codes.json.bak
```

也可在后台页面「导出Excel」备份，通过「导入Excel」恢复。

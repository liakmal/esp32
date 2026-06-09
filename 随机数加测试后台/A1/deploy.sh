#!/bin/bash
echo "============================================"
echo "  一键部署脚本"
echo "  BLE 云后端 + 设备编号生成器"
echo "============================================"
echo ""

cd /root/A1

# 1. Fix Windows line endings
echo "[1/6] Fixing line endings..."
sed -i 's/\r$//' *.sh *.js
echo "[OK] Line endings fixed"
echo ""

# 2. Ensure Node.js is installed
if ! command -v node &>/dev/null || ! command -v npm &>/dev/null; then
    echo "[2/6] Node.js not found, installing from Chinese mirror..."
    NODE_VER="v16.20.2"
    ARCH=$(uname -m)
    if [ "$ARCH" = "x86_64" ]; then
        NODE_PKG="node-${NODE_VER}-linux-x64"
    elif [ "$ARCH" = "aarch64" ]; then
        NODE_PKG="node-${NODE_VER}-linux-arm64"
    else
        echo "[ERROR] Unsupported architecture: $ARCH"
        exit 1
    fi
    cd /usr/local/src
    curl -fSL "https://npmmirror.com/mirrors/node/${NODE_VER}/${NODE_PKG}.tar.xz" -o "${NODE_PKG}.tar.xz"
    if [ $? -ne 0 ]; then
        echo "[ERROR] Download Node.js failed!"
        exit 1
    fi
    tar xf "${NODE_PKG}.tar.xz"
    cp -rf ${NODE_PKG}/bin/* /usr/local/bin/
    cp -rf ${NODE_PKG}/lib/* /usr/local/lib/
    cp -rf ${NODE_PKG}/include/* /usr/local/include/ 2>/dev/null
    rm -rf "${NODE_PKG}" "${NODE_PKG}.tar.xz"
    cd /root/A1
    echo "[OK] Node.js $(node -v) installed"
else
    echo "[2/6] Node.js $(node -v) already installed, skipping"
fi
echo ""

# 3. Install npm dependencies
echo "[3/6] Installing npm dependencies..."
npm install --registry=https://registry.npmmirror.com
if [ $? -ne 0 ]; then
    echo "[ERROR] npm install failed!"
    exit 1
fi
echo "[OK] Dependencies installed"
echo ""

# 4. Setup Nginx reverse proxy
if ! command -v nginx &>/dev/null; then
    echo "[4/6] Nginx not found, installing..."
    if command -v apt-get &>/dev/null; then
        apt-get update -y && apt-get install -y nginx
    elif command -v yum &>/dev/null; then
        # Fix CentOS 7 EOL: switch to vault archive
        if grep -q "mirrorlist=" /etc/yum.repos.d/CentOS-Base.repo 2>/dev/null; then
            echo "  Fixing CentOS 7 EOL repos (switching to vault)..."
            sed -i 's|mirrorlist=|#mirrorlist=|g' /etc/yum.repos.d/CentOS-*.repo
            sed -i 's|#baseurl=http://mirror.centos.org|baseurl=http://vault.centos.org|g' /etc/yum.repos.d/CentOS-*.repo
            yum clean all >/dev/null 2>&1
        fi
        yum install -y epel-release && yum install -y nginx
    else
        echo "[ERROR] No supported package manager (apt/yum) found!"
        exit 1
    fi
    if ! command -v nginx &>/dev/null; then
        echo "[ERROR] Nginx installation failed! Please install manually:"
        echo "  Ubuntu/Debian: apt-get install -y nginx"
        echo "  CentOS/RHEL:   yum install -y epel-release && yum install -y nginx"
        exit 1
    fi
    systemctl enable nginx >/dev/null 2>&1
    echo "[OK] Nginx installed"
else
    echo "[4/6] Nginx already installed, skipping install"
fi
mkdir -p /etc/nginx/conf.d
echo "Configuring Nginx..."
cat > /etc/nginx/conf.d/ble.conf << 'NEOF'
server {
    listen 80;
    server_name ble.qyan10.store;
    location / {
        proxy_pass http://127.0.0.1:3001;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    }
}
NEOF

cat > /etc/nginx/conf.d/code.conf << 'NEOF'
server {
    listen 80;
    server_name code.qyan10.store;
    location / {
        proxy_pass http://127.0.0.1:3002;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    }
}
NEOF

cat > /etc/nginx/conf.d/mqtt.conf << 'NEOF'
server {
    listen 80;
    server_name mqtt.qyan10.store;
    location / {
        proxy_pass http://127.0.0.1:3003;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    }
}
NEOF

nginx -t
if [ $? -eq 0 ]; then
    systemctl start nginx 2>/dev/null
    systemctl reload nginx
    echo "[OK] Nginx configured and reloaded"
else
    echo "[ERROR] Nginx config test failed!"
    exit 1
fi
echo ""

# 5. Setup systemd auto-start services
echo "[5/6] Setting up auto-start services..."
cat > /etc/systemd/system/ble-backend.service << 'SEOF'
[Unit]
Description=BLE Cloud Backend
After=network.target

[Service]
Type=simple
WorkingDirectory=/root/A1
ExecStart=/usr/local/bin/node /root/A1/server.js
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
SEOF

cat > /etc/systemd/system/code-generator.service << 'SEOF'
[Unit]
Description=Device Code Generator
After=network.target

[Service]
Type=simple
WorkingDirectory=/root/A1
ExecStart=/usr/local/bin/node /root/A1/app.js
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
SEOF

cat > /etc/systemd/system/mqtt-viewer.service << 'SEOF'
[Unit]
Description=MQTT Data Viewer
After=network.target

[Service]
Type=simple
WorkingDirectory=/root/A1
ExecStart=/usr/local/bin/node /root/A1/mqtt.js
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
SEOF

systemctl daemon-reload
systemctl enable ble-backend
systemctl enable code-generator
systemctl enable mqtt-viewer
echo "[OK] Auto-start services created"
echo ""

# 6. Start/restart services
echo "[6/6] Starting services..."
systemctl restart ble-backend
systemctl restart code-generator
systemctl restart mqtt-viewer
sleep 3

# Check status
BLE_STATUS=$(systemctl is-active ble-backend)
CODE_STATUS=$(systemctl is-active code-generator)
MQTT_STATUS=$(systemctl is-active mqtt-viewer)
echo ""
echo "============================================"
echo "  Deployment complete!"
echo "============================================"
echo ""
echo "  BLE backend:      $BLE_STATUS"
echo "  Code generator:   $CODE_STATUS"
echo "  MQTT viewer:      $MQTT_STATUS"
echo ""
echo "  Access:"
echo "    http://ble.qyan10.store/admin"
echo "    http://code.qyan10.store/"
echo "    http://code.qyan10.store/admin"
echo "    http://mqtt.qyan10.store/"
echo "    http://mqtt.qyan10.store/admin"
echo ""
echo "  Manage:"
echo "    systemctl status ble-backend"
echo "    systemctl status code-generator"
echo "    systemctl status mqtt-viewer"
echo "    systemctl restart ble-backend"
echo "    systemctl restart code-generator"
echo "    systemctl restart mqtt-viewer"
echo "============================================"

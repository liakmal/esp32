#!/bin/bash
echo "=== Setting up auto-start services ==="

# Install dependencies first
echo "[INFO] Installing npm dependencies..."
cd /root/A1
npm install --registry=https://registry.npmmirror.com
echo "[OK] Dependencies installed"

# Create BLE backend service
cat > /etc/systemd/system/ble-backend.service << 'EOF'
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
EOF
echo "[OK] ble-backend.service created"

# Create code generator service
cat > /etc/systemd/system/code-generator.service << 'EOF'
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
EOF
echo "[OK] code-generator.service created"

# Reload and enable
systemctl daemon-reload
systemctl enable ble-backend
systemctl enable code-generator
systemctl start ble-backend
systemctl start code-generator

echo ""
echo "=== Done! Both services will auto-start on boot ==="
echo ""
echo "Check status:"
echo "  systemctl status ble-backend"
echo "  systemctl status code-generator"
echo ""
echo "View logs:"
echo "  journalctl -u ble-backend -f"
echo "  journalctl -u code-generator -f"

#!/bin/bash
echo "=== Setting up Nginx reverse proxy ==="

# Start nginx
systemctl start nginx

# Create BLE backend config
cat > /etc/nginx/conf.d/ble.conf << 'EOF'
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
EOF
echo "[OK] ble.conf created"

# Create code generator config
cat > /etc/nginx/conf.d/code.conf << 'EOF'
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
EOF
echo "[OK] code.conf created"

# Test and reload
nginx -t
if [ $? -eq 0 ]; then
    systemctl reload nginx
    echo "[OK] Nginx reloaded"
else
    echo "[ERROR] Nginx config test failed!"
    exit 1
fi

# Firewall
firewall-cmd --zone=public --add-port=80/tcp --permanent
firewall-cmd --reload
echo "[OK] Firewall port 80 opened"

echo ""
echo "=== Done! ==="
echo "  BLE:  http://ble.qyan10.store/admin"
echo "  Code: http://code.qyan10.store/"

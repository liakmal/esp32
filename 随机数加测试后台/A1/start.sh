#!/bin/bash
echo "============================================"
echo "  Device Code Generator - CentOS 7.6"
echo "============================================"
echo ""

# Go to script directory
cd "$(dirname "$0")"

# Check if Node.js is installed
if ! command -v node &> /dev/null; then
    echo "[INFO] Node.js not found, downloading v16 from China mirror..."
    echo "(Node.js 16 is the latest version compatible with CentOS 7.6)"
    echo ""
    cd /home
    curl -L -o node-v16.20.2-linux-x64.tar.xz https://npmmirror.com/mirrors/node/v16.20.2/node-v16.20.2-linux-x64.tar.xz
    if [ $? -ne 0 ]; then
        echo "[ERROR] Download failed! Please download manually:"
        echo "  https://npmmirror.com/mirrors/node/v16.20.2/node-v16.20.2-linux-x64.tar.xz"
        exit 1
    fi
    tar -xf node-v16.20.2-linux-x64.tar.xz
    ln -sf /home/node-v16.20.2-linux-x64/bin/node /usr/local/bin/node
    ln -sf /home/node-v16.20.2-linux-x64/bin/npm /usr/local/bin/npm
    ln -sf /home/node-v16.20.2-linux-x64/bin/npx /usr/local/bin/npx
    cd "$(dirname "$0")"
    echo "[OK] Node.js installed successfully"
fi

echo "[OK] Node.js version: $(node -v)"
echo ""

# Install dependencies
if [ ! -d "node_modules" ]; then
    echo "[INFO] Installing dependencies..."
    npm install --registry=https://registry.npmmirror.com
    if [ $? -ne 0 ]; then
        echo "[ERROR] npm install failed!"
        exit 1
    fi
    echo ""
fi

echo "[INFO] Starting both services..."
echo ""
echo "  BLE Cloud Backend:    http://localhost:3001/"
echo "  BLE Admin:            http://localhost:3001/admin"
echo "  Device Code Generator: http://localhost:3002/"
echo "  Device Code Admin:    http://localhost:3002/admin"
echo ""
echo "  Press Ctrl+C to stop all"
echo "============================================"
echo ""

# Start BLE backend in background
node server.js &
BLE_PID=$!

# Start device code generator in foreground
node app.js &
CODE_PID=$!

# Wait for both, stop all on Ctrl+C
trap "kill $BLE_PID $CODE_PID 2>/dev/null; exit" INT TERM
wait

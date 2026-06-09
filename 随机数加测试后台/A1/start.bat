@echo off
chcp 65001 >nul
title 三服务融合启动

echo.
echo ============================================
echo   三服务融合启动脚本
echo   BLE云后端 + 设备编号生成器 + MQTT数据查询
echo ============================================
echo.

rem 检查Node.js是否安装
node -v >nul 2>&1
if %errorlevel% neq 0 (
    echo [错误] 未检测到Node.js，请先安装Node.js
    echo 下载地址: https://nodejs.org/
    pause
    exit /b 1
)

echo [INFO] Node.js版本:
node -v

rem 检查依赖是否安装
if not exist "node_modules" (
    echo.
    echo [INFO] 首次运行，正在安装依赖包...
    npm install
    if %errorlevel% neq 0 (
        echo [错误] 依赖安装失败
        pause
        exit /b 1
    )
) else (
    echo [INFO] 依赖已存在，跳过安装
)

echo.
echo [INFO] 启动三个服务...
echo [INFO] BLE云后端: http://localhost:3001/admin
echo [INFO] 编号生成: http://localhost:3002/
echo [INFO] 编号管理: http://localhost:3002/admin  
echo [INFO] MQTT查询: http://localhost:3003/
echo [INFO] MQTT管理: http://localhost:3003/admin
echo [INFO] 按 Ctrl+C 可停止所有服务
echo.

rem 同时启动三个服务
start /B node server.js
start /B node app.js
start /B node mqtt.js

echo [INFO] 所有服务已启动
echo.
pause

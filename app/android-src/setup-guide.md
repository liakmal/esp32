# Android 项目配置步骤

完成以下步骤即可在 Android Studio 中编译并生成 APK。

## 前置条件
- 安装 [Android Studio](https://developer.android.com/studio)（含 JDK 17）
- 安装完成后在 Android Studio 中下载 SDK（API 33 或以上）

---

## 一、初始化 Capacitor Android 项目

在 `app/` 目录下执行：

```bash
npm install
npm run build
npx cap add android
npx cap sync android
```

---

## 二、复制 WiFi 插件到 Android 项目

将 `android-src/WifiPlugin.java` 复制到：

```
android/app/src/main/java/com/bctech/devicemanager/WifiPlugin.java
```

---

## 三、注册插件到 MainActivity

打开 `android/app/src/main/java/com/bctech/devicemanager/MainActivity.java`，修改为：

```java
package com.bctech.devicemanager;

import android.os.Bundle;
import com.getcapacitor.BridgeActivity;

public class MainActivity extends BridgeActivity {
    @Override
    public void onCreate(Bundle savedInstanceState) {
        registerPlugin(WifiPlugin.class);   // ← 添加这一行
        super.onCreate(savedInstanceState);
    }
}
```

---

## 四、添加 AndroidManifest 权限

打开 `android/app/src/main/AndroidManifest.xml`，在 `<manifest>` 标签内（`<application>` 之前）添加：

```xml
<uses-permission android:name="android.permission.ACCESS_WIFI_STATE" />
<uses-permission android:name="android.permission.CHANGE_WIFI_STATE" />
<uses-permission android:name="android.permission.ACCESS_FINE_LOCATION" />
<uses-permission android:name="android.permission.ACCESS_COARSE_LOCATION" />
<uses-permission android:name="android.permission.CHANGE_NETWORK_STATE" />
<uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />
<uses-permission android:name="android.permission.INTERNET" />
```

同时在 `<application>` 标签内添加（允许 HTTP 明文访问 192.168.4.1）：

```xml
android:usesCleartextTraffic="true"
```

---

## 五、在 Android Studio 中编译 APK

```bash
npx cap open android
```

在 Android Studio 中：
1. 菜单 → **Build → Build Bundle(s) / APK(s) → Build APK(s)**
2. APK 文件生成在 `android/app/build/outputs/apk/debug/app-debug.apk`

---

## 六、安装到手机

```bash
# 用数据线连接手机（开启USB调试）
adb install android/app/build/outputs/apk/debug/app-debug.apk
```

或直接将 `app-debug.apk` 传到手机安装。

---

## Android 10+ 说明

Android 10 以上连接 WiFi 时系统会弹出一个确认框：
> "是否允许 **设备配置助手** 连接到 O5-XXXX？"

用户点击 **连接** 后，App 会绑定到该网络并访问 192.168.4.1。  
这是 Android 的安全机制，无法绕过，属正常现象。

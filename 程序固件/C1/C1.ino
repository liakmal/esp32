#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLECharacteristic.h>
#include <esp_sleep.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <nvs_flash.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_mac.h>
#include <esp_task_wdt.h>
#include <Update.h>
#include <mbedtls/aes.h>
#include <map>

// OTA 加密密钥（必须与 encrypt_firmware.py 中的 OTA_KEY 一致）
static const uint8_t OTA_AES_KEY[16] = {
  0x4F, 0x35, 0xC1, 0xA7, 0x9E, 0x2B, 0x8D, 0x6F,
  0x1C, 0xE4, 0x73, 0xB9, 0x5A, 0xD2, 0x0F, 0x88
};
// 加密固件魔数头: C1 07 A0 01
static const uint8_t OTA_MAGIC[4] = {0xC1, 0x07, 0xA0, 0x01};
static bool otaEncrypted = false;
static uint32_t otaOriginalSize = 0;
static size_t otaHeaderParsed = 0;
static mbedtls_aes_context otaAesCtx;

// 硬件看门狗配置
#define WDT_TIMEOUT_SEC 60  // 60秒看门狗超时

// 硬件配置
#define STATUS_LED 13  // 状态指示灯引脚（初始化时高频闪烁）
#define CONFIG_BUTTON_PIN 9
#define BLE_MAC_OFFSET 2
#define PRESET_COUNT 3
#define MAX_ADV_DATA_LEN 31                 // BLE广播数据最大长度
#define SLEEP_TIMEOUT 1200000               // 20分钟深度睡眠超时(ms)
#define AUTO_RESTART_MS (30UL * 60 * 1000)  // 30分钟自动重启（清理内存碎片）

// 提前声明
class MyCallbacks;
void updateAdvertisingData(std::string newData);
void updateMacAddress(uint8_t* targetBleMac);
void updateMacAddress(std::string newMacStr);
String formatMacAddress(uint8_t* mac);
void getBluetoothMac(uint8_t* outMac);
bool saveMacToNVS(uint8_t* mac);
bool loadMacFromNVS(uint8_t* mac);
bool savePresetsToNVS();
bool loadPresetsFromNVS();
bool saveCustomDeviceNameToNVS();
bool loadCustomDeviceNameFromNVS();
void setStatusLed(bool state);
String getSleepRemainingTime();

// 全局状态
uint8_t baseMac[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
bool isAdvertising = false;
BLEAdvertising* pAdvertising = nullptr;
unsigned long startTime = 0;  // 记录启动时间

// 预设MAC结构体
struct Preset {
  uint8_t mac[6];
  String name;
} presets[PRESET_COUNT] = {
  { { 0xF8, 0xA7, 0x63, 0x9F, 0xF4, 0x8E }, "目标设备MAC" },
  { { 0xF8, 0xA7, 0x63, 0xA1, 0x9B, 0x86 }, "SuperMini实际MAC" },
  { { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 }, "备用MAC" }
};

// BLE广播数据 - 增加缓冲区大小以支持扫描响应数据
uint8_t bleRaw[64] = {
  0x02, 0x01, 0x06, 0x03, 0x03, 0x3C, 0xFE, 0x17, 0xFF, 0x00, 0x02,
  0x72, 0x00, 0x1F, 0x71, 0x5A, 0x4B, 0xA6, 0xBA, 0xB5, 0x00, 0xC4,
  0xF8, 0xA7, 0x63, 0x9E, 0xCA, 0xCC, 0x00, 0x02, 0x20
};
int bleDataLen = 31;     // 记录实际数据长度
int bleSplitIndex = 31;  // 记录广播包和扫描响应包的分割点
boolean rawMoreThan31 = false;
// uint8_t bleRaw32[] = {0x0C,0x09,0x52,0x54,0x4B,0x5F,0x42,0x54,0x5F,0x34,0x2E,0x31,0x00};

// BLE UUID定义
#define SERVICE_UUID "0000FE3C-0000-1000-8000-00805F9B34FB"
#define CHAR_ONE_UUID "0000FE1B-0000-1000-8000-00805F9B34FB"
#define CHAR_TWO_UUID "0000FE1C-0000-1000-8000-00805F9B34FB"

// AP配置
const char* AP_SSID = "ESP32-BLE-配置";
const char* AP_PASSWORD = "12345678";
IPAddress AP_LOCAL_IP = IPAddress(192, 168, 4, 1);
IPAddress AP_GATEWAY = IPAddress(192, 168, 4, 1);
IPAddress AP_SUBNET = IPAddress(255, 255, 255, 0);
const byte DNS_PORT = 53;

// Web服务器
AsyncWebServer server(80);
AsyncWebServer statusServer(8080);  // 状态AP服务器（配置完成后运行）
DNSServer dnsServer;

// 状态AP（运行时热点）
bool statusApActive = false;
String statusApSsid = "";

// 静默模式（关闭LED和配置WiFi热点，仅恢复出厂可解除）
bool silentMode = false;

// 互联网配置（与 O5 同一后台）
const char* DEFAULT_STA_SSID = "Xiaomi 14";         // 可作为首次引导默认
const char* DEFAULT_STA_PASSWORD = "asdzx12345";    // 可作为首次引导默认
String SERVER_BASE = "http://124.132.136.17:9025";  // 将被持久化覆盖

// 可变配置（从持久化加载）
String wifiSsid = "";
String wifiPass = "";
bool isConfigured = false;
bool apModeActive = false;
bool factoryResetHeld = false;
bool pendingRestart = false;
bool pendingFactoryReset = false;
unsigned long restartAt = 0;
Preferences preferences;

// C1 设备标识
String c1DeviceId = "";
String customDeviceName = "V999999";  // 自定义设备名称 - 直接在这里修改（建议使用英文避免URL编码问题）
String chipSerialNumber = "";         // ESP32芯片唯一序列号（用于MQTT客户端ID）

String mqttTargetMac = "";  // 目标MAC，用于过滤（可选）
String mqttHostName = "";   // 要订阅的主机名称（如 ASXD）

// MQTT配置
String mqttHost = "";
int mqttPort = 8883;
String mqttUser = "";
String mqttPass = "";
// Topic格式: {主机名称}/{mac}，例如 ASXD/F8A7639FF48E
String mqttSubscribedTopic = "";
unsigned long lastMqttMsgMs = 0;
int64_t lastAppliedDataTs = 0;
// 按MAC地址追踪时间戳，防止不同设备的消息互相覆盖
std::map<String, int64_t> macTimestamps;

static const char mqtt_ca_cert[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH
MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI
2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx
1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ
q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz
tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ
vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP
BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV
5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY
1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4
NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG
Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91
8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe
pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl
MrY=
-----END CERTIFICATE-----
)EOF";

WiFiClientSecure mqttNetClientTls;
WiFiClient mqttNetClientPlain;  // 用于非TLS连接（端口1883）
PubSubClient mqttClientTls(mqttNetClientTls);
PubSubClient mqttClientPlain(mqttNetClientPlain);

// ===== 多Broker同时监听（自动模式） =====
struct BrokerInfo {
  const char* host;
  int port;
};
static const BrokerInfo MULTI_BROKERS[] = {
  { "broker.emqx.io", 1883 },
  { "broker.hivemq.com", 1883 },
};
static const int MULTI_BROKER_COUNT = 2;

WiFiClient multiBrokerNet[3];
PubSubClient multiBrokerClient[3] = {
  PubSubClient(multiBrokerNet[0]),
  PubSubClient(multiBrokerNet[1]),
  PubSubClient(multiBrokerNet[2]),
};
String multiBrokerSubTopic[3] = {"", "", ""};
static unsigned long multiBrokerLastAttempt[3] = {0, 0, 0};
static unsigned long multiBrokerInterval[3] = {5000, 5000, 5000};
bool mqttAutoMode = true;  // true = 同时监听所有broker（默认开启）

// 获取当前活跃的MQTT客户端（根据端口选择TLS或明文）
PubSubClient& getActiveMqttClient() {
  return (mqttPort == 8883) ? mqttClientTls : mqttClientPlain;
}

// 数据模式: "mqtt" 或 "server"
String dataMode = "mqtt";

// 云端配置：启动后拉取一次目标蓝牙MAC与广播数据
String cloudTargetMac = "";
String cloudAdvData = "";
unsigned long lastCloudFetch = 0;
unsigned long lastCloudPoll = 0;
bool webDataReceived = false;  // 标记是否接收到Web数据
bool neverSleep = false;       // 永不休眠标志
bool dataSynced = false;

// Web调试日志
String debugLog = "";
const int MAX_DEBUG_LOG_SIZE = 8192;  // 最大日志大小8KB

void connectWiFiSTA();
void fetchCloudAdvOnce();
void tryApplyCloudAdv();
void reportC1StatusToServer();
void addDebugLog(String message);
void loadConfiguration();
bool saveConfiguration(const String& ssid, const String& pass, const String& serverBase, const String& deviceName = "",
                       const String& mode = "mqtt", const String& targetMac = "", bool neverSleepVal = false,
                       const String& mHost = "", int mPort = 1883, const String& mUser = "", const String& mPass = "", const String& mHostName = "");
void startFirstBootAP();
void stopFirstBootAP();
void startStatusAP();
void stopStatusAP();
void checkFactoryResetButton();
void factoryReset();

void mqttCallback(char* topic, byte* payload, unsigned int length);
bool ensureMqttConnection();
void mqttLoop();
String resolveMqttTopicForMac(const String& mac);

void migrateLegacyKeys();

String normalizeMacString(String s);
String macFromMqttTopic(const String& topic);
String resolveMqttSubscribeTopic();

void mqttSyncOnce(unsigned long timeoutMs);

int64_t parseDataTimestampFromJson(const String& json);

void startBleAdvertisingIfAllowed();

// BLE回调类
class MyCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* pCharacteristic) override {
    std::string value = pCharacteristic->getValue();
    Serial.printf("特性[%s] 读取: %s\n",
                  pCharacteristic->getUUID().toString().c_str(),
                  value.c_str());
    setStatusLed(true);
    delay(100);
    setStatusLed(false);
  }

  void onWrite(BLECharacteristic* pCharacteristic) override {
    std::string value = pCharacteristic->getValue();
    Serial.printf("特性[%s] 写入: %s\n",
                  pCharacteristic->getUUID().toString().c_str(),
                  value.c_str());

    if (pCharacteristic->getUUID().toString() == CHAR_TWO_UUID && !value.empty()) {
      updateAdvertisingData(value);
    }
    setStatusLed(true);
    delay(200);
    setStatusLed(false);
  }
};

// 智能配置广播数据（自动分包）
void configureAdvertising(uint8_t* rawData, int len) {
  int splitIndex = 0;
  int i = 0;

  // 遍历寻找最佳分割点（保证不截断BLE结构体）
  while (i < len) {
    int structLen = rawData[i];
    if (structLen == 0) break;           // 遇到无效长度，停止
    int totalStructLen = structLen + 1;  // 长度字节 + 数据

    if (i + totalStructLen > 31) {
      // 如果当前结构体加上去会超过31字节，则在这里分割
      splitIndex = i;
      break;
    }
    i += totalStructLen;
  }

  // 如果所有数据都能放入 (i >= len)
  if (i >= len) {
    splitIndex = len;
  }

  // 特殊情况：如果第一条数据就超过31字节（虽然不符合标准），或者是其他异常情况导致 splitIndex 为 0 但有数据
  if (splitIndex == 0 && len > 0) {
    // 强制分割，虽然可能破坏结构，但比不发好
    splitIndex = (len > 31) ? 31 : len;
  }

  Serial.printf("[BLE] 分包结果: 广播包 %d 字节, 响应包 %d 字节\n", splitIndex, len - splitIndex);

  esp_err_t err = esp_ble_gap_config_adv_data_raw(rawData, splitIndex);
  if (err != ESP_OK) {
    Serial.printf("广播数据配置失败: %d\n", err);
  }

  if (len > splitIndex) {
    err = esp_ble_gap_config_scan_rsp_data_raw(rawData + splitIndex, len - splitIndex);
    if (err != ESP_OK) {
      Serial.printf("扫描响应配置失败: %d\n", err);
    }
  }
}

// 计算广播包和扫描响应包的智能分割点
void calculateSplitIndex() {
  bleSplitIndex = 0;
  int currentIdx = 0;

  while (currentIdx < bleDataLen) {
    int len = bleRaw[currentIdx];
    if (len == 0) break;       // 防止死循环
    int structSize = len + 1;  // Length byte + Body

    // 检查加上当前结构体是否会超过31
    if (currentIdx + structSize > MAX_ADV_DATA_LEN) {
      // 超过了，当前结构体必须放入扫描响应
      // 分割点保持为上一个完整结构体的结束位置 (currentIdx)
      // 如果第一个结构体就超大 (currentIdx == 0)，强制设为31避免逻辑错误
      if (currentIdx == 0) {
        bleSplitIndex = MAX_ADV_DATA_LEN;
      } else {
        bleSplitIndex = currentIdx;
      }
      return;
    }

    // 没超过，继续
    currentIdx += structSize;
    bleSplitIndex = currentIdx;  // 更新分割点
  }
}

// 更新广播数据
void updateAdvertisingData(std::string newData) {
  Serial.println("\n=== 更新广播数据 ===");
  setStatusLed(true);

  if (isAdvertising && pAdvertising) {
    pAdvertising->stop();
    isAdvertising = false;
  }

  int dataIdx = 0;
  char* token = strtok((char*)newData.c_str(), ",");
  while (token != NULL && dataIdx < sizeof(bleRaw)) {
    if (strncmp(token, "0x", 2) == 0) token += 2;
    bleRaw[dataIdx++] = (uint8_t)strtol(token, NULL, 16);
    token = strtok(NULL, ",");
  }

  // 更新全局数据长度
  bleDataLen = dataIdx;

  // 使用智能分包配置
  configureAdvertising(bleRaw, bleDataLen);

  if (pAdvertising) {
    if (dataSynced) {
      pAdvertising->start();
      isAdvertising = true;
      Serial.println("广播更新成功");
    } else {
      Serial.println("广播已更新（等待同步完成后再启动）");
    }
  }

  setStatusLed(false);
}

void startBleAdvertisingIfAllowed() {
  if (!dataSynced) return;
  if (!pAdvertising) return;
  if (isAdvertising) return;
  pAdvertising->start();
  isAdvertising = true;
  addDebugLog("[BLE] 广播已启动");
}

// 静态回调实例（避免内存泄漏）
static MyCallbacks* pSharedCallbacks = nullptr;

// 上次设置的MAC（用于避免重复初始化）
static uint8_t lastSetBleMac[6] = { 0 };
static bool bleInitialized = false;

// 更新MAC地址（核心逻辑）
void updateMacAddress(uint8_t* targetBleMac) {
  Serial.printf("\n=== 更新蓝牙MAC: 目标=%s ===\n", formatMacAddress(targetBleMac).c_str());

  // 检查MAC是否真的改变了
  if (bleInitialized && memcmp(lastSetBleMac, targetBleMac, 6) == 0) {
    Serial.println("[BLE] MAC未改变，跳过重新初始化");
    addDebugLog("[BLE] MAC未改变，跳过重新初始化");
    return;
  }

  setStatusLed(true);
  esp_task_wdt_reset();  // 喂狗，因为BLE初始化较慢

  if (isAdvertising && pAdvertising) {
    pAdvertising->stop();
    isAdvertising = false;
  }

  // 计算基本MAC（目标-偏移）
  uint8_t newBaseMac[6];
  memcpy(newBaseMac, targetBleMac, 6);
  newBaseMac[5] = (uint8_t)(targetBleMac[5] - BLE_MAC_OFFSET);

  addDebugLog("[DEBUG] 目标蓝牙MAC: " + formatMacAddress(targetBleMac));
  addDebugLog("[DEBUG] 计算基本MAC: " + formatMacAddress(newBaseMac));

  // 读取当前基本MAC（设置前）
  uint8_t currentBaseMac[6];
  esp_read_mac(currentBaseMac, ESP_MAC_WIFI_STA);
  addDebugLog("[DEBUG] 设置前WiFi MAC: " + formatMacAddress(currentBaseMac));

  // 设置基本MAC
  esp_err_t err = esp_base_mac_addr_set(newBaseMac);
  if (err != ESP_OK) {
    addDebugLog("[ERROR] 基本MAC设置失败: " + String(err));
    setStatusLed(false);
    return;
  }
  memcpy(baseMac, newBaseMac, 6);

  // 验证设置后的MAC
  uint8_t verifyBaseMac[6];
  esp_read_mac(verifyBaseMac, ESP_MAC_WIFI_STA);
  addDebugLog("[DEBUG] 设置后WiFi MAC: " + formatMacAddress(verifyBaseMac));

  // 保存当前MAC到NVS
  saveMacToNVS(targetBleMac);

  // 重启BLE - 添加延时确保资源释放
  if (bleInitialized) {
    BLEDevice::deinit(false);  // false = 不释放内存，稍后手动处理
    delay(100);                // 等待资源释放
    esp_task_wdt_reset();
  }

  delay(50);
  BLEDevice::init("ESP32-C1-Mimicker");
  esp_task_wdt_reset();

  BLEServer* pServer = BLEDevice::createServer();
  BLEService* pService = pServer->createService(SERVICE_UUID);

  BLECharacteristic* pChar1 = pService->createCharacteristic(
    CHAR_ONE_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  BLECharacteristic* pChar2 = pService->createCharacteristic(
    CHAR_TWO_UUID,
    BLECharacteristic::PROPERTY_WRITE);

  // 使用共享回调实例，避免内存泄漏
  if (pSharedCallbacks == nullptr) {
    pSharedCallbacks = new MyCallbacks();
  }
  pChar1->setCallbacks(pSharedCallbacks);
  pChar2->setCallbacks(pSharedCallbacks);
  pService->start();

  pAdvertising = pServer->getAdvertising();
  BLEAdvertisementData advData;
  BLEAdvertisementData scanRespData;
  pAdvertising->setAdvertisementData(advData);
  pAdvertising->setScanResponseData(scanRespData);

  // 使用智能分包配置
  configureAdvertising(bleRaw, bleDataLen);

  if (dataSynced) {
    pAdvertising->start();
    isAdvertising = true;
  }

  // 记录已设置的MAC
  memcpy(lastSetBleMac, targetBleMac, 6);
  bleInitialized = true;

  // 验证实际MAC
  uint8_t actualBleMac[6];
  getBluetoothMac(actualBleMac);
  Serial.printf("MAC更新完成: 实际蓝牙MAC=%s\n", formatMacAddress(actualBleMac).c_str());
  setStatusLed(false);
}

// 重载：接收字符串格式MAC
void updateMacAddress(std::string newMacStr) {
  uint8_t targetBleMac[6];
  int macIdx = 0;
  char* token = strtok((char*)newMacStr.c_str(), ":");

  while (token != NULL && macIdx < 6) {
    targetBleMac[macIdx++] = (uint8_t)strtol(token, NULL, 16);
    token = strtok(NULL, ":");
  }

  if (macIdx == 6) {
    updateMacAddress(targetBleMac);
  } else {
    Serial.println("MAC格式错误（需XX:XX:XX:XX:XX:XX）");
  }
}

// 格式化MAC地址
String formatMacAddress(uint8_t* mac) {
  String result;
  for (int i = 0; i < 6; i++) {
    if (mac[i] < 0x10) result += "0";
    result += String(mac[i], HEX);
    if (i < 5) result += ":";
  }
  result.toUpperCase();
  return result;
}

// 获取实际蓝牙MAC
void getBluetoothMac(uint8_t* outMac) {
  esp_read_mac(outMac, ESP_MAC_BT);
}

// 保存当前MAC到NVS
bool saveMacToNVS(uint8_t* mac) {
  nvs_handle_t nvsHandle;
  esp_err_t err = nvs_open("ble_config", NVS_READWRITE, &nvsHandle);
  if (err != ESP_OK) return false;

  err = nvs_set_blob(nvsHandle, "last_mac", mac, 6);
  if (err != ESP_OK) {
    nvs_close(nvsHandle);
    return false;
  }

  err = nvs_commit(nvsHandle);
  nvs_close(nvsHandle);
  return err == ESP_OK;
}

// 从NVS加载当前MAC
bool loadMacFromNVS(uint8_t* mac) {
  nvs_handle_t nvsHandle;
  esp_err_t err = nvs_open("ble_config", NVS_READONLY, &nvsHandle);
  if (err != ESP_OK) return false;

  size_t macSize = 6;
  err = nvs_get_blob(nvsHandle, "last_mac", mac, &macSize);
  nvs_close(nvsHandle);

  return (err == ESP_OK && macSize == 6);
}

// 保存预设到NVS
bool savePresetsToNVS() {
  nvs_handle_t nvsHandle;
  esp_err_t err = nvs_open("ble_config", NVS_READWRITE, &nvsHandle);
  if (err != ESP_OK) return false;

  // 保存预设数量
  err = nvs_set_u8(nvsHandle, "preset_count", PRESET_COUNT);
  if (err != ESP_OK) {
    nvs_close(nvsHandle);
    return false;
  }

  // 逐个保存预设
  for (int i = 0; i < PRESET_COUNT; i++) {
    String nameKey = "preset_name_" + String(i);
    err = nvs_set_str(nvsHandle, nameKey.c_str(), presets[i].name.c_str());
    if (err != ESP_OK) break;

    String macKey = "preset_mac_" + String(i);
    err = nvs_set_blob(nvsHandle, macKey.c_str(), presets[i].mac, 6);
    if (err != ESP_OK) break;
  }

  if (err != ESP_OK) {
    nvs_close(nvsHandle);
    return false;
  }

  err = nvs_commit(nvsHandle);
  nvs_close(nvsHandle);
  return err == ESP_OK;
}

// 从NVS加载预设
bool loadPresetsFromNVS() {
  nvs_handle_t nvsHandle;
  esp_err_t err = nvs_open("ble_config", NVS_READONLY, &nvsHandle);
  if (err != ESP_OK) return false;

  // 验证预设数量
  uint8_t count;
  err = nvs_get_u8(nvsHandle, "preset_count", &count);
  if (err != ESP_OK || count != PRESET_COUNT) {
    nvs_close(nvsHandle);
    return false;
  }

  // 逐个加载预设
  for (int i = 0; i < PRESET_COUNT; i++) {
    String nameKey = "preset_name_" + String(i);
    size_t nameLen;
    err = nvs_get_str(nvsHandle, nameKey.c_str(), NULL, &nameLen);
    if (err != ESP_OK || nameLen == 0) break;

    char* nameBuf = new char[nameLen];
    err = nvs_get_str(nvsHandle, nameKey.c_str(), nameBuf, &nameLen);
    if (err == ESP_OK) {
      presets[i].name = String(nameBuf);
    }
    delete[] nameBuf;
    if (err != ESP_OK) break;

    String macKey = "preset_mac_" + String(i);
    size_t macSize = 6;
    err = nvs_get_blob(nvsHandle, macKey.c_str(), presets[i].mac, &macSize);
    if (err != ESP_OK || macSize != 6) break;
  }

  nvs_close(nvsHandle);
  return err == ESP_OK;
}

// 保存自定义设备名称到NVS
bool saveCustomDeviceNameToNVS() {
  nvs_handle_t nvsHandle;
  esp_err_t err = nvs_open("ble_config", NVS_READWRITE, &nvsHandle);
  if (err != ESP_OK) return false;

  err = nvs_set_str(nvsHandle, "custom_device_name", customDeviceName.c_str());
  if (err != ESP_OK) {
    nvs_close(nvsHandle);
    return false;
  }

  err = nvs_commit(nvsHandle);
  nvs_close(nvsHandle);
  return err == ESP_OK;
}

// 从NVS加载自定义设备名称
bool loadCustomDeviceNameFromNVS() {
  nvs_handle_t nvsHandle;
  esp_err_t err = nvs_open("ble_config", NVS_READONLY, &nvsHandle);
  if (err != ESP_OK) return false;

  size_t nameLen;
  err = nvs_get_str(nvsHandle, "custom_device_name", NULL, &nameLen);
  if (err != ESP_OK || nameLen == 0) {
    nvs_close(nvsHandle);
    return false;
  }

  char* nameBuf = new char[nameLen];
  err = nvs_get_str(nvsHandle, "custom_device_name", nameBuf, &nameLen);
  if (err == ESP_OK) {
    customDeviceName = String(nameBuf);
  }
  delete[] nameBuf;
  nvs_close(nvsHandle);
  return err == ESP_OK;
}

// 状态指示灯控制
void setStatusLed(bool state) {
  if (silentMode) {
    digitalWrite(STATUS_LED, LOW);
    return;
  }
  digitalWrite(STATUS_LED, state ? HIGH : LOW);
}

// 获取剩余睡眠时间
String getSleepRemainingTime() {
  unsigned long elapsed = millis() - startTime;
  unsigned long remaining = SLEEP_TIMEOUT - elapsed;

  if (remaining <= 0) return "即将进入睡眠";

  int minutes = remaining / 60000;
  int seconds = (remaining % 60000) / 1000;

  return String(minutes) + "分" + String(seconds) + "秒";
}

// 初始化AP和DNS
void setupAPAndDNS() {
  Serial.println("\n=== 启动AP和DNS服务器 ===");
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_LOCAL_IP, AP_GATEWAY, AP_SUBNET);
  String apSsid = String("C1-Setup-") + String((uint32_t)ESP.getEfuseMac(), HEX);
  WiFi.softAP(apSsid.c_str(), AP_PASSWORD);
  Serial.printf("AP启动: %s, IP: %s\n", apSsid.c_str(), WiFi.softAPIP().toString().c_str());

  // 配置DNS服务器：所有域名解析到本地IP
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", AP_LOCAL_IP);
  Serial.println("DNS服务器启动：所有请求将重定向到配置中心");
}

// ===== 配置与AP门户 =====
void loadConfiguration() {
  migrateLegacyKeys();

  preferences.begin("c1cfg", true);
  isConfigured = preferences.getBool("configured", false);
  String ssid = preferences.getString("ssid", "");
  String pass = preferences.getString("pass", "");
  String server = preferences.getString("server", SERVER_BASE);
  String deviceName = preferences.getString("device_name", "");
  mqttTargetMac = preferences.getString("mqtt_target_mac", "");
  mqttHostName = preferences.getString("mqtt_h_name", "");  // 主机名称
  mqttHost = preferences.getString("mqtt_host", "");
  mqttPort = preferences.getInt("mqtt_port", 8883);
  mqttUser = preferences.getString("mqtt_user", "");
  mqttPass = preferences.getString("mqtt_pass", "");

  dataMode = preferences.getString("data_mode", "mqtt");
  neverSleep = preferences.getBool("never_sleep", false);
  silentMode = preferences.getBool("silent", false);
  mqttAutoMode = preferences.getBool("mqtt_auto", true);
  preferences.end();

  if (silentMode) {
    Serial.println("[CONFIG] 静默模式已开启（LED关闭、配置热点关闭）");
  }

  wifiSsid = ssid;
  wifiPass = pass;
  SERVER_BASE = server;
  if (deviceName.length() > 0) {
    customDeviceName = deviceName;
  }

  Serial.printf("[CONFIG] isConfigured=%s, SSID='%s', SERVER_BASE='%s', DeviceName='%s'\n",
                isConfigured ? "true" : "false",
                wifiSsid.c_str(), SERVER_BASE.c_str(), customDeviceName.c_str());
  Serial.printf("[CONFIG] DataMode='%s'\n", dataMode.c_str());
  Serial.printf("[CONFIG] MQTT: Host='%s', Port=%d, User='%s', Pass='%s'\n",
                mqttHost.c_str(), mqttPort, mqttUser.c_str(), mqttPass.length() > 0 ? "***" : "");
  Serial.printf("[CONFIG] MQTT HostName='%s', TargetMac='%s'\n", mqttHostName.c_str(), mqttTargetMac.c_str());
  Serial.printf("[CONFIG] 休眠模式: %s\n", neverSleep ? "永不休眠" : "20分钟自动休眠");
}

void migrateLegacyKeys() {
  preferences.begin("c1cfg", false);
  String cur = preferences.getString("mqtt_target_mac", "");
  String legacy = preferences.getString("lc_target_mac", "");

  if (preferences.isKey("lc_app_id") || preferences.isKey("lc_app_key") || preferences.isKey("lc_api_url") || preferences.isKey("lc_target_mac")) {
    preferences.putString("data_mode", "mqtt");
  }

  if (cur.length() == 0 && legacy.length() > 0) {
    preferences.putString("mqtt_target_mac", legacy);
  }

  if (preferences.isKey("lc_target_mac")) {
    preferences.remove("lc_target_mac");
  }
  if (preferences.isKey("lc_app_id")) {
    preferences.remove("lc_app_id");
  }
  if (preferences.isKey("lc_app_key")) {
    preferences.remove("lc_app_key");
  }
  if (preferences.isKey("lc_api_url")) {
    preferences.remove("lc_api_url");
  }
  preferences.end();
}

bool saveConfiguration(const String& ssid, const String& pass, const String& serverBase, const String& deviceName,
                       const String& mode, const String& targetMac, bool neverSleepVal,
                       const String& mHost, int mPort, const String& mUser, const String& mPass, const String& mHostName) {
  preferences.begin("c1cfg", false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", pass);
  preferences.putString("server", serverBase);
  if (deviceName.length() > 0) {
    preferences.putString("device_name", deviceName);
    customDeviceName = deviceName;
  }

  preferences.putString("mqtt_target_mac", targetMac);
  if (mHostName.length() > 0) preferences.putString("mqtt_h_name", mHostName);  // 主机名称

  if (mHost.length() > 0) preferences.putString("mqtt_host", mHost);
  preferences.putInt("mqtt_port", mPort);
  if (mUser.length() > 0) preferences.putString("mqtt_user", mUser);
  if (mPass.length() > 0) preferences.putString("mqtt_pass", mPass);

  preferences.putString("data_mode", mode);
  preferences.putBool("never_sleep", neverSleepVal);
  preferences.putBool("mqtt_auto", mqttAutoMode);

  preferences.putBool("configured", true);
  preferences.end();

  wifiSsid = ssid;
  wifiPass = pass;
  SERVER_BASE = serverBase;

  mqttTargetMac = targetMac;
  mqttHostName = mHostName;

  mqttHost = mHost;
  mqttPort = mPort;
  mqttUser = mUser;
  mqttPass = mPass;

  dataMode = mode;
  neverSleep = neverSleepVal;

  isConfigured = true;
  addDebugLog("[CONFIG] 保存配置成功, HostName=" + mqttHostName + ", TargetMac=" + mqttTargetMac);
  return true;
}

String resolveMqttTopicForMac(const String& mac) {
  // Topic格式: {主机名称}/{mac}
  String prefix = mqttHostName.length() > 0 ? mqttHostName : "o5";
  String normalizedMac = mac;
  normalizedMac.replace(":", "");
  normalizedMac.toUpperCase();
  return prefix + "/" + normalizedMac;
}

String normalizeMacString(String s) {
  s.trim();
  s.toUpperCase();
  s.replace("-", "");
  s.replace(":", "");
  s.replace(" ", "");
  if (s.length() != 12) return "";
  String out;
  out.reserve(17);
  for (int i = 0; i < 12; i += 2) {
    if (i > 0) out += ":";
    out += s.substring(i, i + 2);
  }
  return out;
}

String macFromMqttTopic(const String& topic) {
  // Topic格式: {主机名称}/{mac}，从topic中提取mac部分
  int idx = topic.lastIndexOf('/');
  if (idx < 0 || idx + 1 >= (int)topic.length()) return "";
  String tail = topic.substring(idx + 1);
  if (tail.length() == 17) {
    tail.trim();
    tail.toUpperCase();
    return tail;
  }
  return normalizeMacString(tail);
}

String resolveMqttSubscribeTopic() {
  // Topic格式: {主机名称}/{mac}
  // 如果主机名称为空，使用默认前缀 "o5"
  String prefix = mqttHostName.length() > 0 ? mqttHostName : "o5";

  // 如果配置了目标MAC，只订阅该MAC的消息；否则订阅所有（通配符+）
  if (mqttTargetMac.length() > 0) {
    String normalizedMac = mqttTargetMac;
    normalizedMac.replace(":", "");
    normalizedMac.toUpperCase();
    return prefix + "/" + normalizedMac;
  } else {
    // 订阅所有该主机下的消息
    return prefix + "/+";
  }
}

int64_t parseDataTimestampFromJson(const String& json) {
  if (!json.startsWith("{")) return 0;
  DynamicJsonDocument doc(256);
  DeserializationError err = deserializeJson(doc, json);
  if (err) return 0;

  if (doc.containsKey("ts")) {
    return doc["ts"].as<long long>();
  }
  if (doc.containsKey("updatedAt")) {
    return doc["updatedAt"].as<long long>();
  }
  return 0;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String t = String(topic);
  String msg;
  msg.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  lastMqttMsgMs = millis();

  // 打印详细的接收日志
  Serial.println("\n[MQTT] ========== 收到消息 ==========");
  Serial.printf("[MQTT] Topic: %s\n", t.c_str());
  Serial.printf("[MQTT] 长度: %d bytes\n", length);
  addDebugLog("[MQTT] === 收到消息 ===");
  addDebugLog("[MQTT] topic=" + t);
  addDebugLog("[MQTT] len=" + String(length) + " bytes");
  if (length < 500) {
    addDebugLog("[MQTT] payload=" + msg);
    Serial.printf("[MQTT] Payload: %s\n", msg.c_str());
  } else {
    addDebugLog("[MQTT] payload(截断)=" + msg.substring(0, 200) + "...");
    Serial.printf("[MQTT] Payload(截断): %s...\n", msg.substring(0, 200).c_str());
  }
  Serial.println("[MQTT] ==================================\n");

  // 过滤非广播数据的消息（心跳、配置状态等）
  if (msg.startsWith("{")) {
    DynamicJsonDocument typeCheck(256);
    if (!deserializeJson(typeCheck, msg) && typeCheck.containsKey("type")) {
      String msgType = typeCheck["type"].as<String>();
      if (msgType == "heartbeat" || msgType == "config_status") {
        Serial.printf("[MQTT] 忽略非广播消息 type=%s\n", msgType.c_str());
        return;
      }
    }
  }

  // 先解析消息内容
  String newMac = "";
  String newAdv = "";
  int64_t incomingTs = parseDataTimestampFromJson(msg);

  if (msg.startsWith("{")) {
    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, msg);
    if (!err) {
      if (doc.containsKey("mac")) newMac = doc["mac"].as<String>();
      if (doc.containsKey("advData")) newAdv = doc["advData"].as<String>();
      if (newAdv.length() == 0 && doc.containsKey("data")) newAdv = doc["data"].as<String>();
    }
  }

  if (newMac.length() == 0) {
    newMac = macFromMqttTopic(t);
  }

  if (newAdv.length() == 0) {
    newAdv = msg;
  }

  if (newMac.length() == 0) {
    // 如果 payload 没带 mac，则使用当前订阅的目标
    if (cloudTargetMac.length() > 0) newMac = cloudTargetMac;
    else if (mqttTargetMac.length() > 0) newMac = mqttTargetMac;
  }

  // 按MAC地址检查时间戳（不同设备的时间戳独立，不互相覆盖）
  String macKey = newMac;
  macKey.replace(":", "");
  macKey.toUpperCase();

  if (incomingTs > 0 && macKey.length() > 0) {
    int64_t lastTsForMac = 0;
    if (macTimestamps.count(macKey) > 0) {
      lastTsForMac = macTimestamps[macKey];
    }
    // 仅在时间戳量级相同时才比较（防止millis vs Unix epoch冲突）
    bool incomingIsEpoch = incomingTs > 1577836800LL;   // > 2020-01-01
    bool lastIsEpoch     = lastTsForMac > 1577836800LL;
    if (lastTsForMac > 0 && incomingTs <= lastTsForMac && incomingIsEpoch == lastIsEpoch) {
      addDebugLog("[MQTT] 忽略旧数据 mac=" + macKey + " ts=" + String((long long)incomingTs) + " last=" + String((long long)lastTsForMac));
      return;
    }
  }

  newMac.trim();
  newMac.toUpperCase();
  newAdv.trim();

  if (newMac.length() != 17) {
    String norm = normalizeMacString(newMac);
    if (norm.length() == 17) newMac = norm;
  }

  if (newMac.length() == 17) {
    cloudTargetMac = newMac;
    updateMacAddress(std::string(newMac.c_str()));
  }

  if (newAdv.length() > 0) {
    // 有效性校验：必须是合法的BLE hex数据（去掉0x前缀后纯hex字符、长度>=6且为偶数）
    String validateAdv = newAdv;
    validateAdv.replace("0x", "");
    validateAdv.replace("0X", "");
    validateAdv.trim();
    bool isValidHex = (validateAdv.length() >= 6 && (validateAdv.length() % 2) == 0);
    for (int k = 0; k < (int)validateAdv.length() && isValidHex; k++) {
      char c = validateAdv[k];
      if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
        isValidHex = false;
      }
    }
    if (!isValidHex) {
      addDebugLog("[MQTT] 忽略非hex数据(可能是JSON或retained无效消息) len=" + String(newAdv.length()));
      return;
    }
    cloudAdvData = newAdv;
    webDataReceived = true;
    tryApplyCloudAdv();
    // 保存该MAC的时间戳
    if (incomingTs > 0 && macKey.length() > 0) {
      macTimestamps[macKey] = incomingTs;
      addDebugLog("[MQTT] 更新MAC时间戳 mac=" + macKey + " ts=" + String((long long)incomingTs));
    }
    addDebugLog("[MQTT] 数据应用完成 mac=" + newMac);
  }
}

// MQTT重连退避机制变量
static unsigned long lastMqttConnectAttempt = 0;
static unsigned long mqttReconnectInterval = 5000;               // 初始5秒
static const unsigned long MQTT_RECONNECT_MAX_INTERVAL = 60000;  // 最大60秒
static int mqttConnectFailCount = 0;

// 多Broker自动模式：连接并订阅单个broker
bool ensureMultiBrokerConnection(int idx) {
  if (WiFi.status() != WL_CONNECTED) return false;
  PubSubClient& c = multiBrokerClient[idx];
  if (!c.connected()) {
    unsigned long now = millis();
    if (now - multiBrokerLastAttempt[idx] < multiBrokerInterval[idx]) return false;
    multiBrokerLastAttempt[idx] = now;
    c.setServer(MULTI_BROKERS[idx].host, MULTI_BROKERS[idx].port);
    c.setCallback(mqttCallback);
    c.setBufferSize(1024);
    String clientId = String("C1-") + chipSerialNumber + "-" + String(idx);
    bool ok = c.connect(clientId.c_str());
    if (!ok) {
      multiBrokerInterval[idx] = min(multiBrokerInterval[idx] * 2, (unsigned long)60000);
      addDebugLog("[MQTT-AUTO] 连接失败 broker=" + String(MULTI_BROKERS[idx].host));
      return false;
    }
    multiBrokerInterval[idx] = 5000;
    multiBrokerSubTopic[idx] = "";
    addDebugLog("[MQTT-AUTO] 连接成功 broker=" + String(MULTI_BROKERS[idx].host));
  }
  String topic = resolveMqttSubscribeTopic();
  if (topic != multiBrokerSubTopic[idx]) {
    if (c.subscribe(topic.c_str())) {
      multiBrokerSubTopic[idx] = topic;
      addDebugLog("[MQTT-AUTO] 订阅成功 broker=" + String(MULTI_BROKERS[idx].host) + " topic=" + topic);
    }
  }
  return true;
}

bool ensureMqttConnection() {
  if (dataMode != "mqtt") return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  // 自动模式：由mqttLoop处理多broker
  if (mqttAutoMode) return true;

  if (mqttHost.length() == 0) {
    addDebugLog("[MQTT] mqttHost 为空，无法连接");
    return false;
  }

  PubSubClient& mqttClient = getActiveMqttClient();

  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastMqttConnectAttempt < mqttReconnectInterval) {
      return false;
    }
    lastMqttConnectAttempt = now;

    if (mqttPort == 8883) {
      mqttNetClientTls.setCACert(mqtt_ca_cert);
    }

    mqttClient.setServer(mqttHost.c_str(), mqttPort);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(1024);

    String clientId = String("C1-") + chipSerialNumber;
    addDebugLog("[MQTT] 正在连接... host=" + mqttHost + " port=" + String(mqttPort));

    bool ok;
    if (mqttUser.length() > 0) {
      ok = mqttClient.connect(clientId.c_str(), mqttUser.c_str(), mqttPass.c_str());
    } else {
      ok = mqttClient.connect(clientId.c_str());
    }

    if (!ok) {
      mqttConnectFailCount++;
      mqttReconnectInterval = min(mqttReconnectInterval * 2, MQTT_RECONNECT_MAX_INTERVAL);
      addDebugLog("[MQTT] 连接失败 host=" + mqttHost + " state=" + String(mqttClient.state()));
      return false;
    }

    mqttConnectFailCount = 0;
    mqttReconnectInterval = 5000;
    addDebugLog("[MQTT] 连接成功！ host=" + mqttHost);
    mqttSubscribedTopic = "";
  }

  String topic = resolveMqttSubscribeTopic();
  if (topic != mqttSubscribedTopic) {
    bool ok = mqttClient.subscribe(topic.c_str());
    if (!ok) {
      addDebugLog("[MQTT] subscribe 失败 topic=" + topic);
      return false;
    }
    mqttSubscribedTopic = topic;
    addDebugLog("[MQTT] subscribe 成功 topic=" + topic);
    addDebugLog("[MQTT] 订阅前缀(mqttHostName)=" + (mqttHostName.length() > 0 ? mqttHostName : "o5(默认)"));
    addDebugLog("[MQTT] 目标MAC(mqttTargetMac)=" + (mqttTargetMac.length() > 0 ? mqttTargetMac : "(空，使用通配符)"));
  }
  return true;
}

void mqttLoop() {
  if (dataMode != "mqtt") return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (mqttAutoMode) {
    // 自动模式：同时监听所有broker
    for (int i = 0; i < MULTI_BROKER_COUNT; i++) {
      ensureMultiBrokerConnection(i);
      if (multiBrokerClient[i].connected()) {
        multiBrokerClient[i].loop();
      }
    }
    return;
  }

  if (!ensureMqttConnection()) return;
  getActiveMqttClient().loop();
}

void mqttSyncOnce(unsigned long timeoutMs) {
  if (dataMode != "mqtt") return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (mqttAutoMode) {
    // 自动模式：先确保所有broker连接，再轮询等待消息
    for (int i = 0; i < MULTI_BROKER_COUNT; i++) {
      ensureMultiBrokerConnection(i);
    }
  } else {
    if (!ensureMqttConnection()) return;
  }

  unsigned long before = lastMqttMsgMs;
  unsigned long start = millis();
  unsigned long lastWdtReset = millis();
  while (millis() - start < timeoutMs) {
    if (mqttAutoMode) {
      for (int i = 0; i < MULTI_BROKER_COUNT; i++) {
        if (multiBrokerClient[i].connected()) multiBrokerClient[i].loop();
      }
    } else {
      getActiveMqttClient().loop();
    }
    delay(10);
    if (millis() - lastWdtReset > 500) {
      esp_task_wdt_reset();
      lastWdtReset = millis();
    }
    if (lastMqttMsgMs != before) {
      break;
    }
  }

  if (lastMqttMsgMs == before) {
    addDebugLog("[MQTT] 启动同步未收到消息（请确认发布端使用 retain）");
  }
}

void startFirstBootAP() {
  addDebugLog("[AP] 启动首次配置端点 / (GET) 与 /save (POST)");

  // 预扫描周边WiFi，供页面展示
  int n = WiFi.scanNetworks();
  String options = "";
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.length() > 0) options += "<option>" + s + "</option>";
  }

  // 根路径表单
  server.on("/", HTTP_GET, [options](AsyncWebServerRequest* request) mutable {
    String page = R"HTML(
<!DOCTYPE html>
<html lang='zh-CN'>
<head>
  <meta charset='utf-8'>
  <meta name='viewport' content='width=device-width,initial-scale=1.0'>
  <title>设备初始化配置</title>
  <style>
    *{box-sizing:border-box;margin:0;padding:0;}
    body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Arial,sans-serif;background:#f0f2f5;padding:16px;color:#333;}
    .wrap{max-width:480px;margin:0 auto;}
    h1{font-size:17px;font-weight:700;color:#222;margin-bottom:14px;letter-spacing:.3px;}
    .card{background:#fff;border-radius:12px;box-shadow:0 1px 6px rgba(0,0,0,.07);padding:16px 16px 8px;margin-bottom:12px;}
    .sec{font-size:12px;font-weight:700;color:#888;text-transform:uppercase;letter-spacing:.8px;margin-bottom:10px;}
    label{display:block;font-size:14px;color:#333;margin-bottom:5px;margin-top:14px;}
    label:first-of-type{margin-top:0;}
    input[type=text],input[type=number],input[type=password],select{
      width:100%;padding:10px 12px;border:1px solid #e5e7eb;border-radius:8px;
      font-size:16px;color:#333;background:#fafafa;outline:none;
      -webkit-appearance:none;appearance:none;}
    input:focus,select:focus{border-color:#7eb8f7;background:#fff;}
    small{font-size:12px;color:#888;display:block;margin-top:5px;line-height:1.5;}
    .row{display:flex;align-items:center;gap:8px;}
    .row select{flex:1;}
    .scan-btn{padding:10px 14px;background:#f0f0f0;color:#444;border:none;border-radius:8px;font-size:14px;white-space:nowrap;flex-shrink:0;}
    #scanStatus{font-size:13px;color:#555;margin-top:5px;min-height:18px;}
    .cb-row{display:flex;align-items:center;gap:10px;padding:4px 0 8px;}
    .cb-row input[type=checkbox]{width:18px;height:18px;flex-shrink:0;accent-color:#5b8dee;}
    .cb-row label{margin:0;font-size:14px;color:#444;font-weight:normal;}
    .submit-btn{display:block;width:100%;padding:14px;background:#5b8dee;color:#fff;border:none;border-radius:10px;font-size:16px;font-weight:600;margin-top:4px;cursor:pointer;}
    .submit-btn:active{background:#4a7de0;}
    .req{color:#e05c65;font-size:12px;margin-left:4px;}
  </style>
</head>
<body>
  <div class='wrap'>
  <h1>设备初始化配置</h1>
  <form method='POST' action='/save'>
    <div class='card'>
      <div class='sec'>基本信息</div>
      <label>设备名称</label>
      <input type='text' name='device_name' value=')HTML"
                  + customDeviceName + R"HTML(' placeholder='例如：V999999' maxlength='32' required>
      <label>WiFi 网络</label>
      <div class='row'>
        <select name='ssid' id='ssidSelect' required>)HTML"
                  + options + R"HTML(</select>
        <button type='button' class='scan-btn' onclick='scanWifi()' id='scanBtn'>扫描</button>
      </div>
      <div id='scanStatus'></div>
      <label>WiFi 密码</label>
      <input type='text' name='pass' value=')HTML"
                  + wifiPass + R"HTML(' placeholder='留空表示无密码'>
    </div>

    <div class='card'>
      <div class='sec'>数据传输</div>
      <input type='hidden' name='data_mode' value='mqtt'>

      <div id='mqtt-fields'>
        <label>Broker 选择</label>
        <select id='mqtt_preset' name='mqtt_preset' onchange='toggleMqttPreset()'>
          <option value='auto' )HTML"
                  + (mqttAutoMode ? "selected" : "") + R"HTML(>自动（同时监听全部）</option>
          <option value='emqx' )HTML"
                  + (!mqttAutoMode && (mqttHost == "broker.emqx.io" || mqttHost.length() == 0) ? "selected" : "") + R"HTML(>EMQX 国际服务器</option>
          <option value='hivemq' )HTML"
                  + (!mqttAutoMode && mqttHost == "broker.hivemq.com" ? "selected" : "") + R"HTML(>HiveMQ 国际服务器</option>
          <option value='custom' )HTML"
                  + (!mqttAutoMode && mqttHost.length() > 0 && mqttHost != "broker.emqx.io" && mqttHost != "broker.hivemq.com" ? "selected" : "") + R"HTML(>自定义</option>
        </select>

        <div id='mqtt_custom_fields' style='display:none'>
          <label>Broker Host</label>
          <input type='text' id='mqtt_host_input' name='mqtt_host' value=')HTML"
                  + mqttHost + R"HTML(' placeholder='例如：broker.hivemq.com'>
          <label>Broker Port</label>
          <input type='number' id='mqtt_port_input' name='mqtt_port' value=')HTML"
                  + String(mqttPort) + R"HTML(' placeholder='1883'>
          <label>用户名 (可选)</label>
          <input type='text' id='mqtt_user_input' name='mqtt_user' value=')HTML"
                  + mqttUser + R"HTML(' placeholder='MQTT Username'>
          <label>密码 (可选)</label>
          <input type='text' id='mqtt_pass_input' name='mqtt_pass' value=')HTML"
                  + mqttPass + R"HTML(' placeholder='MQTT Password'>
        </div>

        <label>序列号 <span class='req' id='mqtt_required_hint'>*必填</span></label>
        <input type='text' name='mqtt_host_name' id='mqtt_host_name_input' value=')HTML"
                  + mqttHostName + R"HTML(' placeholder='如: ASXD'>
        <small>填写与主机相同的序列号</small>

        <label>目标 MAC <span style='font-weight:normal;color:#aaa;font-size:11px;'>可选，留空订阅所有</span></label>
        <input type='text' name='mqtt_target_mac' value=')HTML"
                  + mqttTargetMac + R"HTML(' placeholder='XX:XX:XX:XX:XX:XX 或留空'>
        <small>指定则只订阅该MAC；留空则订阅主机下所有数据</small>
      </div>
    </div>

    <div class='card'>
      <div class='cb-row'>
        <input type='checkbox' name='never_sleep' id='never_sleep' value='true' )HTML"
                  + (neverSleep ? "checked" : "") + R"HTML(>
        <label for='never_sleep'>永不休眠（持续运行）</label>
      </div>
    </div>

    <script>
      function toggleMqttPreset() {
        var preset = document.getElementById('mqtt_preset').value;
        var customFields = document.getElementById('mqtt_custom_fields');
        var hostInput = document.getElementById('mqtt_host_input');
        var portInput = document.getElementById('mqtt_port_input');
        var userInput = document.getElementById('mqtt_user_input');
        var passInput = document.getElementById('mqtt_pass_input');
        if (preset === 'custom') {
          customFields.style.display = 'block';
        } else {
          customFields.style.display = 'none';
          if (preset === 'auto') {
            if (hostInput) hostInput.value = '';
            if (portInput) portInput.value = '1883';
          } else {
            var presets = {emqx:['broker.emqx.io',1883], hivemq:['broker.hivemq.com',1883]};
            var cfg = presets[preset] || presets.emqx;
            if (hostInput) hostInput.value = cfg[0];
            if (portInput) portInput.value = cfg[1];
          }
          if (userInput) userInput.value = '';
          if (passInput) passInput.value = '';
        }
      }
      document.addEventListener('DOMContentLoaded', function() { toggleMqttPreset(); });

      function rssiText(rssi) {
        if (rssi >= -50) return '极强';
        if (rssi >= -60) return '强';
        if (rssi >= -70) return '中';
        return '弱';
      }

      function scanWifi() {
        const btn = document.getElementById('scanBtn');
        const status = document.getElementById('scanStatus');
        const select = document.getElementById('ssidSelect');
        btn.disabled = true;
        btn.textContent = '扫描中...';
        status.textContent = '正在扫描附近的 WiFi 网络...';
        status.style.color = '#888';
        fetch('/scan')
          .then(r => r.json())
          .then(data => {
            if (data.networks && data.networks.length > 0) {
              const currentVal = select.value;
              select.innerHTML = '';
              data.networks.forEach(net => {
                const opt = document.createElement('option');
                opt.value = net.ssid;
                opt.textContent = net.ssid + ' (' + rssiText(net.rssi) + ' ' + net.rssi + 'dBm)';
                if (net.ssid === currentVal) opt.selected = true;
                select.appendChild(opt);
              });
              status.textContent = '找到 ' + data.networks.length + ' 个网络';
              status.style.color = '#27ae60';
            } else {
              status.textContent = '未找到 WiFi 网络';
              status.style.color = '#e05c65';
            }
          })
          .catch(e => { status.textContent = '扫描失败: ' + e.message; status.style.color = '#e05c65'; })
          .finally(() => {
            btn.disabled = false;
            btn.textContent = '扫描';
            setTimeout(() => { status.textContent = ''; }, 3000);
          });
      }

      window.onload = function() { toggleMqttPreset(); };
    </script>

    <button type='submit' class='submit-btn'>保存配置</button>
  </form>
  <a href='/update' class='submit-btn' style='display:block;text-align:center;text-decoration:none;background:#4a90e2;margin-top:10px;'>固件升级 (OTA)</a>
  </div>
</body>
</html>
)HTML";
    request->send(200, "text/html; charset=utf-8", page);
  });

  server.on("/scan", HTTP_GET, [](AsyncWebServerRequest* request) {
    int n = WiFi.scanNetworks();
    String json = "{\"networks\":[";
    bool first = true;
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);
      if (ssid.length() > 0) {
        if (!first) json += ",";
        ssid.replace("\\", "\\\\");
        ssid.replace("\"", "\\\"");
        json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(rssi) + "}";
        first = false;
      }
    }
    json += "]}";
    AsyncWebServerResponse* scanResp = request->beginResponse(200, "application/json", json);
    scanResp->addHeader("Access-Control-Allow-Origin", "*");
    request->send(scanResp);
  });

  // 获取当前配置 JSON（供App原生UI读取预填）
  server.on("/api/config.json", HTTP_GET, [](AsyncWebServerRequest* request) {
    String mqttPreset = "custom";
    if (mqttAutoMode) mqttPreset = "auto";
    else if (mqttHost == "broker.emqx.io" || mqttHost.length() == 0) mqttPreset = "emqx";
    else if (mqttHost == "broker.hivemq.com") mqttPreset = "hivemq";
    String json = "{";
    json += "\"device_name\":\"" + customDeviceName + "\",";
    json += "\"ssid\":\"" + wifiSsid + "\",";
    json += "\"data_mode\":\"" + dataMode + "\",";
    json += "\"mqtt_preset\":\"" + mqttPreset + "\",";
    json += "\"mqtt_host\":\"" + mqttHost + "\",";
    json += "\"mqtt_port\":" + String(mqttPort) + ",";
    json += "\"mqtt_user\":\"" + mqttUser + "\",";
    json += "\"mqtt_host_name\":\"" + mqttHostName + "\",";
    json += "\"mqtt_target_mac\":\"" + mqttTargetMac + "\",";
    json += "\"never_sleep\":" + String(neverSleep ? "true" : "false");
    json += "}";
    AsyncWebServerResponse* cfgResp = request->beginResponse(200, "application/json", json);
    cfgResp->addHeader("Access-Control-Allow-Origin", "*");
    request->send(cfgResp);
  });

  // 保存并连接
  server.on("/save", HTTP_POST, [&](AsyncWebServerRequest* request) {
    if (!request->hasParam("ssid", true)) {
      request->send(400, "text/plain", "参数无效: SSID必填");
      return;
    }
    String ssid = request->getParam("ssid", true)->value();
    String pass = request->hasParam("pass", true) ? request->getParam("pass", true)->value() : "";
    String serverBase = request->hasParam("server", true) ? request->getParam("server", true)->value() : SERVER_BASE;

    String deviceName = request->hasParam("device_name", true) ? request->getParam("device_name", true)->value() : "";
    deviceName.trim();
    if (deviceName.length() == 0) {
      deviceName = customDeviceName;  // 如果未填写，使用默认值
    }

    String targetMac = request->hasParam("mqtt_target_mac", true) ? request->getParam("mqtt_target_mac", true)->value() : mqttTargetMac;

    // MQTT预设处理
    String mqttPreset = request->hasParam("mqtt_preset", true) ? request->getParam("mqtt_preset", true)->value() : "emqx";
    String mHost, mUser, mPass;
    int mPort;
    bool autoMode = false;
    if (mqttPreset == "auto") {
      autoMode = true;
      mHost = "broker.emqx.io";  // 占位，自动模式下不使用单broker
      mPort = 1883;
      mUser = "";
      mPass = "";
    } else if (mqttPreset == "emqx") {
      mHost = "broker.emqx.io";
      mPort = 1883;
      mUser = "";
      mPass = "";
    } else if (mqttPreset == "hivemq") {
      mHost = "broker.hivemq.com";
      mPort = 1883;
      mUser = "";
      mPass = "";
    } else {
      mHost = request->hasParam("mqtt_host", true) ? request->getParam("mqtt_host", true)->value() : mqttHost;
      mPort = request->hasParam("mqtt_port", true) ? request->getParam("mqtt_port", true)->value().toInt() : mqttPort;
      mUser = request->hasParam("mqtt_user", true) ? request->getParam("mqtt_user", true)->value() : mqttUser;
      mPass = request->hasParam("mqtt_pass", true) ? request->getParam("mqtt_pass", true)->value() : mqttPass;
    }
    mqttAutoMode = autoMode;
    String mHostName = request->hasParam("mqtt_host_name", true) ? request->getParam("mqtt_host_name", true)->value() : mqttHostName;

    String mode = request->hasParam("data_mode", true) ? request->getParam("data_mode", true)->value() : dataMode;

    bool neverSleepVal = request->hasParam("never_sleep", true);

    saveConfiguration(ssid, pass, serverBase, deviceName, mode, targetMac, neverSleepVal, mHost, mPort, mUser, mPass, mHostName);
    // 更新设备ID
    c1DeviceId = customDeviceName;

    // 成功页面 HTML
    String successPage = R"HTML(
<!DOCTYPE html>
<html lang='zh-CN'>
<head>
  <meta charset='utf-8'>
  <meta name='viewport' content='width=device-width,initial-scale=1.0'>
  <title>配置完成</title>
  <style>
    *{box-sizing:border-box;margin:0;padding:0;}
    body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Arial,sans-serif;background:#f0f2f5;padding:60px 20px;text-align:center;}
    .card{background:#fff;border-radius:12px;box-shadow:0 1px 6px rgba(0,0,0,.07);padding:32px 24px;max-width:360px;margin:0 auto;}
    h1{font-size:20px;color:#27ae60;font-weight:600;margin-bottom:12px;}
    p{font-size:14px;color:#888;margin:6px 0;line-height:1.6;}
    .name{color:#333;font-weight:600;}
    .cd{font-size:13px;color:#bbb;margin-top:14px;}
  </style>
</head>
<body>
  <div class='card'>
    <h1>配置成功</h1>
    <p>设备配置已保存</p>
    <p class='name'>)HTML"
                         + deviceName + R"HTML(</p>
    <p class='cd'>设备将在 <span id='sec'>5</span> 秒后自动重启...</p>
  </div>
  <script>
    let s=5; setInterval(()=>{ s--; if(s>=0) document.getElementById('sec').textContent=s; },1000);
  </script>
</body>
</html>
)HTML";

    AsyncWebServerResponse* saveResp = request->beginResponse(200, "text/html; charset=utf-8", successPage);
    saveResp->addHeader("Access-Control-Allow-Origin", "*");
    request->send(saveResp);
    // 延时重启（不能delay，会阻塞AsyncWebServer发送响应）
    pendingRestart = true;
    restartAt = millis() + 5000;
  });

  // ========== OTA 固件升级 ==========
  server.on("/update", HTTP_GET, [](AsyncWebServerRequest* request) {
    String html = R"HTML(<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1.0'><title>C1 固件升级</title>
<style>*{box-sizing:border-box;margin:0;padding:0;}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Arial,sans-serif;background:#f0f2f5;padding:16px;color:#333;}
.wrap{max-width:520px;margin:0 auto;}
h1{font-size:18px;margin-bottom:14px;color:#222;}
.card{background:#fff;border-radius:12px;box-shadow:0 1px 6px rgba(0,0,0,.07);padding:18px;margin-bottom:12px;}
.tip{font-size:13px;color:#666;line-height:1.7;margin-bottom:14px;}
.tip b{color:#e67e22;}
input[type=file]{width:100%;padding:10px;border:1px dashed #aaa;border-radius:8px;margin-bottom:12px;background:#fafafa;}
.btn{width:100%;padding:14px;border:none;border-radius:10px;background:#4a90e2;color:#fff;font-size:15px;font-weight:600;cursor:pointer;}
.btn:disabled{background:#aaa;cursor:not-allowed;}
.bar{width:100%;height:14px;background:#eee;border-radius:7px;margin-top:14px;overflow:hidden;display:none;}
.bar > div{height:100%;width:0;background:linear-gradient(90deg,#4a90e2,#27ae60);transition:width .2s;}
.msg{margin-top:12px;font-size:13px;color:#444;line-height:1.6;display:none;padding:10px;border-radius:8px;}
.msg.ok{background:#e8f5e9;color:#2e7d32;}
.msg.err{background:#ffebee;color:#c62828;}
a.back{display:block;text-align:center;margin-top:14px;font-size:13px;color:#888;text-decoration:none;}
</style></head><body><div class='wrap'>
<h1>C1 固件升级</h1>
<div class='card'>
<div class='tip'>选择 <b>.bin</b> 文件上传，升级期间请<b>勿断电、勿关闭页面</b>。完成后设备会自动重启。</div>
<form id='f' method='POST' action='/update' enctype='multipart/form-data'>
<input type='file' name='update' id='fi' accept='.bin' required>
<button class='btn' type='submit' id='sb'>开始升级</button>
</form>
<div class='bar' id='bar'><div id='bv'></div></div>
<div class='msg' id='msg'></div>
</div>
<a class='back' href='/'>← 返回配置页</a>
</div>
<script>
var f=document.getElementById('f'),fi=document.getElementById('fi'),sb=document.getElementById('sb');
var bar=document.getElementById('bar'),bv=document.getElementById('bv'),msg=document.getElementById('msg');
f.onsubmit=function(e){
  e.preventDefault();
  if(!fi.files.length){alert('请选择文件');return;}
  var fd=new FormData();fd.append('update',fi.files[0]);
  var xhr=new XMLHttpRequest();
  xhr.upload.onprogress=function(ev){
    if(ev.lengthComputable){
      var pct=Math.round(ev.loaded*100/ev.total);
      bar.style.display='block';bv.style.width=pct+'%';
      sb.disabled=true;sb.textContent='上传中 '+pct+'%';
    }
  };
  xhr.onload=function(){
    msg.style.display='block';
    if(xhr.status==200){
      msg.className='msg ok';msg.textContent='升级成功！设备正在重启，约15秒后可重新访问。';
      sb.textContent='完成';
    }else{
      msg.className='msg err';msg.textContent='升级失败: '+xhr.responseText;
      sb.disabled=false;sb.textContent='重试';
    }
  };
  xhr.onerror=function(){
    msg.style.display='block';msg.className='msg err';msg.textContent='网络错误，上传中断';
    sb.disabled=false;sb.textContent='重试';
  };
  xhr.open('POST','/update');xhr.send(fd);
};
</script></body></html>)HTML";
    AsyncWebServerResponse* r = request->beginResponse(200, "text/html; charset=utf-8", html);
    r->addHeader("Access-Control-Allow-Origin", "*");
    request->send(r);
  });

  server.on(
    "/update", HTTP_POST,
    [](AsyncWebServerRequest* request) {
      bool ok = !Update.hasError();
      const char* errStr = ok ? "OK" : Update.errorString();
      AsyncWebServerResponse* r = request->beginResponse(ok ? 200 : 500, "text/plain", errStr);
      r->addHeader("Connection", "close");
      r->addHeader("Access-Control-Allow-Origin", "*");
      request->send(r);
      if (ok) {
        Serial.println("[OTA] Success, restarting...");
        pendingRestart = true;
        restartAt = millis() + 1500;
      }
    },
    [](AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
      // async_tcp 任务栈有限，严禁 String/addDebugLog
      if (index == 0) {
        Serial.printf("[OTA] Start: %s, size=%u\n", filename.c_str(), (unsigned)request->contentLength());
        esp_task_wdt_delete(NULL);
        otaEncrypted = false;
        otaOriginalSize = 0;
        otaHeaderParsed = 0;
      }

      // 前8字节判断是否为加密固件（魔数4字节 + 原始大小4字节）
      if (index < 8 && !otaEncrypted && otaHeaderParsed < 8) {
        // 积累 header
        size_t headerRemain = 8 - otaHeaderParsed;
        size_t copyLen = (len < headerRemain) ? len : headerRemain;
        static uint8_t headerBuf[8];
        memcpy(headerBuf + otaHeaderParsed, data, copyLen);
        otaHeaderParsed += copyLen;
        data += copyLen;
        len -= copyLen;

        if (otaHeaderParsed == 8) {
          if (memcmp(headerBuf, OTA_MAGIC, 4) == 0) {
            // 加密固件
            memcpy(&otaOriginalSize, headerBuf + 4, 4);
            otaEncrypted = true;
            Serial.printf("[OTA] Encrypted firmware detected, original=%u\n", otaOriginalSize);
            mbedtls_aes_init(&otaAesCtx);
            mbedtls_aes_setkey_dec(&otaAesCtx, OTA_AES_KEY, 128);
            if (!Update.begin(otaOriginalSize, U_FLASH)) {
              Update.printError(Serial);
              return;
            }
          } else {
            // 明文固件（兼容未加密bin）
            Serial.println("[OTA] Plain firmware (no encryption header)");
            size_t contentLen = request->contentLength();
            if (contentLen == 0) contentLen = UPDATE_SIZE_UNKNOWN;
            if (!Update.begin(contentLen, U_FLASH)) {
              Update.printError(Serial);
              return;
            }
            // 把 header 当作固件数据写入
            if (Update.write(headerBuf, 8) != 8) {
              Update.printError(Serial);
              return;
            }
          }
        } else {
          return; // header 还没收完
        }
      }

      if (Update.hasError()) return;
      if (len == 0 && !final) return;

      if (len > 0) {
        if (otaEncrypted) {
          // AES-128-ECB 逐16字节块解密
          uint8_t decBuf[16];
          size_t offset = 0;
          while (offset + 16 <= len) {
            mbedtls_aes_crypt_ecb(&otaAesCtx, MBEDTLS_AES_DECRYPT, data + offset, decBuf);
            // 如果是最后一块数据的最后几个block，可能有padding
            size_t writeLen = 16;
            if (final && offset + 16 >= len) {
              // 最后一个block，去掉PKCS7 padding
              uint8_t padVal = decBuf[15];
              if (padVal >= 1 && padVal <= 16) {
                writeLen = 16 - padVal;
              }
            }
            if (writeLen > 0) {
              if (Update.write(decBuf, writeLen) != writeLen) {
                Update.printError(Serial);
                return;
              }
            }
            offset += 16;
          }
        } else {
          if (Update.write(data, len) != len) {
            Update.printError(Serial);
            return;
          }
        }
      }

      if (final) {
        if (otaEncrypted) {
          mbedtls_aes_free(&otaAesCtx);
        }
        if (Update.end(true)) {
          Serial.printf("[OTA] Done: %u bytes\n", (unsigned)(index + len));
        } else {
          Update.printError(Serial);
        }
        esp_task_wdt_add(NULL);
      }
    });

  // 捕获所有未匹配的请求，重定向到配置页面（实现自动跳转）
  server.onNotFound([](AsyncWebServerRequest* request) {
    Serial.printf("[AP] 捕获未匹配请求: %s，重定向到配置页面\n", request->url().c_str());
    request->redirect("/");
  });

  // 启动服务器
  server.begin();
}

void stopFirstBootAP() {
  if (!apModeActive) return;
  addDebugLog("[AP] 配置完成，关闭AP和DNS");
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  apModeActive = false;
}

// ========== 状态AP功能（配置完成后的运行状态热点）==========
void startStatusAP() {
  if (statusApActive) return;

  // 统一使用与初始配置相同的SSID前缀
  statusApSsid = "C1-Setup-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  // 设置为AP+STA模式（同时连接WiFi和开启热点）
  WiFi.mode(WIFI_AP_STA);

  // 开启状态热点
  WiFi.softAPConfig(AP_LOCAL_IP, AP_LOCAL_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(statusApSsid.c_str(), "12345678");

  Serial.printf("[STATUS-AP] 状态热点已启动: %s (密码: 12345678)\n", statusApSsid.c_str());
  Serial.printf("[STATUS-AP] 访问 http://192.168.4.1 或 http://192.168.4.1:8080/status 查看状态\n");

  // 启动DNS劫持，所有域名解析到本机IP，实现captive portal自动弹出页面
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", AP_LOCAL_IP);

  // 在端口80启动服务，返回HTML页面触发captive portal并跳转到状态页
  // 注意：不能用302重定向，手机captive portal检测不跟随跨端口重定向
  server.onNotFound([](AsyncWebServerRequest* request) {
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                  "<meta http-equiv='refresh' content='1;url=http://192.168.4.1:8080/status'>"
                  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<style>body{font-family:sans-serif;text-align:center;padding:60px 20px;background:#f0f2f5;}"
                  ".card{background:#fff;border-radius:12px;padding:32px 24px;max-width:360px;margin:0 auto;"
                  "box-shadow:0 1px 6px rgba(0,0,0,.07);}"
                  "a{display:block;margin-top:16px;padding:12px;background:#5b8dee;color:#fff;border-radius:8px;"
                  "text-decoration:none;font-size:16px;}</style></head>"
                  "<body><div class='card'>"
                  "<h2 style='color:#333;font-size:17px;'>C1 设备管理</h2>"
                  "<p style='color:#999;font-size:14px;'>正在跳转到管理页面...</p>"
                  "<a href='http://192.168.4.1:8080/status'>打开管理页面</a>"
                  "</div></body></html>";
    request->send(200, "text/html; charset=utf-8", html);
  });
  server.begin();

  // 状态页面路由 - 根路径显示状态
  statusServer.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->redirect("/status");
  });

  // 状态监控页面 - 使用AsyncResponseStream分块发送，避免巨大String导致堆分配失败白屏
  statusServer.on("/status", HTTP_GET, [](AsyncWebServerRequest* request) {
    AsyncResponseStream* response = request->beginResponseStream("text/html; charset=utf-8");

    // 获取设备信息
    String deviceMac = WiFi.macAddress();
    uint32_t chipId = (uint32_t)ESP.getEfuseMac();
    String chipIdStr = String(chipId, HEX);
    chipIdStr.toUpperCase();
    uint8_t actualBleMac[6];
    getBluetoothMac(actualBleMac);
    String bleMacStr = formatMacAddress(actualBleMac);
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t totalHeap = ESP.getHeapSize();
    int heapPercent = (freeHeap * 100) / totalHeap;
    const char* heapClass = (heapPercent > 30) ? "ok" : ((heapPercent > 15) ? "warn" : "err");
    unsigned long runMs = millis();
    int runSec = (runMs / 1000) % 60;
    int runMin = (runMs / 60000) % 60;
    int runHour = (runMs / 3600000) % 24;
    int runDay = runMs / 86400000;
    String deviceName = customDeviceName.length() > 0 ? customDeviceName : c1DeviceId;
    String lastMsgInfo = "";
    if (lastMqttMsgMs > 0) {
      unsigned long elapsed = (millis() - lastMqttMsgMs) / 1000;
      if (elapsed < 60) lastMsgInfo = String(elapsed) + " 秒前";
      else if (elapsed < 3600) lastMsgInfo = String(elapsed / 60) + " 分前";
      else lastMsgInfo = String(elapsed / 3600) + " 小时前";
    } else {
      lastMsgInfo = "尚未收到";
    }

    // ---- HTML head + CSS ----
    response->print(R"HTML(<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1.0'><title>C1 管理</title>
<style>*{box-sizing:border-box;margin:0;padding:0;}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Arial,sans-serif;background:#f0f2f5;padding:16px;color:#333;}
.wrap{max-width:520px;margin:0 auto;}
h1{font-size:17px;font-weight:700;color:#222;margin-bottom:14px;letter-spacing:.3px;}
.card{background:#fff;border-radius:12px;box-shadow:0 1px 6px rgba(0,0,0,.07);margin-bottom:12px;overflow:hidden;}
.sec{font-size:11px;color:#999;padding:8px 14px;font-weight:600;letter-spacing:.5px;background:#fafafa;border-bottom:1px solid #f0f0f0;}
.row{display:flex;align-items:flex-start;padding:11px 14px;border-bottom:1px solid #f0f0f0;}
.row:last-child{border-bottom:none;}
.lbl{width:34%;font-size:13px;color:#666;flex-shrink:0;padding-top:2px;font-weight:500;}
.val{flex:1;font-size:14px;color:#222;word-break:break-all;line-height:1.6;}
.status-ok{color:#27ae60;font-weight:700;}.status-err{color:#e05c65;font-weight:700;}.status-warn{color:#e67e22;font-weight:700;}
.acts{display:flex;gap:10px;margin-top:6px;}
.btn{flex:1;display:block;padding:14px 0;text-align:center;text-decoration:none;border-radius:10px;font-size:15px;font-weight:600;}
.btn-r{background:#fdecea;color:#c0392b;}
.ft{text-align:center;font-size:12px;color:#aaa;margin-top:14px;}
.tabs{display:flex;margin-bottom:14px;background:#fff;border-radius:10px;box-shadow:0 1px 4px rgba(0,0,0,.06);overflow:hidden;}
.tab{flex:1;padding:12px 0;text-align:center;font-size:14px;font-weight:600;border:none;background:#fff;color:#888;cursor:pointer;transition:all .2s;}
.tab.active{background:#4a90e2;color:#fff;}
.cfg-input{width:100%;padding:9px 11px;border:1px solid #ddd;border-radius:8px;font-size:13px;margin-bottom:8px;box-sizing:border-box;background:#fff;color:#333;}
.cfg-input:focus{outline:none;border-color:#4a90e2;}
.cfg-row{display:flex;gap:8px;margin-bottom:8px;}.cfg-row .cfg-input{margin-bottom:0;}
.cfg-sec{font-size:11px;color:#999;font-weight:600;margin:12px 0 8px;padding-top:10px;border-top:1px solid #f0f0f0;}
.cfg-sec:first-child{margin-top:0;padding-top:0;border-top:none;}
</style></head><body><div class='wrap'><h1>C1 设备管理</h1>
<div class='tabs'><button class='tab active' id='tabBtnStatus' onclick='switchTab("status")'>状态</button>
<button class='tab' id='tabBtnConfig' onclick='switchTab("config")'>配置</button></div>
<div id='tab-status'><div class='card'>)HTML");

    // ---- 设备信息卡片 ----
    response->printf("<div class='row'><span class='lbl'>设备名称</span><span class='val'>%s</span></div>", deviceName.c_str());
    response->printf("<div class='row'><span class='lbl'>MAC / 芯片ID</span><span class='val'>%s / %s</span></div>", deviceMac.c_str(), chipIdStr.c_str());
    response->printf("<div class='row'><span class='lbl'>运行时间</span><span class='val' id='v_uptime'>");
    if (runDay > 0) response->printf("%d天 ", runDay);
    response->printf("%d:%02d:%02d</span></div>", runHour, runMin, runSec);
    response->printf("<div class='row'><span class='lbl'>空闲内存</span><span id='v_heap' class='val %s'>%u KB (%d%%)</span></div>", heapClass, freeHeap / 1024, heapPercent);
    response->print("</div><div class='card'>");

    // ---- WiFi状态 ----
    response->print("<div class='row'><span class='lbl'>WiFi</span><span class='val' id='v_net'>");
    if (WiFi.status() == WL_CONNECTED) {
      int rssi = WiFi.RSSI();
      int bars = (rssi >= -50) ? 4 : ((rssi >= -60) ? 3 : ((rssi >= -70) ? 2 : 1));
      response->print("✅ 已连接<br>SSID: <b>");
      response->print(wifiSsid);
      response->printf("</b><br>IP: %s<br>信号: ", WiFi.localIP().toString().c_str());
      for (int i = 0; i < 4; i++) response->print(i < bars ? "▓" : "░");
      response->printf(" %ddBm", rssi);
    } else {
      response->print("❌ 未连接<br>SSID: ");
      response->print(wifiSsid);
    }
    response->print("</span></div>");

    // ---- MQTT状态 ----
    if (dataMode == "mqtt") {
      bool mqttOk = false;
      if (mqttAutoMode) {
        for (int i = 0; i < MULTI_BROKER_COUNT; i++) {
          if (multiBrokerClient[i].connected()) { mqttOk = true; break; }
        }
      } else {
        mqttOk = getActiveMqttClient().connected();
      }
      response->print("<div class='row'><span class='lbl'>MQTT</span><span class='val' id='v_mqtt'>");
      response->print(mqttOk ? "<span class='status-ok'>已连接</span>" : "<span class='status-err'>未连接</span>");
      if (mqttAutoMode) {
        response->print("<br>模式: 自动（多Broker）");
      } else {
        response->printf("<br>服务器: %s:%d", mqttHost.c_str(), mqttPort);
      }
      String subPrefix = mqttHostName.length() > 0 ? mqttHostName : "o5";
      response->printf("<br>订阅前缀: <b>%s</b>", subPrefix.c_str());
      if (mqttSubscribedTopic.length() > 0) {
        response->printf("<br>订阅Topic: <code>%s</code>", mqttSubscribedTopic.c_str());
      }
      if (mqttTargetMac.length() > 0) {
        response->printf("<br>过滤MAC: <code>%s</code>", mqttTargetMac.c_str());
      } else {
        response->print("<br>过滤MAC: <span class='status-warn'>无 (接收所有)</span>");
      }
      response->print("</span></div>");
    }

    // ---- BLE / 传输 / 数据 ----
    response->print("<div class='row'><span class='lbl'>BLE 广播</span><span class='val'>");
    response->print(isAdvertising ? "<span class='status-ok'>广播中</span>" : "<span class='status-err'>已停止</span>");
    response->printf("<br>MAC: <code>%s</code>", bleMacStr.c_str());
    if (cloudTargetMac.length() > 0) {
      response->printf("<br>目标MAC: <code>%s</code>", cloudTargetMac.c_str());
    }
    response->print("</span></div>");
    response->printf("<div class='row'><span class='lbl'>数据来源</span><span class='val'>%s</span></div>", webDataReceived ? "<span class='status-warn'>Web端优先</span>" : "<span class='status-ok'>本地配置</span>");
    response->printf("<div class='row'><span class='lbl'>剩余时间</span><span class='val'>%s</span></div>", getSleepRemainingTime().c_str());
    response->printf("<div class='row'><span class='lbl'>最后消息</span><span class='val'>%s</span></div>", lastMsgInfo.c_str());
    response->print("</div>");

    // ---- 诊断 + 日志 + 操作按钮 ----
    response->print(R"HTML(<div class='card'>
<div class='sec' style='display:flex;justify-content:space-between;align-items:center;'>
<span>网络诊断</span>
<button id='diagBtn' onclick='runDiag()' style='padding:4px 12px;border:1px solid #4a90e2;border-radius:6px;background:#fff;color:#4a90e2;font-size:11px;font-weight:600;cursor:pointer;'>开始诊断</button></div>
<div id='diagResult' style='display:none;'>
<div class='row'><span class='lbl'>WiFi</span><span class='val' id='dg_wifi'>--</span></div>
<div class='row'><span class='lbl'>DNS</span><span class='val' id='dg_dns'>--</span></div>
<div class='row'><span class='lbl'>TCP</span><span class='val' id='dg_tcp'>--</span></div>
<div class='row'><span class='lbl'>MQTT</span><span class='val' id='dg_mqtt'>--</span></div>
<div class='row'><span class='lbl'>发布</span><span class='val' id='dg_pub'>--</span></div>
</div></div>
<div class='card'>
<div class='sec' style='cursor:pointer;user-select:none;' onclick='toggleLog()'>运行日志 <span id='logArrow'>▶</span></div>
<div id='logPanel' style='display:none;'>
<div style='display:flex;gap:6px;padding:8px 14px;'>
<button onclick='refreshLog()' style='flex:1;padding:7px 0;border:1px solid #ddd;border-radius:6px;background:#fff;font-size:12px;cursor:pointer;'>刷新日志</button>
<button onclick='clearLog()' style='flex:1;padding:7px 0;border:1px solid #ddd;border-radius:6px;background:#fff;font-size:12px;cursor:pointer;color:#c0392b;'>清空日志</button></div>
<pre id='logContent' style='margin:0;padding:10px 14px;font-size:11px;color:#444;background:#fafafa;max-height:300px;overflow-y:auto;white-space:pre-wrap;word-break:break-all;border-top:1px solid #f0f0f0;'>点击"刷新日志"加载...</pre></div></div>
<div class='acts'><a href='/reinit' class='btn btn-r' onclick='return confirm("确定要恢复出厂设置吗？\n这将清除所有配置并重启！")'>恢复出厂</a></div>
</div>)HTML");

    // ---- 配置Tab ----
    response->print(R"HTML(<div id='tab-config' style='display:none;'><div class='card'><div style='padding:14px;'>
<div class='cfg-sec' style='margin-top:0;padding-top:0;border-top:none;'>WiFi 配置</div>
<div class='cfg-row'>
<input id='cf_ssid' type='text' class='cfg-input' style='margin-bottom:0;' placeholder='WiFi SSID'>
<button onclick='doScanWifi()' id='wfScanBtn' style='padding:9px 14px;border:none;border-radius:8px;background:#4a90e2;color:#fff;font-size:12px;font-weight:600;cursor:pointer;white-space:nowrap;'>扫描</button></div>
<div id='wfScanList' style='display:none;max-height:150px;overflow-y:auto;border:1px solid #eee;border-radius:8px;margin-bottom:8px;background:#fafafa;'></div>
<input id='cf_pass' type='password' class='cfg-input' placeholder='WiFi 密码'>
<div class='cfg-sec'>设备序列号</div>
<input id='cf_hname' type='text' class='cfg-input' placeholder='序列号（MQTT主机名称）'>
<div class='cfg-sec'>目标MAC</div>
<input id='cf_targetmac' type='text' class='cfg-input' placeholder='目标MAC地址'>
<div class='cfg-sec'>MQTT 服务器</div>
<select id='cf_mpreset' class='cfg-input' onchange='toggleMqttCustom()'>
<option value='auto'>自动（同时监听全部）</option>
<option value='emqx'>EMQX 国际服务器</option>
<option value='hivemq'>HiveMQ 国际服务器</option>
<option value='custom'>自定义</option></select>
<div id='mqttCustomFields' style='display:none;'>
<input id='cf_mhost' type='text' class='cfg-input' placeholder='MQTT Host'>
<div class='cfg-row'>
<input id='cf_mport' type='number' class='cfg-input' style='width:100px;flex:none;margin-bottom:0;' placeholder='端口'>
<input id='cf_muser' type='text' class='cfg-input' style='margin-bottom:0;' placeholder='用户名 (可选)'></div>
<input id='cf_mpass' type='password' class='cfg-input' placeholder='密码 (可选)'></div>
<button onclick='saveConfig()' style='width:100%;padding:12px;border:none;border-radius:10px;background:#4a90e2;color:#fff;font-size:15px;font-weight:600;cursor:pointer;margin-top:14px;'>保存并应用（免重启）</button>
<div id='cfgResult' style='font-size:12px;margin-top:8px;text-align:center;color:#666;'></div>
</div></div>
<div class='acts'>
<a href='/restart' class='btn' style='background:#e8f4fd;color:#2980b9;' onclick='return confirm("确定要重启设备吗？")'>重启设备</a>
<a href='javascript:void(0)' class='btn' style='background:#f0f0f0;color:#666;' onclick='enableSilent()'>开启静默模式</a>
<a href='/reinit' class='btn btn-r' onclick='return confirm("确定要恢复出厂设置吗？\n这将清除所有配置！")'>恢复出厂</a>
</div></div><p class='ft'>@北晨科技</p></div>)HTML");

    // ---- JavaScript ----
    response->print(R"HTML(<script>
function enableSilent(){
if(!confirm('确定开启静默模式？\n\n开启后将：\n• 永久关闭指示灯\n• 永久关闭配置WiFi热点\n• 仅可通过长按初始化按钮5秒恢复\n\n设备将立即重启！'))return;
var x=new XMLHttpRequest();x.open('GET','/api/silent',true);x.timeout=5000;
x.onload=function(){document.body.innerHTML='<div style="text-align:center;padding:60px;font-size:16px;color:#666;">静默模式已开启，设备正在重启...</div>';};
x.onerror=x.ontimeout=function(){alert('请求失败');};x.send();}
var logOpen=false;
function toggleLog(){logOpen=!logOpen;document.getElementById('logPanel').style.display=logOpen?'block':'none';document.getElementById('logArrow').textContent=logOpen?'▼':'▶';if(logOpen)refreshLog();}
function refreshLog(){var x=new XMLHttpRequest();x.open('GET','/api/log',true);x.onload=function(){var el=document.getElementById('logContent');el.textContent=x.responseText||'(暂无日志)';el.scrollTop=el.scrollHeight;};x.send();}
function clearLog(){var x=new XMLHttpRequest();x.open('GET','/api/log/clear',true);x.onload=function(){document.getElementById('logContent').textContent='(已清空)';};x.send();}
function runDiag(){var btn=document.getElementById('diagBtn');var panel=document.getElementById('diagResult');btn.disabled=true;btn.textContent='诊断中...';btn.style.color='#999';panel.style.display='block';
['dg_wifi','dg_dns','dg_tcp','dg_mqtt','dg_pub'].forEach(function(id){document.getElementById(id).innerHTML='<span style="color:#999">测试中...</span>';});
var x=new XMLHttpRequest();x.open('GET','/api/diag',true);x.timeout=20000;
x.onload=function(){try{var d=JSON.parse(x.responseText);var e;
e=document.getElementById('dg_wifi');if(d.wifi_ok){e.innerHTML='<span class="status-ok">已连接</span> IP:'+d.wifi_ip+' GW:'+d.wifi_gw+' '+d.wifi_rssi+'dBm';}else{e.innerHTML='<span class="status-err">未连接</span>';}
e=document.getElementById('dg_dns');if(d.dns_ok){e.innerHTML='<span class="status-ok">成功</span> '+d.dns_target+' → '+d.dns_ip+' <small>('+d.dns_ms+'ms)</small>';}else{e.innerHTML='<span class="status-err">失败</span> '+(d.dns_target||'');}
e=document.getElementById('dg_tcp');if(d.tcp_ok){e.innerHTML='<span class="status-ok">成功</span> '+(d.tcp_target||'')+' <small>('+d.tcp_ms+'ms)</small>';}else{e.innerHTML='<span class="status-err">失败</span> '+(d.tcp_target||'未配置');}
e=document.getElementById('dg_mqtt');e.innerHTML=d.mqtt_connected?'<span class="status-ok">已连接</span>':'<span class="status-err">未连接</span>';
e=document.getElementById('dg_pub');if(d.pub_ok){e.innerHTML='<span class="status-ok">成功</span> → <code style="font-size:11px">'+d.pub_topic+'</code>';}else if(d.mqtt_connected){e.innerHTML='<span class="status-err">发布失败</span>';}else{e.innerHTML='<span class="status-warn">跳过（MQTT未连接）</span>';}
}catch(ex){document.getElementById('dg_wifi').innerHTML='<span class="status-err">解析失败</span>';}btn.disabled=false;btn.textContent='开始诊断';btn.style.color='#4a90e2';};
x.onerror=x.ontimeout=function(){document.getElementById('dg_wifi').innerHTML='<span class="status-err">请求失败/超时</span>';btn.disabled=false;btn.textContent='开始诊断';btn.style.color='#4a90e2';};x.send();}
var cfgLoaded=false;
function switchTab(t){document.getElementById('tab-status').style.display=(t==='status')?'block':'none';document.getElementById('tab-config').style.display=(t==='config')?'block':'none';document.getElementById('tabBtnStatus').className='tab'+(t==='status'?' active':'');document.getElementById('tabBtnConfig').className='tab'+(t==='config'?' active':'');if(t==='config'&&!cfgLoaded){cfgLoaded=true;loadConfigValues();}}
function toggleMqttCustom(){var sel=document.getElementById('cf_mpreset').value;document.getElementById('mqttCustomFields').style.display=(sel==='custom')?'block':'none';}
function loadConfigValues(){var x=new XMLHttpRequest();x.open('GET','/api/status.json',true);x.onload=function(){try{var d=JSON.parse(x.responseText);document.getElementById('cf_ssid').value=d.wifi_ssid||'';document.getElementById('cf_hname').value=d.mqtt_host_name||'';document.getElementById('cf_targetmac').value=d.mqtt_target_mac||'';var h=d.mqtt_host||'';var preset='custom';if(h==='auto')preset='auto';else if(h==='broker.emqx.io'||h==='')preset='emqx';else if(h==='broker.hivemq.com')preset='hivemq';document.getElementById('cf_mpreset').value=preset;toggleMqttCustom();if(preset==='custom'){document.getElementById('cf_mhost').value=h;document.getElementById('cf_mport').value=d.mqtt_port||'';}}catch(e){}};x.send();}
function doScanWifi(){var btn=document.getElementById('wfScanBtn');var list=document.getElementById('wfScanList');btn.disabled=true;btn.textContent='扫描中...';list.style.display='none';list.innerHTML='';var x=new XMLHttpRequest();x.open('GET','/scan',true);x.timeout=15000;x.onload=function(){try{var d=JSON.parse(x.responseText);if(d.networks&&d.networks.length>0){list.style.display='block';d.networks.forEach(function(n){var row=document.createElement('div');row.style.cssText='padding:8px 10px;cursor:pointer;border-bottom:1px solid #eee;font-size:13px;';row.textContent=n.ssid+' ('+n.rssi+'dBm)';row.onmouseover=function(){row.style.background='#e3f2fd';};row.onmouseout=function(){row.style.background='';};row.onclick=function(){document.getElementById('cf_ssid').value=n.ssid;list.style.display='none';};list.appendChild(row);});}}catch(e){}btn.disabled=false;btn.textContent='扫描';};x.onerror=x.ontimeout=function(){btn.disabled=false;btn.textContent='扫描';};x.send();}
function saveConfig(){var p='wifi_ssid='+encodeURIComponent(document.getElementById('cf_ssid').value);p+='&wifi_pass='+encodeURIComponent(document.getElementById('cf_pass').value);var preset=document.getElementById('cf_mpreset').value;if(preset==='emqx'){p+='&mqtt_host=broker.emqx.io&mqtt_port=1883&mqtt_user=&mqtt_pass=';}else if(preset==='hivemq'){p+='&mqtt_host=broker.hivemq.com&mqtt_port=1883&mqtt_user=&mqtt_pass=';}else if(preset!=='auto'){p+='&mqtt_host='+encodeURIComponent(document.getElementById('cf_mhost').value);p+='&mqtt_port='+encodeURIComponent(document.getElementById('cf_mport').value);p+='&mqtt_user='+encodeURIComponent(document.getElementById('cf_muser').value);p+='&mqtt_pass='+encodeURIComponent(document.getElementById('cf_mpass').value);}p+='&mqtt_h_name='+encodeURIComponent(document.getElementById('cf_hname').value);p+='&mqtt_target_mac='+encodeURIComponent(document.getElementById('cf_targetmac').value);var el=document.getElementById('cfgResult');el.textContent='正在保存...';el.style.color='#666';var x=new XMLHttpRequest();x.open('GET','/api/save-config?'+p,true);x.timeout=8000;x.onload=function(){try{var d=JSON.parse(x.responseText);if(d.ok){var msg='已保存并应用';if(d.wifi_changed)msg+=', WiFi正在重连';if(d.mqtt_changed)msg+=', MQTT正在重连';el.textContent=msg;el.style.color='#27ae60';}else{el.textContent='保存失败';el.style.color='#e05c65';}}catch(e){el.textContent='响应异常';el.style.color='#e05c65';}};x.onerror=function(){el.textContent='请求失败';el.style.color='#e05c65';};x.ontimeout=function(){el.textContent='请求超时';el.style.color='#e05c65';};x.send();}
function updateStatus(){var x=new XMLHttpRequest();x.open('GET','/api/status.json',true);x.timeout=6000;x.onload=function(){try{var d=JSON.parse(x.responseText);var e;
e=document.getElementById('v_uptime');if(e&&d.uptime_ms!==undefined){var ms=d.uptime_ms,s=Math.floor(ms/1000)%60,m=Math.floor(ms/60000)%60,h=Math.floor(ms/3600000),dy=Math.floor(h/24);h=h%24;e.textContent=(dy>0?dy+'天 ':'')+h+':'+(m<10?'0':'')+m+':'+(s<10?'0':'')+s;}
e=document.getElementById('v_heap');if(e&&d.free_heap!==undefined){e.textContent=Math.floor(d.free_heap/1024)+' KB';}
e=document.getElementById('v_net');if(e){if(d.wifi_connected){var r=d.wifi_rssi||0,bn=(r>=-50)?4:((r>=-60)?3:((r>=-70)?2:1)),bar='';for(var i=0;i<4;i++)bar+=(i<bn)?'\u2593':'\u2591';e.innerHTML='\u2705 \u5df2\u8fde\u63a5<br>SSID: <b>'+d.wifi_ssid+'</b><br>IP: '+d.wifi_ip+'<br>\u4fe1\u53f7: '+bar+' '+r+'dBm';}else{e.innerHTML='\u274c \u672a\u8fde\u63a5<br>SSID: '+d.wifi_ssid;}}
e=document.getElementById('v_mqtt');if(e&&d.data_mode==='mqtt'){var st=d.mqtt_connected?'<span class="status-ok">\u5df2\u8fde\u63a5</span>':'<span class="status-err">\u672a\u8fde\u63a5</span>';var md=(d.mqtt_host==='auto')?'\u6a21\u5f0f: \u81ea\u52a8\uff08\u591aBroker\uff09':'\u670d\u52a1\u5668: '+d.mqtt_host+':'+d.mqtt_port;if(d.mqtt_host_name)md+='<br>\u5e8f\u5217\u53f7: <b>'+d.mqtt_host_name+'</b>';e.innerHTML=st+'<br>'+md;}}catch(ex){}};x.send();}
setInterval(updateStatus,8000);setInterval(function(){if(logOpen)refreshLog();},5000);
</script></body></html>)HTML");

    request->send(response);
  });

  // 状态 JSON（供App原生UI轮询）
  statusServer.on("/api/status.json", HTTP_GET, [](AsyncWebServerRequest* request) {
    bool wifiOk = WiFi.status() == WL_CONNECTED;
    String json = "{";
    json += "\"device_name\":\"" + (customDeviceName.length() > 0 ? customDeviceName : c1DeviceId) + "\",";
    json += "\"mac\":\"" + WiFi.macAddress() + "\",";
    json += "\"data_mode\":\"" + dataMode + "\",";
    json += "\"wifi_connected\":" + String(wifiOk ? "true" : "false") + ",";
    json += "\"wifi_ssid\":\"" + wifiSsid + "\",";
    json += "\"wifi_ip\":\"" + (wifiOk ? WiFi.localIP().toString() : String("")) + "\",";
    json += "\"wifi_rssi\":" + String(wifiOk ? WiFi.RSSI() : 0) + ",";
    bool mqttOk = false;
    if (dataMode == "mqtt") {
      if (mqttAutoMode) {
        for (int i = 0; i < MULTI_BROKER_COUNT; i++) {
          if (multiBrokerClient[i].connected()) { mqttOk = true; break; }
        }
      } else {
        mqttOk = getActiveMqttClient().connected();
      }
    }
    json += "\"mqtt_connected\":" + String(mqttOk ? "true" : "false") + ",";
    json += "\"mqtt_host\":\"" + (mqttAutoMode ? String("auto") : mqttHost) + "\",";
    json += "\"mqtt_port\":" + String(mqttPort) + ",";
    json += "\"mqtt_host_name\":\"" + mqttHostName + "\",";
    json += "\"mqtt_target_mac\":\"" + mqttTargetMac + "\",";
    long lastMsgAgoS = lastMqttMsgMs > 0 ? (long)((millis() - lastMqttMsgMs) / 1000) : -1;
    json += "\"last_msg_ago_s\":" + String(lastMsgAgoS) + ",";
    json += "\"ble_advertising\":" + String(isAdvertising ? "true" : "false") + ",";
    json += "\"never_sleep\":" + String(neverSleep ? "true" : "false") + ",";
    json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"uptime_ms\":" + String(millis());
    json += "}";
    AsyncWebServerResponse* stResp = request->beginResponse(200, "application/json", json);
    stResp->addHeader("Access-Control-Allow-Origin", "*");
    request->send(stResp);
  });

  // 重启设备（保留配置）
  statusServer.on("/restart", HTTP_GET, [](AsyncWebServerRequest* request) {
    String accept = request->header("Accept");
    bool wantJson = accept.indexOf("application/json") >= 0 || accept.indexOf("text/html") < 0;
    String body = wantJson ? "{\"ok\":true}" : "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
                                               "<style>*{box-sizing:border-box;margin:0;padding:0;}"
                                               "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Arial,sans-serif;background:#f0f2f5;padding:60px 20px;text-align:center;}"
                                               ".card{background:#fff;border-radius:12px;box-shadow:0 1px 6px rgba(0,0,0,.07);padding:32px 24px;max-width:360px;margin:0 auto;}"
                                               "h2{font-size:17px;color:#444;margin-bottom:10px;font-weight:600;}"
                                               "p{font-size:14px;color:#999;line-height:1.6;}</style></head>"
                                               "<body><div class='card'><h2>正在重启设备...</h2><p>配置将保留，请稍候。</p></div></body></html>";
    String ct = wantJson ? "application/json" : "text/html; charset=utf-8";
    AsyncWebServerResponse* r = request->beginResponse(200, ct, body);
    r->addHeader("Access-Control-Allow-Origin", "*");
    request->send(r);
    pendingRestart = true;
    restartAt = millis() + 800;
  });

  // 恢复出厂设置（清除所有配置）
  statusServer.on("/reinit", HTTP_GET, [](AsyncWebServerRequest* request) {
    String accept = request->header("Accept");
    bool wantJson = accept.indexOf("application/json") >= 0 || accept.indexOf("text/html") < 0;
    String body = wantJson ? "{\"ok\":true}" : "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
                                               "<style>*{box-sizing:border-box;margin:0;padding:0;}"
                                               "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Arial,sans-serif;background:#f0f2f5;padding:60px 20px;text-align:center;}"
                                               ".card{background:#fff;border-radius:12px;box-shadow:0 1px 6px rgba(0,0,0,.07);padding:32px 24px;max-width:360px;margin:0 auto;}"
                                               "h2{font-size:17px;color:#c0392b;margin-bottom:10px;font-weight:600;}"
                                               "p{font-size:14px;color:#999;line-height:1.6;}</style></head>"
                                               "<body><div class='card'><h2>正在恢复出厂设置...</h2><p>所有配置将被清除，请稍候。</p></div></body></html>";
    String ct = wantJson ? "application/json" : "text/html; charset=utf-8";
    AsyncWebServerResponse* r = request->beginResponse(200, ct, body);
    r->addHeader("Access-Control-Allow-Origin", "*");
    request->send(r);
    pendingFactoryReset = true;
    restartAt = millis() + 800;
  });

  // 兼容旧的reset路由
  statusServer.on("/reset", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->redirect("/reinit");
  });

  // 运行日志接口
  statusServer.on("/api/log", HTTP_GET, [](AsyncWebServerRequest* request) {
    AsyncWebServerResponse* r = request->beginResponse(200, "text/plain; charset=utf-8", debugLog);
    r->addHeader("Access-Control-Allow-Origin", "*");
    request->send(r);
  });

  // 清空日志接口
  statusServer.on("/api/log/clear", HTTP_GET, [](AsyncWebServerRequest* request) {
    debugLog = "";
    AsyncWebServerResponse* r = request->beginResponse(200, "text/plain", "ok");
    r->addHeader("Access-Control-Allow-Origin", "*");
    request->send(r);
  });

  // 网络诊断接口
  statusServer.on("/api/diag", HTTP_GET, [](AsyncWebServerRequest* request) {
    String json = "{";
    addDebugLog("[DIAG] 开始网络诊断...");

    // 1. WiFi 状态
    bool wifiOk = (WiFi.status() == WL_CONNECTED);
    json += "\"wifi_ok\":" + String(wifiOk ? "true" : "false");
    if (wifiOk) {
      json += ",\"wifi_ip\":\"" + WiFi.localIP().toString() + "\"";
      json += ",\"wifi_gw\":\"" + WiFi.gatewayIP().toString() + "\"";
      json += ",\"wifi_rssi\":" + String(WiFi.RSSI());
    }

    // 2. DNS 解析测试
    bool dnsOk = false;
    String dnsTarget = mqttHost.length() > 0 ? mqttHost : "baidu.com";
    IPAddress resolved;
    if (wifiOk) {
      unsigned long dnsStart = millis();
      dnsOk = WiFi.hostByName(dnsTarget.c_str(), resolved);
      unsigned long dnsMs = millis() - dnsStart;
      json += ",\"dns_target\":\"" + dnsTarget + "\"";
      json += ",\"dns_ok\":" + String(dnsOk ? "true" : "false");
      if (dnsOk) json += ",\"dns_ip\":\"" + resolved.toString() + "\"";
      json += ",\"dns_ms\":" + String(dnsMs);
      addDebugLog("[DIAG] DNS " + dnsTarget + " → " + (dnsOk ? resolved.toString() : "失败") + " (" + String(dnsMs) + "ms)");
    } else {
      json += ",\"dns_ok\":false,\"dns_target\":\"" + dnsTarget + "\"";
    }

    // 3. TCP 连接测试
    bool tcpOk = false;
    unsigned long tcpMs = 0;
    if (wifiOk && mqttHost.length() > 0) {
      WiFiClient testClient;
      unsigned long tcpStart = millis();
      tcpOk = testClient.connect(mqttHost.c_str(), mqttPort, 5000);
      tcpMs = millis() - tcpStart;
      if (tcpOk) testClient.stop();
      json += ",\"tcp_target\":\"" + mqttHost + ":" + String(mqttPort) + "\"";
      json += ",\"tcp_ok\":" + String(tcpOk ? "true" : "false");
      json += ",\"tcp_ms\":" + String(tcpMs);
      addDebugLog("[DIAG] TCP " + mqttHost + ":" + String(mqttPort) + " → " + (tcpOk ? "成功" : "失败") + " (" + String(tcpMs) + "ms)");
    } else {
      json += ",\"tcp_ok\":false";
    }

    // 4. MQTT 连接状态
    bool mqttOk = false;
    if (dataMode == "mqtt") {
      if (mqttAutoMode) {
        for (int i = 0; i < MULTI_BROKER_COUNT; i++) {
          if (multiBrokerClient[i].connected()) { mqttOk = true; break; }
        }
      } else {
        mqttOk = getActiveMqttClient().connected();
      }
    }
    json += ",\"mqtt_connected\":" + String(mqttOk ? "true" : "false");

    // 5. MQTT 发布测试
    bool pubOk = false;
    if (mqttOk) {
      String testTopic = "c1/" + (mqttHostName.length() > 0 ? mqttHostName : WiFi.macAddress()) + "/diag";
      String testPayload = "{\"test\":true,\"ts\":" + String(millis()) + "}";
      if (mqttAutoMode) {
        for (int i = 0; i < MULTI_BROKER_COUNT; i++) {
          if (multiBrokerClient[i].connected()) {
            pubOk = multiBrokerClient[i].publish(testTopic.c_str(), testPayload.c_str());
            break;
          }
        }
      } else {
        PubSubClient& mc = getActiveMqttClient();
        pubOk = mc.publish(testTopic.c_str(), testPayload.c_str());
      }
      json += ",\"pub_ok\":" + String(pubOk ? "true" : "false");
      json += ",\"pub_topic\":\"" + testTopic + "\"";
      addDebugLog("[DIAG] MQTT Publish → " + testTopic + " " + (pubOk ? "成功" : "失败"));
    } else {
      json += ",\"pub_ok\":false";
    }

    json += "}";
    addDebugLog("[DIAG] 诊断完成");
    AsyncWebServerResponse* r = request->beginResponse(200, "application/json", json);
    r->addHeader("Access-Control-Allow-Origin", "*");
    request->send(r);
  });

  // WiFi扫描接口（配置tab用）
  statusServer.on("/scan", HTTP_GET, [](AsyncWebServerRequest* request) {
    int n = WiFi.scanNetworks();
    String json = "{\"networks\":[";
    for (int i = 0; i < n && i < 15; i++) {
      if (i > 0) json += ",";
      String ssid = WiFi.SSID(i);
      ssid.replace("\"", "\\\"");
      json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    json += "]}";
    AsyncWebServerResponse* r = request->beginResponse(200, "application/json", json);
    r->addHeader("Access-Control-Allow-Origin", "*");
    request->send(r);
  });

  // 不重启保存WiFi/MQTT配置接口
  statusServer.on("/api/save-config", HTTP_GET, [](AsyncWebServerRequest* request) {
    String result = "{";
    bool wifiChanged = false;
    bool mqttChanged = false;

    String newSsid = request->hasParam("wifi_ssid") ? request->getParam("wifi_ssid")->value() : "";
    String newPass = request->hasParam("wifi_pass") ? request->getParam("wifi_pass")->value() : "";
    String newMqttHost = request->hasParam("mqtt_host") ? request->getParam("mqtt_host")->value() : "";
    String newMqttPortStr = request->hasParam("mqtt_port") ? request->getParam("mqtt_port")->value() : "";
    String newMqttUser = request->hasParam("mqtt_user") ? request->getParam("mqtt_user")->value() : "";
    String newMqttPass = request->hasParam("mqtt_pass") ? request->getParam("mqtt_pass")->value() : "";
    String newHostName = request->hasParam("mqtt_h_name") ? request->getParam("mqtt_h_name")->value() : "";
    String newTargetMac = request->hasParam("mqtt_target_mac") ? request->getParam("mqtt_target_mac")->value() : "";

    preferences.begin("c1cfg", false);

    if (newSsid.length() > 0 && newSsid != wifiSsid) {
      wifiSsid = newSsid;
      preferences.putString("ssid", newSsid);
      wifiChanged = true;
    }
    if (newPass.length() > 0 && newPass != wifiPass) {
      wifiPass = newPass;
      preferences.putString("pass", newPass);
      wifiChanged = true;
    }
    if (newMqttHost.length() > 0 && newMqttHost != mqttHost) {
      mqttHost = newMqttHost;
      preferences.putString("mqtt_host", newMqttHost);
      mqttChanged = true;
    }
    if (newMqttPortStr.length() > 0) {
      int np = newMqttPortStr.toInt();
      if (np > 0 && np != mqttPort) {
        mqttPort = np;
        preferences.putInt("mqtt_port", np);
        mqttChanged = true;
      }
    }
    if (newMqttUser.length() > 0 && newMqttUser != mqttUser) {
      mqttUser = newMqttUser;
      preferences.putString("mqtt_user", newMqttUser);
      mqttChanged = true;
    }
    if (newMqttPass.length() > 0 && newMqttPass != mqttPass) {
      mqttPass = newMqttPass;
      preferences.putString("mqtt_pass", newMqttPass);
      mqttChanged = true;
    }
    if (newHostName.length() > 0 && newHostName != mqttHostName) {
      mqttHostName = newHostName;
      preferences.putString("mqtt_h_name", newHostName);
      mqttChanged = true;
    }
    if (newTargetMac.length() > 0 && newTargetMac != mqttTargetMac) {
      mqttTargetMac = newTargetMac;
      preferences.putString("mqtt_target_mac", newTargetMac);
      mqttChanged = true;
    }
    preferences.end();

    if (wifiChanged) {
      addDebugLog("[CONFIG] WiFi配置已更新: " + wifiSsid);
      // 不调用WiFi.disconnect()，直接begin()覆盖旧配置，避免破坏AP
      WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    }
    if (mqttChanged) {
      addDebugLog("[CONFIG] MQTT配置已更新: " + mqttHost + ":" + String(mqttPort) + " name=" + mqttHostName);
      PubSubClient& c = getActiveMqttClient();
      if (c.connected()) c.disconnect();
    }

    result += "\"ok\":true";
    result += ",\"wifi_changed\":" + String(wifiChanged ? "true" : "false");
    result += ",\"mqtt_changed\":" + String(mqttChanged ? "true" : "false");
    result += "}";
    AsyncWebServerResponse* r = request->beginResponse(200, "application/json", result);
    r->addHeader("Access-Control-Allow-Origin", "*");
    request->send(r);
  });

  // 静默模式开关接口
  statusServer.on("/api/silent", HTTP_GET, [](AsyncWebServerRequest* request) {
    silentMode = true;
    preferences.begin("c1cfg", false);
    preferences.putBool("silent", true);
    preferences.end();
    addDebugLog("[SILENT] 静默模式已开启，LED和配置热点将在重启后关闭");
    AsyncWebServerResponse* r = request->beginResponse(200, "application/json", "{\"ok\":true,\"msg\":\"silent mode enabled, rebooting...\"}");
    r->addHeader("Access-Control-Allow-Origin", "*");
    request->send(r);
    pendingRestart = true;
    restartAt = millis() + 500;
  });

  statusServer.begin();
  statusApActive = true;
  addDebugLog("[STATUS-AP] 状态热点已启动: " + statusApSsid);
}

void stopStatusAP() {
  if (!statusApActive) return;
  statusServer.end();
  WiFi.softAPdisconnect(true);
  statusApActive = false;
  addDebugLog("[STATUS-AP] 状态热点已关闭");
}

void checkFactoryResetButton() {
  static unsigned long pressedAt = 0;
  static bool wasPressed = false;
  static unsigned long lastLedToggle = 0;
  static bool ledState = false;
  bool pressed = digitalRead(CONFIG_BUTTON_PIN) == LOW;
  unsigned long now = millis();
  if (pressed) {
    factoryResetHeld = true;
    if (!wasPressed) {
      pressedAt = now;
      ledState = false;
      lastLedToggle = now;
    }
    // 按住期间LED每250ms边沿切换，不依赖loop周期精度
    if (now - lastLedToggle >= 250) {
      lastLedToggle = now;
      ledState = !ledState;
      setStatusLed(ledState);
    }
    if (now - pressedAt >= 5000) {
      addDebugLog("[RESET] 检测到GPIO9长按5秒，恢复初始化...");
      factoryReset();
    }
  } else {
    if (wasPressed) {
      setStatusLed(false);
      factoryResetHeld = false;
    }
  }
  wasPressed = pressed;
}

void factoryReset() {
  preferences.begin("c1cfg", false);
  preferences.clear();
  preferences.end();
  addDebugLog("[RESET] 配置已清除，重启中...");
  // 急速闪烁5次提示恢复出厂设置已触发
  for (int i = 0; i < 5; i++) {
    setStatusLed(true);
    delay(80);
    setStatusLed(false);
    delay(80);
  }
  ESP.restart();
}

void setup() {
  // 初始化硬件
  pinMode(STATUS_LED, OUTPUT);
  setStatusLed(false);
  pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);
  startTime = millis();

  Serial.begin(115200);
  delay(1000);

  // 初始化硬件看门狗 (ESP-IDF v4.x API)
  esp_task_wdt_init(WDT_TIMEOUT_SEC, true);  // 60秒超时, true=触发panic重启
  esp_task_wdt_add(NULL);                    // 监控主任务
  Serial.println("[WDT] 硬件看门狗已启用 (60秒超时)");

  Serial.println("ESP32-C3 BLE配置程序V3.7-OTA启动");

  // 获取芯片唯一序列号
  uint64_t chipId64 = ESP.getEfuseMac();
  char chipIdBuf[17];
  snprintf(chipIdBuf, sizeof(chipIdBuf), "%04X%08X",
           (uint16_t)(chipId64 >> 32), (uint32_t)chipId64);
  chipSerialNumber = String(chipIdBuf);
  Serial.printf("[CHIP] 芯片序列号: %s\n", chipSerialNumber.c_str());

  // 初始化NVS
  esp_err_t nvsErr = nvs_flash_init();
  if (nvsErr == ESP_ERR_NVS_NO_FREE_PAGES || nvsErr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  // 启动早期检测：GPIO9 长按 ≥5秒 恢复初始化
  if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
    Serial.println("[RESET] 启动时检测到初始化键按下，长按5秒可恢复初始化...");
    unsigned long t0 = millis();
    while (millis() - t0 < 5000) {
      if (digitalRead(CONFIG_BUTTON_PIN) != LOW) {
        Serial.println("[RESET] 取消恢复（按键已松开）");
        break;
      }
      setStatusLed(((millis() / 200) % 2) == 0);
      delay(10);
    }
    setStatusLed(false);
    if (digitalRead(CONFIG_BUTTON_PIN) == LOW && millis() - t0 >= 5000) {
      Serial.println("[RESET] 启动阶段长按5秒确认，执行恢复初始化");
      factoryReset();
      return;
    }
  }

  // 加载持久化配置
  loadConfiguration();

  // 加载用户预设（但不覆盖代码中的原始预设）
  if (loadPresetsFromNVS()) {
    addDebugLog("[INFO] 从NVS加载预设成功");
  } else {
    addDebugLog("[INFO] 使用代码中的默认预设（首次启动或NVS无数据）");
    // 显示代码中的原始预设值
    for (int i = 0; i < PRESET_COUNT; i++) {
      addDebugLog("[PRESET] 预设" + String(i + 1) + ": " + formatMacAddress(presets[i].mac) + " - " + presets[i].name);
    }
    savePresetsToNVS();  // 保存默认预设到NVS
  }

  // 加载上次使用的MAC
  uint8_t targetBleMac[6];

  // 先显示预设MAC的值
  // addDebugLog("[DEBUG] 预设1 MAC: " + formatMacAddress(presets[0].mac));

  /*
  if (loadMacFromNVS(targetBleMac)) {
    addDebugLog("[INFO] 从NVS加载MAC成功: " + formatMacAddress(targetBleMac));
  } else {
    addDebugLog("[INFO] 使用预设1作为初始MAC");
    memcpy(targetBleMac, presets[0].mac, 6);
  }
  */

  // 读取设备原始MAC地址
  uint8_t originalMac[6];
  esp_read_mac(originalMac, ESP_MAC_WIFI_STA);
  // addDebugLog("[DEBUG] 设备原始WiFi MAC: " + formatMacAddress(originalMac));

  uint8_t originalBleMac[6];
  esp_read_mac(originalBleMac, ESP_MAC_BT);
  // addDebugLog("[DEBUG] 设备原始蓝牙MAC: " + formatMacAddress(originalBleMac));

  // 初始化基本MAC
  /*
  memcpy(baseMac, targetBleMac, 6);
  baseMac[5] = (uint8_t)(targetBleMac[5] - BLE_MAC_OFFSET);
  addDebugLog("[DEBUG] 初始化目标MAC: " + formatMacAddress(targetBleMac));
  addDebugLog("[DEBUG] 初始化基本MAC: " + formatMacAddress(baseMac));
  
  esp_err_t macSetResult = esp_base_mac_addr_set(baseMac);
  addDebugLog("[DEBUG] 基本MAC设置结果: " + String(macSetResult));
  */

  // 验证初始蓝牙MAC
  uint8_t actualBleMac[6];
  getBluetoothMac(actualBleMac);
  Serial.printf("[INFO] 初始蓝牙MAC: %s\n", formatMacAddress(actualBleMac).c_str());

  delay(100);  // 给予串口输出和堆整理一点时间

  // 初始化BLE
  BLEDevice::init("ESP32-C1-Mimicker");  // 避免传入空字符串，防止部分版本下的堆损坏
  BLEServer* pServer = BLEDevice::createServer();
  BLEService* pService = pServer->createService(SERVICE_UUID);

  BLECharacteristic* pChar1 = pService->createCharacteristic(
    CHAR_ONE_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  BLECharacteristic* pChar2 = pService->createCharacteristic(
    CHAR_TWO_UUID,
    BLECharacteristic::PROPERTY_WRITE);

  // 使用共享回调实例，避免内存泄漏
  if (pSharedCallbacks == nullptr) {
    pSharedCallbacks = new MyCallbacks();
  }
  pChar1->setCallbacks(pSharedCallbacks);
  pChar2->setCallbacks(pSharedCallbacks);
  pService->start();

  pAdvertising = pServer->getAdvertising();
  BLEAdvertisementData advData;
  BLEAdvertisementData scanRespData;
  pAdvertising->setAdvertisementData(advData);
  pAdvertising->setScanResponseData(scanRespData);

  // 使用智能分包配置
  configureAdvertising(bleRaw, bleDataLen);

  // 标记BLE已初始化（避免updateMacAddress重复初始化）
  bleInitialized = true;

  // 未同步数据前，不启动广播
  isAdvertising = false;

  // 首次未配置则进入AP引导；已配置后连接STA访问服务器
  if (!isConfigured) {
    Serial.println("[CONFIG] 首次启动未配置，进入AP配置模式");
    setupAPAndDNS();
    startFirstBootAP();
    apModeActive = true;
    // 进入初始化AP模式：慢速长闪5次（亮500ms灭300ms）区别于普通启动
    for (int i = 0; i < 5; i++) {
      setStatusLed(true);
      delay(500);
      setStatusLed(false);
      delay(300);
    }
  } else {
    Serial.println("[CONFIG] 已配置，跳过AP，连接STA访问服务器");

    // 先启动状态AP（运行时热点），确保WiFi断开时仍可访问后台
    if (!silentMode) {
      startStatusAP();
    } else {
      Serial.println("[SILENT] 静默模式，跳过配置热点");
    }

    // 再连接STA（此时statusApActive=true，会保持AP+STA模式）
    connectWiFiSTA();
  }

  // 使用代码中定义的设备名称
  c1DeviceId = customDeviceName;  // 直接使用代码中定义的设备名称
  addDebugLog("[INFO] 使用代码中定义的设备名称: " + customDeviceName);

  // 显示设备信息
  Serial.println("\n=== C1设备信息 ===");
  Serial.printf("设备ID: %s\n", c1DeviceId.c_str());
  Serial.printf("服务器地址: %s\n", SERVER_BASE.c_str());
  Serial.printf("睡眠超时: %d 分钟\n", SLEEP_TIMEOUT / 60000);
  Serial.printf("广播数据长度: %d 字节\n", sizeof(bleRaw));
  Serial.printf("当前蓝牙MAC: %s\n", formatMacAddress(actualBleMac).c_str());
  Serial.printf("广播状态: %s\n", isAdvertising ? "运行中" : "已停止");

  // 设置默认目标MAC（如果没有Web指定的话）
  /*
  if (cloudTargetMac.length() == 0) {
    cloudTargetMac = formatMacAddress(presets[0].mac);  // 使用预设1作为默认目标
    addDebugLog("[INFO] 设置默认目标MAC: " + cloudTargetMac);
  }
  */

  // 上电后立即从云端获取一次目标MAC对应的广播数据（若后台已存在）
  addDebugLog("[INFO] 开始从云端拉取广播数据...");
  if (dataMode == "mqtt") {
    mqttSyncOnce(4000);
  } else {
    fetchCloudAdvOnce();
  }

  // 启动成功闪烁指示灯（仅已配置模式）
  if (!apModeActive) {
    for (int i = 0; i < 3; i++) {
      setStatusLed(true);
      delay(200);
      setStatusLed(false);
      delay(200);
    }
  }
}

void loop() {
  // 喂狗，防止看门狗超时重启
  esp_task_wdt_reset();

  // 延时重启（由/save或/restart请求触发）
  if (pendingFactoryReset && millis() >= restartAt) {
    factoryReset();
  } else if (pendingRestart && millis() >= restartAt) {
    ESP.restart();
  }

  // AP模式或状态热点模式时处理DNS请求（captive portal）
  if (apModeActive || statusApActive) {
    dnsServer.processNextRequest();
  }
  // 初始化模式LED闪烁（仅首次配置AP时）
  if (apModeActive) {
    static unsigned long apLedToggle = 0;
    static bool apLedState = false;
    if (millis() - apLedToggle >= 300) {
      apLedToggle = millis();
      apLedState = !apLedState;
      setStatusLed(apLedState);
    }
  }
  delay(100);

  // 运行时长按恢复初始化检测
  checkFactoryResetButton();

  // 检查是否需要进入深度睡眠（AP配置模式或状态热点运行时不进入睡眠）
  if (!neverSleep && !apModeActive && !statusApActive && (millis() - startTime > SLEEP_TIMEOUT)) {
    Serial.println("运行时间已到，进入深度睡眠");
    addDebugLog("[SLEEP] 运行时间到，进入深度睡眠");
    setStatusLed(true);
    delay(1000);
    setStatusLed(false);
    esp_deep_sleep_start();
  }

  // 状态指示灯心跳（仅在非初始化模式且未长按复位键时生效）
  if (!apModeActive && !factoryResetHeld) {
    if (isAdvertising && (millis() % 2000 < 100)) {
      setStatusLed(true);
    } else {
      setStatusLed(false);
    }
  }

  // WiFi断连保护：低频温和重连，绝不破坏AP
  static unsigned long wifiDisconnectedSince = 0;
  static unsigned long lastWiFiReconnectAttempt = 0;
  static int wifiReconnectCount = 0;

  if (!apModeActive && isConfigured && WiFi.status() != WL_CONNECTED) {
    if (wifiDisconnectedSince == 0) {
      wifiDisconnectedSince = millis();
      wifiReconnectCount = 0;
      Serial.println("[WiFi] 检测到WiFi断开");
      addDebugLog("[WiFi] 检测到断开");
    }

    // AP健康保护：确保AP模式没有被WiFi操作破坏
    if (statusApActive) {
      wifi_mode_t curMode = WiFi.getMode();
      if (curMode != WIFI_MODE_APSTA && curMode != WIFI_MODE_AP) {
        Serial.println("[WiFi] AP模式异常，恢复AP_STA");
        addDebugLog("[WiFi] AP模式被破坏，恢复中");
        WiFi.mode(WIFI_AP_STA);
      }
    }

    // 每60秒做一次温和的手动重连
    // ★ 不调用WiFi.disconnect()或WiFi.reconnect()（它们会重置WiFi栈破坏AP）
    //   直接WiFi.begin()覆盖旧配置尝试连接
    if (millis() - lastWiFiReconnectAttempt > 60000) {
      lastWiFiReconnectAttempt = millis();
      wifiReconnectCount++;
      Serial.printf("[WiFi] 重连尝试 #%d...\n", wifiReconnectCount);
      addDebugLog("[WiFi] 重连 #" + String(wifiReconnectCount));
      const char* ssid = (wifiSsid.length() > 0) ? wifiSsid.c_str() : DEFAULT_STA_SSID;
      const char* pass = (wifiPass.length() > 0) ? wifiPass.c_str() : DEFAULT_STA_PASSWORD;
      WiFi.begin(ssid, pass);
    }

    // 不再强制重启——30分钟定期重启(AUTO_RESTART_MS)已涵盖长时间离线场景
  } else if (WiFi.status() == WL_CONNECTED) {
    if (wifiDisconnectedSince != 0) {
      // 刚恢复连接
      Serial.printf("[WiFi] 重连成功！断开时长: %lu ms, IP: %s, RSSI: %d dBm\n",
                    millis() - wifiDisconnectedSince,
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      addDebugLog("[WiFi] 重连成功");
      // WiFi恢复后，重新连接MQTT
      if (dataMode == "mqtt") {
        ensureMqttConnection();
      }
    }
    wifiDisconnectedSince = 0;
    wifiReconnectCount = 0;
  }

  // 周期性（30秒）重试拉取一次云端数据（B设备可能离线后再上电）
  if (dataMode == "mqtt") {
    mqttLoop();
  }

  if (dataMode == "server" && WiFi.status() == WL_CONNECTED && millis() - lastCloudFetch > 30000) {
    Serial.printf("\n[PERIODIC] 周期性拉取云端数据 (距离上次: %lu ms)\n", millis() - lastCloudFetch);
    fetchCloudAdvOnce();
  }

  // 自定义服务器模式：周期性上报C1状态（每60秒）
  static unsigned long lastC1StatusReport = 0;
  if (dataMode == "server" && WiFi.status() == WL_CONNECTED && millis() - lastC1StatusReport > 60000) {
    lastC1StatusReport = millis();
    reportC1StatusToServer();
  }

  // 30分钟自动重启（清理内存碎片，提高稳定性）
  // 仅在配置完成且非AP模式时生效
  if (isConfigured && !apModeActive && millis() > AUTO_RESTART_MS) {
    Serial.printf("\n[SYSTEM] 已运行%.1f分钟，执行定期重启以优化性能...\n", millis() / 60000.0);
    addDebugLog("[SYSTEM] 30分钟定期重启");
    delay(1000);
    esp_restart();
  }

  // 定期清理macTimestamps（每5分钟清理一次，保留最近10分钟的条目）
  static unsigned long lastMacTimestampCleanup = 0;
  if (millis() - lastMacTimestampCleanup > 300000) {  // 5分钟
    lastMacTimestampCleanup = millis();
    int64_t cutoff = (int64_t)millis() - 600000;  // 10分钟前的时间戳
    int removed = 0;
    for (auto it = macTimestamps.begin(); it != macTimestamps.end();) {
      // 注意：ts是O5的millis()，不是当前设备的，这里简化处理，清理最小的那些
      if (macTimestamps.size() > 20) {  // 只有超过20个条目才清理
        it = macTimestamps.erase(it);
        removed++;
        if (macTimestamps.size() <= 10) break;  // 保留10个
      } else {
        ++it;
      }
    }
    if (removed > 0) {
      Serial.printf("[MEM] 清理macTimestamps: 移除%d条旧记录, 剩余%d条\n", removed, macTimestamps.size());
    }
  }

  // 定期显示设备状态
  static unsigned long lastStatusTime = 0;
  if (millis() - lastStatusTime > 15000) {  // 每15秒显示一次
    lastStatusTime = millis();
    Serial.println("\n=== C1设备状态 ===");
    Serial.printf("设备ID: %s\n", c1DeviceId.c_str());
    Serial.printf("数据模式: %s\n", dataMode.c_str());
    Serial.printf("目标MAC: %s\n", cloudTargetMac.c_str());
    Serial.printf("广播状态: %s\n", isAdvertising ? "运行中" : "已停止");
    Serial.printf("WiFi状态: %s\n", WiFi.status() == WL_CONNECTED ? "已连接" : "未连接");
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("WiFi信号: %d dBm\n", WiFi.RSSI());
    }

    // 显示MQTT连接状态
    if (dataMode == "mqtt") {
      PubSubClient& client = getActiveMqttClient();
      Serial.printf("MQTT状态: %s\n", client.connected() ? "已连接" : "未连接");
      Serial.printf("MQTT服务器: %s:%d (%s)\n", mqttHost.c_str(), mqttPort, mqttPort == 8883 ? "TLS" : "TCP");
      Serial.printf("订阅Topic: %s\n", mqttSubscribedTopic.c_str());
      Serial.printf("最后消息: %lu ms前\n", lastMqttMsgMs > 0 ? (millis() - lastMqttMsgMs) : 0);
    }

    uint8_t actualBleMac[6];
    getBluetoothMac(actualBleMac);
    Serial.printf("当前蓝牙MAC: %s\n", formatMacAddress(actualBleMac).c_str());
    Serial.printf("广播数据长度: %d 字节\n", sizeof(bleRaw));

    // 内存监控日志
    Serial.printf("空闲堆内存: %u bytes\n", ESP.getFreeHeap());
    Serial.printf("最小空闲堆: %u bytes\n", ESP.getMinFreeHeap());
    Serial.printf("堆碎片率: %.1f%%\n", 100.0 - (100.0 * ESP.getMaxAllocHeap() / ESP.getFreeHeap()));

    // 内存警告
    if (ESP.getFreeHeap() < 20000) {
      Serial.println("[WARNING] 空闲内存不足20KB，可能存在内存泄漏！");
      addDebugLog("[MEM] 警告: 空闲内存不足20KB");
    }
  }
}

// ===== 新增：云端通信 =====

void connectWiFiSTA() {
  Serial.println("\n=== 连接外部WiFi(STA) ===");
  const char* ssid = (wifiSsid.length() > 0) ? wifiSsid.c_str() : DEFAULT_STA_SSID;
  const char* pass = (wifiPass.length() > 0) ? wifiPass.c_str() : DEFAULT_STA_PASSWORD;
  Serial.printf("尝试连接WiFi: %s\n", ssid);

  // 如果状态热点已启动，WiFi已在AP+STA模式，不再调WiFi.mode()避免重置AP
  if (!statusApActive) {
    WiFi.mode(WIFI_STA);
  }

  WiFi.begin(ssid, pass);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    esp_task_wdt_reset();                                // 喂狗
    if (statusApActive) dnsServer.processNextRequest();  // 等待连接期间保持后台可访问
    delay(300);
    Serial.print('.');
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[SUCCESS] STA已连接\n");
    Serial.printf("  - IP地址: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("  - 网关: %s\n", WiFi.gatewayIP().toString().c_str());
    Serial.printf("  - 子网掩码: %s\n", WiFi.subnetMask().toString().c_str());
    Serial.printf("  - DNS: %s\n", WiFi.dnsIP().toString().c_str());
    Serial.printf("  - 信号强度: %d dBm\n", WiFi.RSSI());

    if (dataMode == "mqtt") {
      ensureMqttConnection();
    }
  } else {
    Serial.println("\n[ERROR] STA连接失败");
    Serial.printf("  - 连接超时: %lu ms\n", millis() - start);
    Serial.printf("  - 最后状态: %d\n", WiFi.status());
    if (!isConfigured && !apModeActive) {
      // 首次配置失败，回到AP
      setupAPAndDNS();
      startFirstBootAP();
      apModeActive = true;
    }
  }
}

// 从云端拉取一次目标MAC的广播数据
void fetchCloudAdvOnce() {
  lastCloudFetch = millis();
  addDebugLog("[CLOUD] === 开始云端拉取广播数据 ===");

  if (WiFi.status() != WL_CONNECTED) {
    addDebugLog("[ERROR] 云端拉取跳过：未连接网络");
    return;
  }

  if (dataMode == "mqtt") {
    addDebugLog("[MQTT] MQTT模式下不使用HTTP拉取，等待订阅消息");
    return;
  }

  // Server 模式 (原有逻辑)
  if (SERVER_BASE.length() == 0) {
    addDebugLog("[ERROR] 服务器地址未配置");
    return;
  }

  HTTPClient http;
  String url = SERVER_BASE + "/api/c1/poll?deviceId=" + c1DeviceId;

  addDebugLog("[CLOUD] Requesting: " + url);
  http.begin(url);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    // 假设服务器返回的是 JSON，包含 data 字段，或者直接是 raw string
    // 这里为了简单，假设直接返回 raw string (hex string)
    // 如果是 JSON，需要解析
    if (payload.startsWith("{")) {
      DynamicJsonDocument doc(1024);
      deserializeJson(doc, payload);

      if (doc.containsKey("updatedAt")) {
        int64_t incomingTs = doc["updatedAt"].as<long long>();
        if (incomingTs > 0 && lastAppliedDataTs > 0 && incomingTs <= lastAppliedDataTs) {
          addDebugLog("[CLOUD] 忽略旧数据 updatedAt=" + String((long long)incomingTs) + " last=" + String((long long)lastAppliedDataTs));
          http.end();
          return;
        }
        if (incomingTs > 0) lastAppliedDataTs = incomingTs;
      }

      if (doc.containsKey("advData")) {  // 改为 advData，因为 O5 上报的字段是 advData
        cloudAdvData = doc["advData"].as<String>();
      } else if (doc.containsKey("data")) {
        cloudAdvData = doc["data"].as<String>();
      } else {
        cloudAdvData = payload;  // Fallback
      }

      // 如果返回包含MAC，更新目标MAC
      if (doc.containsKey("mac")) {
        String newTargetMac = doc["mac"].as<String>();
        if (newTargetMac.length() == 17) {
          cloudTargetMac = newTargetMac;
          addDebugLog("[CLOUD] 从服务器获取到新目标MAC: " + cloudTargetMac);
        }
      }
    } else {
      cloudAdvData = payload;
    }

    addDebugLog("[CLOUD] Server returned: " + cloudAdvData);
    tryApplyCloudAdv();
  } else {
    addDebugLog("[CLOUD] Server request failed: " + String(code));
  }
  http.end();

  // 拉取成功后上报状态
  reportC1StatusToServer();
}

// 上报C1状态到服务器（自定义服务器模式）
void reportC1StatusToServer() {
  if (dataMode != "server") return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (SERVER_BASE.length() == 0) return;

  // 获取当前实际的蓝牙MAC
  uint8_t actualBleMac[6];
  getBluetoothMac(actualBleMac);
  String currentMac = formatMacAddress(actualBleMac);

  // 构建广播数据的hex字符串
  String advDataHex = "0x";
  for (int i = 0; i < bleDataLen; i++) {
    if (bleRaw[i] < 0x10) advDataHex += "0";
    advDataHex += String(bleRaw[i], HEX);
  }
  advDataHex.toUpperCase();

  HTTPClient http;
  String url = SERVER_BASE + "/api/c1/report";

  // 构建JSON payload
  String payload = "{";
  payload += "\"deviceId\":\"" + c1DeviceId + "\",";
  payload += "\"mac\":\"" + currentMac + "\",";
  payload += "\"advData\":\"" + advDataHex + "\",";
  payload += "\"source\":\"" + String(dataMode == "mqtt" ? "mqtt" : "server") + "\",";
  payload += "\"isAdvertising\":" + String(isAdvertising ? "true" : "false") + ",";
  payload += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  payload += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
  payload += "\"uptime\":" + String(millis() / 1000);
  payload += "}";

  Serial.printf("[C1-REPORT] 上报状态: %s\n", url.c_str());
  Serial.printf("[C1-REPORT] Payload: %s\n", payload.c_str());

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);

  if (code == 200) {
    Serial.println("[C1-REPORT] 状态上报成功");
    addDebugLog("[C1] 状态上报成功");
  } else {
    Serial.printf("[C1-REPORT] 状态上报失败: %d\n", code);
    addDebugLog("[C1] 状态上报失败: " + String(code));
  }
  http.end();
}

// 尝试应用云端数据
void tryApplyCloudAdv() {
  Serial.println("\n=== 开始应用云端广播数据 ===");
  Serial.printf("原始数据: %s\n", cloudAdvData.c_str());

  // 将连续0x.. hex 串转成逗号分隔形式复用已有逻辑
  String s = cloudAdvData;

  // 去掉所有0x和0X前缀
  s.replace("0x", "");
  s.replace("0X", "");

  // 去掉非十六进制字符
  String clean;
  for (int k = 0; k < s.length(); k++) {
    char c = s[k];
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) {
      clean += c;
    }
  }

  Serial.printf("清理后数据: %s\n", clean.c_str());
  Serial.printf("数据长度: %d 字符\n", clean.length());

  if (clean.length() % 2 != 0) {
    Serial.println("[ERROR] 数据长度不是偶数，无法解析");
    return;
  }

  // 转换为逗号分隔的字节格式
  String bytesStr = "";
  int i = 0;
  while (i < clean.length()) {
    String byteStr = clean.substring(i, i + 2);
    bytesStr += "0x" + byteStr + ",";
    i += 2;
  }

  Serial.printf("转换后格式: %s\n", bytesStr.c_str());
  Serial.printf("字节数量: %d\n", clean.length() / 2);

  // 应用广播数据
  updateAdvertisingData(std::string(bytesStr.c_str()));

  // 强制更新MAC为目标MAC (如果目标MAC与当前不同)
  // 这里从 cloudTargetMac 获取目标MAC
  if (cloudTargetMac.length() == 17) {
    Serial.printf("[CLOUD] 尝试更新MAC至: %s\n", cloudTargetMac.c_str());
    updateMacAddress(std::string(cloudTargetMac.c_str()));
  }

  dataSynced = true;
  startBleAdvertisingIfAllowed();

  Serial.println("[SUCCESS] 已应用云端广播数据和MAC到BLE");
}

// Web调试日志函数
void addDebugLog(String message) {
  // 添加时间戳
  String timestamp = String(millis() / 1000) + "s: ";
  String logEntry = timestamp + message + "\n";

  // 检查日志大小，如果太大则清理旧日志
  if (debugLog.length() + logEntry.length() > MAX_DEBUG_LOG_SIZE) {
    // 保留最后一半的日志
    int halfSize = debugLog.length() / 2;
    int newlinePos = debugLog.indexOf('\n', halfSize);
    if (newlinePos > 0) {
      debugLog = debugLog.substring(newlinePos + 1);
    } else {
      debugLog = "";  // 如果找不到换行符，清空日志
    }
  }

  debugLog += logEntry;

  // 同时输出到串口（如果有的话）
  Serial.print(logEntry);
}

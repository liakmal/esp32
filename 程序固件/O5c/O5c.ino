#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <esp_bt.h>
#include <driver/rtc_io.h>
#include <driver/gpio.h>
#include <time.h>
#include <sys/time.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <esp_task_wdt.h>
#include <Update.h>
#include <mbedtls/aes.h>

// OTA 加密密钥（必须与 encrypt_firmware_o5.py 中的 OTA_KEY 一致）
static const uint8_t OTA_AES_KEY[16] = {
  0xA3, 0x7D, 0xE1, 0x5B, 0x42, 0x96, 0xF8, 0x0C,
  0xD7, 0x63, 0x1A, 0x4E, 0xB5, 0x89, 0x2F, 0xC4
};
static const uint8_t OTA_MAGIC[4] = {0xC1, 0x07, 0xA0, 0x01};
static bool otaEncrypted = false;
static uint32_t otaOriginalSize = 0;
static size_t otaHeaderParsed = 0;
static uint8_t otaHeaderBuf[8];
static mbedtls_aes_context otaAesCtx;
static size_t otaTotalWritten = 0;
static uint8_t otaAesBuf[16];
static size_t otaAesBufLen = 0;

// 硬件看门狗配置
#define WDT_TIMEOUT_SEC 60  // 60秒看门狗超时

 #include <mbedtls/ssl.h>
 #include <mbedtls/ssl_cache.h>
 #include <mbedtls/ctr_drbg.h>
 #include <mbedtls/entropy.h>
 #include <mbedtls/net_sockets.h>
 #include <mbedtls/x509_crt.h>
 #include <mbedtls/error.h>

HardwareSerial ModemSerial(1);

// 硬件配置
#define STATUS_LED 13  // 状态指示灯引脚（初始化时高频闪烁）
#define CONFIG_BUTTON_PIN 9

#define ML307_TX_PIN 2
#define ML307_RX_PIN 3
#define ML307_EN_PIN 10   // EN引脚，板载电源使能（HIGH=供电，LOW=断电）

// 功率管理配置
// #define SCAN_DURATION 3  // 扫描持续时间 (降低以减少内存压力)
// #define SCAN_INTERVAL 5  // 扫描间隔 (增加间隔以防止内存耗尽)
int scanDuration = 3;
int scanInterval = 5;

// 定时配置（默认值，可被配置覆盖）
#define WAKE_HOUR 06     // 唤醒小时 (24小时制)
#define WAKE_MINUTE 45   // 唤醒分钟
#define SLEEP_HOUR 07    // 睡眠小时
#define SLEEP_MINUTE 15  // 睡眠分钟

// 时间段结构体
struct TimeSlot {
  int wakeHour;
  int wakeMinute;
  int sleepHour;
  int sleepMinute;
  bool enabled;
};

// 时间段列表（最多支持5个时间段）
#define MAX_TIME_SLOTS 5
TimeSlot timeSlots[MAX_TIME_SLOTS];
int timeSlotCount = 0;

// 心跳报告配置
#define HEARTBEAT_INTERVAL_MS 7200000  // 2小时 = 2 * 60 * 60 * 1000 毫秒

// 默认网络配置（首次启动未配置时）
const char* DEFAULT_STA_SSID = "CDIVTC";
const char* DEFAULT_STA_PASSWORD = "";
String SERVER_BASE = "http://124.132.136.17:9025";

// 可变网络配置（从持久化加载）
String wifiSsid = "";
String wifiPass = "";

String netMode = "wifi";
String ml307Apn = "cmnet";
int ml307Baud = 115200;

// 配置与AP门户
Preferences preferences;
WebServer configServer(80);
DNSServer dnsServer;
const byte DNS_PORT = 53;
IPAddress AP_LOCAL_IP = IPAddress(192, 168, 4, 1);
bool isConfigured = false;
bool apModeActive = false;
bool configApTransition = false;  // 标记从配置AP过渡到运行模式

// 静默模式（关闭LED和配置WiFi热点，仅恢复出厂可解除）
bool silentMode = false;

// 状态AP（运行时热点，复用configServer 80端口）
bool statusApActive = false;
String statusApSsid = "";
void startStatusAP();
void stopStatusAP();
void handleStatusPage();
void handleStatusReset();
void handleStatusSetScan();

// Web运行日志系统
#define MAX_DEBUG_LOG_SIZE 4096  // 日志缓冲区最大4KB，防止内存溢出
String debugLog = "";
void addDebugLog(String message);

// 设备标识
String deviceId = "";
String chipSerialNumber = "";  // ESP32芯片唯一序列号（用于MQTT客户端ID）

// LeanCloud配置
String lcAppId = "";
String lcAppKey = "";
String lcApiUrl = "";
String lcTargetMac = "";
const String lcClassName = "/1.1/classes/AdvertisingData";

// MQTT配置
// EMQX公共服务器: broker.emqx.io, TCP:1883, TLS:8883
// 注意: 公共服务器不需要用户名密码，所有消息公开可见
String mqttHost = "broker.emqx.io";
int mqttPort = 1883;  // 使用TCP明文端口（ML307R兼容性更好）
String mqttUser = "";
String mqttPass = "";
String mqttDeviceName = "";  // 主机名称，用于MQTT topic前缀（如 ASXD）
// Topic格式: {主机名称}/{mac}，例如 ASXD/F8A7639FF48E

// ===== MQTT自动模式：自动选择broker，连接失败自动切换 =====
bool mqttAutoMode = true;  // 默认开启自动模式
static const char* AUTO_BROKERS[] = { "broker.emqx.io", "broker.hivemq.com" };
static const int AUTO_BROKER_COUNT = 2;
int mqttAutoBrokerIdx = 0;  // 当前使用的broker索引

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

WiFiClientSecure mqttNetClient;

WiFiClient mqttNetClientPlain;

// 扫描到的设备信息（提前定义以便编译器生成函数原型）
struct ScannedDevice {
  String mac;
  String name;
  int rssi;
  String advData;
  String matchedUuid;
  unsigned long lastSeen;
};

// forward declarations for ML307 MQTT transport
extern SemaphoreHandle_t modemMutex;
bool ml307EnsureNetwork();
bool ml307SendAT(const String& cmd, const char* expect1, const char* expect2, unsigned long timeoutMs, String* outResp);
static bool ml307MIPOpen(int connectId, const String& protoType, const String& host, int port, int timeoutSec, int accessMode, int localPort, unsigned long urcTimeoutMs);
static bool ml307MIPClose(int connectId);

static bool ml307MIPSendRaw(int connectId, const uint8_t* data, size_t len);
static int ml307MIPRDReadRaw(int connectId, int readLen, uint8_t* out, int outCap, int& outUnreadLeft, unsigned long timeoutMs);
static int ml307WaitForRecvUrc(int connectId, unsigned long timeoutMs);

static bool ml307NetOpenSupported();
static bool ml307MdialupSupported();
static bool ml307MIPCallSupported();
static bool ml307TryConfigureDnsServers();
static bool ml307ResolveHostToIp(const String& host, String& outIp);

extern String g_ml307LastMIPRD;
extern String g_ml307LastMIPSEND;
extern String g_ml307LastMIPOPEN;

class ML307MqttClient : public Client {
public:
  explicit ML307MqttClient(int connectId = 1) : _id(connectId) {
    _rx.reserve(1024);
  }

  int connect(IPAddress ip, uint16_t port) override {
    (void)ip;
    (void)port;
    return 0;
  }

  int connect(const char* host, uint16_t port) override {
    if (host == nullptr) return 0;
    if (_connected) return 1;
    if (!ml307EnsureNetwork()) return 0;

    String openHost = String(host);
    String ip;
    if (ml307ResolveHostToIp(openHost, ip)) {
      openHost = ip;
    }

    // 按《SSL用户手册 V5.3.0》流程：
    // 1) 写入 CA 证书(如不存在) 2) 配置 ssl_id 单向认证 3) MIPCFG 绑定 ssl_id 4) MIPOPEN 使用 TCP
    static const int sslId = 1; // 避免与 HTTP(常用 sslId=0)冲突
    static const char* caName = "mqtt_ca_20260125.cer";
    static bool caEnsured = false;
    static bool capsPrinted = false;

    String r;
    if (!capsPrinted) {
      String caps;
      if (ml307SendAT("AT+MSSLCFG=?", "OK", nullptr, 3000, &caps)) {
        Serial.println("[ML307][SSL] AT+MSSLCFG=?");
        Serial.println(caps);
      }
      caps = "";
      if (ml307SendAT("AT+MIPCFG=?", "OK", nullptr, 3000, &caps)) {
        Serial.println("[ML307][NET] AT+MIPCFG=?");
        Serial.println(caps);
      }
      caps = "";
      if (ml307SendAT("AT+MSSLCIPHER=?", "OK", nullptr, 3000, &caps)) {
        Serial.println("[ML307][SSL] AT+MSSLCIPHER=?");
        Serial.println(caps);
      }
      capsPrinted = true;
    }
    if (!caEnsured) {
      String list;
      ml307SendAT("AT+MSSLLIST=1", "OK", nullptr, 3000, &list);
      if (list.indexOf(String("\"") + caName + "\"") < 0) {
        Serial.printf("[ML307][SSL] writing CA cert: %s\n", caName);
        // 写入 CA 证书（PEM），证书内容需使用 \r\n 换行
        String cert = String(mqtt_ca_cert);
        cert.replace("\r\n", "\n");
        cert.replace("\r", "\n");
        cert.replace("\n", "\r\n");
        int certLen = cert.length();

        if (modemMutex) {
          xSemaphoreTake(modemMutex, portMAX_DELAY);
        }
        ModemSerial.print(String("AT+MSSLCERTWR=\"") + caName + "\",0," + String(certLen) + "\r\n");

        unsigned long start = millis();
        bool gotPrompt = false;
        while (millis() - start < 8000) {
          while (ModemSerial.available()) {
            int c = ModemSerial.read();
            if (c < 0) break;
            if (c == '>') {
              gotPrompt = true;
              break;
            }
          }
          if (gotPrompt) break;
          delay(1);
        }
        if (!gotPrompt) {
          if (modemMutex) xSemaphoreGive(modemMutex);
          Serial.println("[ML307][SSL] MSSLCERTWR no prompt");
          return 0;
        }

        ModemSerial.print(cert);

        start = millis();
        String resp;
        while (millis() - start < 15000) {
          while (ModemSerial.available()) {
            char ch = (char)ModemSerial.read();
            resp += ch;
            if (resp.indexOf("OK") >= 0) {
              if (modemMutex) xSemaphoreGive(modemMutex);
              break;
            }
            if (resp.indexOf("ERROR") >= 0 || resp.indexOf("+CME ERROR") >= 0) {
              if (modemMutex) xSemaphoreGive(modemMutex);
              Serial.println("[ML307][SSL] MSSLCERTWR failed");
              Serial.println(resp);
              return 0;
            }
          }
          if (resp.indexOf("OK") >= 0) break;
          delay(1);
        }
        if (resp.indexOf("OK") < 0) {
          if (modemMutex) xSemaphoreGive(modemMutex);
          Serial.println("[ML307][SSL] MSSLCERTWR timeout");
          return 0;
        }
        Serial.printf("[ML307][SSL] CA cert written: %s\n", caName);
      } else {
        Serial.printf("[ML307][SSL] CA cert already present: %s\n", caName);
      }
      caEnsured = true;
    }

    // 配置 TCP 通道参数（避免默认值导致握手异常）
    ml307SendAT(String("AT+MIPCFG=\"cid\",") + String(_id) + ",1", "OK", nullptr, 1000, &r);
    // encoding: 2=转义字符串；最后一项具体含义见TCP/IP手册，先用0(默认)
    ml307SendAT(String("AT+MIPCFG=\"encoding\",") + String(_id) + ",2,0", "OK", nullptr, 1000, &r);
    ml307SendAT(String("AT+MIPCFG=\"timeout\",") + String(_id) + ",120", "OK", nullptr, 1000, &r);
    ml307SendAT(String("AT+MIPCFG=\"rcvbuf\",") + String(_id) + ",8192", "OK", nullptr, 1000, &r);

    // 单向认证 + 绑定 CA（MSSLCFG=? 显示当前固件支持这些项）
    ml307SendAT(String("AT+MSSLCFG=\"encoding\",") + String(sslId) + ",2", "OK", nullptr, 1000, &r);
    ml307SendAT(String("AT+MSSLCFG=\"auth\",") + String(sslId) + ",1", "OK", nullptr, 1000, &r);
    ml307SendAT(String("AT+MSSLCFG=\"cert\",") + String(sslId) + ",\"" + caName + "\",\"\",\"\"", "OK", nullptr, 1000, &r);
    // 某些固件未在 MSSLCFG=? 列出 ignoreverify/ignorestamp，但仍可能支持；尝试设置
    ml307SendAT(String("AT+MSSLCFG=\"ignoreverify\",") + String(sslId) + ",0", "OK", nullptr, 1000, &r);
    ml307SendAT(String("AT+MSSLCFG=\"ignorestamp\",") + String(sslId) + ",1", "OK", nullptr, 1000, &r);
    ml307SendAT(String("AT+MSSLCFG=\"negotime\",") + String(sslId) + ",300", "OK", nullptr, 1000, &r);

    // 探测式设置 SNI（不同固件可能需要 enable 参数）
    ml307SendAT(String("AT+MSSLCFG=\"sni\",") + String(sslId) + ",\"" + String(host) + "\"", "OK", nullptr, 1000, &r);
    ml307SendAT(String("AT+MSSLCFG=\"sni\",") + String(sslId) + ",1,\"" + String(host) + "\"", "OK", nullptr, 1000, &r);
    ml307SendAT(String("AT+MSSLCFG=\"servername\",") + String(sslId) + ",\"" + String(host) + "\"", "OK", nullptr, 1000, &r);
    ml307SendAT(String("AT+MSSLCFG=\"servername\",") + String(sslId) + ",1,\"" + String(host) + "\"", "OK", nullptr, 1000, &r);

    // 绑定 TCP 通道到 ssl_id，并以 SSL 方式建立连接
    ml307SendAT(String("AT+MIPCFG=\"ssl\",") + String(_id) + ",1," + String(sslId), "OK", nullptr, 1000, &r);

    auto tryOpenWithProto = [&](const String& h, uint16_t p) -> bool {
      // 不同固件对 protoType 的要求不同：有的用 TCP+MIPCFG ssl，有的用 SSL/TLS
      const char* protos[] = {"TCP", "SSL", "TLS"};
      for (size_t i = 0; i < (sizeof(protos) / sizeof(protos[0])); i++) {
        Serial.printf("[ML307][MIPOPEN] try proto=%s host=%s port=%u\n", protos[i], h.c_str(), (unsigned)p);
        if (ml307MIPOpen(_id, protos[i], h, (int)p, 60, 0, 0, 20000)) {
          Serial.printf("[ML307][MIPOPEN] open success with proto=%s\n", protos[i]);
          return true;
        }
      }
      return false;
    };

    if (!tryOpenWithProto(String(host), port)) {
      Serial.println("[ML307][SSL] open failed with verify=ON. Will retry with ignoreverify=1 (insecure) to diagnose CA/chain issues.");
      String cfg;
      if (ml307SendAT(String("AT+MSSLCFG=\"auth\",") + String(sslId), "OK", nullptr, 1500, &cfg)) {
        Serial.println("[ML307][SSL] MSSLCFG auth:");
        Serial.println(cfg);
      }
      cfg = "";
      if (ml307SendAT(String("AT+MSSLCFG=\"cert\",") + String(sslId), "OK", nullptr, 1500, &cfg)) {
        Serial.println("[ML307][SSL] MSSLCFG cert:");
        Serial.println(cfg);
      }
      cfg = "";
      if (ml307SendAT(String("AT+MSSLCFG=\"version\",") + String(sslId), "OK", nullptr, 1500, &cfg)) {
        Serial.println("[ML307][SSL] MSSLCFG version:");
        Serial.println(cfg);
      }
      cfg = "";
      if (ml307SendAT(String("AT+MSSLCFG=\"ignoreverify\",") + String(sslId), "OK", nullptr, 1500, &cfg)) {
        Serial.println("[ML307][SSL] MSSLCFG ignoreverify:");
        Serial.println(cfg);
      }
      cfg = "";
      if (ml307SendAT(String("AT+MSSLCFG=\"ignorestamp\",") + String(sslId), "OK", nullptr, 1500, &cfg)) {
        Serial.println("[ML307][SSL] MSSLCFG ignorestamp:");
        Serial.println(cfg);
      }
      // 降级为“无身份认证”SSL，排除 CA/链问题；若仍失败，则更可能是 SNI/套件/服务器策略不兼容。
      ml307SendAT(String("AT+MSSLCFG=\"auth\",") + String(sslId) + ",0", "OK", nullptr, 1000, &r);
      ml307SendAT(String("AT+MSSLCFG=\"cert\",") + String(sslId) + ",\"\",\"\",\"\"", "OK", nullptr, 1000, &r);
      ml307SendAT(String("AT+MSSLCFG=\"ignoreverify\",") + String(sslId) + ",1", "OK", nullptr, 1000, &r);
      ml307SendAT(String("AT+MIPCFG=\"ssl\",") + String(_id) + ",1," + String(sslId), "OK", nullptr, 1000, &r);
      if (!tryOpenWithProto(String(host), port)) {
        _connected = false;
        return 0;
      }
      Serial.println("[ML307][SSL] connected with ignoreverify=1. Your MQTT server certificate chain may not match mqtt_ca.cer.");
    }
    _connected = true;
    _rx.clear();
    _rxPos = 0;
    _unreadLeft = 0;
    _lastPoll = 0;
    _waitingForData = false;  // 连接后不立即等待，等发送数据后再等
    return 1;
  }

  size_t write(uint8_t b) override {
    return write(&b, 1);
  }

  size_t write(const uint8_t* buf, size_t size) override {
    if (!_connected) return 0;
    if (buf == nullptr || size == 0) return 0;
    if (!ml307MIPSendRaw(_id, buf, size)) {
      _connected = false;
      return 0;
    }
    _waitingForData = true;  // 发送后期待响应
    Serial.printf("[ML307MQTT] 已发送 %d 字节，等待响应\n", (int)size);
    return size;
  }

  int available() override {
    if (!_connected) return 0;
    fillRx();
    if (_rxPos >= _rx.size()) return 0;
    return (int)(_rx.size() - _rxPos);
  }

  int read() override {
    uint8_t b = 0;
    int n = read(&b, 1);
    if (n <= 0) return -1;
    return (int)b;
  }

  int read(uint8_t* buf, size_t size) override {
    if (!_connected) return -1;
    if (buf == nullptr || size == 0) return 0;
    fillRx();
    size_t avail = (_rxPos < _rx.size()) ? (_rx.size() - _rxPos) : 0;
    if (avail == 0) return 0;
    size_t n = (size < avail) ? size : avail;
    memcpy(buf, _rx.data() + _rxPos, n);
    _rxPos += n;
    if (_rxPos >= _rx.size()) {
      _rx.clear();
      _rxPos = 0;
    }
    return (int)n;
  }

  int peek() override {
    if (!_connected) return -1;
    fillRx();
    if (_rxPos >= _rx.size()) return -1;
    return (int)_rx[_rxPos];
  }

  void flush() override {
  }

  void stop() override {
    if (_connected) {
      ml307MIPClose(_id);
    }
    _connected = false;
    _rx.clear();
    _rxPos = 0;
    _unreadLeft = 0;
  }

  uint8_t connected() override {
    return _connected ? 1 : 0;
  }

  operator bool() override {
    return connected();
  }

private:
  int _id;
  bool _connected = false;
  bool _waitingForData = false;
  std::vector<uint8_t> _rx;
  size_t _rxPos = 0;
  int _unreadLeft = 0;
  unsigned long _lastPoll = 0;

  void fillRx() {
    if (_rxPos < _rx.size()) return;
    unsigned long now = millis();
    if (_lastPoll > 0 && (now - _lastPoll) < 20) return;
    _lastPoll = now;
    
    int unread = 0;
    uint8_t tmp[512];
    int got = 0;
    
    // 发送数据后等待响应：轮询方式
    if (_waitingForData) {
      Serial.println("[ML307MQTT][fillRx] 等待服务器响应...");
      unsigned long startWait = millis();
      while (millis() - startWait < 20000) {
        delay(500);
        got = ml307MIPRDReadRaw(_id, (int)sizeof(tmp), tmp, (int)sizeof(tmp), unread, 1000);
        if (got > 0) {
          Serial.printf("[ML307MQTT][fillRx] 收到响应 %d 字节 (等待 %lu ms)\n", got, millis() - startWait);
          break;
        }
        if (got < 0) {
          Serial.println("[ML307MQTT][fillRx] 连接断开");
          _connected = false;
          _rx.clear();
          _rxPos = 0;
          _waitingForData = false;
          return;
        }
        Serial.printf("[ML307MQTT][fillRx] 轮询中... %lu ms\n", millis() - startWait);
      }
      _waitingForData = false;
      if (got == 0) {
        Serial.println("[ML307MQTT][fillRx] 等待超时，未收到响应");
      }
    } else {
      got = ml307MIPRDReadRaw(_id, (int)sizeof(tmp), tmp, (int)sizeof(tmp), unread, 300);
    }
    
    if (got > 0) {
      _rx.assign(tmp, tmp + got);
      _rxPos = 0;
      _unreadLeft = unread;
    } else if (got < 0) {
      _connected = false;
      _rx.clear();
      _rxPos = 0;
      _unreadLeft = 0;
    } else {
      _unreadLeft = unread;
    }
  }
};

// 等待串口中出现指定URC通知（保留但不再使用）
static int ml307WaitForRecvUrc(int connectId, unsigned long timeoutMs) {
  if (modemMutex) {
    xSemaphoreTake(modemMutex, portMAX_DELAY);
  }
  
  unsigned long start = millis();
  String buf;
  buf.reserve(256);
  String target = String("+MIPURC: \"recv\",") + String(connectId) + ",";
  String target2 = String("+MIPURC:\"recv\",") + String(connectId) + ",";
  
  while (millis() - start < timeoutMs) {
    while (ModemSerial.available()) {
      char c = (char)ModemSerial.read();
      buf += c;
      if (buf.length() > 200) {
        buf.remove(0, buf.length() - 200);
      }
      
      // 检查是否收到recv URC
      int pos = buf.indexOf(target);
      if (pos < 0) pos = buf.indexOf(target2);
      if (pos >= 0) {
        // 解析数据长度: +MIPURC: "recv",<id>,<len>
        int commaAfterLen = buf.indexOf('\r', pos);
        if (commaAfterLen < 0) commaAfterLen = buf.indexOf('\n', pos);
        if (commaAfterLen > 0) {
          String line = buf.substring(pos, commaAfterLen);
          int lastComma = line.lastIndexOf(',');
          if (lastComma > 0) {
            String lenStr = line.substring(lastComma + 1);
            lenStr.trim();
            int dataLen = lenStr.toInt();
            Serial.printf("[ML307TCP] 收到URC通知: 有 %d 字节数据\n", dataLen);
            if (modemMutex) xSemaphoreGive(modemMutex);
            return dataLen;
          }
        }
      }
      
      // 检查断开通知
      if (buf.indexOf("+MIPURC: \"disconn\"") >= 0 || buf.indexOf("+MIPURC:\"disconn\"") >= 0) {
        Serial.println("[ML307TCP] 收到断开通知");
        if (modemMutex) xSemaphoreGive(modemMutex);
        return -1;
      }
    }
    delay(10);
  }
  
  if (modemMutex) xSemaphoreGive(modemMutex);
  return 0;  // 超时
}

class ML307TcpClient : public Client {
public:
  explicit ML307TcpClient(int connectId = 3) : _id(connectId) {
    _rx.reserve(1024);
  }

  int connect(IPAddress ip, uint16_t port) override {
    (void)ip;
    (void)port;
    return 0;
  }

  int connect(const char* host, uint16_t port) override {
    if (host == nullptr) return 0;
    if (_connected) return 1;
    if (!ml307EnsureNetwork()) return 0;

    String openHost = String(host);
    String ip;
    if (ml307ResolveHostToIp(openHost, ip)) {
      openHost = ip;
    }

    String r;
    // 配置TCP通道
    ml307SendAT(String("AT+MIPCFG=\"cid\",") + String(_id) + ",1", "OK", nullptr, 1000, &r);
    ml307SendAT(String("AT+MIPCFG=\"encoding\",") + String(_id) + ",0,0", "OK", nullptr, 1000, &r);
    ml307SendAT(String("AT+MIPCFG=\"timeout\",") + String(_id) + ",120", "OK", nullptr, 1000, &r);

    if (!ml307MIPOpen(_id, "TCP", openHost, (int)port, 60, 0, 0, 20000)) {
      _connected = false;
      return 0;
    }
    _connected = true;
    _rx.clear();
    _rxPos = 0;
    _unreadLeft = 0;
    _lastPoll = 0;
    _waitingForData = false;  // 连接后不立即等待，等发送数据后再等
    return 1;
  }

  size_t write(uint8_t b) override { return write(&b, 1); }

  size_t write(const uint8_t* buf, size_t size) override {
    if (!_connected) return 0;
    if (buf == nullptr || size == 0) return 0;
    if (!ml307MIPSendRaw(_id, buf, size)) {
      _connected = false;
      return 0;
    }
    _waitingForData = true;  // 发送后期待响应
    Serial.printf("[ML307TCP] 已发送 %d 字节，等待响应\n", (int)size);
    return size;
  }

  int available() override {
    if (!_connected) return 0;
    fillRx();
    if (_rxPos >= _rx.size()) return 0;
    return (int)(_rx.size() - _rxPos);
  }

  int read() override {
    uint8_t b = 0;
    int n = read(&b, 1);
    if (n <= 0) return -1;
    return (int)b;
  }

  int read(uint8_t* buf, size_t size) override {
    if (!_connected) return -1;
    if (buf == nullptr || size == 0) return 0;
    fillRx();
    size_t avail = (_rxPos < _rx.size()) ? (_rx.size() - _rxPos) : 0;
    if (avail == 0) return 0;
    size_t n = (size < avail) ? size : avail;
    memcpy(buf, _rx.data() + _rxPos, n);
    _rxPos += n;
    if (_rxPos >= _rx.size()) {
      _rx.clear();
      _rxPos = 0;
    }
    return (int)n;
  }

  int peek() override {
    if (!_connected) return -1;
    fillRx();
    if (_rxPos >= _rx.size()) return -1;
    return (int)_rx[_rxPos];
  }

  void flush() override {
  }

  void stop() override {
    if (_connected) {
      ml307MIPClose(_id);
    }
    _connected = false;
    _rx.clear();
    _rxPos = 0;
    _unreadLeft = 0;
  }

  uint8_t connected() override { return _connected ? 1 : 0; }

  operator bool() override { return connected(); }

private:
  int _id;
  bool _connected = false;
  bool _waitingForData = false;
  std::vector<uint8_t> _rx;
  size_t _rxPos = 0;
  int _unreadLeft = 0;
  unsigned long _lastPoll = 0;

  void fillRx() {
    if (_rxPos < _rx.size()) return;
    unsigned long now = millis();
    if (_lastPoll > 0 && (now - _lastPoll) < 20) return;
    _lastPoll = now;
    
    int unread = 0;
    uint8_t tmp[512];
    int got = 0;
    
    // 发送数据后等待响应：轮询方式
    if (_waitingForData) {
      Serial.println("[ML307TCP][fillRx] 等待服务器响应...");
      unsigned long startWait = millis();
      // 最多等待20秒，每500ms轮询一次
      while (millis() - startWait < 20000) {
        // 先等待一小段时间让数据到达
        delay(500);
        got = ml307MIPRDReadRaw(_id, (int)sizeof(tmp), tmp, (int)sizeof(tmp), unread, 1000);
        if (got > 0) {
          Serial.printf("[ML307TCP][fillRx] 收到响应 %d 字节 (等待 %lu ms)\n", got, millis() - startWait);
          break;
        }
        if (got < 0) {
          Serial.println("[ML307TCP][fillRx] 连接断开");
          _connected = false;
          _rx.clear();
          _rxPos = 0;
          _waitingForData = false;
          return;
        }
        Serial.printf("[ML307TCP][fillRx] 轮询中... %lu ms\n", millis() - startWait);
      }
      _waitingForData = false;
      if (got == 0) {
        Serial.println("[ML307TCP][fillRx] 等待超时，未收到响应");
      }
    } else {
      // 后续读取直接轮询
      got = ml307MIPRDReadRaw(_id, (int)sizeof(tmp), tmp, (int)sizeof(tmp), unread, 300);
    }
    
    if (got > 0) {
      _rx.assign(tmp, tmp + got);
      _rxPos = 0;
      _unreadLeft = unread;
    } else if (got < 0) {
      _connected = false;
      _rx.clear();
      _rxPos = 0;
      _unreadLeft = 0;
    } else {
      _unreadLeft = unread;
    }
  }
};

class ML307TlsClient : public Client {
public:
  explicit ML307TlsClient(int connectId = 2) : _id(connectId) {
    mbedtls_ssl_init(&_ssl);
    mbedtls_ssl_config_init(&_conf);
    mbedtls_x509_crt_init(&_cacert);
    mbedtls_ctr_drbg_init(&_ctrDrbg);
    mbedtls_entropy_init(&_entropy);
  }

  ~ML307TlsClient() override {
    stop();
    mbedtls_ssl_free(&_ssl);
    mbedtls_ssl_config_free(&_conf);
    mbedtls_x509_crt_free(&_cacert);
    mbedtls_ctr_drbg_free(&_ctrDrbg);
    mbedtls_entropy_free(&_entropy);
  }

  int connect(IPAddress ip, uint16_t port) override {
    (void)ip;
    (void)port;
    return 0;
  }

  int connect(const char* host, uint16_t port) override {
    if (host == nullptr) return 0;
    if (_connected) return 1;
    if (!ml307EnsureNetwork()) return 0;

    auto logErr = [&](const char* where, int rc) {
      char msg[160];
      msg[0] = '\0';
      mbedtls_strerror(rc, msg, sizeof(msg));
      int e = (rc < 0) ? -rc : rc;
      Serial.printf("[ML307][TLS] %s rc=-0x%04X %s\n", where, e, msg);
    };

    stop();
    _host = String(host);

    String openHost = _host;
    String ip;
    if (ml307ResolveHostToIp(openHost, ip)) {
      openHost = ip;
    }

    String r;
    ml307SendAT(String("AT+MIPCFG=\"cid\",") + String(_id) + ",1", "OK", nullptr, 1000, &r);
    ml307SendAT(String("AT+MIPCFG=\"encoding\",") + String(_id) + ",0,0", "OK", nullptr, 1000, &r);
    ml307SendAT(String("AT+MIPCFG=\"timeout\",") + String(_id) + ",120", "OK", nullptr, 1000, &r);

    if (!ml307MIPOpen(_id, "TCP", openHost, (int)port, 60, 0, 0, 20000)) {
      return 0;
    }

    const char* pers = "o5_ml307_tls";
    int rc = mbedtls_ctr_drbg_seed(&_ctrDrbg, mbedtls_entropy_func, &_entropy,
                                  (const unsigned char*)pers, strlen(pers));
    if (rc != 0) {
      logErr("ctr_drbg_seed", rc);
      stop();
      return 0;
    }

    mbedtls_x509_crt_free(&_cacert);
    mbedtls_x509_crt_init(&_cacert);
    rc = mbedtls_x509_crt_parse(&_cacert, (const unsigned char*)mqtt_ca_cert, strlen(mqtt_ca_cert) + 1);
    if (rc < 0) {
      logErr("x509_crt_parse", rc);
      stop();
      return 0;
    }

    mbedtls_ssl_free(&_ssl);
    mbedtls_ssl_config_free(&_conf);
    mbedtls_ssl_init(&_ssl);
    mbedtls_ssl_config_init(&_conf);

    rc = mbedtls_ssl_config_defaults(&_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
      logErr("ssl_config_defaults", rc);
      stop();
      return 0;
    }

    mbedtls_ssl_conf_authmode(&_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&_conf, &_cacert, nullptr);
    mbedtls_ssl_conf_rng(&_conf, mbedtls_ctr_drbg_random, &_ctrDrbg);
    mbedtls_ssl_conf_read_timeout(&_conf, 3000);
    (void)mbedtls_ssl_conf_max_frag_len(&_conf, MBEDTLS_SSL_MAX_FRAG_LEN_1024);

    rc = mbedtls_ssl_setup(&_ssl, &_conf);
    if (rc != 0) {
      logErr("ssl_setup", rc);
      stop();
      return 0;
    }

    rc = mbedtls_ssl_set_hostname(&_ssl, host);
    if (rc != 0) {
      logErr("set_hostname", rc);
      stop();
      return 0;
    }

    mbedtls_ssl_set_bio(&_ssl, this, &ML307TlsClient::bioSend, &ML307TlsClient::bioRecv, nullptr);

    unsigned long start = millis();
    while (millis() - start < 30000) {
      rc = mbedtls_ssl_handshake(&_ssl);
      if (rc == 0) break;
      if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        delay(2);
        continue;
      }
      logErr("handshake", rc);
      stop();
      return 0;
    }
    if (rc != 0) {
      logErr("handshake_timeout", rc);
      stop();
      return 0;
    }

    uint32_t vr = mbedtls_ssl_get_verify_result(&_ssl);
    if (vr != 0) {
      Serial.printf("[ML307][TLS] verify failed flags=0x%08lX\n", (unsigned long)vr);
      stop();
      return 0;
    }

    _connected = true;
    _peeked = false;
    return 1;
  }

  size_t write(uint8_t b) override { return write(&b, 1); }

  size_t write(const uint8_t* buf, size_t size) override {
    if (!_connected) return 0;
    if (buf == nullptr || size == 0) return 0;
    size_t sent = 0;
    while (sent < size) {
      int rc = mbedtls_ssl_write(&_ssl, buf + sent, size - sent);
      if (rc > 0) {
        sent += (size_t)rc;
        continue;
      }
      if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        delay(1);
        continue;
      }
      _connected = false;
      return sent;
    }
    return sent;
  }

  int available() override {
    if (!_connected) return 0;
    if (_peeked) return 1;
    size_t n = mbedtls_ssl_get_bytes_avail(&_ssl);
    if (n > 0) return (int)n;
    uint8_t b;
    int rc = mbedtls_ssl_read(&_ssl, &b, 1);
    if (rc == 1) {
      _peeked = true;
      _peekByte = b;
      return 1;
    }
    if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
      _connected = false;
      return 0;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE || rc == MBEDTLS_ERR_SSL_TIMEOUT) {
      return 0;
    }
    _connected = false;
    return 0;
  }

  int read() override {
    uint8_t b = 0;
    int n = read(&b, 1);
    if (n <= 0) return -1;
    return (int)b;
  }

  int read(uint8_t* buf, size_t size) override {
    if (!_connected) return -1;
    if (buf == nullptr || size == 0) return 0;
    size_t off = 0;
    if (_peeked) {
      buf[0] = _peekByte;
      _peeked = false;
      off = 1;
      if (size == 1) return 1;
    }
    int rc = mbedtls_ssl_read(&_ssl, buf + off, size - off);
    if (rc > 0) return (int)(off + (size_t)rc);
    if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
      _connected = false;
      return (int)off;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE || rc == MBEDTLS_ERR_SSL_TIMEOUT) {
      return (int)off;
    }
    _connected = false;
    return (int)off;
  }

  int peek() override {
    if (!_connected) return -1;
    if (!_peeked) {
      (void)available();
    }
    if (!_peeked) return -1;
    return (int)_peekByte;
  }

  void flush() override {
  }

  void stop() override {
    if (_connected) {
      (void)mbedtls_ssl_close_notify(&_ssl);
    }
    _connected = false;
    _peeked = false;
    if (_id >= 0) {
      ml307MIPClose(_id);
    }
  }

  uint8_t connected() override {
    return _connected ? 1 : 0;
  }

  operator bool() override { return connected(); }

private:
  int _id;
  bool _connected = false;
  bool _peeked = false;
  uint8_t _peekByte = 0;
  String _host;

  mbedtls_ssl_context _ssl;
  mbedtls_ssl_config _conf;
  mbedtls_x509_crt _cacert;
  mbedtls_ctr_drbg_context _ctrDrbg;
  mbedtls_entropy_context _entropy;

  static int bioSend(void* ctx, const unsigned char* buf, size_t len) {
    ML307TlsClient* self = reinterpret_cast<ML307TlsClient*>(ctx);
    if (self == nullptr || buf == nullptr || len == 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    size_t n = len;
    if (n > 512) n = 512;
    if (!ml307MIPSendRaw(self->_id, (const uint8_t*)buf, n)) {
      return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return (int)n;
  }

  static int bioRecv(void* ctx, unsigned char* buf, size_t len) {
    ML307TlsClient* self = reinterpret_cast<ML307TlsClient*>(ctx);
    if (self == nullptr || buf == nullptr || len == 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    int unread = 0;
    int want = (int)len;
    if (want > 512) want = 512;
    int got = ml307MIPRDReadRaw(self->_id, want, (uint8_t*)buf, (int)len, unread, 3000);
    if (got > 0) return got;
    if (got < 0) return MBEDTLS_ERR_NET_CONN_RESET;
    return MBEDTLS_ERR_SSL_WANT_READ;
  }
};

ML307MqttClient ml307MqttTransport(1);

ML307TlsClient ml307TlsTransport(2);

ML307TcpClient ml307TcpTransport(3);

PubSubClient mqttClientWiFi(mqttNetClient);
PubSubClient mqttClientWiFiPlain(mqttNetClientPlain);
PubSubClient mqttClientMl307(ml307TlsTransport);
PubSubClient mqttClientMl307Plain(ml307TcpTransport);

// 扫描目标模式: "mac" 或 "uuid"
String scanTargetMode = "mac";
String scanTargetUuid = "";
std::vector<String> targetUuids;

// 云控：是否已订阅cmd topic
static bool cmdTopicSubscribed = false;
// 云控：是否启用云控功能
static bool cloudControlEnabled = true;

// 云控：ML307 MQTT URC缓冲队列（防止AT命令交互时丢失URC）
static std::vector<String> mqttUrcBuffer;
static bool isMqttMsgUrc(const String& line);
static String ml307ExtractMqttUrc(const String& raw);

// 数据模式: "mqtt" 或 "server"
String dataMode = "mqtt";

// 目标设备列表
struct TargetDevice {
  String mac;
  String name;
  bool enabled;
  String objectId; // 缓存LeanCloud ObjectId
  String lastUploadedData; // 缓存上次上传的数据，用于去重
};

// 全局变量
std::vector<TargetDevice> targetDevices;
std::vector<ScannedDevice> scannedDevices;
std::vector<ScannedDevice> pendingReports; // 待发送报告队列
SemaphoreHandle_t dataMutex = NULL;        // 数据互斥锁
SemaphoreHandle_t modemMutex = NULL;       // Modem AT 互斥锁
BLEScan* pBLEScan = nullptr;
bool isScanning = false;
unsigned long lastScanTime = 0;
unsigned long scanStartTime = 0; // 扫描开始时间，用于看门狗
unsigned long lastReportTime = 0;
unsigned long lastTargetFoundTime = 0; // 上次发现目标MAC的时间
String lastFoundMac = ""; // 上次发现的目标MAC地址
int serverFailCount = 0;
#define MAX_SERVER_FAILURES 5
#define MAX_PENDING_REPORTS 50 // 最大待发送报告数
#define MAX_SCANNED_DEVICES 100 // 最大扫描设备记录数（防止内存耗尽）
#define PERMANENT_RESTART_MS (30UL * 60 * 1000) // 30分钟重启一次
#define ALWAYS_ONLINE_RESTART_MS (60UL * 60 * 1000) // 1小时重启一次 (一直在线模式)

// 功率管理变量
enum PowerMode {
  POWER_ACTIVE,  // 活跃模式 - 正常扫描
  POWER_SLEEP    // 睡眠模式 - 深度睡眠
};

PowerMode currentPowerMode = POWER_ACTIVE;
bool isTimeToSleep = false;
bool isTimeToWake = false;
unsigned long lastPowerCheck = 0;
unsigned long lastHeartbeat = 0;   // 上次心跳报告时间
uint64_t sleepStartTime = 0;       // 开始休眠时间 (改为64位避免溢出)
uint64_t wakeStartTime = 0;        // 开始苏醒时间 (改为64位避免溢出)
unsigned long wifiDisconnectedSince = 0; // WiFi断连开始时间（ms），用于防止永久离线

static int getWiFiRestartCount() {
  Preferences p;
  p.begin("config", false);
  int c = p.getInt("wifi_rst", 0);
  p.end();
  return c;
}

static void setWiFiRestartCount(int c) {
  Preferences p;
  p.begin("config", false);
  p.putInt("wifi_rst", c);
  p.end();
}

static void resetWiFiRestartCount() {
  setWiFiRestartCount(0);
}

// 函数声明
void setupWiFi();
void setupBLE();
void startScan();
bool ensureWiFiConnection();
bool ensureNetworkConnection();
void reportToServer(const ScannedDevice& device);
String formatMacAddress(const uint8_t* mac);
void addTargetDevice(String mac, String name);
void setStatusLed(bool state);
static String normalizeUuidToken(const String& s);
static void parseServiceUuidsFromAdvPayload(const uint8_t* payload, size_t len, std::vector<String>& outUuids);
void checkTimeSchedule();
void enterSleepMode();
void enterActiveMode();
void setupRTC();
void updatePowerMode();
String getPowerModeString(PowerMode mode);
void sendHeartbeat();
void publishMqttHeartbeat();
void publishMqttConfigStatus();
void loadConfiguration();
bool saveConfiguration(const String& ssid, const String& pass, const String& serverBase, const String& timeSlotsJson = "",
                       const String& mode = "mqtt", const String& targetMac = "", int duration = 3, int interval = 5,
                       const String& net_mode = "wifi", const String& apn = "cmnet", int baud = 115200,
                       const String& scan_mode = "mac", const String& target_uuid = "",
                       const String& mHost = "", int mPort = 1883, const String& mUser = "", const String& mPass = "", const String& mTopic = "");
void startConfigAP();
void stopConfigAP();
void handleRoot();
void handleSave();
void handleNotFound();
void beginStationWithCurrentConfig();
void checkFactoryResetButton();
void factoryReset();

String getDeviceIdString();
bool parseUrl(const String& url, bool& isHttps, String& host, int& port, String& path);

bool httpRequest(const String& method, const String& url, const String& contentType, const String& payload, int& outCode, String& outBody, const String& extraHeaders = "");
bool httpRequestWiFi(const String& method, const String& url, const String& contentType, const String& payload, int& outCode, String& outBody, const String& extraHeaders);
bool httpRequestMl307(const String& method, const String& url, const String& contentType, const String& payload, int& outCode, String& outBody, const String& extraHeaders);
bool ml307Init();
void ml307PowerOff();  // 关闭ML307模块以省电
void ml307PowerOn();   // 重新开启ML307模块
bool ml307EnsureNetwork();
bool ml307SendAT(const String& cmd, const char* expect1, const char* expect2, unsigned long timeoutMs, String* outResp);
String ml307ReadUntil(unsigned long timeoutMs);
String ml307ReadRaw(unsigned long timeoutMs);

void mqttCallback(char* topic, byte* payload, unsigned int length);
bool ensureMqttConnection();
void mqttLoop();
void publishMqttReport(const ScannedDevice& device);
String resolveMqttTopicForMac(const String& mac);

void setupTimeZone();
bool ml307SyncTime();
bool ml307SyncTimeFromCCLK();
bool syncTimeFromHttpDate(const String& fullHttpResponse);

// WiFi设置
void setupWiFi() {
  Serial.println("=== 连接WiFi ===");
  Serial.println("正在尝试连接WiFi，请稍候...");

  if (wifiSsid.length() == 0) {
    wifiSsid = DEFAULT_STA_SSID;
    wifiPass = DEFAULT_STA_PASSWORD;
  }

  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    esp_task_wdt_reset();  // 喂狗，防止连接期间触发看门狗
    // 连接期间仍允许访问状态AP，用户可随时手动初始化
    if (statusApActive) {
      configServer.handleClient();
    }
    checkFactoryResetButton();

    delay(1000);
    Serial.print(".");
    attempts++;

    // 每10次尝试显示一次进度
    if (attempts % 10 == 0) {
      Serial.printf("\n[WiFi] 已尝试 %d 次，继续连接中...\n", attempts);
    }
    
    // 如果启动时长期连不上（例如2分钟），也允许系统继续往下走（虽然可能没有时间同步等功能）
    // 或者选择重启。这里选择重启，因为没有WiFi设备基本不可用。
    if (attempts > 120) {
       Serial.println("\n[WiFi] 启动连接超时(120s)，重启设备...");
       delay(500);
       esp_restart();
    }
  }

  Serial.println("\nWiFi连接成功！");
  addDebugLog("[WiFi] 连接成功 IP=" + WiFi.localIP().toString());
  resetWiFiRestartCount();
  Serial.printf("IP地址: %s\n", WiFi.localIP().toString().c_str());
  deviceId = getDeviceIdString();
  Serial.printf("设备ID: %s\n", deviceId.c_str());
  Serial.printf("连接耗时: %d 秒\n", attempts);
}

bool ensureNetworkConnection() {
  if (netMode == "ml307" || netMode == "ml307r") {
    return ml307EnsureNetwork();
  }
  return ensureWiFiConnection();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // 云控指令处理：只处理 {设备名}/cmd topic
  String topicStr(topic);
  String prefix = mqttDeviceName.length() > 0 ? mqttDeviceName : "o5";
  String cmdTopic = prefix + "/cmd";
  if (topicStr != cmdTopic) return;

  // 解析JSON
  String msg;
  msg.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  if (!cloudControlEnabled) {
    Serial.println("[CMD] 云控已关闭，忽略指令");
    return;
  }
  Serial.printf("[CMD] 收到云控指令: %s\n", msg.c_str());
  addDebugLog("[CMD] 收到: " + msg);

  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, msg);
  if (err) {
    Serial.printf("[CMD] JSON解析失败: %s\n", err.c_str());
    return;
  }

  // token 校验：匹配 deviceId 或 chipSerialNumber
  String token = doc["token"] | "";
  if (token != deviceId && token != chipSerialNumber) {
    Serial.printf("[CMD] token不匹配 (收到=%s, 期望deviceId=%s或chipSN=%s), 忽略\n",
                  token.c_str(), deviceId.c_str(), chipSerialNumber.c_str());
    return;
  }

  unsigned long ts = doc["ts"] | 0UL;

  String action = doc["action"] | "";
  if (action == "setScanConfig") {
    String newMode = doc["scanMode"] | "";
    String newTargets = doc["targets"] | "";

    if (newMode != "mac" && newMode != "uuid") {
      Serial.println("[CMD] 无效的scanMode, 忽略");
      return;
    }

    // 内容对比：构建当前targets字符串
    String currentTargets;
    if (scanTargetMode == "uuid") {
      currentTargets = scanTargetUuid;
    } else {
      for (int i = 0; i < (int)targetDevices.size(); i++) {
        if (i > 0) currentTargets += ",";
        currentTargets += targetDevices[i].mac;
      }
    }

    // 对比当前配置与期望配置，一致则跳过
    if (newMode == scanTargetMode && newTargets == currentTargets) {
      Serial.printf("[CMD] 配置已一致(mode=%s targets=%s), 无需修改\n", newMode.c_str(), newTargets.c_str());
      // 仍然发送ack告知服务器已同步
      String ackTopic = prefix + "/cmd/ack";
      String ackPayload = String("{") +
        "\"action\":\"setScanConfig\"," +
        "\"status\":\"ok\"," +
        "\"scanMode\":\"" + scanTargetMode + "\"," +
        "\"targets\":\"" + currentTargets + "\"," +
        "\"synced\":true," +
        "\"ts\":" + String(ts) + "}";
      bool useMl307 = (netMode == "ml307r");
      if (useMl307) {
        ml307MqttAtPublishPlain(ackTopic, ackPayload, false);
      } else {
        PubSubClient& c = getActiveMqttClient(false);
        if (c.connected()) c.publish(ackTopic.c_str(), ackPayload.c_str());
      }
      return;
    }

    Serial.printf("[CMD] 配置不一致，执行同步: mode=%s->%s targets=%s->%s\n",
                  scanTargetMode.c_str(), newMode.c_str(), currentTargets.c_str(), newTargets.c_str());
    addDebugLog("[CMD] 同步 mode=" + newMode + " targets=" + newTargets);

    // 切换扫描模式
    scanTargetMode = newMode;
    if (scanTargetMode == "uuid") {
      targetDevices.clear();
      scanTargetUuid = newTargets;
      targetUuids.clear();
      String s = newTargets;
      s.replace(",", " ");
      int start = 0;
      while (start < (int)s.length()) {
        while (start < (int)s.length() && (s[start] == ' ' || s[start] == '\t')) start++;
        if (start >= (int)s.length()) break;
        int end = start;
        while (end < (int)s.length() && s[end] != ' ' && s[end] != '\t') end++;
        String tok = s.substring(start, end);
        tok = normalizeUuidToken(tok);
        if (tok.length() > 0) targetUuids.push_back(tok);
        start = end + 1;
      }
    } else {
      // MAC模式
      targetDevices.clear();
      scanTargetUuid = "";
      targetUuids.clear();
      String s = newTargets;
      s.replace(",", " ");
      int start = 0;
      while (start < (int)s.length()) {
        while (start < (int)s.length() && (s[start] == ' ' || s[start] == '\t')) start++;
        if (start >= (int)s.length()) break;
        int end = start;
        while (end < (int)s.length() && s[end] != ' ' && s[end] != '\t') end++;
        String mac = s.substring(start, end);
        mac.trim();
        if (mac.length() > 0) addTargetDevice(mac, "云控");
        start = end + 1;
      }
    }

    // 持久化到Preferences
    preferences.begin("config", false);
    preferences.putString("scan_mode", scanTargetMode);
    preferences.putString("target_uuid", scanTargetUuid);
    if (scanTargetMode == "mac") {
      String saved = "";
      for (int i = 0; i < (int)targetDevices.size(); i++) {
        if (i > 0) saved += ",";
        saved += targetDevices[i].mac;
      }
      preferences.putString("targets", saved);
      preferences.putString("lc_target_mac", saved);
    }
    preferences.end();
    Serial.printf("[CMD] 配置已持久化: mode=%s\n", scanTargetMode.c_str());

    // 发送 ack 确认
    String ackTopic = prefix + "/cmd/ack";
    String ackPayload = String("{") +
      "\"action\":\"setScanConfig\"," +
      "\"status\":\"ok\"," +
      "\"scanMode\":\"" + scanTargetMode + "\"," +
      "\"targets\":\"" + newTargets + "\"," +
      "\"synced\":false," +
      "\"ts\":" + String(ts) + "}";

    bool useMl307 = (netMode == "ml307r");
    if (useMl307) {
      ml307MqttAtPublishPlain(ackTopic, ackPayload, false);
    } else {
      PubSubClient& c = getActiveMqttClient(false);
      if (c.connected()) {
        c.publish(ackTopic.c_str(), ackPayload.c_str());
      }
    }
    Serial.printf("[CMD] ACK已发送，配置已同步\n");
    addDebugLog("[CMD] 已同步: mode=" + scanTargetMode);

    // 立即上报新的配置状态
    publishMqttConfigStatus();
  }
}

static String ml307QuoteSafe(String s) {
  s.replace("\"", "'");
  return s;
}

static String mqttMakeSafeClientId(String s) {
  s.trim();
  s.replace(" ", "-");
  s.replace(":", "-");
  s.replace("/", "-");
  s.replace("\\", "-");
  s.replace("\r", "");
  s.replace("\n", "");
  if (s.length() > 128) {
    s = s.substring(0, 128);
  }
  return s;
}

static String ml307ToHex(const String& s) {
  static const char* hex = "0123456789ABCDEF";
  String out;
  out.reserve(s.length() * 2);
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t b = (uint8_t)s[i];
    out += hex[(b >> 4) & 0x0F];
    out += hex[b & 0x0F];
  }
  return out;
}

static PubSubClient& getActiveMqttClient(bool useMl307) {
  if (useMl307) {
    return (mqttPort == 8883) ? mqttClientMl307 : mqttClientMl307Plain;
  }
  return (mqttPort == 8883) ? mqttClientWiFi : mqttClientWiFiPlain;
}

static bool ml307ParseMqttState(const String& resp, int* outState) {
  int p = resp.indexOf("+MQTTSTATE:");
  if (p < 0) return false;
  int eol = resp.indexOf('\n', p);
  String line = (eol >= 0) ? resp.substring(p, eol) : resp.substring(p);
  int colon = line.indexOf(':');
  if (colon < 0) return false;
  String v = line.substring(colon + 1);
  v.trim();
  if (outState) *outState = v.toInt();
  return true;
}

static bool ml307ParseMqttConnUrc(const String& resp, int connectId, int* outConnState) {
  int p = resp.indexOf("+MQTTURC: \"conn\"");
  if (p < 0) return false;
  int eol = resp.indexOf('\n', p);
  String line = (eol >= 0) ? resp.substring(p, eol) : resp.substring(p);
  int c1 = line.indexOf(',');
  if (c1 < 0) return false;
  int c2 = line.indexOf(',', c1 + 1);
  if (c2 < 0) return false;
  String idStr = line.substring(c1 + 1, c2);
  String stStr = line.substring(c2 + 1);
  idStr.trim();
  stStr.trim();
  int id = idStr.toInt();
  int st = stStr.toInt();
  if (id != connectId) return false;
  if (outConnState) *outConnState = st;
  return true;
}

static bool ml307ParseLastMqttConnUrc(const String& resp, int connectId, int* outConnState, String* outLine) {
  int lastPos = -1;
  int searchFrom = 0;
  while (true) {
    int p = resp.indexOf("+MQTTURC: \"conn\"", searchFrom);
    if (p < 0) break;
    lastPos = p;
    searchFrom = p + 1;
  }
  if (lastPos < 0) return false;
  int eol = resp.indexOf('\n', lastPos);
  String line = (eol >= 0) ? resp.substring(lastPos, eol) : resp.substring(lastPos);
  int c1 = line.indexOf(',');
  if (c1 < 0) return false;
  int c2 = line.indexOf(',', c1 + 1);
  if (c2 < 0) return false;
  String idStr = line.substring(c1 + 1, c2);
  String stStr = line.substring(c2 + 1);
  idStr.trim();
  stStr.trim();
  int id = idStr.toInt();
  int st = stStr.toInt();
  if (id != connectId) return false;
  if (outConnState) *outConnState = st;
  if (outLine) *outLine = line;
  return true;
}

// ML307R内置MQTT连接（非TLS，端口1883）
static bool ml307MqttAtConnectedPlain = false;

static bool ml307MqttAtEnsureConnectedPlain() {
  static const int connectId = 0;
  static const int cid = 1;

  if (!ml307EnsureNetwork()) return false;
  if (mqttHost.length() == 0) return false;

  // 检查是否已连接
  {
    String st;
    if (ml307SendAT(String("AT+MQTTSTATE=") + String(connectId), "OK", nullptr, 1500, &st)) {
      int v = 0;
      if (ml307ParseMqttState(st, &v) && v == 2) {
        ml307MqttAtConnectedPlain = true;
        return true;
      }
    }
  }

  // 断开旧连接
  {
    String r;
    ml307SendAT(String("AT+MQTTDISC=") + String(connectId), "OK", nullptr, 3000, &r);
  }

  // 配置MQTT参数（非TLS）
  {
    String r;
    ml307SendAT(String("AT+MQTTCFG=\"version\",") + String(connectId) + ",4", "OK", nullptr, 1500, &r);
    ml307SendAT(String("AT+MQTTCFG=\"cid\",") + String(connectId) + "," + String(cid), "OK", nullptr, 1500, &r);
    ml307SendAT(String("AT+MQTTCFG=\"ssl\",") + String(connectId) + ",0", "OK", nullptr, 1500, &r);  // 禁用SSL
    ml307SendAT(String("AT+MQTTCFG=\"keepalive\",") + String(connectId) + ",60", "OK", nullptr, 1500, &r);
    ml307SendAT(String("AT+MQTTCFG=\"clean\",") + String(connectId) + ",1", "OK", nullptr, 1500, &r);
    ml307SendAT(String("AT+MQTTCFG=\"encoding\",") + String(connectId) + ",1,0", "OK", nullptr, 1500, &r);
  }

  String clientId = mqttMakeSafeClientId(String("O5-") + chipSerialNumber);
  clientId = ml307QuoteSafe(clientId);
  String host = ml307QuoteSafe(mqttHost);
  String user = ml307QuoteSafe(mqttUser);
  String pass = ml307QuoteSafe(mqttPass);

  String cmd = String("AT+MQTTCONN=") + String(connectId) + ",\"" + host + "\"," + String(mqttPort) + ",\"" + clientId + "\"";
  if (mqttUser.length() > 0) {
    cmd += String(",\"") + user + "\",\"" + pass + "\"";
  }

  Serial.printf("[ML307][MQTT-Plain] 连接 host=%s port=%d clientId=%s (芯片序列号)\n", mqttHost.c_str(), mqttPort, clientId.c_str());
  Serial.printf("[ML307][MQTT-Plain] AT: %s\n", cmd.c_str());

  if (modemMutex) {
    xSemaphoreTake(modemMutex, portMAX_DELAY);
  }

  // 清缓冲前先提取可能存在的MQTT URC
  {
    String preFlush;
    while (ModemSerial.available()) {
      char c = (char)ModemSerial.read();
      preFlush += c;
    }
    if (preFlush.length() > 0 && isMqttMsgUrc(preFlush)) {
      ml307ExtractMqttUrc(preFlush);
    }
  }

  ModemSerial.print(cmd);
  ModemSerial.print("\r\n");

  // 等待+MQTTURC: "conn"响应
  String resp;
  int connState = -1;
  String connLine;
  unsigned long start = millis();
  while (millis() - start < 30000) {
    String chunk = ml307ReadRaw(800);
    if (chunk.length() > 0) {
      resp += chunk;
      Serial.print(chunk);  // 实时打印响应
      if (ml307ParseLastMqttConnUrc(resp, connectId, &connState, &connLine)) {
        break;
      }
    }
  }

  // 从连接响应中提取混入的MQTT msg URC
  if (isMqttMsgUrc(resp)) {
    ml307ExtractMqttUrc(resp);
  }

  if (modemMutex) {
    xSemaphoreGive(modemMutex);
  }

  Serial.println();
  Serial.printf("[ML307][MQTT-Plain] conn_state=%d\n", connState);

  if (connState == 0) {
    Serial.println("[ML307][MQTT-Plain] 连接成功！");
    ml307MqttAtConnectedPlain = true;
    return true;
  } else {
    Serial.printf("[ML307][MQTT-Plain] 连接失败 state=%d\n", connState);
    ml307MqttAtConnectedPlain = false;
    cmdTopicSubscribed = false;  // 连接断开，需重新订阅
    // 自动模式：切换到下一个broker
    if (mqttAutoMode) {
      int oldIdx = mqttAutoBrokerIdx;
      mqttAutoBrokerIdx = (mqttAutoBrokerIdx + 1) % AUTO_BROKER_COUNT;
      mqttHost = String(AUTO_BROKERS[mqttAutoBrokerIdx]);
      mqttPort = 1883;
      addDebugLog("[MQTT-AUTO][ML307] 切换broker: " + String(AUTO_BROKERS[oldIdx]) + " -> " + mqttHost);
    }
    return false;
  }
}

// ML307R内置MQTT发布（非TLS）
static bool ml307MqttAtPublishPlain(const String& topic, const String& payload, bool retain) {
  static const int connectId = 0;
  if (!ml307MqttAtEnsureConnectedPlain()) return false;
  
  String hex = ml307ToHex(payload);
  int msgLen = (int)payload.length();
  String t = ml307QuoteSafe(topic);
  int qos = 0;
  int r = retain ? 1 : 0;
  int dup = 0;
  String cmd = String("AT+MQTTPUB=") + String(connectId) + ",\"" + t + "\"," + String(qos) + "," + String(r) + "," + String(dup) + "," + String(msgLen) + ",\"" + hex + "\"";
  String resp;
  if (!ml307SendAT(cmd, "OK", nullptr, 8000, &resp)) {
    Serial.println("[ML307][MQTT-Plain] publish failed");
    Serial.println(resp);
    return false;
  }
  Serial.printf("[ML307][MQTT-Plain] publish OK topic=%s len=%d\n", topic.c_str(), msgLen);
  return true;
}

// ML307R内置MQTT订阅（非TLS，用于云控指令接收）
static bool ml307MqttAtSubscribePlain(const String& topic, int qos) {
  static const int connectId = 0;
  if (!ml307MqttAtEnsureConnectedPlain()) return false;
  String t = ml307QuoteSafe(topic);
  String cmd = String("AT+MQTTSUB=") + String(connectId) + ",\"" + t + "\"," + String(qos);
  String resp;
  if (!ml307SendAT(cmd, "OK", nullptr, 5000, &resp)) {
    Serial.printf("[ML307][MQTT-Plain] subscribe failed topic=%s\n", topic.c_str());
    Serial.println(resp);
    return false;
  }
  Serial.printf("[ML307][MQTT-Plain] subscribe OK topic=%s qos=%d\n", topic.c_str(), qos);
  return true;
}

// ML307R内置MQTT取消订阅（用于重新订阅触发retained消息）
static bool ml307MqttAtUnsubscribePlain(const String& topic) {
  static const int connectId = 0;
  if (!ml307MqttAtConnectedPlain) return false;
  String t = ml307QuoteSafe(topic);
  String cmd = String("AT+MQTTUNSUB=") + String(connectId) + ",\"" + t + "\"";
  String resp;
  ml307SendAT(cmd, "OK", nullptr, 5000, &resp);
  Serial.printf("[ML307][MQTT-Plain] unsubscribe topic=%s\n", topic.c_str());
  return true;
}

static bool ml307MqttAtEnsureConnected() {
  static const int connectId = 0;
  static const int cid = 1;
  static const int sslId = 1;
  static const char* caName = "mqtt_ca_20260125.cer";
  static bool caEnsured = false;
  const bool useTls = true;

  if (!ml307EnsureNetwork()) return false;
  if (mqttHost.length() == 0) return false;

  {
    String st;
    if (ml307SendAT(String("AT+MQTTSTATE=") + String(connectId), "OK", nullptr, 1500, &st)) {
      int v = 0;
      if (ml307ParseMqttState(st, &v) && v == 2) {
        return true;
      }
    }
  }

  {
    String r;
    ml307SendAT(String("AT+MQTTDISC=") + String(connectId), "OK", nullptr, 3000, &r);
  }

  if (!caEnsured) {
    String list;
    ml307SendAT("AT+MSSLLIST=1", "OK", nullptr, 3000, &list);
    if (list.indexOf(String("\"") + caName + "\"") < 0) {
      Serial.printf("[ML307][MQTT] writing CA cert: %s\n", caName);
      String cert = String(mqtt_ca_cert);
      cert.replace("\r\n", "\n");
      cert.replace("\r", "\n");
      cert.replace("\n", "\r\n");
      int certLen = cert.length();

      if (modemMutex) {
        xSemaphoreTake(modemMutex, portMAX_DELAY);
      }
      ModemSerial.print(String("AT+MSSLCERTWR=\"") + caName + "\",0," + String(certLen) + "\r\n");

      unsigned long start = millis();
      bool gotPrompt = false;
      while (millis() - start < 8000) {
        while (ModemSerial.available()) {
          int c = ModemSerial.read();
          if (c < 0) break;
          if (c == '>') {
            gotPrompt = true;
            break;
          }
        }
        if (gotPrompt) break;
        delay(1);
      }
      if (!gotPrompt) {
        if (modemMutex) xSemaphoreGive(modemMutex);
        Serial.println("[ML307][MQTT] MSSLCERTWR no prompt");
        return false;
      }

      ModemSerial.print(cert);

      start = millis();
      String resp;
      while (millis() - start < 15000) {
        while (ModemSerial.available()) {
          char ch = (char)ModemSerial.read();
          resp += ch;
          if (resp.indexOf("OK") >= 0) {
            if (modemMutex) xSemaphoreGive(modemMutex);
            break;
          }
          if (resp.indexOf("ERROR") >= 0 || resp.indexOf("+CME ERROR") >= 0) {
            if (modemMutex) xSemaphoreGive(modemMutex);
            Serial.println("[ML307][MQTT] MSSLCERTWR failed");
            Serial.println(resp);
            return false;
          }
        }
        if (resp.indexOf("OK") >= 0) break;
        delay(1);
      }
      if (resp.indexOf("OK") < 0) {
        if (modemMutex) xSemaphoreGive(modemMutex);
        Serial.println("[ML307][MQTT] MSSLCERTWR timeout");
        return false;
      }
      Serial.printf("[ML307][MQTT] CA cert written: %s\n", caName);
    } else {
      Serial.printf("[ML307][MQTT] CA cert already present: %s\n", caName);
    }
    caEnsured = true;
  }

  auto configSsl = [&](bool insecure) {
    String r;
    (void)insecure;

    static bool sniProbed = false;
    if (!sniProbed) {
      sniProbed = true;
      String h = ml307QuoteSafe(mqttHost);
      if (h.length() > 0) {
        String sr;
        String cmd1 = String("AT+MSSLCFG=\"sni\",") + String(sslId) + ",\"" + h + "\"";
        (void)ml307SendAT(cmd1, "OK", nullptr, 1500, &sr);
        Serial.printf("[ML307][MQTT] SNI probe: %s\n", cmd1.c_str());
        Serial.println(sr);
        sr = "";
        String cmd2 = String("AT+MSSLCFG=\"sni\",") + String(sslId) + ",1,\"" + h + "\"";
        (void)ml307SendAT(cmd2, "OK", nullptr, 1500, &sr);
        Serial.printf("[ML307][MQTT] SNI probe: %s\n", cmd2.c_str());
        Serial.println(sr);
        sr = "";
        String cmd3 = String("AT+MSSLCFG=\"servername\",") + String(sslId) + ",\"" + h + "\"";
        (void)ml307SendAT(cmd3, "OK", nullptr, 1500, &sr);
        Serial.printf("[ML307][MQTT] SNI probe: %s\n", cmd3.c_str());
        Serial.println(sr);
        sr = "";
        String cmd4 = String("AT+MSSLCFG=\"servername\",") + String(sslId) + ",1,\"" + h + "\"";
        (void)ml307SendAT(cmd4, "OK", nullptr, 1500, &sr);
        Serial.printf("[ML307][MQTT] SNI probe: %s\n", cmd4.c_str());
        Serial.println(sr);
      }
    }

    ml307SendAT(String("AT+MSSLCFG=\"encoding\",") + String(sslId) + ",2", "OK", nullptr, 1500, &r);
    ml307SendAT(String("AT+MSSLCFG=\"auth\",") + String(sslId) + ",1", "OK", nullptr, 1500, &r);
    ml307SendAT(String("AT+MSSLCFG=\"cert\",") + String(sslId) + ",\"" + String(caName) + "\",\"\",\"\"", "OK", nullptr, 1500, &r);
    ml307SendAT(String("AT+MSSLCFG=\"ignoreverify\",") + String(sslId) + ",0", "OK", nullptr, 1500, &r);
    ml307SendAT(String("AT+MSSLCFG=\"ignorestamp\",") + String(sslId) + ",1", "OK", nullptr, 1500, &r);
    ml307SendAT(String("AT+MSSLCFG=\"negotime\",") + String(sslId) + ",300", "OK", nullptr, 1500, &r);
  };

  {
    String r;
    ml307SendAT(String("AT+MQTTCFG=\"version\",") + String(connectId) + ",4", "OK", nullptr, 1500, &r);
    ml307SendAT(String("AT+MQTTCFG=\"cid\",") + String(connectId) + "," + String(cid), "OK", nullptr, 1500, &r);
    ml307SendAT(String("AT+MQTTCFG=\"ssl\",") + String(connectId) + ",1," + String(sslId), "OK", nullptr, 1500, &r);
    ml307SendAT(String("AT+MQTTCFG=\"keepalive\",") + String(connectId) + ",120", "OK", nullptr, 1500, &r);
    ml307SendAT(String("AT+MQTTCFG=\"clean\",") + String(connectId) + ",1", "OK", nullptr, 1500, &r);
    ml307SendAT(String("AT+MQTTCFG=\"encoding\",") + String(connectId) + ",1,0", "OK", nullptr, 1500, &r);
  }

  String clientId = mqttMakeSafeClientId(String("O5-") + chipSerialNumber);
  clientId = ml307QuoteSafe(clientId);
  String host = ml307QuoteSafe(mqttHost);
  String user = ml307QuoteSafe(mqttUser);
  String pass = ml307QuoteSafe(mqttPass);

  String cmd = String("AT+MQTTCONN=") + String(connectId) + ",\"" + host + "\"," + String(mqttPort) + ",\"" + clientId + "\"";
  if (mqttUser.length() > 0) {
    cmd += String(",\"") + user + "\",\"" + pass + "\"";
  }

  Serial.printf("[ML307][MQTT] MQTTCONN host=%s port=%d clientId=%s (芯片序列号) user=%s\n",
                mqttHost.c_str(), mqttPort, clientId.c_str(), (mqttUser.length() > 0 ? "YES" : "NO"));
  Serial.printf("[ML307][MQTT] AT cmd: %s\n", cmd.c_str());

  auto doConnectOnce = [&](bool insecure) -> int {
    configSsl(insecure);
    if (modemMutex) {
      xSemaphoreTake(modemMutex, portMAX_DELAY);
    }

    while (ModemSerial.available()) {
      (void)ModemSerial.read();
    }
    (void)ml307ReadRaw(200);

    ModemSerial.print(String("AT+MQTTDISC=") + String(connectId) + "\r\n");
    (void)ml307ReadUntil(3000);
    (void)ml307ReadRaw(1500);

    while (ModemSerial.available()) {
      (void)ModemSerial.read();
    }
    (void)ml307ReadRaw(200);

    ModemSerial.print(cmd);
    ModemSerial.print("\r\n");

    String resp;
    int connState = -1;
    String connLine;
    unsigned long start = millis();
    while (millis() - start < 35000) {
      String chunk = ml307ReadRaw(1200);
      if (chunk.length() > 0) {
        resp += chunk;
        if (ml307ParseLastMqttConnUrc(resp, connectId, &connState, &connLine)) {
          break;
        }
      }
    }
    String urc = connLine;

    if (modemMutex) {
      xSemaphoreGive(modemMutex);
    }

    Serial.println("[ML307][MQTT] MQTTCONN resp:");
    Serial.println(resp);
    Serial.println("[ML307][MQTT] MQTT conn urc:");
    Serial.println(urc);

    if (connState != 0) {
      Serial.printf("[ML307][MQTT] connect failed conn_state=%d insecure=%d\n", connState, insecure ? 1 : 0);
      String cfg;
      if (ml307SendAT(String("AT+MSSLCFG=\"auth\",") + String(sslId), "OK", nullptr, 1500, &cfg)) {
        Serial.println("[ML307][MQTT] MSSLCFG auth:");
        Serial.println(cfg);
      }
      cfg = "";
      if (ml307SendAT(String("AT+MSSLCFG=\"cert\",") + String(sslId), "OK", nullptr, 1500, &cfg)) {
        Serial.println("[ML307][MQTT] MSSLCFG cert:");
        Serial.println(cfg);
      }
      cfg = "";
      if (ml307SendAT(String("AT+MSSLCFG=\"ignoreverify\",") + String(sslId), "OK", nullptr, 1500, &cfg)) {
        Serial.println("[ML307][MQTT] MSSLCFG ignoreverify:");
        Serial.println(cfg);
      }
      cfg = "";
      if (ml307SendAT(String("AT+MQTTCFG=\"query\",") + String(connectId), "OK", nullptr, 2000, &cfg)) {
        Serial.println("[ML307][MQTT] MQTTCFG query:");
        Serial.println(cfg);
      }
    }
    return connState;
  };

  int connState = doConnectOnce(false);
  if (connState != 0) {
    return false;
  }
  Serial.println("[ML307][MQTT] connected (MQTTS AT)");
  return true;
}

static bool ml307MqttAtPublish(const String& topic, const String& payload, bool retain) {
  static const int connectId = 0;
  if (!ml307MqttAtEnsureConnected()) return false;
  String hex = ml307ToHex(payload);
  int msgLen = (int)payload.length();
  String t = ml307QuoteSafe(topic);
  int qos = 0;
  int r = retain ? 1 : 0;
  int dup = 0;
  String cmd = String("AT+MQTTPUB=") + String(connectId) + ",\"" + t + "\"," + String(qos) + "," + String(r) + "," + String(dup) + "," + String(msgLen) + ",\"" + hex + "\"";
  String resp;
  if (!ml307SendAT(cmd, "OK", nullptr, 8000, &resp)) {
    Serial.println("[ML307][MQTT] publish failed");
    Serial.println(resp);
    return false;
  }
  return true;
}

// MQTT重连退避机制变量
static unsigned long lastMqttConnectAttempt = 0;
static unsigned long mqttReconnectInterval = 5000;  // 初始5秒
static const unsigned long MQTT_RECONNECT_MAX_INTERVAL = 60000;  // 最大60秒
static int mqttConnectFailCount = 0;

bool ensureMqttConnection() {
  if (dataMode != "mqtt") return false;
  bool useMl307 = (netMode == "ml307r");
  if (!useMl307) {
    if (!ensureWiFiConnection()) return false;
  } else {
    if (!ml307EnsureNetwork()) return false;
  }

  // 自动模式：用当前索引的broker作为mqttHost
  String effectiveHost = mqttHost;
  int effectivePort = mqttPort;
  if (mqttAutoMode) {
    effectiveHost = String(AUTO_BROKERS[mqttAutoBrokerIdx]);
    effectivePort = 1883;
  }

  if (effectiveHost.length() == 0) {
    Serial.println("[MQTT] mqttHost 为空，无法连接。请在AP配置页填写 mqtt_host");
    return false;
  }

  PubSubClient& c = getActiveMqttClient(useMl307);

  if (!c.connected()) {
    unsigned long now = millis();
    if (now - lastMqttConnectAttempt < mqttReconnectInterval) {
      return false;
    }
    lastMqttConnectAttempt = now;

    if (!useMl307) {
      if (effectivePort == 8883) {
        mqttNetClient.setCACert(mqtt_ca_cert);
      }
    }
    c.setServer(effectiveHost.c_str(), effectivePort);
    c.setCallback(mqttCallback);

    String clientId = String("O5-") + chipSerialNumber;
    Serial.printf("[MQTT] 客户端ID: %s (芯片序列号)\n", clientId.c_str());

    bool ok;
    if (!mqttAutoMode && mqttUser.length() > 0) {
      ok = c.connect(clientId.c_str(), mqttUser.c_str(), mqttPass.c_str());
    } else {
      ok = c.connect(clientId.c_str());
    }

    if (!ok) {
      mqttConnectFailCount++;
      mqttReconnectInterval = min(mqttReconnectInterval * 2, MQTT_RECONNECT_MAX_INTERVAL);
      Serial.printf("[MQTT] 连接失败 host=%s port=%d state=%d (下次重试间隔: %lums)\n",
                    effectiveHost.c_str(), effectivePort, c.state(), mqttReconnectInterval);
      addDebugLog("[MQTT] 连接失败 " + effectiveHost + " state=" + String(c.state()));

      // 自动模式：连接失败，切换到下一个broker
      if (mqttAutoMode) {
        int oldIdx = mqttAutoBrokerIdx;
        mqttAutoBrokerIdx = (mqttAutoBrokerIdx + 1) % AUTO_BROKER_COUNT;
        mqttReconnectInterval = 5000;  // 切换broker后立即重试，重置退避
        addDebugLog("[MQTT-AUTO] 切换broker: " + String(AUTO_BROKERS[oldIdx]) + " -> " + String(AUTO_BROKERS[mqttAutoBrokerIdx]));
      }

      if (useMl307) {
        Serial.print("[ML307][MIPOPEN][LAST] >>>\n");
        Serial.print(g_ml307LastMIPOPEN);
        Serial.print("\n[ML307][MIPOPEN][LAST] <<<\n");
        Serial.print("[ML307][MIPSEND][LAST] >>>\n");
        Serial.print(g_ml307LastMIPSEND);
        Serial.print("\n[ML307][MIPSEND][LAST] <<<\n");
        Serial.print("[ML307][MIPRD][LAST] >>>\n");
        Serial.print(g_ml307LastMIPRD);
        Serial.print("\n[ML307][MIPRD][LAST] <<<\n");
      }
      return false;
    }

    // 连接成功，重置退避参数
    mqttConnectFailCount = 0;
    mqttReconnectInterval = 5000;
    Serial.printf("[MQTT] 已连接 host=%s port=%d clientId=%s\n",
                  effectiveHost.c_str(), effectivePort, clientId.c_str());
    addDebugLog("[MQTT] 连接成功 host=" + effectiveHost + ":" + String(effectivePort));

    // 订阅云控指令topic
    String prefix = mqttDeviceName.length() > 0 ? mqttDeviceName : "o5";
    String cmdTopic = prefix + "/cmd";
    if (c.subscribe(cmdTopic.c_str(), 1)) {
      Serial.printf("[MQTT] 已订阅云控: %s (QoS1)\n", cmdTopic.c_str());
      addDebugLog("[MQTT] 订阅云控: " + cmdTopic);
      cmdTopicSubscribed = true;
    } else {
      Serial.printf("[MQTT] 订阅云控失败: %s\n", cmdTopic.c_str());
    }
  }
  return true;
}

// 检测行是否包含MQTT消息URC（"msg"或"publish"两种格式）
static bool isMqttMsgUrc(const String& line) {
  return line.indexOf("+MQTTURC: \"msg\"") >= 0 || line.indexOf("+MQTTURC: \"publish\"") >= 0;
}

// 解析ML307 MQTT消息URC，支持两种格式：
// 格式1: +MQTTURC: "msg",0,"TOPIC",LEN,"HEXDATA"
// 格式2: +MQTTURC: "publish",0,msgId,"TOPIC",payloadLen,totalLen,PLAINTEXT
static void ml307ParseMqttMsgUrc(const String& line) {
  String topic;
  String payload;

  int pMsg = line.indexOf("+MQTTURC: \"msg\"");
  int pPub = line.indexOf("+MQTTURC: \"publish\"");

  if (pPub >= 0) {
    // 格式2: +MQTTURC: "publish",0,7,"AAA1/cmd",105,105,{...}
    // 找topic：第一个引号对在 "publish" 之后的第3个逗号后
    int commaCount = 0;
    int idx = pPub + 19; // 跳过 +MQTTURC: "publish"
    while (idx < (int)line.length() && commaCount < 2) {
      if (line[idx] == ',') commaCount++;
      idx++;
    }
    // 现在idx指向msgId后面，找topic引号
    int q1 = line.indexOf('"', idx);
    int q2 = line.indexOf('"', q1 + 1);
    if (q1 < 0 || q2 < 0) return;
    topic = line.substring(q1 + 1, q2);

    // payload在topic引号后跳过两个逗号分隔的数字后的剩余部分
    int afterTopic = q2 + 1;
    commaCount = 0;
    idx = afterTopic;
    while (idx < (int)line.length() && commaCount < 2) {
      if (line[idx] == ',') commaCount++;
      idx++;
    }
    // 跳过第二个逗号后的数字和逗号
    int payloadComma = line.indexOf(',', idx);
    if (payloadComma >= 0) {
      payload = line.substring(payloadComma + 1);
    } else {
      payload = line.substring(idx);
    }
    payload.trim();

  } else if (pMsg >= 0) {
    // 格式1: +MQTTURC: "msg",0,"TOPIC",LEN,"HEXDATA"
    int q1 = line.indexOf('"', line.indexOf(',', pMsg + 15) + 1);
    int q2 = line.indexOf('"', q1 + 1);
    if (q1 < 0 || q2 < 0) return;
    topic = line.substring(q1 + 1, q2);

    int lastQ = line.lastIndexOf('"');
    int prevQ = line.lastIndexOf('"', lastQ - 1);
    if (prevQ < 0 || lastQ <= prevQ) return;
    String hex = line.substring(prevQ + 1, lastQ);

    payload.reserve(hex.length() / 2 + 1);
    for (int i = 0; i + 1 < (int)hex.length(); i += 2) {
      char c = (char)strtol(hex.substring(i, i + 2).c_str(), nullptr, 16);
      payload += c;
    }
  } else {
    return;
  }

  if (topic.length() == 0 || payload.length() == 0) return;

  Serial.printf("[ML307][MQTT-RX] topic=%s payload=%s\n", topic.c_str(), payload.c_str());

  // 调用mqttCallback
  mqttCallback((char*)topic.c_str(), (byte*)payload.c_str(), payload.length());
}

void mqttLoop() {
  if (dataMode != "mqtt") return;

  // 自动模式：同步全局mqttHost/mqttPort为当前选中的broker（让ML307路径也生效）
  if (mqttAutoMode) {
    String autoHost = String(AUTO_BROKERS[mqttAutoBrokerIdx]);
    if (mqttHost != autoHost) {
      mqttHost = autoHost;
      mqttPort = 1883;
    }
  }

  String prefix = mqttDeviceName.length() > 0 ? mqttDeviceName : "o5";
  String cmdTopic = prefix + "/cmd";

  // ML307R模式：订阅云控cmd topic + 轮询URC消息
  if (netMode == "ml307r") {
    // 诊断：每30秒打印一次云控状态
    static unsigned long lastDiagMillis = 0;
    if (millis() - lastDiagMillis >= 30000) {
      Serial.printf("[ML307][DIAG] cmdSubscribed=%d plainConnected=%d urcBuf=%d\n",
                    cmdTopicSubscribed, ml307MqttAtConnectedPlain, (int)mqttUrcBuffer.size());
      lastDiagMillis = millis();
    }

    if (cloudControlEnabled && !cmdTopicSubscribed && ml307MqttAtConnectedPlain) {
      Serial.printf("[ML307] 正在订阅云控: %s\n", cmdTopic.c_str());
      if (ml307MqttAtSubscribePlain(cmdTopic, 1)) {
        cmdTopicSubscribed = true;
        Serial.printf("[ML307] 已订阅云控: %s\n", cmdTopic.c_str());
        // 订阅后等待retained消息到达
        delay(500);
        // 立即检查是否有URC
        if (ModemSerial.available()) {
          String urc = ml307ReadRaw(500);
          Serial.printf("[ML307][SUB-URC] 订阅后收到: %s\n", urc.c_str());
          if (isMqttMsgUrc(urc)) {
            int s = 0;
            while (s < (int)urc.length()) {
              int nl = urc.indexOf('\n', s);
              if (nl < 0) nl = urc.length();
              String line = urc.substring(s, nl);
              if (isMqttMsgUrc(line)) {
                ml307ParseMqttMsgUrc(line);
              }
              s = nl + 1;
            }
          }
        }
      } else {
        Serial.println("[ML307] 订阅云控失败！");
      }
    }

    // 先处理缓冲队列中的URC（被AT命令交互捕获的）
    if (!mqttUrcBuffer.empty()) {
      Serial.printf("[ML307][URC] 处理缓冲URC，共%d条\n", (int)mqttUrcBuffer.size());
      for (const auto& line : mqttUrcBuffer) {
        ml307ParseMqttMsgUrc(line);
      }
      mqttUrcBuffer.clear();
    }

    // 检查串口是否有新URC消息（非阻塞）
    if (cmdTopicSubscribed && ModemSerial.available()) {
      String urc = ml307ReadRaw(200);
      if (urc.length() > 0) {
        Serial.printf("[ML307][URC-RAW] 收到数据(%d字节): %s\n", (int)urc.length(), urc.c_str());
      }
      if (isMqttMsgUrc(urc)) {
        int start = 0;
        while (start < (int)urc.length()) {
          int nl = urc.indexOf('\n', start);
          if (nl < 0) nl = urc.length();
          String line = urc.substring(start, nl);
          if (isMqttMsgUrc(line)) {
            ml307ParseMqttMsgUrc(line);
          }
          start = nl + 1;
        }
      }
    }
    return;
  }

  // WiFi模式
  if (!ensureMqttConnection()) return;
  PubSubClient& c = getActiveMqttClient(false);
  c.loop();

  // WiFi模式：首次订阅cmd topic
  if (cloudControlEnabled && !cmdTopicSubscribed && c.connected()) {
    c.subscribe(cmdTopic.c_str(), 1);
    cmdTopicSubscribed = true;
    Serial.printf("[WiFi] 已订阅云控: %s\n", cmdTopic.c_str());
  }
}

String resolveMqttTopicForMac(const String& mac) {
  // Topic格式: {主机名称}/{mac}
  // 如果主机名称为空，则使用默认格式 o5/{mac}
  String prefix = mqttDeviceName.length() > 0 ? mqttDeviceName : "o5";
  String normalizedMac = mac;
  normalizedMac.replace(":", "");  // 移除冒号
  normalizedMac.toUpperCase();
  return prefix + "/" + normalizedMac;
}

void publishMqttReport(const ScannedDevice& device) {
  bool useMl307 = (netMode == "ml307r");

  DynamicJsonDocument doc(768);
  doc["src"] = "O5";
  doc["deviceId"] = deviceId;
  doc["scanMode"] = scanTargetMode;
  doc["mac"] = device.mac;
  doc["rssi"] = device.rssi;
  doc["advData"] = device.advData;
  doc["ts"] = (uint32_t)device.lastSeen;
  if (device.matchedUuid.length() > 0) {
    doc["uuid"] = device.matchedUuid;
  }

  String payload;
  serializeJson(doc, payload);

  String topic = resolveMqttTopicForMac(device.mac);
  
  // ML307R模式：使用内置MQTT AT命令（绕过TCP接收问题）
  if (useMl307) {
    if (!ml307EnsureNetwork()) {
      Serial.printf("[MQTT] 跳过发布（网络未连接） mac=%s\n", device.mac.c_str());
      return;
    }
    bool ok = ml307MqttAtPublishPlain(topic, payload, true);
    if (ok) {
      Serial.printf("[MQTT] publish 成功 topic=%s len=%d (retain)\n", topic.c_str(), payload.length());
      addDebugLog("[MQTT] 发布成功 " + device.mac);
    } else {
      addDebugLog("[MQTT] 发布失败(ML307) " + device.mac);
    }
    return;
  }

  // WiFi模式：使用PubSubClient
  if (!ensureMqttConnection()) {
    Serial.printf("[MQTT] 跳过发布（未连接） mac=%s\n", device.mac.c_str());
    return;
  }

  PubSubClient& c = getActiveMqttClient(useMl307);
  bool ok = c.publish(topic.c_str(), payload.c_str(), true);
  if (!ok) {
    Serial.printf("[MQTT] publish 失败 topic=%s len=%d state=%d\n", topic.c_str(), payload.length(), c.state());
    addDebugLog("[MQTT] 发布失败 " + device.mac + " state=" + String(c.state()));
  } else {
    Serial.printf("[MQTT] publish 成功 topic=%s len=%d (retain)\n", topic.c_str(), payload.length());
    addDebugLog("[MQTT] 发布成功 " + device.mac);
  }
}

// 确保WiFi连接
bool ensureWiFiConnection() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  // 避免频繁重连——间隔30秒，减少对AP的干扰
  static unsigned long lastConnectAttempt = 0;
  if (millis() - lastConnectAttempt < 30000) {
    return false;
  }
  lastConnectAttempt = millis();

  // ★ 不调用 WiFi.disconnect() 和 WiFi.mode()
  //   这两个调用会重置WiFi栈/触发信道扫描，导致AP短暂下线
  //   直接调用 WiFi.begin() 即可覆盖旧配置并尝试连接
  Serial.println("[WiFi] WiFi未连接，尝试重新连接...");
  addDebugLog("[WiFi] 尝试重连");
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

  // 短暂等待5秒，穿插处理Web请求保持AP可访问
  for (int i = 0; i < 50 && WiFi.status() != WL_CONNECTED; i++) {
    esp_task_wdt_reset();
    delay(100);
    if (statusApActive) configServer.handleClient();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] 重连成功! IP: %s\n", WiFi.localIP().toString().c_str());
    addDebugLog("[WiFi] 重连成功 IP=" + WiFi.localIP().toString());
    return true;
  } else {
    Serial.println("[WiFi] 重连失败，30秒后重试");
    addDebugLog("[WiFi] 重连失败");
    return false;
  }
}

// BLE设置
void setupBLE() {
  Serial.println("=== 初始化BLE ===");
  BLEDevice::init("ESP32-O5-Scanner");
  pBLEScan = BLEDevice::getScan();

  // 设置扫描参数
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);

  Serial.println("BLE初始化完成");
}

// 添加目标设备
void addTargetDevice(String mac, String name) {
  TargetDevice target;
  target.mac = mac;
  target.name = name;
  target.enabled = true;
  targetDevices.push_back(target);
  Serial.printf("添加目标设备: %s (%s)\n", mac.c_str(), name.c_str());
}

static String normalizeUuidToken(const String& s) {
  String t = s;
  t.trim();
  t.replace("-", "");
  t.replace(" ", "");
  t.replace("\t", "");
  if (t.startsWith("0x") || t.startsWith("0X")) {
    t = t.substring(2);
  }
  t.toUpperCase();
  return t;
}

static void parseServiceUuidsFromAdvPayload(const uint8_t* payload, size_t len, std::vector<String>& outUuids) {
  outUuids.clear();
  if (payload == nullptr || len < 2) return;
  const size_t MAX_UUIDS = 20;
  size_t i = 0;
  while (i + 1 < len) {
    uint8_t fieldLen = payload[i];
    if (fieldLen == 0) break;
    size_t fieldEnd = i + 1 + fieldLen;
    if (fieldEnd > len) break;
    uint8_t adType = payload[i + 1];
    const uint8_t* data = payload + i + 2;
    size_t dataLen = fieldLen >= 1 ? (fieldLen - 1) : 0;

    auto pushHex = [&](const uint8_t* d, size_t n, bool reverseAll) {
      if (d == nullptr || n == 0) return;
      if (outUuids.size() >= MAX_UUIDS) return;
      String s;
      s.reserve(n * 2);
      if (reverseAll) {
        for (int k = (int)n - 1; k >= 0; k--) {
          char buf[3];
          sprintf(buf, "%02X", d[k]);
          s += buf;
        }
      } else {
        for (size_t k = 0; k < n; k++) {
          char buf[3];
          sprintf(buf, "%02X", d[k]);
          s += buf;
        }
      }
      for (const auto& e : outUuids) {
        if (e == s) return;
      }
      outUuids.push_back(s);
    };

    if (adType == 0x02 || adType == 0x03) {
      for (size_t off = 0; off + 2 <= dataLen; off += 2) {
        pushHex(data + off, 2, true);
      }
    } else if (adType == 0x14) {
      for (size_t off = 0; off + 2 <= dataLen; off += 2) {
        pushHex(data + off, 2, true);
      }
    } else if (adType == 0x04 || adType == 0x05) {
      for (size_t off = 0; off + 4 <= dataLen; off += 4) {
        pushHex(data + off, 4, true);
      }
    } else if (adType == 0x1F) {
      for (size_t off = 0; off + 4 <= dataLen; off += 4) {
        pushHex(data + off, 4, true);
      }
    } else if (adType == 0x06 || adType == 0x07) {
      for (size_t off = 0; off + 16 <= dataLen; off += 16) {
        pushHex(data + off, 16, true);
      }
    } else if (adType == 0x15) {
      for (size_t off = 0; off + 16 <= dataLen; off += 16) {
        pushHex(data + off, 16, true);
      }
    } else if (adType == 0x16) {
      if (dataLen >= 2) {
        pushHex(data, 2, true);
      }
    } else if (adType == 0x20) {
      if (dataLen >= 4) {
        pushHex(data, 4, true);
      }
    } else if (adType == 0x21) {
      if (dataLen >= 16) {
        pushHex(data, 16, true);
      }
    } else if (adType == 0xFF) {
      // iBeacon: CompanyID(2) + 0x02 0x15 + UUID(16) + Major(2) + Minor(2) + Tx(1)
      if (dataLen >= (2 + 2 + 16) && data[2] == 0x02 && data[3] == 0x15) {
        pushHex(data + 4, 16, false);
      }
    }

    i = fieldEnd;
  }
}

// 格式化MAC地址
String formatMacAddress(const uint8_t* mac) {
  String result;
  for (int i = 0; i < 6; i++) {
    if (mac[i] < 0x10) result += "0";
    result += String(mac[i], HEX);
    if (i < 5) result += ":";
  }
  result.toUpperCase();
  return result;
}

// 辅助函数: 64位整数转字符串
String uint64ToString(uint64_t input) {
  String result = "";
  uint8_t base = 10;

  do {
    char c = input % base;
    input /= base;

    if (c < 10)
      c += '0';
    else
      c += 'A' - 10;
    result = c + result;
  } while (input);
  return result;
}

// 状态LED控制
void setStatusLed(bool state) {
  if (silentMode) { digitalWrite(STATUS_LED, LOW); return; }
  digitalWrite(STATUS_LED, state ? HIGH : LOW);
}

// Web运行日志函数
void addDebugLog(String message) {
  String timestamp = String(millis() / 1000) + "s: ";
  String logEntry = timestamp + message + "\n";
  
  // 检查日志大小，超限时裁剪掉前半段
  if (debugLog.length() + logEntry.length() > MAX_DEBUG_LOG_SIZE) {
    int halfSize = debugLog.length() / 2;
    int newlinePos = debugLog.indexOf('\n', halfSize);
    if (newlinePos > 0) {
      debugLog = debugLog.substring(newlinePos + 1);
    } else {
      debugLog = "";
    }
  }
  
  debugLog += logEntry;
  Serial.print(logEntry);
}

// LeanCloud HTTP 请求封装 - 直连 LeanCloud（使用 HTTP）
bool httpRequestLeanCloud(const String& method, const String& lcPath, const String& payload, int& outCode, String& outBody) {
  // 将 HTTPS 转换为 HTTP（LeanCloud 支持 HTTP）
  String apiUrl = lcApiUrl;
  if (apiUrl.startsWith("https://")) {
    apiUrl = "http://" + apiUrl.substring(8);
  }
  
  String fullUrl = apiUrl + lcPath;
  String headers = String("X-LC-Id: ") + lcAppId + "\r\n" + "X-LC-Key: " + lcAppKey + "\r\n";
  return httpRequest(method, fullUrl, "application/json", payload, outCode, outBody, headers);
}

// 更新LeanCloud数据
void updateOrCreateData(const ScannedDevice& device) {
  if (!ensureNetworkConnection()) {
      Serial.println("[CLOUD] 网络未连接，跳过上报");
      serverFailCount++;
      if (serverFailCount >= MAX_SERVER_FAILURES) {
        Serial.println("[SYSTEM] WiFi连续失败次数过多，重启设备...");
        delay(1000);
        esp_restart();
      }
      return;
  }
  
  if (lcAppId.length() == 0 || lcAppKey.length() == 0 || lcApiUrl.length() == 0) {
      Serial.println("[CLOUD] LeanCloud未配置，跳过上报");
      return;
  }

  // 查找目标设备以获取/更新ObjectId
  TargetDevice* target = nullptr;
  for (auto& t : targetDevices) {
    if (t.mac.equalsIgnoreCase(device.mac)) {
      target = &t;
      break;
    }
  }

  if (!target) {
    TargetDevice t;
    t.mac = device.mac;
    t.name = "动态目标";
    t.enabled = true;
    targetDevices.push_back(t);
    target = &targetDevices.back();
  }

  // 检查数据是否变化（筛选功能）
  if (target->lastUploadedData == device.advData) {
    Serial.printf("[CLOUD] 数据未变化(%s)，跳过更新\n", device.mac.c_str());
    // 数据虽未更新，但通信检查应视为成功（或至少不计入失败），重置失败计数
    serverFailCount = 0; 
    return;
  }

  int httpResponseCode = 0;
  String httpBody;

  // 如果没有ObjectId，先查询
  if (target->objectId.length() == 0) {
      String queryPath = lcClassName + "?where={\"mac\":\"" + device.mac + "\"}";
      httpRequestLeanCloud("GET", queryPath, "", httpResponseCode, httpBody);
      
      if (httpResponseCode > 0) {
          serverFailCount = 0;
          DynamicJsonDocument queryDoc(1024);
          deserializeJson(queryDoc, httpBody);
          if (queryDoc["results"].size() > 0) {
              target->objectId = queryDoc["results"][0]["objectId"].as<String>();
              // 既然是从云端查回来的，假设云端数据可能和本地不同，或者我们不知道云端数据具体是什么，
              // 为保险起见，这里不更新 lastUploadedData，强制走下面的更新流程，
              // 这样会执行一次 PUT，确保云端数据与当前扫描数据一致，并同步 lastUploadedData。
              Serial.println("[CLOUD] 查询到已有记录，ObjectId: " + target->objectId);
          } else {
              // 新增
              
              DynamicJsonDocument createDoc(256);
              createDoc["mac"] = device.mac;
              createDoc["data"] = device.advData;
              createDoc["rssi"] = device.rssi;
              if (device.matchedUuid.length() > 0) {
                createDoc["uuid"] = device.matchedUuid;
              }
              
              JsonObject acl = createDoc.createNestedObject("ACL");
              acl["*"]["read"] = true;
              acl["*"]["write"] = true;
              
              String createStr;
              serializeJson(createDoc, createStr);
              httpBody = "";
              httpRequestLeanCloud("POST", lcClassName, createStr, httpResponseCode, httpBody);
              
              if (httpResponseCode == 201) {
                  serverFailCount = 0;
                  DynamicJsonDocument respDoc(256);
                  deserializeJson(respDoc, httpBody);
                  target->objectId = respDoc["objectId"].as<String>();
                  target->lastUploadedData = device.advData; // 更新本地缓存
                  Serial.println("[CLOUD] 新增记录成功，ObjectId: " + target->objectId);
              } else {
                  Serial.printf("[CLOUD] 新增失败: %d\n", httpResponseCode);
                  serverFailCount++;
              }
              
              // 检查失败次数
              if (serverFailCount >= MAX_SERVER_FAILURES) {
                Serial.println("[SYSTEM] HTTP连续失败次数过多，重启设备...");
                delay(1000);
                esp_restart();
              }
              return; // 新增完成
          }
      } else {
          Serial.printf("[CLOUD] 查询失败: %d\n", httpResponseCode);
          serverFailCount++;
          
          // 检查失败次数
          if (serverFailCount >= MAX_SERVER_FAILURES) {
            Serial.println("[SYSTEM] HTTP连续失败次数过多，重启设备...");
            delay(1000);
            esp_restart();
          }
          return;
      }
  }

  // 执行更新
  if (target->objectId.length() > 0) {
      String updatePath = lcClassName + "/" + target->objectId;
      
      DynamicJsonDocument updateDoc(256);
      updateDoc["data"] = device.advData;
      updateDoc["rssi"] = device.rssi;
      if (device.matchedUuid.length() > 0) {
        updateDoc["uuid"] = device.matchedUuid;
      }
      
      String updateStr;
      serializeJson(updateDoc, updateStr);
      httpBody = "";
      httpRequestLeanCloud("PUT", updatePath, updateStr, httpResponseCode, httpBody);
      
      if (httpResponseCode == 200) {
          serverFailCount = 0;
          target->lastUploadedData = device.advData; // 更新本地缓存
          Serial.printf("[CLOUD] 更新成功: %s\n", device.mac.c_str());
      } else {
          Serial.printf("[CLOUD] 更新失败: %d\n", httpResponseCode);
          serverFailCount++;
          // 如果更新失败（可能是被删除了），清除ObjectId以便下次重查
          if (httpResponseCode == 404) {
              target->objectId = "";
          }
      }
  }
  
  // 检查失败次数
  if (serverFailCount >= MAX_SERVER_FAILURES) {
    Serial.println("[SYSTEM] HTTP连续失败次数过多，重启设备...");
    delay(1000);
    esp_restart();
  }
}

// 上报到服务器
void reportToServer(const ScannedDevice& device) {
  if (dataMode == "mqtt") {
    publishMqttReport(device);
    return;
  }

  if (dataMode == "leancloud") {
    updateOrCreateData(device);
    return;
  }

  // 自定义服务器模式
  if (SERVER_BASE.length() == 0 || SERVER_BASE == "http://example.com:port") {
     return;
  }

  if (!ensureNetworkConnection()) {
    Serial.println("网络未连接，跳过上报");
    return;
  }

  Serial.println("\n=== 开始上报到服务器 ===");
  Serial.printf("设备ID: %s\n", deviceId.c_str());
  Serial.printf("目标MAC: %s\n", device.mac.c_str());
  Serial.printf("RSSI: %d dBm\n", device.rssi);
  Serial.printf("广播数据: %s\n", device.advData.c_str());
  Serial.printf("时间戳: %lu\n", device.lastSeen);

  String url = SERVER_BASE + "/api/o5/report";
  String payload = String("{") +
    "\"deviceId\":\"" + deviceId + "\"," +
    "\"scanMode\":\"" + scanTargetMode + "\"," +
    "\"mac\":\"" + device.mac + "\"," +
    "\"rssi\":" + String(device.rssi) + "," +
    "\"advData\":\"" + device.advData + "\"," +
    "\"ts\":" + String(device.lastSeen);
  if (device.matchedUuid.length() > 0) {
    payload += ",\"uuid\":\"" + device.matchedUuid + "\"";
  }
  payload += "}";

  Serial.printf("上报URL: %s\n", url.c_str());
  Serial.printf("上报数据: %s\n", payload.c_str());

  unsigned long startTime = millis();
  int code = 0;
  String resp;
  httpRequest("POST", url, "application/json", payload, code, resp);
  unsigned long duration = millis() - startTime;

  Serial.printf("HTTP响应码: %d\n", code);
  Serial.printf("请求耗时: %lu ms\n", duration);

  if (code == 200) {
    serverFailCount = 0; // 连接成功，重置计数
    Serial.printf("[SUCCESS] 上报成功: %s\n", device.mac.c_str());
    addDebugLog("[REPORT] 上报成功: " + device.mac);
    Serial.printf("服务器响应: %s\n", resp.c_str());
  } else if (code == -1) {
    Serial.printf("[ERROR] 上报失败: %s, 代码: %d (连接失败)\n", device.mac.c_str(), code);
    Serial.println("可能原因: 服务器不可达、网络问题、DNS解析失败");
  } else if (code == -2) {
    Serial.printf("[ERROR] 上报失败: %s, 代码: %d (发送失败)\n", device.mac.c_str(), code);
    Serial.println("可能原因: 网络连接中断、数据发送失败");
  } else if (code == -3) {
    Serial.printf("[ERROR] 上报失败: %s, 代码: %d (响应头解析失败)\n", device.mac.c_str(), code);
    Serial.println("可能原因: 服务器响应格式错误");
  } else if (code == -4) {
    Serial.printf("[ERROR] 上报失败: %s, 代码: %d (连接超时)\n", device.mac.c_str(), code);
    Serial.println("可能原因: 服务器响应超时");
  } else if (code == -5) {
    Serial.printf("[ERROR] 上报失败: %s, 代码: %d (读取响应体超时)\n", device.mac.c_str(), code);
    Serial.println("可能原因: 服务器响应数据过大或网络慢");
  } else if (code == -6) {
    Serial.printf("[ERROR] 上报失败: %s, 代码: %d (响应体解析失败)\n", device.mac.c_str(), code);
    Serial.println("可能原因: 服务器响应格式错误");
  } else if (code == -7) {
    Serial.printf("[ERROR] 上报失败: %s, 代码: %d (流写入超时)\n", device.mac.c_str(), code);
    Serial.println("可能原因: 网络写入超时");
  } else if (code == -8) {
    Serial.printf("[ERROR] 上报失败: %s, 代码: %d (流读取超时)\n", device.mac.c_str(), code);
    Serial.println("可能原因: 网络读取超时");
  } else if (code == -9) {
    Serial.printf("[ERROR] 上报失败: %s, 代码: %d (流写入失败)\n", device.mac.c_str(), code);
    Serial.println("可能原因: 网络写入失败");
  } else if (code == -10) {
    Serial.printf("[ERROR] 上报失败: %s, 代码: %d (流读取失败)\n", device.mac.c_str(), code);
    Serial.println("可能原因: 网络读取失败");
  } else if (code == -11) {
    Serial.printf("[ERROR] 上报失败: %s, 代码: %d (流写入超时)\n", device.mac.c_str(), code);
    Serial.println("可能原因: 网络写入超时");
  } else if (code == -12) {
    Serial.printf("[ERROR] 上报失败: %s, 代码: %d (流读取超时)\n", device.mac.c_str(), code);
    Serial.println("可能原因: 网络读取超时");
  } else {
    Serial.printf("[ERROR] 上报失败: %s, 代码: %d\n", device.mac.c_str(), code);
    if (resp.length() > 0) {
      Serial.printf("错误响应: %s\n", resp.c_str());
    }
  }
}

// 扫描回调类
class SimpleAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    String macStr = formatMacAddress(*advertisedDevice.getAddress().getNative());

    bool isTarget = false;
    String matchedUuid;

    if (scanTargetMode == "uuid") {
      if (!targetUuids.empty()) {
        uint8_t* payloadData = advertisedDevice.getPayload();
        size_t payloadLength = advertisedDevice.getPayloadLength();
        std::vector<String> uuids;
        parseServiceUuidsFromAdvPayload(payloadData, payloadLength, uuids);
        for (const auto& u : uuids) {
          for (const auto& f : targetUuids) {
            if (f.length() > 0 && u.indexOf(f) >= 0) {
              isTarget = true;
              matchedUuid = u;
              break;
            }
          }
          if (isTarget) break;
        }
      }
      if (!targetUuids.empty() && !isTarget) {
        return;
      }
    } else {
      for (auto& target : targetDevices) {
        if (target.enabled && target.mac.equalsIgnoreCase(macStr)) {
          isTarget = true;
          break;
        }
      }

      if (!targetDevices.empty() && !isTarget) {
        return;
      }
    }

    Serial.printf("[FOUND] 发现设备: %s\n", macStr.c_str());

    // 获取广播数据
    uint8_t* payloadData = advertisedDevice.getPayload();
    size_t payloadLength = advertisedDevice.getPayloadLength();
    
    // 优化字符串构建以减少堆碎片
    String advDataStr;
    advDataStr.reserve(2 + payloadLength * 2);
    advDataStr = "0x";
    
    char buf[3];
    for (size_t i = 0; i < payloadLength; i++) {
      sprintf(buf, "%02X", payloadData[i]);
      advDataStr += buf;
    }

    // 创建设备信息
    ScannedDevice device;
    device.mac = macStr;
    device.name = advertisedDevice.haveName() ? advertisedDevice.getName().c_str() : "未知设备";
    device.rssi = advertisedDevice.getRSSI();
    device.advData = advDataStr;
    device.matchedUuid = matchedUuid;
    device.lastSeen = millis();

    // 更新设备列表 (简单线性搜索)
    bool found = false;
    for (auto& existing : scannedDevices) {
      if (existing.mac.equalsIgnoreCase(device.mac)) {
        existing = device;
        found = true;
        break;
      }
    }

    if (!found) {
      // 限制scannedDevices大小，防止内存耗尽
      if (scannedDevices.size() >= MAX_SCANNED_DEVICES) {
        scannedDevices.erase(scannedDevices.begin());  // 移除最旧的
        Serial.println("[SCAN] 设备列表已满，移除最旧记录");
      }
      scannedDevices.push_back(device);
      Serial.printf("[NEW] 新设备: %s (总数: %d)\n", device.mac.c_str(), (int)scannedDevices.size());
    } else {
      Serial.printf("[UPDATE] 更新设备: %s\n", device.mac.c_str());
    }

    // 如果是目标设备，添加到待发送队列
    // 注意：不要在回调中直接进行网络操作，否则会导致 BT_OSI calloc failed
    if (isTarget) {
      Serial.printf("[TARGET] 目标设备发现: %s (加入上报队列)\n", device.mac.c_str());
      addDebugLog("[SCAN] 发现目标: " + device.mac + " RSSI=" + String(device.rssi));
      lastTargetFoundTime = millis(); // 记录发现目标的时间（原子，32位对齐）
      if (dataMutex != NULL) {
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          lastFoundMac = device.mac; // String赋值在mutex保护下执行
          if (pendingReports.size() < MAX_PENDING_REPORTS) {
            bool merged = false;
            if (scanTargetMode == "uuid" && device.matchedUuid.length() > 0) {
              for (auto& p : pendingReports) {
                if (p.matchedUuid.equalsIgnoreCase(device.matchedUuid)) {
                  if (device.rssi > p.rssi) {
                    p = device;
                  }
                  merged = true;
                  break;
                }
              }
            }
            if (!merged) {
              pendingReports.push_back(device);
            }
          } else {
            Serial.println("[WARN] 上报队列已满，丢弃该设备数据");
          }
          xSemaphoreGive(dataMutex);
        }
      } else {
         // Fallback if mutex not init (should not happen)
         if (pendingReports.size() < MAX_PENDING_REPORTS) {
            pendingReports.push_back(device);
         }
      }
    }
  }
};

// 全局静态回调实例（避免内存泄漏）
static SimpleAdvertisedDeviceCallbacks scanCallbacksInstance;
static bool scanCallbacksSet = false;

// 开始扫描
void startScan() {
  if (isScanning) {
    Serial.println("扫描已在进行中");
    return;
  }

  Serial.println("\n=== 开始扫描 ===");
  addDebugLog("[BLE] 开始扫描 模式=" + scanTargetMode);
  Serial.printf("扫描目标模式: %s\n", scanTargetMode.c_str());
  if (scanTargetMode == "uuid") {
    Serial.printf("目标UUID片段数量: %d\n", (int)targetUuids.size());
    for (auto& u : targetUuids) {
      Serial.printf("  UUID片段: %s\n", u.c_str());
    }
  } else {
    Serial.printf("当前目标设备数量: %d\n", (int)targetDevices.size());
    for (auto& target : targetDevices) {
      Serial.printf("  目标: %s (%s)\n", target.mac.c_str(), target.name.c_str());
    }
  }
  isScanning = true;
  scanStartTime = millis();
  setStatusLed(true);

  // 使用静态回调实例（只设置一次）
  if (!scanCallbacksSet) {
    pBLEScan->setAdvertisedDeviceCallbacks(&scanCallbacksInstance);
    scanCallbacksSet = true;
  }

  // 开始扫描
  // 增加扫描前的状态清理
  pBLEScan->clearResults();
  pBLEScan->start(
    scanDuration, [](BLEScanResults results) {
      Serial.printf("扫描完成，发现 %d 个设备\n", results.getCount());
      isScanning = false;
      setStatusLed(false);
      lastScanTime = millis();
      // 清除结果以释放内存
      pBLEScan->clearResults();
    },
    false);
}

// 轮询服务器指令
void pollServerCommands() {
  if (dataMode != "server") {
    return;
  }

  // 增加间隔检查：避免过于频繁调用（虽然 loop 里有 5000ms 检查，但双重保险更好）
  static unsigned long lastPollTime = 0;
  if (millis() - lastPollTime < 5000) return;
  lastPollTime = millis();

  if (SERVER_BASE.length() == 0 || SERVER_BASE == "http://example.com:port") {
      // Serial.println("[CHECK] 服务器未配置，跳过轮询");
      return;
  }

  if (!ensureNetworkConnection()) {
    Serial.println("网络未连接，跳过轮询");
    serverFailCount++;
    Serial.printf("[CHECK] WiFi未连接计数: %d/%d\n", serverFailCount, MAX_SERVER_FAILURES);
    if (serverFailCount > MAX_SERVER_FAILURES) {
      Serial.println("[CHECK] WiFi长期断开，重启设备以尝试恢复...");
      delay(1000);
      esp_restart();
    }
    return;
  }

  Serial.println("\n=== 轮询服务器指令 ===");
  String url = SERVER_BASE + "/api/o5/poll?deviceId=" + deviceId;
  Serial.printf("轮询URL: %s\n", url.c_str());

  unsigned long startTime = millis();
  int code = 0;
  String body;
  httpRequest("GET", url, "application/json", "", code, body);
  unsigned long duration = millis() - startTime;

  Serial.printf("HTTP响应码: %d\n", code);
  Serial.printf("请求耗时: %lu ms\n", duration);

  if (code == 200) {
    serverFailCount = 0; // 连接成功，重置计数
    Serial.printf("服务器响应: %s\n", body.c_str());

    bool configChanged = false;
    String newScanMode = scanTargetMode;
    String newTargetUuid = scanTargetUuid;
    bool hasNewTargets = false;
    String newTargetsSaved = "";

    // 检查是否有立即扫描指令
    if (body.indexOf("\"scan_now\":true") >= 0) {
      Serial.println("收到立即扫描指令");
      startScan();
    }

    // 扫描模式与UUID目标（可选）
    int smPos = body.indexOf("\"scanMode\":");
    if (smPos >= 0) {
      int q1 = body.indexOf('"', smPos + 10);
      int q2 = (q1 >= 0) ? body.indexOf('"', q1 + 1) : -1;
      if (q1 >= 0 && q2 > q1) {
        String sm = body.substring(q1 + 1, q2);
        if (sm == "uuid") {
          newScanMode = "uuid";
        } else {
          newScanMode = "mac";
        }
      }
    }

    int tuPos = body.indexOf("\"targetUuid\":");
    if (tuPos >= 0) {
      int q1 = body.indexOf('"', tuPos + 12);
      int q2 = (q1 >= 0) ? body.indexOf('"', q1 + 1) : -1;
      if (q1 >= 0 && q2 > q1) {
        newTargetUuid = body.substring(q1 + 1, q2);
      }
    }
    if (newScanMode == "uuid") {
      targetUuids.clear();
      String s = newTargetUuid;
      s.replace(",", " ");
      int start = 0;
      while (start < (int)s.length()) {
        while (start < (int)s.length() && (s[start] == ' ' || s[start] == '\t')) start++;
        if (start >= (int)s.length()) break;
        int end = start;
        while (end < (int)s.length() && s[end] != ' ' && s[end] != '\t') end++;
        String tok = s.substring(start, end);
        tok = normalizeUuidToken(tok);
        if (tok.length() > 0) targetUuids.push_back(tok);
        start = end + 1;
      }
    }

    // 检查是否有目标列表更新（仅MAC模式下生效，UUID模式忽略远程MAC目标）
    if (scanTargetMode == "mac") {
      int targetsPos = body.indexOf("\"targets\":");
      if (targetsPos >= 0) {
        int lb = body.indexOf('[', targetsPos);
        int rb = body.indexOf(']', targetsPos);
        if (lb > 0 && rb > lb) {
          String arr = body.substring(lb + 1, rb);
          Serial.printf("收到目标列表: [%s]\n", arr.c_str());

          // 只有当数组不为空时才清空并更新
          if (arr.length() > 0) {
            targetDevices.clear();

            int start = 0;
            while (true) {
              int q1 = arr.indexOf('"', start);
              if (q1 < 0) break;
              int q2 = arr.indexOf('"', q1 + 1);
              if (q2 < 0) break;
              String mac = arr.substring(q1 + 1, q2);
              addTargetDevice(mac, "远程目标");
              start = q2 + 1;
            }
            Serial.printf("更新目标列表，共 %d 个目标\n", (int)targetDevices.size());

            // 记录为持久化字符串（空格分隔）
            hasNewTargets = true;
            for (size_t i = 0; i < targetDevices.size(); i++) {
              if (i > 0) newTargetsSaved += " ";
              newTargetsSaved += targetDevices[i].mac;
            }
          } else {
            Serial.println("收到空目标列表，保持现有目标不变");
          }
        }
      }
    } else {
      // UUID模式下忽略远程MAC目标列表
      if (body.indexOf("\"targets\":") >= 0) {
        Serial.println("[MQTT] UUID模式，忽略远程MAC目标列表");
      }
    }

    // 互斥规则：MAC 与 UUID 只能二选一
    if (newScanMode != "uuid") newScanMode = "mac";
    if (newScanMode != scanTargetMode) {
      configChanged = true;
      scanTargetMode = newScanMode;
      if (scanTargetMode == "uuid") {
        // 切换到 UUID：清空 MAC targets（以免混乱）
        targetDevices.clear();
        hasNewTargets = true;
        newTargetsSaved = "";
      } else {
        // 切换到 MAC：清空 UUID 目标
        scanTargetUuid = "";
        targetUuids.clear();
      }
    }

    if (scanTargetMode == "uuid") {
      if (scanTargetUuid != newTargetUuid) {
        configChanged = true;
        scanTargetUuid = newTargetUuid;
      }
      // UUID 模式下确保不持久化残留 targets
      if (!hasNewTargets) {
        hasNewTargets = true;
        newTargetsSaved = "";
      }
    } else {
      // MAC 模式下确保不持久化残留 UUID
      if (scanTargetUuid.length() > 0) {
        configChanged = true;
        scanTargetUuid = "";
        targetUuids.clear();
      }
    }

    if (hasNewTargets) {
      configChanged = true;
    }

    // 将服务器下发的配置写入 Preferences（断电重启后仍生效）
    if (configChanged) {
      preferences.begin("config", false);
      preferences.putString("scan_mode", scanTargetMode);
      preferences.putString("target_uuid", scanTargetUuid);
      if (hasNewTargets) {
        preferences.putString("targets", newTargetsSaved);
      }
      preferences.end();
      Serial.printf("[CONFIG] 已应用服务器下发配置: scan_mode=%s, target_uuid='%s', targets='%s'\n",
                    scanTargetMode.c_str(), scanTargetUuid.c_str(), newTargetsSaved.c_str());
    }
  } else {
    serverFailCount++;
    Serial.printf("[CHECK] 轮询请求失败计数: %d/%d\n", serverFailCount, MAX_SERVER_FAILURES);
    if (serverFailCount > MAX_SERVER_FAILURES) {
      Serial.println("[CHECK] 与服务器失去连接超过阈值，重启设备...");
      delay(1000);
      esp_restart();
    }

    if (code == -1) {
      Serial.printf("[ERROR] 轮询失败，代码: %d (连接失败)\n", code);
      Serial.println("可能原因: 服务器不可达、网络问题、DNS解析失败");
    } else {
      Serial.printf("[ERROR] 轮询失败，代码: %d\n", code);
      if (body.length() > 0) {
        Serial.printf("错误响应: %s\n", body.c_str());
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // 配置硬件看门狗超时时间
  esp_task_wdt_init(WDT_TIMEOUT_SEC, true);  // 超时秒数, panic=true
  esp_task_wdt_add(NULL);  // 确保主任务被监控
  Serial.printf("[WDT] 硬件看门狗已启用 (%d秒超时)\n", WDT_TIMEOUT_SEC);

  // 获取/生成持久化唯一设备ID（不依赖MAC，兼容随机MAC设备）
  {
    Preferences _p;
    _p.begin("devid", false);
    chipSerialNumber = _p.getString("sn", "");
    if (chipSerialNumber.length() == 0) {
      uint32_t r1 = esp_random();
      uint32_t r2 = esp_random();
      char buf[13];
      snprintf(buf, sizeof(buf), "%04X%08X", (uint16_t)(r2 & 0xFFFF), r1);
      chipSerialNumber = String(buf);
      _p.putString("sn", chipSerialNumber);
      Serial.printf("[CHIP] 首次启动，生成设备序列号: %s\n", chipSerialNumber.c_str());
    }
    _p.end();
  }
  Serial.printf("[CHIP] 设备序列号: %s\n", chipSerialNumber.c_str());

  // 打印唤醒原因，用于判断是否从深度睡眠恢复
  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  Serial.printf("[BOOT] Wakeup cause: %d\n", (int)wakeCause);
  if (wakeCause == ESP_SLEEP_WAKEUP_TIMER) {
    // 从深度睡眠唤醒，先标记为睡眠态，后续由调度逻辑决定是否进入活跃
    currentPowerMode = POWER_SLEEP;
  }

  // 初始化互斥锁
  dataMutex = xSemaphoreCreateMutex();
  modemMutex = xSemaphoreCreateMutex();

  Serial.println("=== ESP32-C3 O5 扫描器 (低功耗版) ===");

  // 初始化硬件
  pinMode(STATUS_LED, OUTPUT);
  setStatusLed(false);
  pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);

  // 启动早期检测：若GPIO9（初始化键）按住≥5秒则恢复初始化
  // 增加简单的防抖逻辑，避免上电瞬间误触
  delay(100);
  if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
    Serial.println("[RESET] 启动时检测到初始化键按下，长按5秒可恢复初始化...");
    unsigned long t0 = millis();
    while (millis() - t0 < 5000) {
      // 若中途松开则取消
      if (digitalRead(CONFIG_BUTTON_PIN) != LOW) {
        Serial.println("[RESET] 取消恢复（按键已松开）");
        break;
      }
      // 简单指示灯闪烁
      setStatusLed(((millis() / 200) % 2) == 0);
      delay(10);
    }
    setStatusLed(false);
    if (digitalRead(CONFIG_BUTTON_PIN) == LOW && millis() - t0 >= 5000) {
      Serial.println("[RESET] 启动阶段长按5秒确认，执行恢复初始化");
      factoryReset();
      return; // 安全返回（factoryReset中会重启）
    }
  }

  // 初始化NVS
  esp_err_t nvsErr = nvs_flash_init();
  if (nvsErr == ESP_ERR_NVS_NO_FREE_PAGES || nvsErr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  // 设置固定MAC地址
  Serial.println("=== 设置固定MAC地址 ===");
  uint8_t fixedMac[6] = { 0x3E, 0xFF, 0xFE, 0x5B, 0xD2, 0x8B };
  esp_err_t macErr = esp_base_mac_addr_set(fixedMac);
  if (macErr == ESP_OK) {
    Serial.println("固定MAC地址设置成功: 3E:FF:FE:5B:D2:8B");
  } else {
    Serial.printf("固定MAC地址设置失败: %d\n", macErr);
  }

  // 加载配置并决定是否进入配置AP
  loadConfiguration();

  if (!isConfigured) {
    Serial.println("[CONFIG] 首次启动未配置，进入AP配置模式");
    startConfigAP();
    // 阻塞在配置模式直到成功保存并连接
    while (apModeActive) {
      esp_task_wdt_reset();  // 喂狗，防止配置期间触发看门狗
      dnsServer.processNextRequest();  // 处理DNS请求，实现自动跳转
      configServer.handleClient();
      checkFactoryResetButton();
      // 初始化模式：LED高频闪烁（每100ms切换一次）
      setStatusLed((millis() / 100) % 2 == 0);
      delay(10);
    }
    // 配置完成，停止DNS，但保持AP和Web服务器运行（避免断开浏览器连接）
    dnsServer.stop();
    configServer.stop();
    configApTransition = true;  // 标记从配置AP过渡
    Serial.println("[CONFIG] 配置完成，切换到运行模式");
  }

  if (netMode == "wifi") {
    if (isConfigured && !silentMode) {
      startStatusAP();  // 会切到AP+STA模式，注册运行时路由
    } else if (isConfigured && silentMode) {
      Serial.println("[SILENT] 静默模式，跳过配置热点");
      WiFi.mode(WIFI_STA);  // 仅STA模式
    }
    setupWiFi();  // 连接WiFi STA
  } else {
    deviceId = getDeviceIdString();
    Serial.printf("设备ID: %s\n", deviceId.c_str());
    Serial.println("[NET] 使用ML307联网模式");
    ml307Init();
  }

  setupTimeZone();
  if (statusApActive) configServer.handleClient();

  if (netMode == "ml307" || netMode == "ml307r") {
    ml307EnsureNetwork();
    ml307SyncTime();
  }

  if (statusApActive) configServer.handleClient();

  // 连接WiFi后立即获取准确时间
  if (netMode == "wifi" && WiFi.status() == WL_CONNECTED) {
    setupRTC();
  } else {
    if (netMode == "wifi") {
      Serial.println("[WARNING] WiFi未连接，无法同步时间，使用默认时间");
    }
  }

  if (statusApActive) configServer.handleClient();

  // 同步时间后立即检查并应用睡眠调度
  checkTimeSchedule();
  updatePowerMode();

  if (statusApActive) configServer.handleClient();

  // 设置BLE
  setupBLE();

  // 如果在 MQTT 模式下配置了专用目标 MAC，添加到扫描目标
  if (dataMode == "mqtt" && scanTargetMode == "mac" && lcTargetMac.length() > 0) {
    Serial.printf("[CONFIG] MQTT模式，添加目标: %s\n", lcTargetMac.c_str());
    addTargetDevice(lcTargetMac, "MQTT目标");
  }
  
  if (targetDevices.empty()) {
      Serial.println("[INFO] 未配置目标设备，将扫描所有设备");
  } else {
      Serial.println("已添加目标设备");
  }

  if (statusApActive) configServer.handleClient();

  // 启动成功指示
  for (int i = 0; i < 3; i++) {
    setStatusLed(true);
    delay(100); // 缩短闪烁时间，加快启动
    setStatusLed(false);
    delay(100);
  }

  // 记录启动时的苏醒时间
  time_t now;
  time(&now);
  wakeStartTime = (uint64_t)now * 1000ULL;  // 转换为毫秒时间戳

  Serial.printf("[STARTUP] 启动苏醒时间戳: %llu\n", wakeStartTime);
  Serial.printf("[STARTUP] 启动苏醒时间: %s", ctime(&now));

  if (statusApActive) configServer.handleClient();

  // 启动时发送一次心跳报告
  if (ensureNetworkConnection()) {
    // 等待网络栈稳定
    for (int i = 0; i < 20; i++) { delay(100); if (statusApActive) configServer.handleClient(); }
    Serial.println("[STARTUP] 启动时发送心跳报告");
    sendHeartbeat();
  } else {
    Serial.println("[STARTUP] WiFi连接失败，跳过启动心跳报告");
  }

  // 检查睡眠唤醒原因
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  switch (wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0:
      Serial.println("[STARTUP] 唤醒原因: 外部信号 (EXT0)");
      break;
    case ESP_SLEEP_WAKEUP_EXT1:
      Serial.println("[STARTUP] 唤醒原因: 外部信号 (EXT1)");
      break;
    case ESP_SLEEP_WAKEUP_TIMER:
      {
        Serial.println("[STARTUP] 唤醒原因: 定时器 - 从睡眠中唤醒");
        // 检查是否到了设定的唤醒时间
        time_t now;
        time(&now);
        struct tm bj;
        getBeijingTime(now, bj);
        int currentTime = bj.tm_hour * 60 + bj.tm_min;
        int wakeTime = WAKE_HOUR * 60 + WAKE_MINUTE;

        if (currentTime >= wakeTime) {
          Serial.println("[STARTUP] 到达设定唤醒时间，进入活跃模式");
          enterActiveMode();
        } else {
          Serial.println("[STARTUP] 心跳唤醒，发送心跳报告后继续睡眠");
          // 发送心跳报告
          if (ensureWiFiConnection()) {
            sendHeartbeat();
            delay(1000);  // 等待心跳发送完成
          }
          // 继续睡眠
          Serial.println("[STARTUP] 心跳发送完成，继续睡眠");
          enterSleepMode();
        }
        break;
      }
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
      Serial.println("[STARTUP] 唤醒原因: 触摸板");
      break;
    case ESP_SLEEP_WAKEUP_ULP:
      Serial.println("[STARTUP] 唤醒原因: ULP程序");
      break;
    case ESP_SLEEP_WAKEUP_UNDEFINED:
    default:
      Serial.println("[STARTUP] 唤醒原因: 未定义 (首次启动或复位)");
      break;
  }

  // 启动状态AP（运行时热点），静默模式跳过
  if (!silentMode) {
    startStatusAP();
  }

  Serial.println("=== 初始化完成 ===");
  addDebugLog("[SYSTEM] 初始化完成 ID=" + deviceId + " mode=" + dataMode + " net=" + netMode);
  Serial.printf("设备ID: %s\n", deviceId.c_str());
  Serial.printf("目标设备数量: %d\n", (int)targetDevices.size());
  Serial.printf("扫描间隔: %d 秒\n", scanInterval);
  Serial.printf("扫描持续时间: %d 秒\n", scanDuration);
  
  Serial.println("=== 时间段配置 (实际生效) ===");
  if (timeSlotCount == 0) Serial.println("  无配置时间段 (一直在线模式)");
  for(int i=0; i<timeSlotCount; i++) {
      Serial.printf("  时间段 %d: 唤醒 %02d:%02d -> 睡眠 %02d:%02d [%s]\n", 
          i+1, timeSlots[i].wakeHour, timeSlots[i].wakeMinute, 
          timeSlots[i].sleepHour, timeSlots[i].sleepMinute, 
          timeSlots[i].enabled ? "启用" : "禁用");
  }
  
  Serial.printf("当前功率模式: %s\n", getPowerModeString(currentPowerMode).c_str());

  // 验证MAC地址设置
  uint8_t actualMac[6];
  esp_read_mac(actualMac, ESP_MAC_WIFI_STA);
  Serial.printf("实际WiFi MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                actualMac[0], actualMac[1], actualMac[2],
                actualMac[3], actualMac[4], actualMac[5]);
}

void loop() {
  // 喂狗，防止看门狗超时重启
  esp_task_wdt_reset();

  if (apModeActive) {
    dnsServer.processNextRequest();
    configServer.handleClient();
    checkFactoryResetButton();
    setStatusLed((millis() / 100) % 2 == 0);
    delay(10);
    return;
  }

  mqttLoop();

  // 处理状态AP请求
  if (statusApActive) {
    configServer.handleClient();
  }

  // 处理待发送报告队列
  if (!pendingReports.empty()) {
      ScannedDevice reportDevice;
      bool hasData = false;
      
      if (dataMutex != NULL) {
          if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              if (!pendingReports.empty()) {
                  reportDevice = pendingReports.front();
                  pendingReports.erase(pendingReports.begin());
                  hasData = true;
              }
              xSemaphoreGive(dataMutex);
          }
      }
      
      if (hasData) {
          reportToServer(reportDevice);
          // 发送报告后立即处理Web请求，避免AT阻塞导致Web超时
          if (statusApActive) configServer.handleClient();
          // 处理完一个后短暂让出CPU，避免 watchdog 触发
          delay(10); 
      }
  }

  // 扫描看门狗：如果处于扫描状态超过预定时间+10秒，强制重置
  if (isScanning && (millis() - scanStartTime > (scanDuration * 1000 + 10000))) {
    Serial.println("[WATCHDOG] 扫描超时（可能卡死），强制重置状态");
    isScanning = false;
    setStatusLed(false);
    if (pBLEScan) pBLEScan->clearResults();
    lastScanTime = millis();
    // 强制释放一下互斥锁，以防万一
    if (dataMutex) xSemaphoreGive(dataMutex); 
  }

  // 检查恢复出厂长按
  checkFactoryResetButton();

  // WiFi断连保护：温和重连，绝不破坏AP
  if (netMode == "wifi") {
    if (WiFi.status() != WL_CONNECTED) {
      if (wifiDisconnectedSince == 0) wifiDisconnectedSince = millis();
      ensureWiFiConnection();
      // 不再强制重启——定期重启(restartThreshold)已涵盖长时间离线场景
    } else {
      wifiDisconnectedSince = 0;
      resetWiFiRestartCount();
    }
  }

  // 功率管理检查（放到扫描逻辑之前，避免进入POWER_SLEEP后return导致不再执行调度）
  if (millis() - lastPowerCheck > 10000) {  // 每10秒检查一次
    lastPowerCheck = millis();
    checkTimeSchedule();
    updatePowerMode();
  }

  // 根据功率模式决定扫描行为
  if (currentPowerMode == POWER_ACTIVE) {
    // 活跃模式：正常扫描
    if (!isScanning && millis() - lastScanTime > scanInterval * 1000) {
      startScan();
    }
  } else {
    // 睡眠模式：不扫描，但仍处理Web请求
    if (statusApActive) configServer.handleClient();
    delay(50);
    return;
  }

  // 定期轮询服务器指令
  if (millis() - lastReportTime > 5000) {  // 每5秒轮询一次
    lastReportTime = millis();
    pollServerCommands();
  }

  // 定期重启以清除缓存（永久运行模式）
  // 如果删掉了所有时间段(timeSlotCount == 0)，则认为一直在线，缩短重启间隔
  unsigned long restartThreshold = (timeSlotCount == 0) ? ALWAYS_ONLINE_RESTART_MS : PERMANENT_RESTART_MS;
  
  if (millis() > restartThreshold) {
    Serial.printf("\n[SYSTEM] 已达到预定运行时间(%.1f分钟)，正在重启以优化性能...\n", restartThreshold / 60000.0);
    delay(1000);
    esp_restart();
  }

  // 定期显示状态
  static unsigned long lastStatusTime = 0;
  if (millis() - lastStatusTime > 15000) {  // 每15秒显示一次
    lastStatusTime = millis();
    Serial.println("\n=== 设备状态 ===");
    Serial.printf("功率模式: %s\n", getPowerModeString(currentPowerMode).c_str());
    Serial.printf("已扫描设备数: %d\n", (int)scannedDevices.size());
    Serial.printf("目标设备数: %d\n", (int)targetDevices.size());

    // 显示时间信息
    time_t now;
    time(&now);
    struct tm bj;
    getBeijingTime(now, bj);
    Serial.printf("当前时间: %02d:%02d:%02d\n",
                  bj.tm_hour, bj.tm_min, bj.tm_sec);

    for (auto& target : targetDevices) {
      if (target.enabled) {
        bool found = false;
        for (auto& device : scannedDevices) {
          if (device.mac.equalsIgnoreCase(target.mac)) {
            unsigned long ago = (millis() - device.lastSeen) / 1000;
            Serial.printf("✓ %s - %lu秒前发现, RSSI: %d dBm\n",
                          target.mac.c_str(), ago, device.rssi);
            found = true;
            break;
          }
        }
        if (!found) {
          Serial.printf("✗ %s - 未发现\n", target.mac.c_str());
        }
      }
    }
    
    // 内存监控日志
    Serial.printf("空闲堆内存: %u bytes\n", ESP.getFreeHeap());
    Serial.printf("最小空闲堆: %u bytes\n", ESP.getMinFreeHeap());
    Serial.printf("堆碎片率: %.1f%%\n", 100.0 - (100.0 * ESP.getMaxAllocHeap() / ESP.getFreeHeap()));
    
    // 内存警告
    if (ESP.getFreeHeap() < 30000) {
      Serial.println("[WARNING] 空闲内存不足30KB，可能存在内存泄漏！");
    }
  }

  // 心跳报告检查（每2小时，或从未发送成功过则频繁重试）
  unsigned long hbInterval = (lastHeartbeat == 0) ? 30000 : HEARTBEAT_INTERVAL_MS; // 如果从未发送成功，每30秒重试一次
  if (millis() - lastHeartbeat > hbInterval) {
    // 注意：如果 lastHeartbeat 是 0，这里条件始终成立（只要 millis > 30000）
    // 但为了避免 loop 此时被阻塞，sendHeartbeat 内部有网络检查
    // 成功后 lastHeartbeat 会更新为当前 millis
    if (lastHeartbeat == 0) {
       Serial.println("[HEARTBEAT] 尚未发送过心跳，尝试发送...");
       lastHeartbeat = millis(); // 暂时标记，避免下一次 loop 立即再次触发，给 30s 缓冲
       sendHeartbeat();
       // 如果发送失败，serverFailCount 会增加，最终触发重启
    } else {
       lastHeartbeat = millis();
       sendHeartbeat();
    }
  }

  // 主循环让出CPU，穿插处理Web请求
  if (statusApActive) configServer.handleClient();
  delay(50);
}


// 检查时间调度
void checkTimeSchedule() {
  time_t now;
  time(&now);
  if (now < 1700000000) {
    if (isTimeToSleep) {
      Serial.println("[SCHEDULE] 时间未同步，强制取消睡眠标记，避免设备长期离线");
      isTimeToSleep = false;
    } else {
      Serial.println("[SCHEDULE] 时间未同步，跳过时间调度检查");
    }
    return;
  }
  if (netMode == "wifi" && WiFi.status() != WL_CONNECTED) {
    Serial.println("[SCHEDULE] WiFi未连接，但仍继续按本地时间执行调度");
  }
  struct tm bj;
  getBeijingTime(now, bj);

  int currentHour = bj.tm_hour;
  int currentMinute = bj.tm_min;
  int currentTime = currentHour * 60 + currentMinute;

  // 如果没有配置时间段，保持 timeSlotCount = 0，意味着不执行休眠逻辑
  /*
  if (timeSlotCount == 0) {
    timeSlotCount = 1;
    timeSlots[0] = {WAKE_HOUR, WAKE_MINUTE, SLEEP_HOUR, SLEEP_MINUTE, true};
  }
  */

  // 正确逻辑：只要任意一个时间段处于活跃期，设备就不应该睡眠
  // 只有所有时间段都不在活跃期时，才进入睡眠
  bool isActive = false;
  String activeSlot = "";
  int enabledCount = 0;

  for (int i = 0; i < timeSlotCount; i++) {
    if (!timeSlots[i].enabled) continue;
    enabledCount++;

    int wakeTime = timeSlots[i].wakeHour * 60 + timeSlots[i].wakeMinute;
    int sleepTime = timeSlots[i].sleepHour * 60 + timeSlots[i].sleepMinute;

    if (wakeTime < sleepTime) {
      // 同一天内的活跃段（如 6:45 - 7:15）：活跃期 [wakeTime, sleepTime)
      if (currentTime >= wakeTime && currentTime < sleepTime) {
        isActive = true;
        activeSlot = String(i + 1);
        break;
      }
    } else if (wakeTime > sleepTime) {
      // 跨天的活跃段（如 22:00 - 6:00）：活跃期 [wakeTime, 24:00) 或 [0:00, sleepTime)
      if (currentTime >= wakeTime || currentTime < sleepTime) {
        isActive = true;
        activeSlot = String(i + 1);
        break;
      }
    }
    // wakeTime == sleepTime：该时间段无效，跳过
  }

  // 有启用的时间段且不在任何活跃期内 → 应睡眠
  bool shouldSleep = (enabledCount > 0) && !isActive;

  Serial.printf("[SCHEDULE] 当前时间: %02d:%02d, 启用时间段: %d, 活跃: %s, 应睡眠: %s\n",
                currentHour, currentMinute, enabledCount,
                isActive ? (String("是[段") + activeSlot + "]").c_str() : "否",
                shouldSleep ? "是" : "否");

  if (shouldSleep) {
    if (!isTimeToSleep) {
      Serial.printf("[SCHEDULE] 不在任何活跃时间段内 (%02d:%02d)，准备进入睡眠模式\n",
                    currentHour, currentMinute);
      isTimeToSleep = true;
    }
  } else {
    if (isTimeToSleep) {
      Serial.printf("[SCHEDULE] 进入活跃时间段 (%02d:%02d) [时间段 %s]，退出睡眠模式\n",
                    currentHour, currentMinute, activeSlot.c_str());
      isTimeToSleep = false;
    }
  }
}

// 进入睡眠模式
void enterSleepMode() {
  Serial.println("[SLEEP] === 进入深度睡眠模式 ===");
  addDebugLog("[POWER] 进入深度睡眠");

  // 记录睡眠开始时间
  time_t now;
  time(&now);
  sleepStartTime = (uint64_t)now * 1000ULL;  // 转换为毫秒时间戳
  currentPowerMode = POWER_SLEEP;

  Serial.printf("[SLEEP] 睡眠开始时间戳: %llu\n", sleepStartTime);
  Serial.printf("[SLEEP] 睡眠开始时间: %s", ctime(&now));
  Serial.printf("[SLEEP] 时间戳验证: %llu (应该是13位数字)\n", sleepStartTime);

  // 发送睡眠开始的心跳报告
  if (netMode == "ml307r" || netMode == "ml307") {
    // ML307模式：通过4G发送心跳
    if (ml307EnsureNetwork()) {
      Serial.println("[SLEEP] 通过ML307发送睡眠开始心跳报告");
      sendHeartbeat();
      delay(1000);  // 等待心跳发送完成
    } else {
      Serial.println("[SLEEP] ML307网络连接失败，跳过睡眠心跳报告");
    }
    
    // 关闭ML307模块以省电
    ml307PowerOff();
  } else {
    // WiFi模式
    if (ensureWiFiConnection()) {
      Serial.println("[SLEEP] 发送睡眠开始心跳报告");
      sendHeartbeat();
      delay(1000);  // 等待心跳发送完成
    } else {
      Serial.println("[SLEEP] WiFi连接失败，跳过睡眠心跳报告");
    }
  }

  // 关闭WiFi和BLE
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  // esp_wifi_stop(); // Arduino框架会自动处理
  // esp_wifi_deinit(); // 避免手动调用导致状态错误

  BLEDevice::deinit(true);
  esp_bt_controller_disable();
  esp_bt_controller_deinit();

  // 关闭状态LED
  setStatusLed(false);

  // 计算睡眠时间 (到下次唤醒时间)
  struct tm bj;
  getBeijingTime(now, bj);
  int currentTime = bj.tm_hour * 60 + bj.tm_min;

  // 找到下一个启用的唤醒时间
  int nextWakeTime = -1;
  int minSleepMinutes = 24 * 60; // 最大一天

  for (int i = 0; i < timeSlotCount; i++) {
    if (!timeSlots[i].enabled) continue;

    int wakeTime = timeSlots[i].wakeHour * 60 + timeSlots[i].wakeMinute;
    int sleepTime = timeSlots[i].sleepHour * 60 + timeSlots[i].sleepMinute;

    int sleepMinutes;
    if (wakeTime > sleepTime) {
      // 跨天时间段：Wake > Sleep (如 Active 22:00-06:00, Sleep 06:00-22:00)
      // 只要当前时间在唤醒时间之前，下一次唤醒就是今天；否则是明天
      if (currentTime < wakeTime) {
        sleepMinutes = wakeTime - currentTime;
      } else {
        sleepMinutes = (24 * 60) - currentTime + wakeTime;
      }
    } else {
      // 同一天时间段：Wake < Sleep (如 Active 06:00-22:00, Sleep 22:00-06:00)
      // 如果当前时间在睡眠时间段内
      if (currentTime >= sleepTime) {
        sleepMinutes = (24 * 60) - currentTime + wakeTime;
      } else if (currentTime < wakeTime) {
        sleepMinutes = wakeTime - currentTime;
      } else {
        // 当前处于 Active 时间段，如果要强行睡眠，则睡到明天的 WakeTime
        sleepMinutes = (24 * 60) - currentTime + wakeTime;
      }
    }

    if (sleepMinutes < minSleepMinutes) {
      minSleepMinutes = sleepMinutes;
      nextWakeTime = wakeTime;
    }
  }

  // 如果没有找到有效时间段，使用默认值
  if (nextWakeTime < 0) {
    nextWakeTime = WAKE_HOUR * 60 + WAKE_MINUTE;
    if (currentTime < nextWakeTime) {
      minSleepMinutes = nextWakeTime - currentTime;
    } else {
      minSleepMinutes = (24 * 60) - currentTime + nextWakeTime;
    }
  }

  Serial.printf("[SLEEP] 睡眠时间: %d 分钟 (到 %02d:%02d 唤醒)\n", 
                minSleepMinutes, nextWakeTime / 60, nextWakeTime % 60);

  // 计算睡眠时间（微秒）
  uint64_t sleepTimeMicros = (uint64_t)minSleepMinutes * 60 * 1000000ULL;

  // 计算心跳间隔时间（微秒）
  uint64_t heartbeatIntervalMicros = HEARTBEAT_INTERVAL_MS * 1000ULL;

  Serial.printf("[SLEEP] 睡眠时间: %llu 微秒 (%.2f 分钟)\n",
                sleepTimeMicros, (double)sleepTimeMicros / 60000000.0);
  Serial.printf("[SLEEP] 心跳间隔: %llu 微秒 (%.2f 小时)\n",
                heartbeatIntervalMicros, (double)heartbeatIntervalMicros / 3600000000.0);

  // 选择较短的唤醒时间（确保既能按时唤醒，又能定期心跳）
  uint64_t wakeupTime = (sleepTimeMicros < heartbeatIntervalMicros) ? sleepTimeMicros : heartbeatIntervalMicros;

  Serial.printf("[SLEEP] 实际唤醒时间: %llu 微秒 (%.2f 分钟)\n",
                wakeupTime, (double)wakeupTime / 60000000.0);

  // 设置定时器唤醒
  esp_sleep_enable_timer_wakeup(wakeupTime);

  // 配置睡眠选项
#ifdef ESP_PD_DOMAIN_RTC_PERIPH
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
#endif
#ifdef ESP_PD_DOMAIN_RTC_SLOW_MEM
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_ON);
#endif
#ifdef ESP_PD_DOMAIN_RTC_FAST_MEM
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_ON);
#endif
#ifdef ESP_PD_DOMAIN_XTAL
  // 确保RTC时钟运行
  esp_sleep_pd_config(ESP_PD_DOMAIN_XTAL, ESP_PD_OPTION_OFF);
#endif

  Serial.println("[SLEEP] 准备进入深度睡眠...");
  Serial.flush();  // 确保所有输出都发送完成

  // 进入深度睡眠
  esp_deep_sleep_start();
}

// 进入活跃模式
void enterActiveMode() {
  currentPowerMode = POWER_ACTIVE;
  Serial.println("[POWER] === 进入活跃模式 ===");
  addDebugLog("[POWER] 进入活跃模式");

  // 记录苏醒开始时间
  time_t now;
  time(&now);
  wakeStartTime = now * 1000ULL;  // 转换为毫秒时间戳，使用ULL避免溢出

  Serial.printf("[POWER] 苏醒开始时间戳: %llu\n", (unsigned long long)wakeStartTime);
  Serial.printf("[POWER] 苏醒开始时间: %s", ctime(&now));

  // 根据网络模式恢复通信
  if (netMode == "ml307r" || netMode == "ml307") {
    // ML307模式：重新开启模块
    Serial.println("[POWER] 重新开启ML307模块");
    ml307PowerOn();
    
    // 等待网络就绪
    for (int i = 0; i < 10; i++) {
      if (ml307EnsureNetwork()) {
        Serial.println("[POWER] ML307网络已就绪");
        break;
      }
      Serial.printf("[POWER] 等待ML307网络... %d/10\n", i + 1);
      delay(2000);
    }
    
    // 发送苏醒开始的心跳报告
    if (ml307EnsureNetwork()) {
      Serial.println("[POWER] 通过ML307发送苏醒开始心跳报告");
      sendHeartbeat();
    } else {
      Serial.println("[POWER] ML307网络连接失败，跳过苏醒心跳报告");
    }
  } else {
    // WiFi模式
    // 恢复WiFi功率
    WiFi.setTxPower(WIFI_POWER_19_5dBm);  // 最大发射功率

    // 恢复BLE功率
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);

    Serial.println("[POWER] 恢复正常扫描和通信");

    // 发送苏醒开始的心跳报告
    if (ensureWiFiConnection()) {
      Serial.println("[POWER] 发送苏醒开始心跳报告");
      sendHeartbeat();
    } else {
      Serial.println("[POWER] WiFi连接失败，跳过苏醒心跳报告");
    }
  }
}

// 设置RTC
void setupRTC() {
  Serial.println("[RTC] === 设置实时时钟 ===");

  // 设置时区 (中国标准时间)
  setenv("TZ", "CST-8", 1);
  tzset();

  // 配置时间服务器
  configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov", "cn.pool.ntp.org");

  Serial.println("[RTC] 等待时间同步...");
  time_t now = 0;
  struct tm timeinfo = { 0 };
  int retry = 0;
  const int maxRetries = 15;  // 增加重试次数

  while (timeinfo.tm_year < (2016 - 1900) && ++retry < maxRetries) {
    delay(2000);  // 增加等待时间
    time(&now);
    localtime_r(&now, &timeinfo);
    Serial.printf("[RTC] 同步尝试 %d/%d...\n", retry, maxRetries);
  }

  if (retry >= maxRetries) {
    Serial.println("[RTC] 时间同步失败，使用当前日期时间");
    // 设置默认时间为当前日期
    struct tm defaultTime = { 0 };
    defaultTime.tm_year = 2024 - 1900;
    defaultTime.tm_mon = 0;    // 1月
    defaultTime.tm_mday = 15;  // 15日
    defaultTime.tm_hour = 12;  // 12点
    defaultTime.tm_min = 0;
    defaultTime.tm_sec = 0;
    time_t defaultTimestamp = mktime(&defaultTime);
    struct timeval tv = { defaultTimestamp, 0 };
    settimeofday(&tv, NULL);

    Serial.printf("[RTC] 使用默认时间: 2024-01-15 12:00:00\n");
    Serial.printf("[RTC] 默认时间戳: %lu\n", defaultTimestamp);
    Serial.printf("[RTC] 默认时间戳(毫秒): %lu\n", defaultTimestamp * 1000);
  } else {
    Serial.printf("[RTC] 时间同步成功: %04d-%02d-%02d %02d:%02d:%02d\n",
                  timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    Serial.printf("[RTC] 同步后时间戳: %lu\n", now);
    Serial.printf("[RTC] 同步后时间戳(毫秒): %lu\n", now * 1000);

    // 立即检查是否需要睡眠
    Serial.println("[RTC] 时间同步完成，立即检查睡眠调度");
    checkTimeSchedule();
  }
}

// 更新功率模式
void updatePowerMode() {
  // 根据时间调度决定是否进入睡眠
  if (isTimeToSleep && currentPowerMode != POWER_SLEEP) {
    Serial.println("[POWER] 根据时间调度进入睡眠模式");
    enterSleepMode();
    return;
  }

  // 如果到了唤醒时间且当前在睡眠模式，则唤醒
  if (!isTimeToSleep && currentPowerMode == POWER_SLEEP) {
    Serial.println("[POWER] 根据时间调度退出睡眠模式");
    enterActiveMode();
    return;
  }
}

// 获取功率模式字符串
String getPowerModeString(PowerMode mode) {
  switch (mode) {
    case POWER_ACTIVE: return "活跃";
    case POWER_SLEEP: return "睡眠";
    default: return "未知";
  }
}

// MQTT模式心跳发布（C3无电池监控，显示“无”）
void publishMqttHeartbeat() {
  bool useMl307 = (netMode == "ml307r");

  time_t now_t;
  time(&now_t);
  uint64_t currentTimestamp = (uint64_t)now_t * 1000ULL;

  String timeSlotsJson = "[";
  for (int i = 0; i < timeSlotCount; i++) {
    if (i > 0) timeSlotsJson += ",";
    timeSlotsJson += "{\"wakeH\":" + String(timeSlots[i].wakeHour) +
                     ",\"wakeM\":" + String(timeSlots[i].wakeMinute) +
                     ",\"sleepH\":" + String(timeSlots[i].sleepHour) +
                     ",\"sleepM\":" + String(timeSlots[i].sleepMinute) +
                     ",\"enabled\":" + String(timeSlots[i].enabled ? "true" : "false") + "}";
  }
  timeSlotsJson += "]";

  int firstSlot = -1;
  for (int i = 0; i < timeSlotCount; i++) { if (timeSlots[i].enabled) { firstSlot = i; break; } }
  if (firstSlot < 0 && timeSlotCount > 0) firstSlot = 0;
  int wH = (firstSlot >= 0) ? timeSlots[firstSlot].wakeHour   : WAKE_HOUR;
  int wM = (firstSlot >= 0) ? timeSlots[firstSlot].wakeMinute : WAKE_MINUTE;
  int sH = (firstSlot >= 0) ? timeSlots[firstSlot].sleepHour  : SLEEP_HOUR;
  int sM = (firstSlot >= 0) ? timeSlots[firstSlot].sleepMinute: SLEEP_MINUTE;

  String payload = String("{") +
    "\"type\":\"heartbeat\"," +
    "\"src\":\"O5\"," +
    "\"deviceId\":\"" + deviceId + "\"," +
    "\"timestamp\":" + uint64ToString(currentTimestamp) + "," +
    "\"powerMode\":\"" + getPowerModeString(currentPowerMode) + "\"," +
    "\"batteryLevel\":\"\u65e0\"," +
    "\"uptime\":" + String(millis()) + "," +
    "\"sleepStartTime\":" + uint64ToString(sleepStartTime) + "," +
    "\"wakeStartTime\":" + uint64ToString(wakeStartTime) + "," +
    "\"sleepHour\":" + String(sH) + "," +
    "\"sleepMinute\":" + String(sM) + "," +
    "\"wakeHour\":" + String(wH) + "," +
    "\"wakeMinute\":" + String(wM) + "," +
    "\"timeSlots\":" + timeSlotsJson + "}";

  String prefix = mqttDeviceName.length() > 0 ? mqttDeviceName : deviceId;
  String topic = prefix + "/heartbeat";
  Serial.printf("[MQTT][HB] topic=%s (C3,无电池)\n", topic.c_str());

  if (useMl307) {
    if (!ml307EnsureNetwork()) { Serial.println("[MQTT][HB] 网络未连接，跳过"); return; }
    bool ok = ml307MqttAtPublishPlain(topic, payload, false);
    if (ok) { serverFailCount = 0; Serial.println("[MQTT][HB] 发布成功 (ML307)"); addDebugLog("[HB] MQTT心跳成功(ML307)"); }
    else { addDebugLog("[HB] MQTT心跳失败(ML307)"); }
    return;
  }

  if (!ensureMqttConnection()) { Serial.println("[MQTT][HB] MQTT未连接，跳过"); return; }
  PubSubClient& mqttC = getActiveMqttClient(useMl307);
  mqttC.setBufferSize(1024);
  bool ok = mqttC.publish(topic.c_str(), payload.c_str(), false);
  if (ok) { serverFailCount = 0; Serial.println("[MQTT][HB] 发布成功 (WiFi)"); addDebugLog("[HB] MQTT心跳成功(WiFi)"); }
  else { Serial.printf("[MQTT][HB] 发布失败 state=%d\n", mqttC.state()); addDebugLog("[HB] MQTT心跳失败 state=" + String(mqttC.state())); }
}

// 上报当前扫描配置到 {host}/config/status (retained)
void publishMqttConfigStatus() {
  bool useMl307 = (netMode == "ml307r");
  String prefix = mqttDeviceName.length() > 0 ? mqttDeviceName : deviceId;
  String topic = prefix + "/config/status";

  // 构建当前targets字符串
  String currentTargets;
  if (scanTargetMode == "uuid") {
    currentTargets = scanTargetUuid;
  } else {
    for (int i = 0; i < (int)targetDevices.size(); i++) {
      if (i > 0) currentTargets += ",";
      currentTargets += targetDevices[i].mac;
    }
  }

  time_t now_t;
  time(&now_t);

  String payload = String("{") +
    "\"type\":\"configStatus\"," +
    "\"deviceId\":\"" + deviceId + "\"," +
    "\"scanMode\":\"" + scanTargetMode + "\"," +
    "\"targets\":\"" + currentTargets + "\"," +
    "\"ts\":" + String((unsigned long)now_t) + "}";

  Serial.printf("[CONFIG-STATUS] topic=%s payload=%s\n", topic.c_str(), payload.c_str());

  if (useMl307) {
    if (!ml307EnsureNetwork()) { Serial.println("[CONFIG-STATUS] 网络未连接，跳过"); return; }
    ml307MqttAtPublishPlain(topic, payload, true);
  } else {
    if (!ensureMqttConnection()) { Serial.println("[CONFIG-STATUS] MQTT未连接，跳过"); return; }
    PubSubClient& c = getActiveMqttClient(false);
    c.publish(topic.c_str(), payload.c_str(), true);
  }
  Serial.println("[CONFIG-STATUS] 上报成功");
}

// 发送心跳报告
void sendHeartbeat() {
  if (dataMode == "mqtt") {
    publishMqttHeartbeat();
    publishMqttConfigStatus();  // 心跳时同步上报配置状态
    return;
  }
  if (dataMode != "server") {
    return;
  }
  
  if (!ensureNetworkConnection()) {
    Serial.println("[HEARTBEAT] 网络连接失败，跳过心跳报告");
    return;
  }

  Serial.println("[HEARTBEAT] === 发送心跳报告 ===");

  String url = SERVER_BASE + "/api/o5/heartbeat";
  time_t now;
  time(&now);
  uint64_t currentTimestamp = (uint64_t)now * 1000ULL;  // 转换为毫秒时间戳，使用ULL避免溢出

  Serial.printf("[HEARTBEAT] 当前时间戳: %llu\n", currentTimestamp);
  Serial.printf("[HEARTBEAT] 睡眠时间戳: %llu\n", sleepStartTime);
  Serial.printf("[HEARTBEAT] 苏醒时间戳: %llu\n", wakeStartTime);
  
  // 使用 localtime 而不是 ctime，确保 TZ 生效
  struct tm* ti = localtime(&now);
  Serial.printf("[HEARTBEAT] 当前时间: %04d-%02d-%02d %02d:%02d:%02d (北京时间)\n",
    ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday,
    ti->tm_hour, ti->tm_min, ti->tm_sec);

  // 生成时间段JSON数据
  String timeSlotsJson = "[";
  for (int i = 0; i < timeSlotCount; i++) {
    if (i > 0) timeSlotsJson += ",";
    timeSlotsJson += "{";
    timeSlotsJson += "\"wakeH\":" + String(timeSlots[i].wakeHour) + ",";
    timeSlotsJson += "\"wakeM\":" + String(timeSlots[i].wakeMinute) + ",";
    timeSlotsJson += "\"sleepH\":" + String(timeSlots[i].sleepHour) + ",";
    timeSlotsJson += "\"sleepM\":" + String(timeSlots[i].sleepMinute) + ",";
    timeSlotsJson += "\"enabled\":" + String(timeSlots[i].enabled ? "true" : "false");
    timeSlotsJson += "}";
  }
  timeSlotsJson += "]";

  // 为了向后兼容，保留单个时间段字段（使用第一个启用的时间段，如果没有则使用第一个）
  int firstEnabledSlot = -1;
  for (int i = 0; i < timeSlotCount; i++) {
    if (timeSlots[i].enabled) {
      firstEnabledSlot = i;
      break;
    }
  }
  if (firstEnabledSlot < 0 && timeSlotCount > 0) {
    firstEnabledSlot = 0;  // 如果没有启用的，使用第一个
  }
  
  int wakeHour = (firstEnabledSlot >= 0) ? timeSlots[firstEnabledSlot].wakeHour : WAKE_HOUR;
  int wakeMinute = (firstEnabledSlot >= 0) ? timeSlots[firstEnabledSlot].wakeMinute : WAKE_MINUTE;
  int sleepHour = (firstEnabledSlot >= 0) ? timeSlots[firstEnabledSlot].sleepHour : SLEEP_HOUR;
  int sleepMinute = (firstEnabledSlot >= 0) ? timeSlots[firstEnabledSlot].sleepMinute : SLEEP_MINUTE;

  String payload = String("{") + 
    "\"deviceId\":\"" + deviceId + "\"," + 
    "\"timestamp\":" + uint64ToString(currentTimestamp) + "," + 
    "\"powerMode\":\"" + getPowerModeString(currentPowerMode) + "\"," + 
    "\"batteryLevel\":\"unknown\"," + 
    "\"uptime\":" + String(millis()) + "," + 
    "\"sleepStartTime\":" + uint64ToString(sleepStartTime) + "," + 
    "\"wakeStartTime\":" + uint64ToString(wakeStartTime) + "," + 
    "\"sleepHour\":" + String(sleepHour) + "," + 
    "\"sleepMinute\":" + String(sleepMinute) + "," + 
    "\"wakeHour\":" + String(wakeHour) + "," + 
    "\"wakeMinute\":" + String(wakeMinute) + "," +
    "\"timeSlots\":" + timeSlotsJson + "}";

  Serial.printf("心跳URL: %s\n", url.c_str());
  Serial.printf("心跳数据: %s\n", payload.c_str());

  unsigned long startTime = millis();
  int code = 0;
  String response;
  httpRequest("POST", url, "application/json", payload, code, response);
  unsigned long duration = millis() - startTime;

  Serial.printf("心跳响应码: %d\n", code);
  Serial.printf("心跳耗时: %lu ms\n", duration);

  if (code == 200) {
    serverFailCount = 0; // 连接成功，重置计数
    Serial.printf("[SUCCESS] 心跳报告成功: %s\n", response.c_str());
    addDebugLog("[HB] 心跳发送成功");
  } else {
    Serial.printf("[ERROR] 心跳报告失败，代码: %d\n", code);
    addDebugLog("[HB] 心跳失败 code=" + String(code));
    if (response.length() > 0) {
      Serial.printf("错误响应: %s\n", response.c_str());
    }
  }
}

// ===== 配置与AP门户 =====
void loadConfiguration() {
  preferences.begin("config", true);
  isConfigured = preferences.getBool("configured", false);
  String ssid = preferences.getString("ssid", "");
  String pass = preferences.getString("pass", "");
  String server = preferences.getString("server", SERVER_BASE);
  String timeSlotsJson = preferences.getString("time_slots", "");
  String targetsSaved = preferences.getString("targets", "");

  netMode = preferences.getString("net_mode", "wifi");
  ml307Apn = preferences.getString("ml307_apn", "cmnet");
  ml307Baud = preferences.getInt("ml307_baud", 115200);
  
  lcAppId = preferences.getString("lc_app_id", "");
  lcAppKey = preferences.getString("lc_app_key", "");
  lcApiUrl = preferences.getString("lc_api_url", "");
  lcTargetMac = preferences.getString("lc_target_mac", "");
  mqttHost = preferences.getString("mqtt_host", "");
  mqttPort = preferences.getInt("mqtt_port", 8883);
  mqttUser = preferences.getString("mqtt_user", "");
  mqttPass = preferences.getString("mqtt_pass", "");
  mqttDeviceName = preferences.getString("mqtt_dname", "");  // 主机名称 (键名限制15字符)
  Serial.printf("[DEBUG] 加载mqtt_dname: '%s'\n", mqttDeviceName.c_str());

  dataMode = preferences.getString("data_mode", "mqtt");

  scanTargetMode = preferences.getString("scan_mode", "mac");
  scanTargetUuid = preferences.getString("target_uuid", "");
  
  // 加载扫描参数
  scanDuration = preferences.getInt("scan_duration", 3);
  scanInterval = preferences.getInt("scan_interval", 5);

  // 静默模式
  silentMode = preferences.getBool("silent", false);

  // 云控开关
  cloudControlEnabled = preferences.getBool("cloud_ctrl", true);

  // MQTT自动模式
  mqttAutoMode = preferences.getBool("mqtt_auto", true);

  preferences.end();

  if (silentMode) {
    Serial.println("[CONFIG] 静默模式已开启（LED关闭、配置热点关闭）");
  }

  wifiSsid = ssid;
  wifiPass = pass;
  SERVER_BASE = server;
  
  Serial.printf("[CONFIG] DataMode='%s'\n", dataMode.c_str());
  Serial.printf("[CONFIG] NetMode='%s', ML307 APN='%s', Baud=%d\n", netMode.c_str(), ml307Apn.c_str(), ml307Baud);
  Serial.printf("[CONFIG] LeanCloud: AppID='%s', AppKey='%s', ApiUrl='%s', TargetMac='%s'\n", 
                lcAppId.c_str(), lcAppKey.length() > 0 ? "***" : "", lcApiUrl.c_str(), lcTargetMac.c_str());

  if (dataMode == "leancloud") {
    dataMode = "mqtt";
  }

  Serial.printf("[CONFIG] MQTT: Host='%s', Port=%d, User='%s', Pass='%s', DeviceName='%s'\n",
                mqttHost.c_str(), mqttPort, mqttUser.c_str(), mqttPass.length() > 0 ? "***" : "", mqttDeviceName.c_str());

  if (scanTargetMode != "uuid") {
    scanTargetMode = "mac";
  }
  targetUuids.clear();
  if (scanTargetMode == "uuid") {
    String s = scanTargetUuid;
    s.replace(",", " ");
    int start = 0;
    while (start < (int)s.length()) {
      while (start < (int)s.length() && (s[start] == ' ' || s[start] == '\t')) start++;
      if (start >= (int)s.length()) break;
      int end = start;
      while (end < (int)s.length() && s[end] != ' ' && s[end] != '\t') end++;
      String tok = s.substring(start, end);
      tok = normalizeUuidToken(tok);
      if (tok.length() > 0) targetUuids.push_back(tok);
      start = end + 1;
    }
  }
  
  // 加载时间段配置
  if (timeSlotsJson.length() > 0) {
    parseTimeSlots(timeSlotsJson);
  } else {
    // 如果没有配置，保持为0，表示一直在线
    timeSlotCount = 0;
    // timeSlots[0] = {WAKE_HOUR, WAKE_MINUTE, SLEEP_HOUR, SLEEP_MINUTE, true};
  }

  Serial.printf("[CONFIG] isConfigured=%s, SSID='%s', SERVER_BASE='%s', TimeSlots=%d\n",
                isConfigured ? "true" : "false",
                wifiSsid.c_str(), SERVER_BASE.c_str(), timeSlotCount);

  // server + mac 模式下，从持久化的 targets 恢复目标列表（保证断电重启后仍保留服务器下发配置）
  if (dataMode == "server" && scanTargetMode == "mac") {
    String s = targetsSaved;
    s.replace(",", " ");
    s.trim();
    if (s.length() > 0) {
      targetDevices.clear();
      int start = 0;
      while (start < (int)s.length()) {
        while (start < (int)s.length() && (s[start] == ' ' || s[start] == '\t')) start++;
        if (start >= (int)s.length()) break;
        int end = start;
        while (end < (int)s.length() && s[end] != ' ' && s[end] != '\t') end++;
        String mac = s.substring(start, end);
        mac.trim();
        if (mac.length() > 0) {
          addTargetDevice(mac, "已保存目标");
        }
        start = end + 1;
      }
    }
  }
}

bool saveConfiguration(const String& ssid, const String& pass, const String& serverBase, const String& timeSlotsJson,
                       const String& mode, const String& targetMac, int duration, int interval,
                       const String& net_mode, const String& apn, int baud, const String& scan_mode, const String& target_uuid,
                       const String& mHost, int mPort, const String& mUser, const String& mPass, const String& mDeviceName) {
  preferences.begin("config", false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", pass);
  preferences.putString("server", serverBase);
  if (timeSlotsJson.length() > 0) {
    preferences.putString("time_slots", timeSlotsJson);
    parseTimeSlots(timeSlotsJson);
  }

  preferences.putString("lc_target_mac", targetMac);

  if (mHost.length() > 0) preferences.putString("mqtt_host", mHost);
  preferences.putInt("mqtt_port", mPort);
  if (mUser.length() > 0) preferences.putString("mqtt_user", mUser);
  if (mPass.length() > 0) preferences.putString("mqtt_pass", mPass);
  // 保存MQTT设备名称（主机名称）- 键名限制15字符，使用mqtt_dname
  Serial.printf("[DEBUG] 保存mqtt_dname: '%s' (len=%d)\n", mDeviceName.c_str(), mDeviceName.length());
  if (mDeviceName.length() > 0) {
    preferences.putString("mqtt_dname", mDeviceName);
    Serial.println("[DEBUG] mqtt_dname 已保存");
  } else {
    Serial.println("[DEBUG] mqtt_dname 为空，跳过保存");
  }
  
  preferences.putString("data_mode", mode);

  preferences.putString("scan_mode", scan_mode);
  preferences.putString("target_uuid", target_uuid);

  preferences.putString("net_mode", net_mode);
  preferences.putString("ml307_apn", apn);
  preferences.putInt("ml307_baud", baud);
  
  // 保存扫描参数
  preferences.putInt("scan_duration", duration);
  preferences.putInt("scan_interval", interval);
  
  preferences.putBool("mqtt_auto", mqttAutoMode);
  
  preferences.putBool("configured", true);
  preferences.end();

  wifiSsid = ssid;
  wifiPass = pass;
  SERVER_BASE = serverBase;

  lcTargetMac = targetMac;
  
  dataMode = mode;
  scanDuration = duration;
  scanInterval = interval;

  scanTargetMode = (scan_mode == "uuid") ? "uuid" : "mac";
  scanTargetUuid = target_uuid;
  targetUuids.clear();
  if (scanTargetMode == "uuid") {
    String s = scanTargetUuid;
    s.replace(",", " ");
    int start = 0;
    while (start < (int)s.length()) {
      while (start < (int)s.length() && (s[start] == ' ' || s[start] == '\t')) start++;
      if (start >= (int)s.length()) break;
      int end = start;
      while (end < (int)s.length() && s[end] != ' ' && s[end] != '\t') end++;
      String tok = s.substring(start, end);
      tok = normalizeUuidToken(tok);
      if (tok.length() > 0) targetUuids.push_back(tok);
      start = end + 1;
    }
  }

  netMode = net_mode;
  ml307Apn = apn;
  ml307Baud = baud;

  mqttHost = mHost;
  mqttPort = mPort;
  mqttUser = mUser;
  mqttPass = mPass;
  mqttDeviceName = mDeviceName;

  isConfigured = true;
  Serial.printf("[CONFIG] 保存配置成功，ScanDuration=%d, ScanInterval=%d, DeviceName=%s\n", 
                scanDuration, scanInterval, mqttDeviceName.c_str());
  return true;
}

void setupTimeZone() {
  setenv("TZ", "CST-8", 1);
  tzset();
}

static void getBeijingTime(time_t utc, struct tm& outTm) {
  time_t t = utc + 8 * 3600;
  gmtime_r(&t, &outTm);
}

static bool parseMl307Clock(const String& resp, int64_t& outEpochUtc) {
  int p = resp.indexOf("+CCLK:");
  if (p < 0) return false;
  int q1 = resp.indexOf('"', p);
  if (q1 < 0) return false;
  int q2 = resp.indexOf('"', q1 + 1);
  if (q2 < 0) return false;
  String s = resp.substring(q1 + 1, q2);
  s.trim();
  // 期望格式: yy/MM/dd,hh:mm:ss(+/-tzQuarter)
  int comma = s.indexOf(',');
  if (comma < 0) return false;
  String datePart = s.substring(0, comma);
  String timePart = s.substring(comma + 1);
  datePart.trim();
  timePart.trim();

  if (datePart.length() < 8 || timePart.length() < 8) return false;

  int yy = datePart.substring(0, 2).toInt();
  int mm = datePart.substring(3, 5).toInt();
  int dd = datePart.substring(6, 8).toInt();
  int year = 2000 + yy;
  if (mm < 1 || mm > 12 || dd < 1 || dd > 31) return false;

  // 只接受合理年份范围，避免 32-bit time_t 溢出导致 epoch 变负
  // (如果模块还没同步成功，常见会返回 70/01/01 之类的默认值)
  if (year < 2020 || year > 2038) return false;

  // 提取时区（四分之一小时），例如 +32 表示 UTC+8
  int tzPos = timePart.indexOf('+', 8);
  if (tzPos < 0) tzPos = timePart.indexOf('-', 8);
  int tzQuarter = 0;
  if (tzPos >= 0) {
    tzQuarter = timePart.substring(tzPos).toInt();
    timePart = timePart.substring(0, tzPos);
    timePart.trim();
  }
  if (tzQuarter < -96 || tzQuarter > 96) tzQuarter = 0;
  int tzOffsetSec = tzQuarter * 15 * 60;

  // 允许出现异常值（例如模块只返回UTC，但仍拼接了时区，或出现负数/越界）
  // 这类情况下按“UTC时间”解析并做归一化处理。
  int c1 = timePart.indexOf(':');
  int c2 = timePart.indexOf(':', c1 + 1);
  if (c1 < 0 || c2 < 0) return false;
  int hh = timePart.substring(0, c1).toInt();
  int mi = timePart.substring(c1 + 1, c2).toInt();
  int ss = timePart.substring(c2 + 1).toInt();

  bool timeLooksNormal = (hh >= 0 && hh <= 23 && mi >= 0 && mi <= 59 && ss >= 0 && ss <= 59);

  // 归一化（处理负数/越界的 hh:mm:ss）
  int64_t secOfDay = (int64_t)hh * 3600 + (int64_t)mi * 60 + (int64_t)ss;
  int64_t dayAdjust = 0;
  if (secOfDay < 0 || secOfDay >= 86400) {
    // floor division
    dayAdjust = secOfDay / 86400;
    if (secOfDay < 0 && (secOfDay % 86400)) dayAdjust -= 1;
    secOfDay -= dayAdjust * 86400;
  }

  struct tm tmUtc;
  memset(&tmUtc, 0, sizeof(tmUtc));
  tmUtc.tm_year = year - 1900;
  tmUtc.tm_mon = mm - 1;
  tmUtc.tm_mday = dd;
  tmUtc.tm_hour = 0;
  tmUtc.tm_min = 0;
  tmUtc.tm_sec = 0;

  char* oldTz = getenv("TZ");
  String oldTzStr = oldTz ? String(oldTz) : String("");
  setenv("TZ", "UTC0", 1);
  tzset();

  // 先求当天 00:00:00(UTC) 的 epoch，再加秒数
  time_t base = mktime(&tmUtc);

  // 恢复时区
  if (oldTzStr.length() > 0) {
    setenv("TZ", oldTzStr.c_str(), 1);
  } else {
    unsetenv("TZ");
  }
  tzset();

  if (base <= 0) return false;

  int64_t epochUtc = (int64_t)base + dayAdjust * 86400 + secOfDay;

  // 若时间字段正常，且存在 tzQuarter，则按标准定义：字符串表示“本地时间”，需要减去 offset 得到 UTC。
  // 若时间字段不正常（例如出现负数/越界），则更可能已经是 UTC 变形结果，此时不再二次应用 offset。
  if (timeLooksNormal && tzPos >= 0 && tzQuarter != 32) {
    epochUtc -= (int64_t)tzOffsetSec;
  }

  outEpochUtc = epochUtc;
  return true;
}

String g_ml307LastMIPRD;
String g_ml307LastMIPSEND;
String g_ml307LastMIPOPEN;

bool ml307SyncTimeFromCCLK() {
  String resp;
  if (!ml307SendAT("AT+CCLK?", "+CCLK:", nullptr, 2000, &resp)) {
    Serial.println("[TIME] AT+CCLK? 失败");
    return false;
  }
  // 打印原始 CCLK 响应
  Serial.printf("[TIME] CCLK原始: %s\n", resp.c_str());
  
  int64_t epochUtc64 = 0;
  if (!parseMl307Clock(resp, epochUtc64)) {
    Serial.println("[TIME] 解析CCLK失败");
    return false;
  }
  Serial.printf("[TIME] 解析得到UTC epoch: %lld\n", (long long)epochUtc64);
  if (epochUtc64 < 1700000000) {
    Serial.println("[TIME] CCLK时间无效");
    return false;
  }

  // 若 time_t 为 32-bit，2038 之后会溢出，直接判定无效，转用 HTTP Date/NTP
  if (sizeof(time_t) <= 4 && epochUtc64 > 2147483647LL) {
    Serial.println("[TIME] CCLK时间超出2038范围，放弃使用，改用HTTP同步");
    return false;
  }

  struct timeval tv;
  tv.tv_sec = (time_t)epochUtc64;
  tv.tv_usec = 0;
  settimeofday(&tv, NULL);
  
  // 重新设置时区确保生效
  setupTimeZone();
  
  // 打印同步后的本地时间
  time_t now;
  time(&now);
  struct tm ti;
  getBeijingTime(now, ti);
  // 手动计算验证: 如果 localtime 不对，说明 TZ 没生效，这里提供一个 debug 参考
  time_t cnTime = (time_t)(epochUtc64 + 8 * 3600LL);
  struct tm ti2;
  gmtime_r(&cnTime, &ti2);
  (void)ti2;
  
  Serial.printf("[TIME] 同步成功: %04d-%02d-%02d %02d:%02d:%02d (UTC+8)\n",
    ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
    ti.tm_hour, ti.tm_min, ti.tm_sec);
    
  return true;
}

static bool parseHttpDateRfc1123(const String& dateStr, time_t& outEpochUtc) {
  String s = dateStr;
  s.trim();
  int comma = s.indexOf(',');
  if (comma >= 0) {
    s = s.substring(comma + 1);
    s.trim();
  }
  if (s.length() < 20) return false;

  int sp1 = s.indexOf(' ');
  if (sp1 < 0) return false;
  String dayStr = s.substring(0, sp1);
  dayStr.trim();
  int day = dayStr.toInt();
  String rest = s.substring(sp1 + 1);
  rest.trim();

  int sp2 = rest.indexOf(' ');
  if (sp2 < 0) return false;
  String monStr = rest.substring(0, sp2);
  String rest2 = rest.substring(sp2 + 1);
  rest2.trim();

  int sp3 = rest2.indexOf(' ');
  if (sp3 < 0) return false;
  int year = rest2.substring(0, sp3).toInt();
  String rest3 = rest2.substring(sp3 + 1);
  rest3.trim();

  int sp4 = rest3.indexOf(' ');
  if (sp4 < 0) return false;
  String timePart = rest3.substring(0, sp4);
  String tzPart = rest3.substring(sp4 + 1);
  tzPart.trim();
  if (!(tzPart == "GMT" || tzPart == "UTC")) {
    return false;
  }

  int c1 = timePart.indexOf(':');
  int c2 = timePart.indexOf(':', c1 + 1);
  if (c1 < 0 || c2 < 0) return false;
  int hh = timePart.substring(0, c1).toInt();
  int mi = timePart.substring(c1 + 1, c2).toInt();
  int ss = timePart.substring(c2 + 1).toInt();

  int mon = 0;
  if (monStr == "Jan") mon = 1;
  else if (monStr == "Feb") mon = 2;
  else if (monStr == "Mar") mon = 3;
  else if (monStr == "Apr") mon = 4;
  else if (monStr == "May") mon = 5;
  else if (monStr == "Jun") mon = 6;
  else if (monStr == "Jul") mon = 7;
  else if (monStr == "Aug") mon = 8;
  else if (monStr == "Sep") mon = 9;
  else if (monStr == "Oct") mon = 10;
  else if (monStr == "Nov") mon = 11;
  else if (monStr == "Dec") mon = 12;
  else return false;

  struct tm tmUtc;
  memset(&tmUtc, 0, sizeof(tmUtc));
  tmUtc.tm_year = year - 1900;
  tmUtc.tm_mon = mon - 1;
  tmUtc.tm_mday = day;
  tmUtc.tm_hour = hh;
  tmUtc.tm_min = mi;
  tmUtc.tm_sec = ss;

  char* oldTz = getenv("TZ");
  String oldTzStr = oldTz ? String(oldTz) : String("");
  setenv("TZ", "UTC0", 1);
  tzset();
  time_t epoch = mktime(&tmUtc);
  if (oldTzStr.length() > 0) {
    setenv("TZ", oldTzStr.c_str(), 1);
  } else {
    unsetenv("TZ");
  }
  tzset();

  if (epoch <= 0) return false;
  outEpochUtc = epoch;
  return true;
}

bool syncTimeFromHttpDate(const String& fullHttpResponse) {
  int p = fullHttpResponse.indexOf("\r\nDate:");
  if (p < 0) p = fullHttpResponse.indexOf("\nDate:");
  if (p < 0) return false;
  int lineEnd = fullHttpResponse.indexOf("\n", p + 1);
  if (lineEnd < 0) return false;
  String line = fullHttpResponse.substring(p, lineEnd);
  int colon = line.indexOf(':');
  if (colon < 0) return false;
  String dateVal = line.substring(colon + 1);
  dateVal.trim();
  time_t epochUtc = 0;
  if (!parseHttpDateRfc1123(dateVal, epochUtc)) {
    return false;
  }
  if (epochUtc < 1700000000) return false;
  struct timeval tv;
  tv.tv_sec = epochUtc;
  tv.tv_usec = 0;
  settimeofday(&tv, NULL);
  return true;
}

bool ml307SyncTime() {
  time_t now = 0;
  time(&now);
  if (now >= 1700000000) {
    struct tm ti;
    getBeijingTime(now, ti);
    Serial.printf("[TIME] 时间已同步: %04d-%02d-%02d %02d:%02d:%02d\n",
      ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
      ti.tm_hour, ti.tm_min, ti.tm_sec);
    return true;
  }
  Serial.println("[TIME] 尝试从模块同步时间...");
  if (ml307SyncTimeFromCCLK()) {
    return true;
  }
  Serial.println("[TIME] 时间同步失败，将从 HTTP 响应同步");
  return false;
}

// 解析时间段JSON字符串
void parseTimeSlots(const String& json) {
  timeSlotCount = 0;
  // 简单JSON解析：查找所有时间段对象
  int pos = 0;
  while (pos < json.length() && timeSlotCount < MAX_TIME_SLOTS) {
    int objStart = json.indexOf('{', pos);
    if (objStart < 0) break;
    
    int objEnd = json.indexOf('}', objStart);
    if (objEnd < 0) break;
    
    String obj = json.substring(objStart, objEnd + 1);
    
    // 解析各个字段
    int wakeHPos = obj.indexOf("\"wakeH\":");
    int wakeMPos = obj.indexOf("\"wakeM\":");
    int sleepHPos = obj.indexOf("\"sleepH\":");
    int sleepMPos = obj.indexOf("\"sleepM\":");
    int enabledPos = obj.indexOf("\"enabled\":");
    
    if (wakeHPos >= 0 && wakeMPos >= 0 && sleepHPos >= 0 && sleepMPos >= 0) {
      timeSlots[timeSlotCount].wakeHour = parseIntValue(obj, wakeHPos);
      timeSlots[timeSlotCount].wakeMinute = parseIntValue(obj, wakeMPos);
      timeSlots[timeSlotCount].sleepHour = parseIntValue(obj, sleepHPos);
      timeSlots[timeSlotCount].sleepMinute = parseIntValue(obj, sleepMPos);
      timeSlots[timeSlotCount].enabled = (enabledPos >= 0 && obj.indexOf("true", enabledPos) > enabledPos);
      timeSlotCount++;
    }
    
    pos = objEnd + 1;
  }
  
  // 如果没有解析到任何时间段，保持为0（一直在线）
  /*
  if (timeSlotCount == 0) {
    timeSlotCount = 1;
    timeSlots[0] = {WAKE_HOUR, WAKE_MINUTE, SLEEP_HOUR, SLEEP_MINUTE, true};
  }
  */
}

// 辅助函数：从JSON字符串中提取整数值
int parseIntValue(const String& json, int keyPos) {
  int colonPos = json.indexOf(':', keyPos);
  if (colonPos < 0) return 0;
  
  int valueStart = colonPos + 1;
  while (valueStart < json.length() && (json[valueStart] == ' ' || json[valueStart] == '\t')) {
    valueStart++;
  }
  
  int valueEnd = valueStart;
  while (valueEnd < json.length() && 
         json[valueEnd] >= '0' && json[valueEnd] <= '9') {
    valueEnd++;
  }
  
  if (valueEnd > valueStart) {
    return json.substring(valueStart, valueEnd).toInt();
  }
  return 0;
}

// WiFi 网络信息结构
struct WifiNetwork {
  String ssid;
  int rssi;
};

// ========== 配置页面BLE扫描专用结构和回调 ==========
struct ApBleScanItem {
  String mac;
  int rssi;
  String name;
  String uuid;      // 主要UUID（16位优先）
  String allUuids;  // 所有UUID列表
  String rawData;   // 原始广播数据
};

static std::vector<ApBleScanItem> apBleScanResults;
static const int AP_BLE_SCAN_MAX = 30;
static bool apBleScanActive = false;

class ApBleAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    if (!apBleScanActive) return;
    
    // 在回调中立即获取所有数据（此时payload数据有效）
    String mac = String(advertisedDevice.getAddress().toString().c_str());
    mac.toUpperCase();
    int rssi = advertisedDevice.getRSSI();
    
    String devName = "";
    if (advertisedDevice.haveName()) {
      devName = String(advertisedDevice.getName().c_str());
    }
    
    // 获取payload数据（在回调中是有效的）
    uint8_t* payloadData = advertisedDevice.getPayload();
    size_t payloadLength = advertisedDevice.getPayloadLength();
    
    // 生成RAW数据字符串
    String rawData = "0x";
    size_t maxRawLen = (payloadLength > 40) ? 40 : payloadLength;
    char hexBuf[3];
    for (size_t i = 0; i < maxRawLen; i++) {
      sprintf(hexBuf, "%02X", payloadData[i]);
      rawData += hexBuf;
    }
    if (payloadLength > 40) rawData += "...";
    
    // 解析UUID
    String uuid = "";
    String allUuids = "";
    std::vector<String> uuids;
    parseServiceUuidsFromAdvPayload(payloadData, payloadLength, uuids);
    
    for (const auto& u : uuids) {
      String formatted = u;
      formatted.toUpperCase();
      
      if (allUuids.length() > 0) allUuids += ",";
      if (formatted.length() == 4) {
        allUuids += formatted;
      } else if (formatted.length() == 32) {
        allUuids += formatted.substring(0, 8) + "-" + formatted.substring(8, 12) + "-" + 
                    formatted.substring(12, 16) + "-" + formatted.substring(16, 20) + "-" + 
                    formatted.substring(20);
      } else {
        allUuids += formatted;
      }
      
      // 优先16位UUID
      if (uuid.length() == 0) {
        if (formatted.length() == 4) {
          uuid = formatted;
        } else if (formatted.length() == 32) {
          String baseTail = "00001000800000805F9B34FB";
          if (formatted.endsWith(baseTail)) {
            uuid = formatted.substring(4, 8);
          }
        }
      }
    }
    
    // 兜底解析16位UUID
    if (uuid.length() == 0 && payloadData != nullptr && payloadLength > 0) {
      size_t p = 0;
      while (p + 1 < payloadLength) {
        uint8_t fl = payloadData[p];
        if (fl == 0) break;
        size_t end = p + 1 + fl;
        if (end > payloadLength) break;
        uint8_t t = payloadData[p + 1];
        const uint8_t* dptr = payloadData + p + 2;
        size_t dlen = (fl >= 1) ? (fl - 1) : 0;
        if ((t == 0x02 || t == 0x03 || t == 0x16) && dlen >= 2) {
          char buf[5];
          sprintf(buf, "%02X%02X", dptr[1], dptr[0]);
          uuid = String(buf);
          if (allUuids.length() == 0) allUuids = uuid;
          break;
        }
        p = end;
      }
    }
    
    // 更新或添加到结果列表
    bool found = false;
    for (auto& item : apBleScanResults) {
      if (item.mac.equalsIgnoreCase(mac)) {
        if (rssi > item.rssi) {
          item.rssi = rssi;
          item.rawData = rawData;
        }
        if (item.name.length() == 0 && devName.length() > 0) item.name = devName;
        if (item.uuid.length() == 0 && uuid.length() > 0) item.uuid = uuid;
        if (item.allUuids.length() == 0 && allUuids.length() > 0) item.allUuids = allUuids;
        found = true;
        break;
      }
    }
    
    if (!found && (int)apBleScanResults.size() < AP_BLE_SCAN_MAX) {
      ApBleScanItem item;
      item.mac = mac;
      item.rssi = rssi;
      item.name = devName;
      item.uuid = uuid;
      item.allUuids = allUuids;
      item.rawData = rawData;
      apBleScanResults.push_back(item);
    } else if (!found) {
      // 替换信号最弱的
      int worstIdx = 0;
      for (int i = 1; i < (int)apBleScanResults.size(); i++) {
        if (apBleScanResults[i].rssi < apBleScanResults[worstIdx].rssi) worstIdx = i;
      }
      if (rssi > apBleScanResults[worstIdx].rssi) {
        apBleScanResults[worstIdx].mac = mac;
        apBleScanResults[worstIdx].rssi = rssi;
        apBleScanResults[worstIdx].name = devName;
        apBleScanResults[worstIdx].uuid = uuid;
        apBleScanResults[worstIdx].allUuids = allUuids;
        apBleScanResults[worstIdx].rawData = rawData;
      }
    }
  }
};

static ApBleAdvertisedDeviceCallbacks* apBleCallbacks = nullptr;
// ========== 配置页面BLE扫描专用结构和回调 结束 ==========

void startConfigAP() {
  // 确保WiFi在STA模式下，避免 scanNetworks 在某些状态下卡死/失败
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  // 扫描周边WiFi用于页面展示（包含信号强度）
  Serial.println("[AP] 扫描周边WiFi...");
  int n = WiFi.scanNetworks();
  std::vector<WifiNetwork> networks;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    int r = WiFi.RSSI(i);
    if (s.length() > 0) {
      networks.push_back({s, r});
    }
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_LOCAL_IP, AP_LOCAL_IP, IPAddress(255, 255, 255, 0));
  String apSsid = "O5-Setup-" + chipSerialNumber.substring(chipSerialNumber.length() - 6);
  WiFi.softAP(apSsid.c_str(), "12345678");
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("[AP] 启动AP: %s, IP: %s\n", apSsid.c_str(), ip.toString().c_str());

  // 配置DNS服务器：所有域名解析到本地IP（实现自动跳转）
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", AP_LOCAL_IP);
  Serial.println("[AP] DNS服务器启动：所有请求将重定向到配置页面");

  // 路由（美化版，响应式设计）
  configServer.on("/", HTTP_GET, [networks]() mutable {
    // 配置完成后刷新页面→显示运行状态页（不再显示初始配置表单）
    if (isConfigured && statusApActive) {
      handleStatusPage();
      return;
    }
    String options = "";
    for (auto &net : networks) {
      String rssiLabel = net.rssi >= -50 ? "极强" : (net.rssi >= -60 ? "强" : (net.rssi >= -70 ? "中" : "弱"));
      String selected = (net.ssid == wifiSsid) ? " selected" : "";
      options += "<option value='" + net.ssid + "'" + selected + ">" + net.ssid + " (" + rssiLabel + " " + String(net.rssi) + "dBm)</option>";
    }
    
    // 每次请求时重新从NVS加载时间段数据，确保显示最新值
    Preferences tempPrefs;
    tempPrefs.begin("config", true);
    String timeSlotsJson = tempPrefs.getString("time_slots", "");
    tempPrefs.end();
    
    // 保存当前全局时间段状态
    int savedSlotCount = timeSlotCount;
    TimeSlot savedSlots[MAX_TIME_SLOTS];
    for (int i = 0; i < savedSlotCount; i++) {
      savedSlots[i] = timeSlots[i];
    }
    
    // 重新加载并解析时间段数据
    if (timeSlotsJson.length() > 0) {
      parseTimeSlots(timeSlotsJson);
    } else {
      timeSlotCount = 0;
    }
    
    // 生成已保存的时间段JSON数据（使用最新加载的数据）
    String savedTimeSlots = "[";
    for (int i = 0; i < timeSlotCount; i++) {
      if (i > 0) savedTimeSlots += ",";
      savedTimeSlots += "{";
      savedTimeSlots += "\"wakeH\":" + String(timeSlots[i].wakeHour) + ",";
      savedTimeSlots += "\"wakeM\":" + String(timeSlots[i].wakeMinute) + ",";
      savedTimeSlots += "\"sleepH\":" + String(timeSlots[i].sleepHour) + ",";
      savedTimeSlots += "\"sleepM\":" + String(timeSlots[i].sleepMinute) + ",";
      savedTimeSlots += "\"enabled\":" + String(timeSlots[i].enabled ? "true" : "false");
      savedTimeSlots += "}";
    }
    savedTimeSlots += "]";
    
    // 恢复全局时间段状态（避免影响其他功能）
    timeSlotCount = savedSlotCount;
    for (int i = 0; i < savedSlotCount; i++) {
      timeSlots[i] = savedSlots[i];
    }
    String page = String(R"HTML(
<!DOCTYPE html>
<html lang='zh-CN'>
<head>
  <meta charset='utf-8'>
  <meta name='viewport' content='width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no'>
  <title>设备初始化配置</title>
  <style>
    *{box-sizing:border-box;margin:0;padding:0;}
    body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Arial,sans-serif;background:#f0f2f5;min-height:100vh;display:flex;align-items:flex-start;justify-content:center;padding:20px;}
    .container{background:#fff;border-radius:12px;box-shadow:0 1px 6px rgba(0,0,0,.07);width:100%;max-width:450px;padding:24px;margin:10px auto;}
    .header{margin-bottom:18px;padding-bottom:14px;border-bottom:1px solid #f0f0f0;}
    .header h1{color:#222;font-size:17px;font-weight:700;margin-bottom:4px;}
    .header p{color:#888;font-size:13px;}
    .form-group{margin-bottom:14px;}
    .form-group label{display:block;color:#555;font-size:13px;font-weight:500;margin-bottom:5px;}
    .form-group input,.form-group select{width:100%;padding:10px 12px;border:1px solid #ddd;border-radius:8px;font-size:14px;background:#fff;color:#333;}
    .form-group input:focus,.form-group select:focus{outline:none;border-color:#4a90e2;}
    .form-group select{cursor:pointer;appearance:none;background-image:url('data:image/svg+xml;utf8,<svg xmlns="http://www.w3.org/2000/svg" width="12" height="12" viewBox="0 0 12 12"><path fill="%23666" d="M6 9L1 4h10z"/></svg>');background-repeat:no-repeat;background-position:right 12px center;padding-right:36px;}
    .form-group small{display:block;color:#aaa;font-size:11px;margin-top:4px;}
    .submit-btn{width:100%;padding:13px;background:#4a90e2;color:#fff;border:none;border-radius:10px;font-size:15px;font-weight:600;cursor:pointer;margin-top:8px;}
    .icon{display:inline-block;width:20px;height:20px;margin-right:8px;vertical-align:middle;}
    .time-slot{border:1px solid #e8e8e8;border-radius:10px;padding:14px;margin-bottom:10px;background:#fafafa;}
    .time-slot-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px;}
    .time-slot-label{display:flex;align-items:center;cursor:pointer;font-weight:500;color:#333;font-size:13px;}
    .time-slot-label input[type="checkbox"]{margin-right:8px;width:16px;height:16px;cursor:pointer;}
    .delete-btn{padding:5px 10px;background:#fdecea;color:#c0392b;border:none;border-radius:6px;font-size:12px;cursor:pointer;}
    .time-inputs{display:grid;grid-template-columns:1fr 1fr;gap:10px;}
    .time-group{display:flex;flex-direction:column;gap:5px;}
    .time-group label{font-size:12px;color:#888;font-weight:400;}
    .time-row{display:flex;gap:8px;align-items:center;}
    .time-row input[type="number"]{width:70px;padding:9px;border:1px solid #ddd;border-radius:8px;font-size:14px;text-align:center;}
    .time-row input[type="number"]:focus{outline:none;border-color:#4a90e2;}
    .time-picker{width:100%;padding:9px;border:1px solid #ddd;border-radius:8px;font-size:14px;text-align:center;background:#fff;appearance:none;font-family:inherit;}
    .time-picker:focus{outline:none;border-color:#4a90e2;}
    .add-slot-btn{padding:7px 14px;background:#4a90e2;color:#fff;border:none;border-radius:7px;font-size:13px;cursor:pointer;}
    .tabs{display:flex;margin-bottom:16px;background:#f0f2f5;border-radius:10px;overflow:hidden;}
    .tab{flex:1;padding:12px 0;text-align:center;font-size:14px;font-weight:600;border:none;background:#f0f2f5;color:#888;cursor:pointer;transition:all .2s;}
    .tab.active{background:#4a90e2;color:#fff;border-radius:10px;}
    .st-card{background:#fff;border-radius:10px;box-shadow:0 1px 4px rgba(0,0,0,.06);margin-bottom:12px;overflow:hidden;}
    .st-row{display:flex;padding:10px 14px;border-bottom:1px solid #f0f0f0;font-size:13px;}
    .st-row:last-child{border-bottom:none;}
    .st-lbl{width:35%;color:#888;flex-shrink:0;}
    .st-val{flex:1;color:#333;word-break:break-all;}
    .st-ok{color:#27ae60;font-weight:600;}
    .st-err{color:#e05c65;font-weight:600;}
    .st-warn{color:#e67e22;font-weight:600;}
    .log-box{margin:0;padding:10px 14px;font-size:11px;color:#444;background:#fafafa;max-height:260px;overflow-y:auto;white-space:pre-wrap;word-break:break-all;font-family:monospace;}
    @media(max-width:480px){.container{padding:18px 16px;}.header h1{font-size:15px;}.form-group input,.form-group select{padding:10px;font-size:14px;}.time-inputs{grid-template-columns:1fr;}.time-row input[type="number"]{width:100%;}}
  </style>
</head>
<body>
  <div class="container">
    <div class='tabs'>
      <button class='tab active' id='tabBtnConfig' onclick='switchTab("config")'>配置</button>
      <button class='tab' id='tabBtnStatus' onclick='switchTab("status")'>状态</button>
    </div>
    <div id='tab-status' style='display:none;'>
      <div class='st-card'>
        <div class='st-row'><span class='st-lbl'>MAC</span><span class='st-val' id='sv_mac'>--</span></div>
        <div class='st-row'><span class='st-lbl'>运行时间</span><span class='st-val' id='sv_uptime'>--</span></div>
        <div class='st-row'><span class='st-lbl'>空闲内存</span><span class='st-val' id='sv_heap'>--</span></div>
      </div>
      <div class='st-card'>
        <div class='st-row'><span class='st-lbl'>WiFi</span><span class='st-val' id='sv_wifi'>未连接</span></div>
        <div class='st-row'><span class='st-lbl'>MQTT</span><span class='st-val' id='sv_mqtt'>未连接</span></div>
        <div class='st-row'><span class='st-lbl'>扫描</span><span class='st-val' id='sv_scan'>--</span></div>
      </div>
      <div class='st-card'>
        <div style='padding:8px 14px;display:flex;justify-content:space-between;align-items:center;'>
          <span style='font-size:12px;color:#999;font-weight:600;'>运行日志</span>
          <div style='display:flex;gap:6px;'>
            <button onclick='refreshLog()' style='padding:5px 10px;border:1px solid #ddd;border-radius:6px;background:#fff;font-size:11px;cursor:pointer;'>刷新</button>
            <button onclick='clearLog()' style='padding:5px 10px;border:1px solid #ddd;border-radius:6px;background:#fff;font-size:11px;cursor:pointer;color:#c0392b;'>清空</button>
          </div>
        </div>
        <pre class='log-box' id='logContent'>加载中...</pre>
      </div>
      <div id='initMsg' style='display:none;text-align:center;padding:12px;background:#e8f5e9;border-radius:10px;margin-bottom:12px;font-size:13px;color:#2e7d32;font-weight:500;'>
        配置已保存，设备正在初始化...
      </div>
    </div>
    <div id='tab-config'>
    <div class="header">
      <h1>设备配置</h1>
      <p>请配置以下信息</p>
    </div>
    <form id='configForm' onsubmit='return submitConfig(event)'>
      <div class="form-group">
        <label>联网方式</label>
        <select name="net_mode" onchange="toggleNetFields()">
          <option value="wifi" )HTML") + (netMode == "wifi" ? "selected" : "") + String(R"HTML(>WiFi</option>
          <option value="ml307r" )HTML") + (netMode == "ml307r" ? "selected" : "") + String(R"HTML(>物联网模块(ML307R)</option>
        </select>
      </div>

      <div id="wifi-ssid-field" class="form-group">
        <label style='display:flex;justify-content:space-between;align-items:center;'>
          <span>WiFi 网络</span>
          <button type='button' onclick='scanWifi()' id='scanBtn' class='add-slot-btn' style='font-size:12px;padding:5px 10px;'>扫描</button>
        </label>
        <select name='ssid' id='ssidSelect'>)HTML") + options + String(R"HTML(</select>
        <div id='scanStatus' style='font-size:12px;color:#666;margin-top:5px;display:none;'></div>
      </div>
      <div id="wifi-pass-field" class="form-group">
        <label>WiFi 密码</label>
        <input type='text' name='pass' id='wifiPass' value=')HTML") + wifiPass + String(R"HTML(' placeholder='留空表示无密码'>
      </div>

      <div id="ml307-fields" style="display:none">
        <div class="form-group">
          <label>ML307 APN</label>
          <input type='text' name='ml307_apn' value=')HTML") + ml307Apn + String(R"HTML(' placeholder='cmnet'>
        </div>
        <div class="form-group">
          <label>ML307 波特率</label>
          <input type='number' name='ml307_baud' value=")HTML") + String(ml307Baud) + String(R"HTML(" min="1200" max="921600">
        </div>
      </div>
      
      <input type='hidden' name='data_mode' value='mqtt'>

      <div id="leancloud-fields" style="display:block">
        <div class="header" style="margin-top:30px;margin-bottom:15px;border-top:1px solid #eee;padding-top:20px;">
          <h1>MQTT 配置</h1>
          <p>用于数据传输</p>
        </div>
        <div class="form-group">
          <label>Broker 选择</label>
          <select id="mqtt_preset" name="mqtt_preset" onchange="toggleMqttPreset()">
            <option value="auto" )HTML") + (mqttAutoMode ? "selected" : "") + String(R"HTML(>自动（自动选择+故障切换）</option>
            <option value="emqx" )HTML") + (!mqttAutoMode && (mqttHost == "broker.emqx.io" || mqttHost.length() == 0) ? "selected" : "") + String(R"HTML(>EMQX 国际服务器</option>
            <option value="hivemq" )HTML") + (!mqttAutoMode && mqttHost == "broker.hivemq.com" ? "selected" : "") + String(R"HTML(>HiveMQ 国际服务器</option>
            <option value="custom" )HTML") + (!mqttAutoMode && mqttHost.length() > 0 && mqttHost != "broker.emqx.io" && mqttHost != "broker.hivemq.com" ? "selected" : "") + String(R"HTML(>自定义</option>
          </select>
        </div>
        <div id="mqtt_custom_fields" style="display:none;">
          <div class="form-group">
            <label>Broker Host</label>
            <input type='text' id='mqtt_host_input' name='mqtt_host' value=')HTML") + mqttHost + String(R"HTML(' placeholder='例如：broker.hivemq.com'>
          </div>
          <div class="form-group">
            <label>Broker Port</label>
            <input type='number' id='mqtt_port_input' name='mqtt_port' value=')HTML") + String(mqttPort) + String(R"HTML(' placeholder='1883'>
          </div>
          <div class="form-group">
            <label>用户名 (可选)</label>
            <input type='text' id='mqtt_user_input' name='mqtt_user' value=')HTML") + mqttUser + String(R"HTML(' placeholder='MQTT Username'>
          </div>
          <div class="form-group">
            <label>密码 (可选)</label>
            <input type='text' id='mqtt_pass_input' name='mqtt_pass' value=')HTML") + mqttPass + String(R"HTML(' placeholder='MQTT Password'>
          </div>
        </div>
        <div class="form-group">
          <label>序列号 <span style='color:red;' id='mqtt_required_hint'>*必填</span></label>
          <input type='text' id='mqtt_device_name_input' name='mqtt_device_name' value=')HTML") + mqttDeviceName + String(R"HTML(' placeholder='如: ASXD'>
          <small>填写与主机相同的序列号，C1子机需一致</small>
        </div>
      </div>

      <div class="form-group">
        <label>扫描目标类型</label>
        <select id="scan_mode_select" name="scan_mode" onchange="toggleScanTargetFields()">
          <option value="mac" )HTML") + (scanTargetMode == "mac" ? "selected" : "") + String(R"HTML(>按 MAC</option>
          <option value="uuid" )HTML") + (scanTargetMode == "uuid" ? "selected" : "") + String(R"HTML(>按 UUID</option>
        </select>
      </div>

      <div class="form-group" id="lc-target-mac-field">
        <label style='display:flex;justify-content:space-between;align-items:center;'>
          <span>目标 MAC (可选)</span>
          <button type='button' onclick='scanBle()' id='bleScanBtn' class='add-slot-btn' style='font-size:12px;padding:5px 10px;'>搜索</button>
        </label>
        <input type='text' id='lc_target_mac_input' name='lc_target_mac' value=')HTML") + lcTargetMac + String(R"HTML(' placeholder='XX:XX:XX:XX:XX:XX'>
        <div id='bleScanStatus' style='font-size:12px;color:#666;margin-top:5px;display:none;'></div>
        <div id='bleResults' style='margin-top:8px;display:none;max-height:220px;overflow:auto;border:1px solid #eee;border-radius:10px;padding:6px;background:#fafafa;'></div>
      </div>
      <div class="form-group" id="uuid-target-wrap">
        <label style='display:flex;justify-content:space-between;align-items:center;'>
          <span>目标 UUID 片段</span>
          <button type='button' onclick='scanBle("uuid")' id='bleScanBtnUuid' class='add-slot-btn' style='font-size:12px;padding:5px 10px;'>搜索</button>
        </label>
        <input type='text' id='target_uuid_input' name='target_uuid' value=')HTML") + scanTargetUuid + String(R"HTML(' placeholder='例如 180D 或 FFE0，可空格/逗号分隔'>
        <div id='bleScanStatusUuid' style='font-size:12px;color:#666;margin-top:5px;display:none;'></div>
        <div id='bleResultsUuid' style='margin-top:8px;display:none;max-height:220px;overflow:auto;border:1px solid #eee;border-radius:10px;padding:6px;background:#fafafa;'></div>
      </div>

      <div class="form-group">
        <label>扫描设置</label>
        <div style="display: flex; gap: 10px;">
          <div style="flex:1">
            <label style="font-size:12px; margin-top:0;">扫描持续时间 (秒)</label>
            <input type="number" name="scan_duration" value=")HTML" + String(scanDuration) + R"HTML(" min="1" max="60">
          </div>
          <div style="flex:1">
            <label style="font-size:12px; margin-top:0;">扫描间隔 (秒)</label>
            <input type="number" name="scan_interval" value=")HTML" + String(scanInterval) + R"HTML(" min="1" max="3600">
          </div>
        </div>
      </div>

      <div class="form-group">
        <label style='display:flex;align-items:center;cursor:pointer;'>
          <input type='checkbox' name='cloud_ctrl' value='1' style='width:18px;height:18px;margin-right:10px;cursor:pointer;' )HTML") + String(cloudControlEnabled ? "checked" : "") + String(R"HTML(>
          <span>启用云控</span>
        </label>
        <small style='color:#aaa;margin-top:4px;display:block;'>开启后可通过MQTT远程下发指令控制设备</small>
      </div>

      <div class="form-group">
        <label style='display:flex;justify-content:space-between;align-items:center;'>
          <span>睡眠时间段（最多5个）</span>
          <button type='button' onclick='addTimeSlot()' class='add-slot-btn'>+ 添加</button>
        </label>
        <div id='timeSlots' style='margin-top:10px;'></div>
        <input type='hidden' name='time_slots' id='timeSlotsData'>
      </div>
      <button type='submit' class='submit-btn' id='saveBtn'>保存配置</button>
      <div id='saveResult' style='font-size:13px;text-align:center;margin-top:10px;color:#666;'></div>
    </form>
    <a href='/update' style='display:block;text-align:center;margin-top:14px;padding:12px;background:#e8f5e9;color:#2e7d32;border-radius:10px;text-decoration:none;font-size:14px;font-weight:600;'>固件升级 (OTA)</a>
    </div>
    <script>
      let slotCount = 0;
      const savedSlots = )HTML") + savedTimeSlots + String(R"HTML(;
      
      function addTimeSlot(wakeH=6, wakeM=45, sleepH=7, sleepM=15, enabled=true) {
        if (slotCount >= 5) {
          alert('最多只能添加5个时间段');
          return;
        }
        
        // 辅助函数：格式化 HH:MM
        const fmt = (h, m) => {
          return String(h).padStart(2,'0') + ':' + String(m).padStart(2,'0');
        };
        
        const container = document.getElementById('timeSlots');
        const slot = document.createElement('div');
        slot.className = 'time-slot';
        slot.innerHTML = `
          <div class='time-slot-header'>
            <label class='time-slot-label'>
              <input type='checkbox' ${enabled?'checked':''} onchange='updateTimeSlots()'>
              <span>时间段 ${slotCount+1}</span>
            </label>
            <button type='button' onclick='this.closest(".time-slot").remove();slotCount--;updateTimeSlots();' class='delete-btn'>删除</button>
          </div>
          <div class='time-inputs'>
            <div class='time-group'>
              <label>唤醒时间</label>
              <input type='time' class='time-picker' value='${fmt(wakeH, wakeM)}' onchange='updateTimeSlots()'>
            </div>
            <div class='time-group'>
              <label>睡眠时间</label>
              <input type='time' class='time-picker' value='${fmt(sleepH, sleepM)}' onchange='updateTimeSlots()'>
            </div>
          </div>
        `;
        container.appendChild(slot);
        slotCount++;
        updateTimeSlots();
      }
      
      function updateTimeSlots() {
        const slots = document.querySelectorAll('.time-slot');
        const data = [];
        slots.forEach((slot, idx) => {
          const inputs = slot.querySelectorAll('input[type="time"]');
          const checkbox = slot.querySelector('input[type="checkbox"]');
          if (inputs.length >= 2) {
            // 更新序号
            const labelSpan = slot.querySelector('.time-slot-label span');
            if (labelSpan) labelSpan.textContent = '时间段 ' + (idx + 1);
            
            // 解析 HH:MM
            const parseTime = (str, defH, defM) => {
               if(!str) return [defH, defM];
               const parts = str.split(':');
               if(parts.length !== 2) return [defH, defM];
               return [parseInt(parts[0]), parseInt(parts[1])];
            };

            const [wh, wm] = parseTime(inputs[0].value, 6, 45);
            const [sh, sm] = parseTime(inputs[1].value, 7, 15);

            data.push({
              enabled: checkbox.checked,
              wakeH: wh,
              wakeM: wm,
              sleepH: sh,
              sleepM: sm
            });
          }
        });
        document.getElementById('timeSlotsData').value = JSON.stringify(data);
      }
      
      // 加载已保存的时间段
      if (savedSlots && savedSlots.length > 0) {
        savedSlots.forEach(slot => {
          // 使用 null/undefined 判断，允许 0 值
          const v = (val, def) => (val !== undefined && val !== null) ? val : def;
          addTimeSlot(
            v(slot.wakeH, 6), 
            v(slot.wakeM, 45), 
            v(slot.sleepH, 7), 
            v(slot.sleepM, 15), 
            slot.enabled !== false
          );
        });
      } else {
        // 默认不添加时间段，让用户自己决定
        // addTimeSlot(); 
      }
      
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

      function toggleScanTargetFields() {
        var scanModeSel = document.getElementById('scan_mode_select');
        var scanMode = scanModeSel ? scanModeSel.value : 'mac';
        var lcMacField = document.getElementById('lc-target-mac-field');
        var lcMacInput = document.getElementById('lc_target_mac_input');
        var bleBtn = document.getElementById('bleScanBtn');
        var bleStatus = document.getElementById('bleScanStatus');
        var bleResults = document.getElementById('bleResults');
        var bleBtnUuid = document.getElementById('bleScanBtnUuid');
        var bleStatusUuid = document.getElementById('bleScanStatusUuid');
        var bleResultsUuid = document.getElementById('bleResultsUuid');
        var uuidWrap = document.getElementById('uuid-target-wrap');
        if (lcMacField && lcMacInput) {
          if (scanMode === 'uuid') {
            lcMacField.style.display = 'none';
            lcMacInput.disabled = true;
            if (bleBtn) bleBtn.disabled = true;
            if (bleStatus) bleStatus.style.display = 'none';
            if (bleResults) bleResults.style.display = 'none';
          } else {
            lcMacField.style.display = 'block';
            lcMacInput.disabled = false;
            if (bleBtn) bleBtn.disabled = false;
          }
        }

        if (scanMode === 'uuid') {
          if (uuidWrap) uuidWrap.style.display = 'block';
          if (bleBtnUuid) bleBtnUuid.disabled = false;
        } else {
          if (uuidWrap) uuidWrap.style.display = 'none';
          if (bleBtnUuid) bleBtnUuid.disabled = true;
          if (bleStatusUuid) bleStatusUuid.style.display = 'none';
          if (bleResultsUuid) bleResultsUuid.style.display = 'none';
        }
      }

      function toggleNetFields() {
        var nm = document.querySelector('select[name="net_mode"]').value;
        var mf = document.getElementById('ml307-fields');
        var wf1 = document.getElementById('wifi-ssid-field');
        var wf2 = document.getElementById('wifi-pass-field');
        var ssidSel = document.querySelector('select[name="ssid"]');
        if (nm === 'ml307') {
          mf.style.display = 'block';
          if (wf1) wf1.style.display = 'none';
          if (wf2) wf2.style.display = 'none';
          if (ssidSel) ssidSel.required = false;
        } else if (nm === 'ml307r') {
          mf.style.display = 'none';
          if (wf1) wf1.style.display = 'none';
          if (wf2) wf2.style.display = 'none';
          if (ssidSel) ssidSel.required = false;
        } else {
          mf.style.display = 'none';
          if (wf1) wf1.style.display = 'block';
          if (wf2) wf2.style.display = 'block';
          if (ssidSel) ssidSel.required = true;
        }
      }
      // 信号强度图标
      function rssiIcon(rssi) {
        if (rssi >= -50) return '📶';
        if (rssi >= -60) return '📶';
        if (rssi >= -70) return '📶';
        return '📶';
      }
      function rssiText(rssi) {
        if (rssi >= -50) return '极强';
        if (rssi >= -60) return '强';
        if (rssi >= -70) return '中';
        return '弱';
      }
      
      // WiFi 扫描功能
      function scanWifi() {
        const btn = document.getElementById('scanBtn');
        const status = document.getElementById('scanStatus');
        const select = document.getElementById('ssidSelect');
        
        btn.disabled = true;
        btn.textContent = '扫描中...';
        status.style.display = 'block';
        status.textContent = '正在扫描附近的 WiFi 网络...';
        status.style.color = '#666';
        
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
              status.style.color = '#4CAF50';
            } else {
              status.textContent = '未找到 WiFi 网络';
              status.style.color = '#f44336';
            }
          })
          .catch(e => {
            status.textContent = '扫描失败: ' + e.message;
            status.style.color = '#f44336';
          })
          .finally(() => {
            btn.disabled = false;
            btn.textContent = '🔄 扫描';
            setTimeout(() => { status.style.display = 'none'; }, 3000);
          });
      }

      function scanBle(kind) {
        const scanModeSel = document.getElementById('scan_mode_select');
        const scanMode = scanModeSel ? scanModeSel.value : 'mac';
        const useUuidUi = (kind === 'uuid');

        const btn = document.getElementById(useUuidUi ? 'bleScanBtnUuid' : 'bleScanBtn');
        const status = document.getElementById(useUuidUi ? 'bleScanStatusUuid' : 'bleScanStatus');
        const resultsEl = document.getElementById(useUuidUi ? 'bleResultsUuid' : 'bleResults');

        const macInput = document.getElementById('lc_target_mac_input');
        const uuidInput = document.getElementById('target_uuid_input');
        if (!btn || !status || !resultsEl) return;
        if (!macInput || !uuidInput) return;

        btn.disabled = true;
        btn.textContent = '扫描中...';
        status.style.display = 'block';
        status.textContent = '正在扫描附近的蓝牙设备（10秒）...';
        status.style.color = '#666';
        resultsEl.style.display = 'none';
        resultsEl.innerHTML = '';

        fetch('/blescan')
          .then(r => r.json())
          .then(data => {
            const list = (data && data.devices) ? data.devices : [];
            if (!list || list.length === 0) {
              status.textContent = '未发现蓝牙设备';
              status.style.color = '#f44336';
              resultsEl.style.display = 'none';
              return;
            }

            resultsEl.style.display = 'block';
            const mkRow = (d) => {
              const row = document.createElement('div');
              row.style.padding = '10px 10px';
              row.style.cursor = 'pointer';
              row.style.borderBottom = '1px solid #eee';
              row.style.background = '#fafafa';
              row.style.marginBottom = '4px';
              row.style.borderRadius = '4px';
              
              const macText = (d && d.mac) ? d.mac : '-';
              const nameText = (d && d.name) ? d.name : '';
              const uuidText = (d && d.uuid) ? d.uuid : '';
              const allUuidsText = (d && d.allUuids) ? d.allUuids : '';
              const rawText = (d && d.raw) ? d.raw : '';
              const rssiText = (d && typeof d.rssi === 'number') ? (d.rssi + 'dBm') : '-';
              
              // 构建显示内容
              let displayName = nameText ? ('<b>' + nameText + '</b>') : '<i style=\"color:#999;\">未知设备</i>';
              let uuidDisplay = '';
              if (allUuidsText) {
                // 分割并格式化所有UUID
                const uuids = allUuidsText.split(',');
                uuidDisplay = '<span style=\"color:#2196F3;font-weight:bold;\">UUID: </span>' + uuids.map(u => '<code style=\"background:#e8f5e9;padding:1px 4px;border-radius:2px;font-size:11px;\">' + u.trim() + '</code>').join(' ');
              } else if (uuidText) {
                uuidDisplay = '<span style=\"color:#2196F3;font-weight:bold;\">UUID: </span><code style=\"background:#e8f5e9;padding:1px 4px;border-radius:2px;font-size:11px;\">' + uuidText + '</code>';
              } else {
                uuidDisplay = '<span style=\"color:#ccc;font-size:11px;\">无UUID</span>';
              }
              
              // RAW数据显示
              let rawDisplay = '';
              if (rawText) {
                rawDisplay = '<span style=\"color:#9C27B0;font-weight:bold;\">RAW: </span><code style=\"background:#f3e5f5;padding:1px 4px;border-radius:2px;font-size:10px;word-break:break-all;\">' + rawText + '</code>';
              }
              
              row.innerHTML =
                "<div style='display:flex;flex-direction:column;gap:4px;'>" +
                  "<div style='display:flex;justify-content:space-between;align-items:center;'>" +
                    "<div>" + displayName + "</div>" +
                    "<div style='color:#666;font-size:12px;'>" + rssiText + "</div>" +
                  "</div>" +
                  "<div style='font-family:monospace;font-size:12px;color:#555;'>" + macText + "</div>" +
                  "<div style='font-size:11px;word-break:break-all;'>" + uuidDisplay + "</div>" +
                  (rawDisplay ? "<div style='font-size:10px;word-break:break-all;margin-top:2px;'>" + rawDisplay + "</div>" : "") +
                "</div>";
              
              row.onmouseover = () => { row.style.background = '#e3f2fd'; };
              row.onmouseout = () => { row.style.background = '#fafafa'; };
              
              row.onclick = () => {
                if (scanMode === 'uuid') {
                  const targetUuid = uuidText || (allUuidsText ? allUuidsText.split(',')[0].trim() : '');
                  if (targetUuid) {
                    uuidInput.value = targetUuid;
                    status.textContent = '已选择 UUID: ' + targetUuid + ' (来自 ' + (nameText || macText) + ')';
                    status.style.color = '#4CAF50';
                  } else {
                    status.textContent = '该设备未携带可识别的 UUID';
                    status.style.color = '#f44336';
                  }
                } else {
                  macInput.value = d.mac;
                  status.textContent = '已选择: ' + d.mac + (nameText ? (' (' + nameText + ')') : '');
                  status.style.color = '#4CAF50';
                }
              };
              return row;
            };
            list.forEach(d => resultsEl.appendChild(mkRow(d)));

            status.textContent = '发现 ' + list.length + ' 个设备（按信号强度排序）';
            status.style.color = '#4CAF50';
          })
          .catch(e => {
            status.textContent = '扫描失败: ' + e.message;
            status.style.color = '#f44336';
            resultsEl.style.display = 'none';
          })
          .finally(() => {
            btn.disabled = false;
            btn.textContent = '🔍 搜索';
          });
      }
      
      // 页面加载时选中已保存的 SSID
      (function() {
        const savedSsid = ')HTML") + wifiSsid + String(R"HTML(';
        if (savedSsid) {
          const select = document.getElementById('ssidSelect');
          for (let i = 0; i < select.options.length; i++) {
            if (select.options[i].value === savedSsid) {
              select.selectedIndex = i;
              break;
            }
          }
        }
      })();
      
      // 初始化
      toggleMqttPreset();
      toggleScanTargetFields();
      toggleNetFields();

      // Tab切换
      function switchTab(t){
        document.getElementById('tab-config').style.display=(t==='config')?'block':'none';
        document.getElementById('tab-status').style.display=(t==='status')?'block':'none';
        document.getElementById('tabBtnConfig').className='tab'+(t==='config'?' active':'');
        document.getElementById('tabBtnStatus').className='tab'+(t==='status'?' active':'');
        if(t==='status'){updateStatusPanel();refreshLog();}
      }

      // AJAX提交配置
      function submitConfig(e){
        e.preventDefault();
        var btn=document.getElementById('saveBtn');
        var res=document.getElementById('saveResult');
        btn.disabled=true;btn.textContent='保存中...';
        res.textContent='';
        var fd=new FormData(document.getElementById('configForm'));
        var x=new XMLHttpRequest();
        x.open('POST','/save',true);
        x.timeout=10000;
        x.onload=function(){
          try{
            var d=JSON.parse(x.responseText);
            if(d.ok){
              res.style.color='#27ae60';
              res.textContent='配置已保存！正在初始化设备...';
              document.getElementById('initMsg').style.display='block';
              setTimeout(function(){switchTab('status');},800);
              if(!statusTimer) statusTimer=setInterval(updateStatusPanel,5000);
              if(!logTimer) logTimer=setInterval(refreshLog,5000);
            }else{res.style.color='#e05c65';res.textContent='保存失败';}
          }catch(ex){res.style.color='#e05c65';res.textContent='响应异常: '+x.responseText;}
          btn.disabled=false;btn.textContent='保存配置';
        };
        x.onerror=function(){res.style.color='#e05c65';res.textContent='请求失败';btn.disabled=false;btn.textContent='保存配置';};
        x.ontimeout=function(){res.style.color='#e05c65';res.textContent='请求超时';btn.disabled=false;btn.textContent='保存配置';};
        x.send(fd);
        return false;
      }

      // 状态面板轮询
      var statusTimer=null,logTimer=null;
      function updateStatusPanel(){
        var x=new XMLHttpRequest();
        x.open('GET','/api/status.json',true);x.timeout=5000;
        x.onload=function(){
          try{
            var d=JSON.parse(x.responseText);
            var e;
            e=document.getElementById('sv_mac');if(e)e.textContent=d.mac||'--';
            e=document.getElementById('sv_uptime');
            if(e&&d.uptime_ms!==undefined){
              var ms=d.uptime_ms,s=Math.floor(ms/1000)%60,m=Math.floor(ms/60000)%60,h=Math.floor(ms/3600000);
              e.textContent=h+':'+(m<10?'0':'')+m+':'+(s<10?'0':'')+s;
            }
            e=document.getElementById('sv_heap');
            if(e&&d.free_heap!==undefined&&d.heap_total){
              var kb=Math.floor(d.free_heap/1024),pct=Math.floor(d.free_heap*100/d.heap_total);
              e.textContent=kb+'KB ('+pct+'%)';
              e.className='st-val '+(pct>30?'st-ok':(pct>15?'st-warn':'st-err'));
            }
            e=document.getElementById('sv_wifi');
            if(e){
              if(d.wifi_connected){
                e.innerHTML='<span class="st-ok">已连接</span> '+d.wifi_ssid+'<br>IP: '+d.wifi_ip;
              }else if(d.wifi_ssid){
                e.innerHTML='<span class="st-warn">连接中...</span> '+d.wifi_ssid;
              }else{
                e.innerHTML='<span class="st-err">未配置</span>';
              }
            }
            e=document.getElementById('sv_mqtt');
            if(e){
              var brokerInfo=d.mqtt_auto_mode?'\u81ea\u52a8 → '+d.mqtt_host:d.mqtt_host+':'+d.mqtt_port;
              if(d.mqtt_connected){
                e.innerHTML='<span class="st-ok">\u5df2\u8fde\u63a5</span> '+brokerInfo;
              }else if(d.mqtt_host){
                e.innerHTML='<span class="st-warn">\u8fde\u63a5\u4e2d...</span> '+brokerInfo;
              }else{
                e.innerHTML='<span class="st-err">\u672a\u914d\u7f6e</span>';
              }
            }
            e=document.getElementById('sv_scan');
            if(e){
              e.textContent=(d.is_scanning?'扫描中':'等待中')+' | '+d.scan_mode+' | '+d.scan_duration+'s/'+d.scan_interval+'s';
            }
          }catch(ex){}
        };
        x.send();
      }

      function refreshLog(){
        var x=new XMLHttpRequest();
        x.open('GET','/api/log',true);
        x.onload=function(){
          var el=document.getElementById('logContent');
          el.textContent=x.responseText||'(暂无日志)';
          el.scrollTop=el.scrollHeight;
        };
        x.send();
      }
      function clearLog(){
        var x=new XMLHttpRequest();
        x.open('GET','/api/log/clear',true);
        x.onload=function(){document.getElementById('logContent').textContent='(已清空)';};
        x.send();
      }
    </script>
  </div>
</body>
</html>
)HTML");
    configServer.send(200, "text/html; charset=utf-8", page);
  });

  // WiFi 扫描 API（返回 SSID 和信号强度）
  configServer.on("/scan", HTTP_GET, []() {
    Serial.println("[AP] 扫描 WiFi 网络...");
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
    Serial.printf("[AP] 找到 %d 个网络\n", n);
    configServer.sendHeader("Access-Control-Allow-Origin", "*");
    configServer.send(200, "application/json", json);
  });

  // BLE 扫描 API（返回 MAC + RSSI + UUID + RAW，按信号强弱排序）
  // 使用回调方式在扫描过程中捕获数据，确保payload数据有效
  configServer.on("/blescan", HTTP_GET, []() {
    Serial.println("[AP] 扫描 BLE 设备 (10s)...");

    // 初始化BLE扫描
    if (pBLEScan == nullptr) {
      BLEDevice::init("ESP32-O5-Scanner");
      pBLEScan = BLEDevice::getScan();
      pBLEScan->setActiveScan(true);
      pBLEScan->setInterval(100);
      pBLEScan->setWindow(99);
    }
    
    // 创建回调（如果还没有）
    if (apBleCallbacks == nullptr) {
      apBleCallbacks = new ApBleAdvertisedDeviceCallbacks();
    }
    
    // 清空上次结果并设置回调
    apBleScanResults.clear();
    apBleScanResults.reserve(AP_BLE_SCAN_MAX);
    apBleScanActive = true;
    
    pBLEScan->setAdvertisedDeviceCallbacks(apBleCallbacks, false);
    pBLEScan->clearResults();
    
    // 执行扫描（回调会在扫描过程中收集数据）
    pBLEScan->start(10, false);
    
    apBleScanActive = false;
    pBLEScan->stop();
    pBLEScan->clearResults();
    
    // 按信号强度排序
    for (size_t i = 0; i < apBleScanResults.size(); i++) {
      size_t best = i;
      for (size_t j = i + 1; j < apBleScanResults.size(); j++) {
        if (apBleScanResults[j].rssi > apBleScanResults[best].rssi) best = j;
      }
      if (best != i) {
        ApBleScanItem tmp = apBleScanResults[i];
        apBleScanResults[i] = apBleScanResults[best];
        apBleScanResults[best] = tmp;
      }
    }
    
    // 构建JSON响应
    int outN = (int)apBleScanResults.size();
    String json = "{\"devices\":[";
    json.reserve(20 + outN * 200);
    
    for (int i = 0; i < outN; i++) {
      if (i > 0) json += ",";
      json += "{\"mac\":\"" + apBleScanResults[i].mac + "\",\"rssi\":" + String(apBleScanResults[i].rssi);
      
      if (apBleScanResults[i].name.length() > 0) {
        String escapedName = apBleScanResults[i].name;
        escapedName.replace("\\", "\\\\");
        escapedName.replace("\"", "\\\"");
        json += ",\"name\":\"" + escapedName + "\"";
      }
      if (apBleScanResults[i].uuid.length() > 0) {
        json += ",\"uuid\":\"" + apBleScanResults[i].uuid + "\"";
      }
      if (apBleScanResults[i].allUuids.length() > 0) {
        json += ",\"allUuids\":\"" + apBleScanResults[i].allUuids + "\"";
      }
      if (apBleScanResults[i].rawData.length() > 0) {
        json += ",\"raw\":\"" + apBleScanResults[i].rawData + "\"";
      }
      json += "}";
    }
    json += "]}";
    
    Serial.printf("[AP] BLE 扫描完成，发现 %d 个设备\n", outN);
    configServer.sendHeader("Access-Control-Allow-Origin", "*");
    configServer.send(200, "application/json", json);
  });

  configServer.on("/save", HTTP_POST, []() {
    String ssid = configServer.arg("ssid");
    String pass = configServer.arg("pass");
    String server = configServer.hasArg("server") ? configServer.arg("server") : SERVER_BASE;
    String timeSlotsJson = configServer.arg("time_slots");

    String net_mode = configServer.hasArg("net_mode") ? configServer.arg("net_mode") : netMode;
    String apn = configServer.hasArg("ml307_apn") ? configServer.arg("ml307_apn") : ml307Apn;
    int baud = configServer.hasArg("ml307_baud") ? configServer.arg("ml307_baud").toInt() : ml307Baud;
    
    String targetMac = configServer.hasArg("lc_target_mac") ? configServer.arg("lc_target_mac") : lcTargetMac;

    // MQTT预设处理
    String mqttPreset = configServer.hasArg("mqtt_preset") ? configServer.arg("mqtt_preset") : "emqx";
    String mHost, mUser, mPass;
    int mPort;
    if (mqttPreset == "auto") {
      mqttAutoMode = true;
      mqttAutoBrokerIdx = 0;
      mHost = "broker.emqx.io"; mPort = 1883; mUser = ""; mPass = "";
    } else {
      mqttAutoMode = false;
      if (mqttPreset == "emqx") {
        mHost = "broker.emqx.io"; mPort = 1883; mUser = ""; mPass = "";
      } else if (mqttPreset == "hivemq") {
        mHost = "broker.hivemq.com"; mPort = 1883; mUser = ""; mPass = "";
      } else {
        mHost = configServer.hasArg("mqtt_host") ? configServer.arg("mqtt_host") : mqttHost;
        mPort = configServer.hasArg("mqtt_port") ? configServer.arg("mqtt_port").toInt() : mqttPort;
        mUser = configServer.hasArg("mqtt_user") ? configServer.arg("mqtt_user") : mqttUser;
        mPass = configServer.hasArg("mqtt_pass") ? configServer.arg("mqtt_pass") : mqttPass;
      }
    }
    String mDeviceName = configServer.hasArg("mqtt_device_name") ? configServer.arg("mqtt_device_name") : mqttDeviceName;
    Serial.printf("[DEBUG] 收到mqtt_device_name参数: hasArg=%d, value='%s'\n", 
                  configServer.hasArg("mqtt_device_name"), mDeviceName.c_str());

    String scan_mode = configServer.hasArg("scan_mode") ? configServer.arg("scan_mode") : scanTargetMode;
    String target_uuid = configServer.hasArg("target_uuid") ? configServer.arg("target_uuid") : scanTargetUuid;
    
    String mode = configServer.hasArg("data_mode") ? configServer.arg("data_mode") : dataMode;
    
    int duration = configServer.hasArg("scan_duration") ? configServer.arg("scan_duration").toInt() : scanDuration;
    int interval = configServer.hasArg("scan_interval") ? configServer.arg("scan_interval").toInt() : scanInterval;

    // 云控开关（checkbox未勾选时不发送参数，所以hasArg为false即表示关闭）
    bool newCloudCtrl = configServer.hasArg("cloud_ctrl");

    if (net_mode == "wifi" && ssid.length() == 0) {
      configServer.sendHeader("Access-Control-Allow-Origin", "*");
      configServer.send(400, "text/plain", "参数无效: SSID必填");
      return;
    }

    saveConfiguration(ssid, pass, server, timeSlotsJson, mode, targetMac, duration, interval, net_mode, apn, baud, scan_mode, target_uuid,
                      mHost, mPort, mUser, mPass, mDeviceName);

    // 热更新全局变量
    wifiSsid = ssid;
    wifiPass = pass;
    SERVER_BASE = server;
    dataMode = mode;
    netMode = net_mode;
    ml307Apn = apn;
    ml307Baud = baud;
    lcTargetMac = targetMac;
    mqttHost = mHost;
    mqttPort = mPort;
    mqttUser = mUser;
    mqttPass = mPass;
    mqttDeviceName = mDeviceName;
    scanTargetMode = scan_mode;
    scanTargetUuid = target_uuid;
    scanDuration = duration;
    scanInterval = interval;
    cloudControlEnabled = newCloudCtrl;
    if (!cloudControlEnabled) cmdTopicSubscribed = false;
    preferences.begin("config", false);
    preferences.putBool("cloud_ctrl", cloudControlEnabled);
    preferences.end();

    addDebugLog("[CONFIG] 配置已保存并热更新");

    // 返回JSON成功响应，不重启
    configServer.sendHeader("Access-Control-Allow-Origin", "*");
    configServer.send(200, "application/json", "{\"ok\":true}");
    Serial.println("[AP] 配置已保存，退出配置模式，进入运行模式...");
    
    // 退出配置AP阻塞循环，让setup()继续初始化WiFi/BLE/MQTT
    isConfigured = true;
    apModeActive = false;
  });

  // 获取当前配置 JSON（供App原生UI预填）
  configServer.on("/api/config.json", HTTP_GET, []() {
    String mqttPreset = "custom";
    if (mqttHost == "broker.emqx.io" || mqttHost.length() == 0) mqttPreset = "emqx";
    else if (mqttHost == "broker.hivemq.com") mqttPreset = "hivemq";
    String json = "{";
    json += "\"ssid\":\"" + wifiSsid + "\",";
    json += "\"data_mode\":\"" + dataMode + "\",";
    json += "\"net_mode\":\"" + netMode + "\",";
    json += "\"mqtt_preset\":\"" + mqttPreset + "\",";
    json += "\"mqtt_host\":\"" + mqttHost + "\",";
    json += "\"mqtt_port\":" + String(mqttPort) + ",";
    json += "\"mqtt_user\":\"" + mqttUser + "\",";
    json += "\"mqtt_device_name\":\"" + mqttDeviceName + "\",";
    json += "\"lc_target_mac\":\"" + lcTargetMac + "\",";
    json += "\"scan_mode\":\"" + scanTargetMode + "\",";
    json += "\"target_uuid\":\"" + scanTargetUuid + "\",";
    json += "\"scan_duration\":" + String(scanDuration) + ",";
    json += "\"scan_interval\":" + String(scanInterval);
    json += "}";
    configServer.sendHeader("Access-Control-Allow-Origin", "*");
    configServer.send(200, "application/json", json);
  });

  // 状态相关API（配置页切到状态tab时使用）
  configServer.on("/api/status.json", HTTP_GET, []() {
    bool wifiOk = WiFi.status() == WL_CONNECTED;
    String json = "{";
    json += "\"device_id\":\"" + deviceId + "\",";
    json += "\"mac\":\"" + WiFi.macAddress() + "\",";
    json += "\"wifi_connected\":" + String(wifiOk ? "true" : "false") + ",";
    json += "\"wifi_ssid\":\"" + wifiSsid + "\",";
    json += "\"wifi_ip\":\"" + (wifiOk ? WiFi.localIP().toString() : String("")) + "\",";
    json += "\"wifi_rssi\":" + String(wifiOk ? WiFi.RSSI() : 0) + ",";
    json += "\"mqtt_host\":\"" + mqttHost + "\",";
    json += "\"mqtt_port\":" + String(mqttPort) + ",";
    json += "\"mqtt_auto_mode\":" + String(mqttAutoMode ? "true" : "false") + ",";
    json += "\"mqtt_device_name\":\"" + mqttDeviceName + "\",";
    json += "\"scan_mode\":\"" + scanTargetMode + "\",";
    json += "\"scan_duration\":" + String(scanDuration) + ",";
    json += "\"scan_interval\":" + String(scanInterval) + ",";
    json += "\"is_scanning\":" + String(isScanning ? "true" : "false") + ",";
    json += "\"lc_target_mac\":\"" + lcTargetMac + "\",";
    json += "\"target_uuid\":\"" + scanTargetUuid + "\",";
    json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"heap_total\":" + String(ESP.getHeapSize()) + ",";
    json += "\"net_mode\":\"" + netMode + "\",";
    json += "\"data_mode\":\"" + dataMode + "\",";
    bool mqttOk = false;
    if (dataMode == "mqtt") {
      if (netMode == "ml307r") {
        mqttOk = ml307MqttAtConnectedPlain;
      } else {
        mqttOk = getActiveMqttClient(netMode == "ml307").connected();
      }
    }
    json += "\"mqtt_connected\":" + String(mqttOk ? "true" : "false") + ",";
    json += "\"cloud_ctrl\":" + String(cloudControlEnabled ? "true" : "false") + ",";
    json += "\"uptime_ms\":" + String(millis());
    json += "}";
    configServer.sendHeader("Access-Control-Allow-Origin", "*");
    configServer.send(200, "application/json", json);
  });

  configServer.on("/api/log", HTTP_GET, []() {
    configServer.sendHeader("Access-Control-Allow-Origin", "*");
    configServer.send(200, "text/plain; charset=utf-8", debugLog);
  });

  configServer.on("/api/log/clear", HTTP_GET, []() {
    debugLog = "";
    configServer.sendHeader("Access-Control-Allow-Origin", "*");
    configServer.send(200, "text/plain", "ok");
  });

  // OTA 升级页面
  configServer.on("/update", HTTP_GET, []() {
    String html = R"HTML(<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1.0'>
<title>固件升级</title>
<style>
*{margin:0;padding:0;box-sizing:border-box;}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:#f0f2f5;display:flex;justify-content:center;align-items:flex-start;min-height:100vh;padding:20px;}
.card{background:#fff;border-radius:12px;padding:24px;max-width:420px;width:100%;box-shadow:0 1px 6px rgba(0,0,0,.07);}
h2{font-size:17px;color:#222;margin-bottom:14px;}
.upload-area{border:2px dashed #ddd;border-radius:10px;padding:30px 20px;text-align:center;margin-bottom:14px;cursor:pointer;transition:border-color .2s;}
.upload-area:hover{border-color:#4a90e2;}
.upload-area input{display:none;}
.upload-area .label{color:#888;font-size:13px;}
.upload-area .fname{color:#333;font-size:14px;font-weight:600;margin-top:8px;}
.bar-wrap{background:#f0f0f0;border-radius:8px;height:8px;margin-bottom:10px;overflow:hidden;display:none;}
.bar{background:#4a90e2;height:100%;width:0%;transition:width .3s;border-radius:8px;}
.msg{display:none;padding:12px;border-radius:8px;font-size:13px;text-align:center;margin-bottom:10px;}
.ok{background:#e8f5e9;color:#2e7d32;}.err{background:#fdecea;color:#c0392b;}
button{width:100%;padding:13px;background:#4a90e2;color:#fff;border:none;border-radius:10px;font-size:15px;font-weight:600;cursor:pointer;}
button:disabled{background:#ccc;cursor:not-allowed;}
.back{display:block;text-align:center;margin-top:14px;color:#4a90e2;font-size:13px;text-decoration:none;}
</style></head><body>
<div class='card'>
<h2>固件升级 (OTA)</h2>
<div class='upload-area' onclick='document.getElementById("fw").click()'>
<input type='file' id='fw' accept='.bin,.enc' onchange='pick(this)'>
<div class='label'>点击选择固件文件 (.bin / .bin.enc)</div>
<div class='fname' id='fn'></div>
</div>
<div class='bar-wrap' id='bw'><div class='bar' id='bar'></div></div>
<div class='msg' id='msg'></div>
<button id='sb' onclick='upload()' disabled>开始升级</button>
<a href='/' class='back'>返回配置页</a>
</div>
<script>
var file=null;
function pick(inp){file=inp.files[0];document.getElementById('fn').textContent=file?file.name:'';document.getElementById('sb').disabled=!file;}
function upload(){
  if(!file)return;
  var sb=document.getElementById('sb'),bw=document.getElementById('bw'),bar=document.getElementById('bar'),msg=document.getElementById('msg');
  sb.disabled=true;sb.textContent='上传中...';bw.style.display='block';msg.style.display='none';bar.style.width='0%';
  var fd=new FormData();fd.append('firmware',file,file.name);
  var xhr=new XMLHttpRequest();
  xhr.upload.onprogress=function(e){if(e.lengthComputable){var p=Math.round(e.loaded*100/e.total);bar.style.width=p+'%';sb.textContent='上传中 '+p+'%';}};
  xhr.onload=function(){
    msg.style.display='block';
    if(xhr.status==200){msg.className='msg ok';msg.textContent='升级成功！设备正在重启...';sb.textContent='完成';}
    else{msg.className='msg err';msg.textContent='升级失败: '+xhr.responseText;sb.disabled=false;sb.textContent='重试';}
  };
  xhr.onerror=function(){msg.style.display='block';msg.className='msg err';msg.textContent='网络错误';sb.disabled=false;sb.textContent='重试';};
  xhr.open('POST','/update');xhr.send(fd);
}
</script></div></body></html>)HTML";
    configServer.send(200, "text/html; charset=utf-8", html);
  });

  // OTA 上传处理（同步WebServer版本）
  configServer.on("/update", HTTP_POST, []() {
    bool ok = !Update.hasError();
    configServer.sendHeader("Connection", "close");
    configServer.sendHeader("Access-Control-Allow-Origin", "*");
    configServer.send(ok ? 200 : 500, "text/plain", ok ? "OK" : Update.errorString());
    if (ok) {
      Serial.println("[OTA] Success, restarting...");
      delay(1500);
      esp_restart();
    }
  }, []() {
    HTTPUpload& upload = configServer.upload();

    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("[OTA] Start: %s\n", upload.filename.c_str());
      esp_task_wdt_delete(NULL);
      otaEncrypted = false;
      otaOriginalSize = 0;
      otaHeaderParsed = 0;
      otaTotalWritten = 0;
      otaAesBufLen = 0;
    }
    else if (upload.status == UPLOAD_FILE_WRITE) {
      uint8_t* data = upload.buf;
      size_t len = upload.currentSize;

      // 前8字节: 判断加密头
      if (otaHeaderParsed < 8) {
        size_t need = 8 - otaHeaderParsed;
        size_t copyLen = (len < need) ? len : need;
        memcpy(otaHeaderBuf + otaHeaderParsed, data, copyLen);
        otaHeaderParsed += copyLen;
        data += copyLen;
        len -= copyLen;

        if (otaHeaderParsed == 8) {
          if (memcmp(otaHeaderBuf, OTA_MAGIC, 4) == 0) {
            memcpy(&otaOriginalSize, otaHeaderBuf + 4, 4);
            otaEncrypted = true;
            Serial.printf("[OTA] Encrypted firmware, original=%u\n", otaOriginalSize);
            mbedtls_aes_init(&otaAesCtx);
            mbedtls_aes_setkey_dec(&otaAesCtx, OTA_AES_KEY, 128);
            if (!Update.begin(otaOriginalSize, U_FLASH)) {
              Update.printError(Serial);
              return;
            }
          } else {
            Serial.println("[OTA] Plain firmware");
            if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
              Update.printError(Serial);
              return;
            }
            if (Update.write(otaHeaderBuf, 8) != 8) {
              Update.printError(Serial);
              return;
            }
          }
        } else {
          return;
        }
      }

      if (Update.hasError() || len == 0) return;

      if (otaEncrypted) {
        uint8_t decBuf[16];
        size_t offset = 0;

        // 先拼接上一轮残余字节
        if (otaAesBufLen > 0 && len > 0) {
          size_t need = 16 - otaAesBufLen;
          if (len >= need) {
            memcpy(otaAesBuf + otaAesBufLen, data, need);
            mbedtls_aes_crypt_ecb(&otaAesCtx, MBEDTLS_AES_DECRYPT, otaAesBuf, decBuf);
            size_t w = 16;
            if (otaTotalWritten + w > otaOriginalSize) w = otaOriginalSize - otaTotalWritten;
            if (w > 0) { if (Update.write(decBuf, w) != w) { Update.printError(Serial); return; } otaTotalWritten += w; }
            offset = need;
            otaAesBufLen = 0;
          } else {
            memcpy(otaAesBuf + otaAesBufLen, data, len);
            otaAesBufLen += len;
            return;
          }
        }

        // 处理完整16字节块
        while (offset + 16 <= len) {
          mbedtls_aes_crypt_ecb(&otaAesCtx, MBEDTLS_AES_DECRYPT, data + offset, decBuf);
          size_t w = 16;
          if (otaTotalWritten + w > otaOriginalSize) w = otaOriginalSize - otaTotalWritten;
          if (w > 0) { if (Update.write(decBuf, w) != w) { Update.printError(Serial); return; } otaTotalWritten += w; }
          offset += 16;
        }

        // 保存不足16字节的残余
        if (offset < len) {
          otaAesBufLen = len - offset;
          memcpy(otaAesBuf, data + offset, otaAesBufLen);
        }
      } else {
        if (Update.write(data, len) != len) {
          Update.printError(Serial);
        }
      }
    }
    else if (upload.status == UPLOAD_FILE_END) {
      if (otaEncrypted) {
        mbedtls_aes_free(&otaAesCtx);
        // 去掉PKCS7 padding: 用 otaOriginalSize 截断
        // Update.end(true) 的 true = evenIfRemaining，允许写入不足
      }
      if (Update.end(true)) {
        Serial.printf("[OTA] Done: %u bytes\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
      esp_task_wdt_add(NULL);
    }
  });

  configServer.onNotFound(handleNotFound);
  configServer.begin();
  apModeActive = true;
}

void stopConfigAP() {
  if (!apModeActive) return;
  dnsServer.stop();
  configServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apModeActive = false;
  setStatusLed(false);  // 关闭LED
  Serial.println("[AP] 配置完成，AP和DNS已关闭");
}

// ========== 状态AP功能 ==========
void startStatusAP() {
  if (statusApActive) return;
  
  statusApSsid = "O5-Setup-" + chipSerialNumber.substring(chipSerialNumber.length() - 6);
  
  if (configApTransition) {
    // 从配置AP过渡：AP已在运行，只需加上STA能力
    configApTransition = false;
    WiFi.mode(WIFI_AP_STA);  // 平滑添加STA，不重启AP
    Serial.printf("[STATUS-AP] 复用配置热点: %s\n", statusApSsid.c_str());
  } else {
    // 正常启动：创建热点
    if (netMode == "wifi") {
      WiFi.mode(WIFI_AP_STA);
    }
    WiFi.softAPConfig(AP_LOCAL_IP, AP_LOCAL_IP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(statusApSsid.c_str(), "12345678");
    Serial.printf("[STATUS-AP] 热点已启动: %s (密码: 12345678)\n", statusApSsid.c_str());
  }
  
  Serial.printf("[STATUS-AP] 访问 http://192.168.4.1 查看配置和状态\n");
  
  // 配置页面路由（复用配置页，含运行状态）
  configServer.on("/", HTTP_GET, handleStatusPage);
  configServer.on("/reset", HTTP_GET, handleStatusReset);
  configServer.on("/set-scan", HTTP_GET, handleStatusSetScan);

  // WiFi 扫描 API（供配置页使用）
  configServer.on("/scan", HTTP_GET, []() {
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
    configServer.sendHeader("Access-Control-Allow-Origin", "*");
    configServer.send(200, "application/json", json);
  });

  // App原生UI动作接口
  configServer.on("/restart", HTTP_GET, []() {
    configServer.sendHeader("Access-Control-Allow-Origin", "*");
    configServer.send(200, "application/json", "{\"ok\":true}");
    delay(500);
    esp_restart();
  });
  configServer.on("/reinit", HTTP_GET, []() {
    configServer.sendHeader("Access-Control-Allow-Origin", "*");
    configServer.send(200, "application/json", "{\"ok\":true}");
    delay(500);
    factoryReset();
  });

  // 状态 JSON（供App原生UI轮询）
  configServer.on("/api/status.json", HTTP_GET, []() {
    bool wifiOk = WiFi.status() == WL_CONNECTED;
    String json = "{";
    json += "\"device_id\":\"" + deviceId + "\",";
    json += "\"mac\":\"" + WiFi.macAddress() + "\",";
    json += "\"data_mode\":\"" + dataMode + "\",";
    json += "\"net_mode\":\"" + netMode + "\",";
    json += "\"wifi_connected\":" + String(wifiOk ? "true" : "false") + ",";
    json += "\"wifi_ssid\":\"" + wifiSsid + "\",";
    json += "\"wifi_ip\":\"" + (wifiOk ? WiFi.localIP().toString() : String("")) + "\",";
    json += "\"wifi_rssi\":" + String(wifiOk ? WiFi.RSSI() : 0) + ",";
    json += "\"mqtt_host\":\"" + mqttHost + "\",";
    json += "\"mqtt_port\":" + String(mqttPort) + ",";
    json += "\"mqtt_auto_mode\":" + String(mqttAutoMode ? "true" : "false") + ",";
    json += "\"mqtt_device_name\":\"" + mqttDeviceName + "\",";
    json += "\"scan_mode\":\"" + scanTargetMode + "\",";
    json += "\"scan_duration\":" + String(scanDuration) + ",";
    json += "\"scan_interval\":" + String(scanInterval) + ",";
    json += "\"is_scanning\":" + String(isScanning ? "true" : "false") + ",";
    String safeFoundMac = "";
    if (dataMutex != NULL && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      safeFoundMac = lastFoundMac;
      xSemaphoreGive(dataMutex);
    } else {
      safeFoundMac = lastFoundMac; // mutex不可用时直接读（降级）
    }
    json += "\"last_found_mac\":\"" + safeFoundMac + "\",";
    long lastFoundAgoS = lastTargetFoundTime > 0 ? (long)((millis() - lastTargetFoundTime) / 1000) : -1;
    json += "\"last_found_ago_s\":" + String(lastFoundAgoS) + ",";
    bool mqttOk = false;
    if (dataMode == "mqtt") {
      if (netMode == "ml307r") {
        mqttOk = ml307MqttAtConnectedPlain;
      } else {
        mqttOk = getActiveMqttClient(netMode == "ml307").connected();
      }
    }
    json += "\"mqtt_connected\":" + String(mqttOk ? "true" : "false") + ",";
    json += "\"cloud_ctrl\":" + String(cloudControlEnabled ? "true" : "false") + ",";
    json += "\"lc_target_mac\":\"" + lcTargetMac + "\",";
    json += "\"target_uuid\":\"" + scanTargetUuid + "\",";
    json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"heap_total\":" + String(ESP.getHeapSize()) + ",";
    setupTimeZone();
    time_t nowT; time(&nowT);
    struct tm ti; getBeijingTime(nowT, ti);
    char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &ti);
    json += "\"time_str\":\"" + String(ts) + "\",";
    json += "\"uptime_ms\":" + String(millis());
    json += "}";
    configServer.sendHeader("Access-Control-Allow-Origin", "*");
    configServer.send(200, "application/json", json);
  });

  // 运行日志接口（供状态页AJAX获取）
  configServer.on("/api/log", HTTP_GET, []() {
    configServer.sendHeader("Access-Control-Allow-Origin", "*");
    configServer.send(200, "text/plain; charset=utf-8", debugLog);
  });

  // 清空日志接口
  configServer.on("/api/log/clear", HTTP_GET, []() {
    debugLog = "";
    configServer.sendHeader("Access-Control-Allow-Origin", "*");
    configServer.send(200, "application/json", "{\"ok\":true}");
  });

  // 网络诊断接口
  configServer.on("/api/diag", HTTP_GET, []() {
    String json = "{";
    addDebugLog("[DIAG] 开始网络诊断...");

    bool isMl307 = (netMode == "ml307" || netMode == "ml307r");
    json += "\"net_mode\":\"" + netMode + "\"";

    // 1. 网络状态
    bool netOk = false;
    if (!isMl307) {
      netOk = (WiFi.status() == WL_CONNECTED);
      json += ",\"wifi_ok\":" + String(netOk ? "true" : "false");
      if (netOk) {
        json += ",\"wifi_ip\":\"" + WiFi.localIP().toString() + "\"";
        json += ",\"wifi_gw\":\"" + WiFi.gatewayIP().toString() + "\"";
        json += ",\"wifi_rssi\":" + String(WiFi.RSSI());
      }
    } else {
      // ML307模式：轻量级AT检测（避免长时间阻塞Web请求）
      String atResp;
      netOk = ml307SendAT("AT", "OK", nullptr, 500, &atResp);
      if (netOk) {
        // 快速检查网络附着
        String attResp;
        ml307SendAT("AT+CGATT?", "+CGATT:", nullptr, 1000, &attResp);
        netOk = (attResp.indexOf("+CGATT: 1") >= 0);
      }
      json += ",\"wifi_ok\":" + String(netOk ? "true" : "false");
      json += ",\"is_4g\":true";
      // 信号质量 CSQ
      String csqResp;
      if (ml307SendAT("AT+CSQ", "+CSQ:", nullptr, 800, &csqResp)) {
        int ci = csqResp.indexOf("+CSQ:");
        if (ci >= 0) {
          String csqLine = csqResp.substring(ci + 5);
          csqLine.trim();
          int comma = csqLine.indexOf(',');
          if (comma > 0) {
            int csq = csqLine.substring(0, comma).toInt();
            int dbm = (csq == 99) ? 0 : (-113 + 2 * csq);
            json += ",\"wifi_rssi\":" + String(dbm);
            json += ",\"csq\":" + String(csq);
          }
        }
      }
      // IP地址
      String ipResp;
      if (ml307SendAT("AT+CGPADDR=1", "+CGPADDR:", nullptr, 2000, &ipResp)) {
        int pi = ipResp.indexOf("+CGPADDR:");
        if (pi >= 0) {
          String ipLine = ipResp.substring(pi);
          int q1 = ipLine.indexOf('"');
          int q2 = (q1 >= 0) ? ipLine.indexOf('"', q1 + 1) : -1;
          if (q1 >= 0 && q2 > q1) {
            json += ",\"wifi_ip\":\"" + ipLine.substring(q1 + 1, q2) + "\"";
          }
        }
      }
      addDebugLog("[DIAG] 4G网络 " + String(netOk ? "正常" : "异常"));
    }

    // 2. DNS 解析测试
    bool dnsOk = false;
    String dnsTarget = mqttHost.length() > 0 ? mqttHost : "baidu.com";
    if (!isMl307 && netOk) {
      IPAddress resolved;
      unsigned long dnsStart = millis();
      dnsOk = WiFi.hostByName(dnsTarget.c_str(), resolved);
      unsigned long dnsMs = millis() - dnsStart;
      json += ",\"dns_target\":\"" + dnsTarget + "\"";
      json += ",\"dns_ok\":" + String(dnsOk ? "true" : "false");
      if (dnsOk) json += ",\"dns_ip\":\"" + resolved.toString() + "\"";
      json += ",\"dns_ms\":" + String(dnsMs);
      addDebugLog("[DIAG] DNS " + dnsTarget + " → " + (dnsOk ? resolved.toString() : "失败") + " (" + String(dnsMs) + "ms)");
    } else if (isMl307 && netOk) {
      // ML307通过AT+MDNSGIP解析
      String resolvedIp;
      unsigned long dnsStart = millis();
      dnsOk = ml307ResolveHostToIp(dnsTarget, resolvedIp);
      unsigned long dnsMs = millis() - dnsStart;
      json += ",\"dns_target\":\"" + dnsTarget + "\"";
      json += ",\"dns_ok\":" + String(dnsOk ? "true" : "false");
      if (dnsOk) json += ",\"dns_ip\":\"" + resolvedIp + "\"";
      json += ",\"dns_ms\":" + String(dnsMs);
      addDebugLog("[DIAG] DNS(4G) " + dnsTarget + " → " + (dnsOk ? resolvedIp : "失败") + " (" + String(dnsMs) + "ms)");
    } else {
      json += ",\"dns_ok\":false,\"dns_target\":\"" + dnsTarget + "\"";
    }

    // 3. TCP 连接测试
    bool tcpOk = false;
    unsigned long tcpMs = 0;
    if (!isMl307 && netOk && mqttHost.length() > 0) {
      WiFiClient testClient;
      unsigned long tcpStart = millis();
      tcpOk = testClient.connect(mqttHost.c_str(), mqttPort, 5000);
      tcpMs = millis() - tcpStart;
      if (tcpOk) testClient.stop();
      json += ",\"tcp_target\":\"" + mqttHost + ":" + String(mqttPort) + "\"";
      json += ",\"tcp_ok\":" + String(tcpOk ? "true" : "false");
      json += ",\"tcp_ms\":" + String(tcpMs);
      addDebugLog("[DIAG] TCP " + mqttHost + ":" + String(mqttPort) + " → " + (tcpOk ? "成功" : "失败") + " (" + String(tcpMs) + "ms)");
    } else if (isMl307) {
      // ML307: TCP由模块内部管理，通过MQTT连接间接验证
      json += ",\"tcp_ok\":\"inferred\"";
      json += ",\"tcp_target\":\"" + mqttHost + ":" + String(mqttPort) + "\"";
    } else {
      json += ",\"tcp_ok\":false";
    }

    // 4. MQTT 连接状态
    bool mqttOk = false;
    if (dataMode == "mqtt") {
      if (netMode == "ml307r") {
        mqttOk = ml307MqttAtConnectedPlain;
      } else {
        mqttOk = getActiveMqttClient(netMode == "ml307").connected();
      }
    }
    json += ",\"mqtt_connected\":" + String(mqttOk ? "true" : "false");

    // 5. MQTT 发布测试
    bool pubOk = false;
    if (mqttOk) {
      String testTopic = "o5/" + (mqttDeviceName.length() > 0 ? mqttDeviceName : WiFi.macAddress()) + "/diag";
      String testPayload = "{\"test\":true,\"ts\":" + String(millis()) + "}";
      if (netMode == "ml307r") {
        pubOk = ml307MqttAtPublishPlain(testTopic, testPayload, false);
      } else {
        PubSubClient& mc = getActiveMqttClient(netMode == "ml307");
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
    configServer.sendHeader("Access-Control-Allow-Origin", "*");
    configServer.send(200, "application/json", json);
  });

  // 不重启保存WiFi/MQTT配置接口
  configServer.on("/api/save-config", HTTP_GET, []() {
    configServer.sendHeader("Access-Control-Allow-Origin", "*");
    String result = "{";
    bool wifiChanged = false;
    bool mqttChanged = false;

    // 读取参数
    String newSsid = configServer.arg("wifi_ssid");
    String newPass = configServer.arg("wifi_pass");
    String newMqttHost = configServer.arg("mqtt_host");
    String newMqttPortStr = configServer.arg("mqtt_port");
    String newMqttUser = configServer.arg("mqtt_user");
    String newMqttPass = configServer.arg("mqtt_pass");
    String newMqttDname = configServer.arg("mqtt_dname");
    String newScanMode = configServer.arg("scan_mode");
    String newScanTarget = configServer.arg("scan_target");
    String newScanDurStr = configServer.arg("scan_duration");
    String newScanIntStr = configServer.arg("scan_interval");

    // 逐字段更新：只有非空值才覆盖，避免空填写清掉已有配置
    preferences.begin("config", false);

    // WiFi
    if (newSsid.length() > 0 && newSsid != wifiSsid) {
      wifiSsid = newSsid; preferences.putString("ssid", newSsid); wifiChanged = true;
    }
    if (newPass.length() > 0 && newPass != wifiPass) {
      wifiPass = newPass; preferences.putString("pass", newPass); wifiChanged = true;
    }

    // MQTT — 每个字段独立判断
    if (newMqttHost.length() > 0 && newMqttHost != mqttHost) {
      mqttHost = newMqttHost; preferences.putString("mqtt_host", newMqttHost); mqttChanged = true;
    }
    if (newMqttPortStr.length() > 0) {
      int np = newMqttPortStr.toInt();
      if (np > 0 && np != mqttPort) { mqttPort = np; preferences.putInt("mqtt_port", np); mqttChanged = true; }
    }
    if (newMqttUser.length() > 0 && newMqttUser != mqttUser) {
      mqttUser = newMqttUser; preferences.putString("mqtt_user", newMqttUser); mqttChanged = true;
    }
    if (newMqttPass.length() > 0 && newMqttPass != mqttPass) {
      mqttPass = newMqttPass; preferences.putString("mqtt_pass", newMqttPass); mqttChanged = true;
    }
    if (newMqttDname.length() > 0 && newMqttDname != mqttDeviceName) {
      mqttDeviceName = newMqttDname; preferences.putString("mqtt_dname", newMqttDname); mqttChanged = true;
    }

    // 扫描配置
    if (newScanMode.length() > 0 && newScanMode != scanTargetMode) {
      preferences.putString("scan_mode", newScanMode);
    }
    if (newScanTarget.length() > 0) {
      String mode = newScanMode.length() > 0 ? newScanMode : scanTargetMode;
      if (mode == "uuid") { preferences.putString("target_uuid", newScanTarget); }
      else { preferences.putString("lc_target_mac", newScanTarget); }
    }
    if (newScanDurStr.length() > 0) { int v = newScanDurStr.toInt(); if (v > 0) preferences.putInt("scan_duration", v); }
    if (newScanIntStr.length() > 0) { int v = newScanIntStr.toInt(); if (v > 0) preferences.putInt("scan_interval", v); }
    preferences.end();

    // 热更新网络连接
    if (wifiChanged) {
      addDebugLog("[CONFIG] WiFi配置已更新: " + wifiSsid);
      // 不调用WiFi.disconnect()，直接begin()覆盖旧配置，避免破坏AP
      WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    }
    if (mqttChanged) {
      addDebugLog("[CONFIG] MQTT配置已更新: " + mqttHost + ":" + String(mqttPort) + " name=" + mqttDeviceName);
      // 断开旧MQTT连接，重置退避，下一次mqttLoop会自动重连
      if (netMode != "ml307r") {
        PubSubClient& c = getActiveMqttClient(netMode == "ml307");
        if (c.connected()) c.disconnect();
      }
      mqttConnectFailCount = 0;
      mqttReconnectInterval = 5000;
      lastMqttConnectAttempt = 0;
    }
    // 热更新扫描配置（仅非空字段）
    if (newScanMode.length() > 0 && newScanMode != scanTargetMode) {
      scanTargetMode = newScanMode;
      addDebugLog("[CONFIG] 扫描模式已更新: " + scanTargetMode);
    }
    if (newScanTarget.length() > 0) {
      String mode = newScanMode.length() > 0 ? newScanMode : scanTargetMode;
      if (mode == "uuid") {
        scanTargetUuid = newScanTarget;
        targetUuids.clear();
        String s = newScanTarget; s.replace(",", " ");
        int start = 0;
        while (start < (int)s.length()) {
          while (start < (int)s.length() && s[start] == ' ') start++;
          if (start >= (int)s.length()) break;
          int end = start;
          while (end < (int)s.length() && s[end] != ' ') end++;
          String tok = s.substring(start, end);
          tok = normalizeUuidToken(tok);
          if (tok.length() > 0) targetUuids.push_back(tok);
          start = end + 1;
        }
        addDebugLog("[CONFIG] UUID目标已更新: " + newScanTarget);
      } else {
        lcTargetMac = newScanTarget;
        targetDevices.clear();
        if (lcTargetMac.length() > 0) {
          TargetDevice td;
          td.mac = lcTargetMac;
          td.mac.toUpperCase();
          td.enabled = true;
          targetDevices.push_back(td);
        }
        addDebugLog("[CONFIG] MAC目标已更新: " + lcTargetMac);
      }
    }
    if (newScanDurStr.length() > 0) { int v = newScanDurStr.toInt(); if (v > 0) scanDuration = v; }
    if (newScanIntStr.length() > 0) { int v = newScanIntStr.toInt(); if (v > 0) scanInterval = v; }

    result += "\"ok\":true";
    result += ",\"wifi_changed\":" + String(wifiChanged ? "true" : "false");
    result += ",\"mqtt_changed\":" + String(mqttChanged ? "true" : "false");
    result += "}";
    configServer.send(200, "application/json", result);
  });

  // 云控开关接口
  configServer.on("/api/cloud-ctrl", HTTP_GET, []() {
    configServer.sendHeader("Access-Control-Allow-Origin", "*");
    String val = configServer.hasArg("on") ? configServer.arg("on") : "";
    if (val == "1") {
      cloudControlEnabled = true;
    } else if (val == "0") {
      cloudControlEnabled = false;
      cmdTopicSubscribed = false;
    } else {
      cloudControlEnabled = !cloudControlEnabled;
      if (!cloudControlEnabled) cmdTopicSubscribed = false;
    }
    preferences.begin("config", false);
    preferences.putBool("cloud_ctrl", cloudControlEnabled);
    preferences.end();
    Serial.printf("[CLOUD] 云控已%s\n", cloudControlEnabled ? "开启" : "关闭");
    addDebugLog(String("[CLOUD] 云控已") + (cloudControlEnabled ? "开启" : "关闭"));
    configServer.send(200, "application/json",
      "{\"ok\":true,\"enabled\":" + String(cloudControlEnabled ? "true" : "false") + "}");
  });

  // 静默模式开关接口
  configServer.on("/api/silent", HTTP_GET, []() {
    configServer.sendHeader("Access-Control-Allow-Origin", "*");
    silentMode = true;
    preferences.begin("config", false);
    preferences.putBool("silent", true);
    preferences.end();
    addDebugLog("[SILENT] 静默模式已开启，LED和配置热点将在重启后关闭");
    configServer.send(200, "application/json", "{\"ok\":true,\"msg\":\"silent mode enabled, rebooting...\"}");
    delay(500);
    esp_restart();
  });

  configServer.onNotFound([]() {
    configServer.sendHeader("Location", "/", true);
    configServer.send(302, "text/plain", "");
  });
  configServer.begin();
  
  statusApActive = true;
}

void stopStatusAP() {
  if (!statusApActive) return;
  configServer.stop();
  WiFi.softAPdisconnect(true);
  statusApActive = false;
}

void handleStatusPage() {
  // 强制确保时区为北京时间
  setupTimeZone();

  // 获取当前时间
  time_t now;
  time(&now);
  struct tm timeinfo;
  getBeijingTime(now, timeinfo);
  char timeStr[32];
  strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
  
  // 获取运行时间
  unsigned long runMs = millis();
  int runSec = (runMs / 1000) % 60;
  int runMin = (runMs / 60000) % 60;
  int runHour = runMs / 3600000;
  int runDay = runHour / 24;
  runHour = runHour % 24;
  
  // 设备信息
  String deviceMac = WiFi.macAddress();
  uint32_t chipId = (uint32_t)ESP.getEfuseMac();
  String chipIdStr = String(chipId, HEX);
  chipIdStr.toUpperCase();
  
  // 内存信息
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t totalHeap = ESP.getHeapSize();
  int heapPercent = (freeHeap * 100) / totalHeap;
  String heapStatus = (heapPercent > 30) ? "status-ok" : ((heapPercent > 15) ? "status-warn" : "status-err");
  
  // 网络连接状态
  String connStatus = "";
  String connDetail = "";
  String connIcon = "";
  int wifiRssi = 0;
  String rssiBar = "";
  
  if (netMode == "wifi") {
    if (WiFi.status() == WL_CONNECTED) {
      connIcon = "✅";
      connStatus = "WiFi 已连接";
      wifiRssi = WiFi.RSSI();
      // RSSI信号条
      int bars = (wifiRssi >= -50) ? 4 : ((wifiRssi >= -60) ? 3 : ((wifiRssi >= -70) ? 2 : 1));
      for (int i = 0; i < 4; i++) {
        rssiBar += (i < bars) ? "▓" : "░";
      }
      connDetail = "SSID: <b>" + wifiSsid + "</b><br>IP: " + WiFi.localIP().toString() + "<br>信号: " + rssiBar + " " + String(wifiRssi) + "dBm";
    } else {
      connIcon = "❌";
      connStatus = "WiFi 未连接";
      connDetail = "SSID: " + wifiSsid + "<br><span class='status-err'>正在尝试重连...</span>";
    }
  } else {
    connIcon = "📡";
    connStatus = (netMode == "ml307r") ? "ML307R 模块" : "ML307 模块";
    connDetail = (netMode != "ml307r") ? "APN: " + ml307Apn : "";
  }
  
  // MQTT状态
  String mqttStatus = "";
  String mqttDetail = "";
  if (dataMode == "mqtt") {
    // 获取当前活动的MQTT客户端来检查连接状态
    bool mqttConnected = false;
    if (netMode == "ml307r") {
      mqttConnected = ml307MqttAtConnectedPlain;
    } else {
      PubSubClient& mqttC = getActiveMqttClient(netMode == "ml307");
      mqttConnected = mqttC.connected();
    }
    mqttStatus = mqttConnected ? "<span class='status-ok'>已连接</span>" : "<span class='status-err'>未连接</span>";
    mqttDetail = "服务器: " + mqttHost + ":" + String(mqttPort);
    if (mqttDeviceName.length() > 0) {
      mqttDetail += "<br>设备名: <b>" + mqttDeviceName + "</b>";
    }
    mqttDetail += "<br>主题: <code>o5/" + (mqttDeviceName.length() > 0 ? mqttDeviceName : deviceMac) + "/data</code>";
  }
  
  // 扫描模式和目标
  String scanModeInfo = (scanTargetMode == "uuid") ? "UUID" : "MAC";
  String targetInfo = "";
  int targetCount = 0;
  if (scanTargetMode == "uuid") {
    if (!targetUuids.empty()) {
      targetCount = targetUuids.size();
      for (size_t i = 0; i < targetUuids.size() && i < 3; i++) {
        if (i > 0) targetInfo += ", ";
        targetInfo += targetUuids[i];
      }
      if (targetUuids.size() > 3) targetInfo += " ...";
    } else if (scanTargetUuid.length() > 0) {
      targetInfo = scanTargetUuid;
      targetCount = 1;
    } else {
      targetInfo = "<span class='status-warn'>未配置</span>";
    }
  } else {
    if (!targetDevices.empty()) {
      targetCount = targetDevices.size();
      for (size_t i = 0; i < targetDevices.size() && i < 3; i++) {
        if (i > 0) targetInfo += "<br>";
        targetInfo += targetDevices[i].mac;
      }
      if (targetDevices.size() > 3) targetInfo += "<br>...";
    } else if (lcTargetMac.length() > 0) {
      targetInfo = lcTargetMac;
      targetCount = 1;
    } else {
      targetInfo = "<span class='status-warn'>未配置</span>";
    }
  }
  
  // 上次发现目标的时间
  String lastFoundInfo = "";
  String lastFoundClass = "";
  if (lastTargetFoundTime == 0) {
    lastFoundInfo = "尚未发现";
    lastFoundClass = "status-warn";
  } else {
    unsigned long elapsed = (millis() - lastTargetFoundTime) / 1000;
    if (elapsed < 60) {
      lastFoundInfo = String(elapsed) + " 秒前";
      lastFoundClass = "status-ok";
    } else if (elapsed < 3600) {
      lastFoundInfo = String(elapsed / 60) + " 分 " + String(elapsed % 60) + " 秒前";
      lastFoundClass = (elapsed < 300) ? "status-ok" : "";
    } else {
      lastFoundInfo = String(elapsed / 3600) + " 小时 " + String((elapsed % 3600) / 60) + " 分前";
      lastFoundClass = "status-warn";
    }
  }
  
  // 扫描设置
  String scanSettingInfo = "扫描 " + String(scanDuration) + "秒 / 间隔 " + String(scanInterval) + "秒";
  
  // 最近发现的设备列表
  String recentDevicesHtml = "";
  int deviceCount = 0;
  for (const auto& dev : scannedDevices) {
    if (deviceCount >= 5) break;
    unsigned long age = (millis() - dev.lastSeen) / 1000;
    String ageStr = (age < 60) ? String(age) + "秒前" : String(age/60) + "分前";
    String nameDisplay = (dev.name.length() > 0 && dev.name != "未知设备") ? dev.name : "";
    recentDevicesHtml += "<div class='device-item'>";
    recentDevicesHtml += "<div class='device-mac'>" + dev.mac;
    if (nameDisplay.length() > 0) recentDevicesHtml += " <small>(" + nameDisplay + ")</small>";
    recentDevicesHtml += "</div>";
    recentDevicesHtml += "<div class='device-info'>" + String(dev.rssi) + "dBm · " + ageStr + "</div>";
    recentDevicesHtml += "</div>";
    deviceCount++;
  }
  if (deviceCount == 0) {
    recentDevicesHtml = "<div style='color:#666;text-align:center;padding:10px;'>暂无设备</div>";
  }
  
  // 数据模式（固定MQTT）
  
  // 构建页面
  String page = R"HTML(
<!DOCTYPE html>
<html lang='zh-CN'>
<head>
  <meta charset='utf-8'>
  <meta name='viewport' content='width=device-width,initial-scale=1.0'>
  <title>O5 管理</title>
  <style>
    *{box-sizing:border-box;margin:0;padding:0;}
    body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Arial,sans-serif;background:#f0f2f5;padding:16px;color:#333;}
    .wrap{max-width:520px;margin:0 auto;}
    h1{font-size:17px;font-weight:700;color:#222;margin-bottom:14px;letter-spacing:.3px;}
    .card{background:#fff;border-radius:12px;box-shadow:0 1px 6px rgba(0,0,0,.07);margin-bottom:12px;overflow:hidden;}
    .sec{font-size:11px;color:#999;padding:8px 14px;font-weight:600;letter-spacing:.5px;background:#fafafa;border-bottom:1px solid #f0f0f0;}
    .row{display:flex;align-items:flex-start;padding:11px 14px;border-bottom:1px solid #f0f0f0;}
    .row:last-child{border-bottom:none;}
    .lbl{width:34%;font-size:13px;color:#666;flex-shrink:0;padding-top:2px;font-weight:500;}
    .val{flex:1;font-size:14px;color:#222;word-break:break-all;line-height:1.6;}
    .status-ok{color:#27ae60;font-weight:700;}
    .status-err{color:#e05c65;font-weight:700;}
    .status-warn{color:#e67e22;font-weight:700;}
    .acts{display:flex;gap:10px;margin-top:6px;}
    .btn{flex:1;display:block;padding:14px 0;text-align:center;text-decoration:none;border-radius:10px;font-size:15px;font-weight:600;}
    .btn-r{background:#fdecea;color:#c0392b;}
    .ft{text-align:center;font-size:12px;color:#aaa;margin-top:14px;}
    .device-item{display:flex;justify-content:space-between;align-items:center;padding:9px 14px;border-bottom:1px solid #f8f8f8;}
    .device-item:last-child{border-bottom:none;}
    .device-mac{font-family:monospace;color:#222;font-size:12px;}
    .device-mac small{color:#888;}
    .device-info{color:#999;font-size:12px;}
    .tabs{display:flex;margin-bottom:14px;background:#fff;border-radius:10px;box-shadow:0 1px 4px rgba(0,0,0,.06);overflow:hidden;}
    .tab{flex:1;padding:12px 0;text-align:center;font-size:14px;font-weight:600;border:none;background:#fff;color:#888;cursor:pointer;transition:all .2s;}
    .tab.active{background:#4a90e2;color:#fff;}
    .cfg-label{font-size:12px;color:#666;font-weight:500;margin-bottom:5px;display:block;}
    .cfg-input{width:100%;padding:9px 11px;border:1px solid #ddd;border-radius:8px;font-size:13px;margin-bottom:8px;box-sizing:border-box;background:#fff;color:#333;}
    .cfg-input:focus{outline:none;border-color:#4a90e2;}
    input:checked+span{background:#4a90e2!important;}
    input:checked+span+span{transform:translateX(20px);}
    .cfg-row{display:flex;gap:8px;margin-bottom:8px;}
    .cfg-row .cfg-input{margin-bottom:0;}
    .cfg-sec{font-size:11px;color:#999;font-weight:600;margin:12px 0 8px;padding-top:10px;border-top:1px solid #f0f0f0;}
    .cfg-sec:first-child{margin-top:0;padding-top:0;border-top:none;}
  </style>
</head>
<body>
  <div class='wrap'>
    <h1>O5 设备管理</h1>
    <div class='tabs'>
      <button class='tab active' id='tabBtnStatus' onclick='switchTab("status")'>状态</button>
      <button class='tab' id='tabBtnConfig' onclick='switchTab("config")'>配置</button>
    </div>
    <div id='tab-status'>
    <div class='card'>
      <div class='row'><span class='lbl'>MAC</span><span class='val'>)HTML" + deviceMac + R"HTML(</span></div>
      <div class='row'><span class='lbl'>芯片 ID</span><span class='val'>)HTML" + chipIdStr + R"HTML(</span></div>
      <div class='row'><span class='lbl'>时间</span><span class='val' id='v_time'>)HTML" + String(timeStr) + R"HTML(</span></div>
      <div class='row'><span class='lbl'>运行时间</span><span class='val' id='v_uptime'>)HTML" + (runDay > 0 ? String(runDay) + "天 " : "") + String(runHour) + ":" + (runMin < 10 ? "0" : "") + String(runMin) + ":" + (runSec < 10 ? "0" : "") + String(runSec) + R"HTML(</span></div>
      <div class='row'><span class='lbl'>空闲内存</span><span id='v_heap' class='val )HTML" + heapStatus + R"HTML('>)HTML" + String(freeHeap/1024) + R"HTML( KB ()HTML" + String(heapPercent) + R"HTML(%)</span></div>
    </div>
    <div class='card'>
      <div class='row'><span class='lbl'>网络</span><span class='val' id='v_net'>)HTML" + connStatus + "<br>" + connDetail + R"HTML(</span></div>
      <div class='row'><span class='lbl'>MQTT</span><span class='val' id='v_mqtt'>)HTML" + mqttStatus + "<br>" + mqttDetail + R"HTML(</span></div>
      <div class='row'><span class='lbl'>云控</span><span class='val'><label style='position:relative;display:inline-block;width:46px;height:26px;'><input type='checkbox' id='ccToggle' onchange='toggleCC()' )HTML" + String(cloudControlEnabled ? "checked" : "") + R"HTML( style='opacity:0;width:0;height:0;'><span style='position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#ccc;border-radius:26px;transition:.3s;'></span><span style='position:absolute;content:"";height:20px;width:20px;left:3px;bottom:3px;background:#fff;border-radius:50%;transition:.3s;'></span></label><span id='ccLabel' style='margin-left:8px;font-size:13px;color:#666;'>)HTML" + String(cloudControlEnabled ? "已开启" : "已关闭") + R"HTML(</span></span></div>
    </div>
    <div class='card'>
      <div id='v_lastfound' class='sec )HTML" + lastFoundClass + R"HTML('>最近发现 · )HTML" + lastFoundInfo + R"HTML(</div>
      )HTML" + recentDevicesHtml + R"HTML(    </div>
    <div class='card'>
      <div class='sec' style='display:flex;justify-content:space-between;align-items:center;'>
        <span>网络诊断</span>
        <button id='diagBtn' onclick='runDiag()' style='padding:4px 12px;border:1px solid #4a90e2;border-radius:6px;background:#fff;color:#4a90e2;font-size:11px;font-weight:600;cursor:pointer;'>开始诊断</button>
      </div>
      <div id='diagResult' style='display:none;'>
        <div class='row'><span class='lbl'>)HTML" + String(netMode == "wifi" ? "WiFi" : "4G") + R"HTML(</span><span class='val' id='dg_wifi'>--</span></div>
        <div class='row'><span class='lbl'>DNS</span><span class='val' id='dg_dns'>--</span></div>
        <div class='row'><span class='lbl'>TCP</span><span class='val' id='dg_tcp'>--</span></div>
        <div class='row'><span class='lbl'>MQTT</span><span class='val' id='dg_mqtt'>--</span></div>
        <div class='row'><span class='lbl'>发布</span><span class='val' id='dg_pub'>--</span></div>
      </div>
    </div>
    <div class='card'>
      <div class='sec' style='cursor:pointer;user-select:none;' onclick='toggleLog()'>
        运行日志 <span id='logArrow'>▶</span>
      </div>
      <div id='logPanel' style='display:none;'>
        <div style='display:flex;gap:6px;padding:8px 14px;'>
          <button onclick='refreshLog()' style='flex:1;padding:7px 0;border:1px solid #ddd;border-radius:6px;background:#fff;font-size:12px;cursor:pointer;'>刷新日志</button>
          <button onclick='clearLog()' style='flex:1;padding:7px 0;border:1px solid #ddd;border-radius:6px;background:#fff;font-size:12px;cursor:pointer;color:#c0392b;'>清空日志</button>
        </div>
        <pre id='logContent' style='margin:0;padding:10px 14px;font-size:11px;color:#444;background:#fafafa;max-height:300px;overflow-y:auto;white-space:pre-wrap;word-break:break-all;border-top:1px solid #f0f0f0;'>点击"刷新日志"加载...</pre>
      </div>
    </div>
    <div class='acts'>
      <a href='/reset' class='btn btn-r' onclick='return confirm("确定要重新初始化设备吗？\n这将清除所有配置并重启！")'>重新初始化</a>
    </div>
    </div>
    <div id='tab-config' style='display:none;'>
    <div class='card'>
      <div style='padding:14px;'>
        )HTML" + (netMode == "wifi" ? String(R"HTML(
        <div class='cfg-sec' style='margin-top:0;padding-top:0;border-top:none;'>WiFi 配置</div>
        <div class='cfg-row'>
          <input id='cf_ssid' type='text' class='cfg-input' style='margin-bottom:0;' placeholder='WiFi SSID'>
          <button onclick='doScanWifi()' id='wfScanBtn' style='padding:9px 14px;border:none;border-radius:8px;background:#4a90e2;color:#fff;font-size:12px;font-weight:600;cursor:pointer;white-space:nowrap;'>扫描</button>
        </div>
        <div id='wfScanList' style='display:none;max-height:150px;overflow-y:auto;border:1px solid #eee;border-radius:8px;margin-bottom:8px;background:#fafafa;'></div>
        <input id='cf_pass' type='password' class='cfg-input' placeholder='WiFi 密码'>
        )HTML") : String(R"HTML(
        <div class='cfg-sec' style='margin-top:0;padding-top:0;border-top:none;color:#999;'>)HTML") + netMode + String(R"HTML( 模式（网络由模块管理）</div>
        )HTML")) + R"HTML(
        <div class='cfg-sec'>设备序列号</div>
        <input id='cf_mdname' type='text' class='cfg-input' placeholder='设备序列号'>
        <div class='cfg-sec'>MQTT 服务器</div>
        <select id='cf_mpreset' class='cfg-input' onchange='toggleMqttCustom()'>
          <option value='auto'>自动（自动选择+故障切换）</option>
          <option value='emqx'>EMQX 国际服务器</option>
          <option value='hivemq'>HiveMQ 国际服务器</option>
          <option value='custom'>自定义</option>
        </select>
        <div id='mqttCustomFields' style='display:none;'>
          <input id='cf_mhost' type='text' class='cfg-input' placeholder='MQTT Host'>
          <div class='cfg-row'>
            <input id='cf_mport' type='number' class='cfg-input' style='width:100px;flex:none;margin-bottom:0;' placeholder='端口'>
            <input id='cf_muser' type='text' class='cfg-input' style='margin-bottom:0;' placeholder='用户名 (可选)'>
          </div>
          <input id='cf_mpass' type='password' class='cfg-input' placeholder='密码 (可选)'>
        </div>
        <div class='cfg-sec'>扫描配置</div>
        <div class='cfg-row'>
          <select id='cf_scanmode' class='cfg-input' style='flex:1;margin-bottom:0;'>
            <option value='mac'>按 MAC</option>
            <option value='uuid'>按 UUID</option>
          </select>
          <input id='cf_scantarget' type='text' class='cfg-input' style='flex:2;margin-bottom:0;' placeholder='目标 MAC 或 UUID'>
        </div>
        <div class='cfg-row'>
          <div style='flex:1;'><span class='cfg-label'>扫描时长(秒)</span><input id='cf_scandur' type='number' class='cfg-input' style='margin-bottom:0;'></div>
          <div style='flex:1;'><span class='cfg-label'>扫描间隔(秒)</span><input id='cf_scanint' type='number' class='cfg-input' style='margin-bottom:0;'></div>
        </div>
        <button onclick='saveConfig()' style='width:100%;padding:12px;border:none;border-radius:10px;background:#4a90e2;color:#fff;font-size:15px;font-weight:600;cursor:pointer;margin-top:14px;'>保存并应用（免重启）</button>
        <div id='cfgResult' style='font-size:12px;margin-top:8px;text-align:center;color:#666;'></div>
      </div>
    </div>
    <div class='acts'>
      <a href='/restart' class='btn' style='background:#e8f4fd;color:#2980b9;' onclick='return confirm("确定要重启设备吗？")'>重启设备</a>
      <a href='javascript:void(0)' class='btn' style='background:#f0f0f0;color:#666;' onclick='enableSilent()'>开启静默模式</a>
      <a href='/reset' class='btn btn-r' onclick='return confirm("确定要恢复出厂设置吗？\n这将清除所有配置！")'>恢复出厂</a>
    </div>
    </div>
    <p class='ft'>@北晨科技</p>
  </div>
  <script>
  function toggleCC(){
    var cb=document.getElementById('ccToggle');
    var lb=document.getElementById('ccLabel');
    lb.textContent='切换中...';
    var x=new XMLHttpRequest();
    x.open('GET','/api/cloud-ctrl?on='+(cb.checked?'1':'0'),true);
    x.timeout=5000;
    x.onload=function(){
      try{var r=JSON.parse(x.responseText);lb.textContent=r.enabled?'已开启':'已关闭';cb.checked=r.enabled;}catch(e){lb.textContent='错误';}
    };
    x.onerror=x.ontimeout=function(){lb.textContent='请求失败';cb.checked=!cb.checked;};
    x.send();
  }
  var _netMode=')HTML" + netMode + R"HTML(';
  function enableSilent(){
    if(!confirm('确定开启静默模式？\n\n开启后将：\n• 永久关闭指示灯\n• 永久关闭配置WiFi热点\n• 仅可通过长按初始化按钮5秒恢复\n\n设备将立即重启！')) return;
    var x=new XMLHttpRequest();
    x.open('GET','/api/silent',true);x.timeout=5000;
    x.onload=function(){document.body.innerHTML='<div style="text-align:center;padding:60px;font-size:16px;color:#666;">静默模式已开启，设备正在重启...</div>';};
    x.onerror=x.ontimeout=function(){alert('请求失败');};
    x.send();
  }
  var logOpen=false;
  function toggleLog(){
    logOpen=!logOpen;
    document.getElementById('logPanel').style.display=logOpen?'block':'none';
    document.getElementById('logArrow').textContent=logOpen?'▼':'▶';
    if(logOpen) refreshLog();
  }
  function refreshLog(){
    var x=new XMLHttpRequest();
    x.open('GET','/api/log',true);
    x.onload=function(){
      var el=document.getElementById('logContent');
      el.textContent=x.responseText||'(暂无日志)';
      el.scrollTop=el.scrollHeight;
    };
    x.send();
  }
  function clearLog(){
    var x=new XMLHttpRequest();
    x.open('GET','/api/log/clear',true);
    x.onload=function(){document.getElementById('logContent').textContent='(已清空)';};
    x.send();
  }
  function runDiag(){
    var btn=document.getElementById('diagBtn');
    var panel=document.getElementById('diagResult');
    btn.disabled=true;btn.textContent='诊断中...';btn.style.color='#999';
    panel.style.display='block';
    ['dg_wifi','dg_dns','dg_tcp','dg_mqtt','dg_pub'].forEach(function(id){document.getElementById(id).innerHTML='<span style="color:#999">测试中...</span>';});
    var x=new XMLHttpRequest();
    x.open('GET','/api/diag',true);x.timeout=20000;
    x.onload=function(){
      try{
        var d=JSON.parse(x.responseText);
        var e;
        var is4g=d.is_4g||false;
        e=document.getElementById('dg_wifi');
        if(d.wifi_ok){
          if(is4g){
            var sig=d.wifi_rssi?(' 信号:'+d.wifi_rssi+'dBm'+(d.csq!==undefined?' CSQ:'+d.csq:'')):'';
            e.innerHTML='<span class="status-ok">4G已连接</span>'+(d.wifi_ip?' IP:'+d.wifi_ip:'')+sig;
          }else{
            e.innerHTML='<span class="status-ok">已连接</span> IP:'+d.wifi_ip+' GW:'+d.wifi_gw+' '+d.wifi_rssi+'dBm';
          }
        }else{
          e.innerHTML='<span class="status-err">'+(is4g?'4G网络异常':'未连接')+'</span>';
        }
        e=document.getElementById('dg_dns');
        if(d.dns_ok){
          e.innerHTML='<span class="status-ok">成功</span> '+d.dns_target+' → '+d.dns_ip+' <small>('+d.dns_ms+'ms)</small>';
        }else{
          e.innerHTML='<span class="status-err">失败</span> '+(d.dns_target||'');
        }
        e=document.getElementById('dg_tcp');
        if(d.tcp_ok==='inferred'){
          e.innerHTML='<span style="color:#999">由模块内部管理 '+(d.tcp_target||'')+'</span>';
        }else if(d.tcp_ok){
          e.innerHTML='<span class="status-ok">成功</span> '+(d.tcp_target||'')+' <small>('+d.tcp_ms+'ms)</small>';
        }else{
          e.innerHTML='<span class="status-err">失败</span> '+(d.tcp_target||'未配置');
        }
        e=document.getElementById('dg_mqtt');
        e.innerHTML=d.mqtt_connected?'<span class="status-ok">已连接</span>':'<span class="status-err">未连接</span>';
        e=document.getElementById('dg_pub');
        if(d.pub_ok){
          e.innerHTML='<span class="status-ok">成功</span> → <code style="font-size:11px">'+d.pub_topic+'</code>';
        }else if(d.mqtt_connected){
          e.innerHTML='<span class="status-err">发布失败</span>';
        }else{
          e.innerHTML='<span class="status-warn">跳过（MQTT未连接）</span>';
        }
      }catch(ex){
        document.getElementById('dg_wifi').innerHTML='<span class="status-err">解析失败</span>';
      }
      btn.disabled=false;btn.textContent='开始诊断';btn.style.color='#4a90e2';
    };
    x.onerror=x.ontimeout=function(){
      document.getElementById('dg_wifi').innerHTML='<span class="status-err">请求失败/超时</span>';
      btn.disabled=false;btn.textContent='开始诊断';btn.style.color='#4a90e2';
    };
    x.send();
  }
  var cfgLoaded=false;
  function switchTab(t){
    document.getElementById('tab-status').style.display=(t==='status')?'block':'none';
    document.getElementById('tab-config').style.display=(t==='config')?'block':'none';
    document.getElementById('tabBtnStatus').className='tab'+(t==='status'?' active':'');
    document.getElementById('tabBtnConfig').className='tab'+(t==='config'?' active':'');
    if(t==='config'&&!cfgLoaded){cfgLoaded=true;loadConfigValues();}
  }
  function toggleMqttCustom(){
    var sel=document.getElementById('cf_mpreset').value;
    document.getElementById('mqttCustomFields').style.display=(sel==='custom')?'block':'none';
  }
  function loadConfigValues(){
    var x=new XMLHttpRequest();
    x.open('GET','/api/status.json',true);
    x.onload=function(){
      try{
        var d=JSON.parse(x.responseText);
        if(_netMode==='wifi'){
          var el=document.getElementById('cf_ssid');
          if(el)el.value=d.wifi_ssid||'';
        }
        document.getElementById('cf_mdname').value=d.mqtt_device_name||'';
        var h=d.mqtt_host||'';
        var preset='custom';
        if(d.mqtt_auto_mode)preset='auto';
        else if(h==='broker.emqx.io'||h==='')preset='emqx';
        else if(h==='broker.hivemq.com')preset='hivemq';
        document.getElementById('cf_mpreset').value=preset;
        toggleMqttCustom();
        if(preset==='custom'){
          document.getElementById('cf_mhost').value=h;
          document.getElementById('cf_mport').value=d.mqtt_port||'';
        }
        document.getElementById('cf_scanmode').value=d.scan_mode||'mac';
        document.getElementById('cf_scantarget').value=(d.scan_mode==='uuid'?(d.target_uuid||''):(d.lc_target_mac||''));
        document.getElementById('cf_scandur').value=d.scan_duration||3;
        document.getElementById('cf_scanint').value=d.scan_interval||5;
      }catch(e){}
    };
    x.send();
  }
  function doScanWifi(){
    var btn=document.getElementById('wfScanBtn');
    var list=document.getElementById('wfScanList');
    btn.disabled=true;btn.textContent='扫描中...';
    list.style.display='none';list.innerHTML='';
    var x=new XMLHttpRequest();
    x.open('GET','/scan',true);x.timeout=15000;
    x.onload=function(){
      try{
        var d=JSON.parse(x.responseText);
        if(d.networks&&d.networks.length>0){
          list.style.display='block';
          d.networks.forEach(function(n){
            var row=document.createElement('div');
            row.style.cssText='padding:8px 10px;cursor:pointer;border-bottom:1px solid #eee;font-size:13px;';
            row.textContent=n.ssid+' ('+n.rssi+'dBm)';
            row.onmouseover=function(){row.style.background='#e3f2fd';};
            row.onmouseout=function(){row.style.background='';};
            row.onclick=function(){document.getElementById('cf_ssid').value=n.ssid;list.style.display='none';};
            list.appendChild(row);
          });
        }
      }catch(e){}
      btn.disabled=false;btn.textContent='扫描';
    };
    x.onerror=x.ontimeout=function(){btn.disabled=false;btn.textContent='扫描';};
    x.send();
  }
  function saveConfig(){
    var p='';
    if(_netMode==='wifi'){
      var ssidEl=document.getElementById('cf_ssid');
      var passEl=document.getElementById('cf_pass');
      p='wifi_ssid='+encodeURIComponent(ssidEl?ssidEl.value:'');
      p+='&wifi_pass='+encodeURIComponent(passEl?passEl.value:'');
    }else{
      p='wifi_ssid=&wifi_pass=';
    }
    var preset=document.getElementById('cf_mpreset').value;
    if(preset==='emqx'){
      p+='&mqtt_host=broker.emqx.io&mqtt_port=1883&mqtt_user=&mqtt_pass=';
    }else if(preset==='hivemq'){
      p+='&mqtt_host=broker.hivemq.com&mqtt_port=1883&mqtt_user=&mqtt_pass=';
    }else if(preset!=='auto'){
      p+='&mqtt_host='+encodeURIComponent(document.getElementById('cf_mhost').value);
      p+='&mqtt_port='+encodeURIComponent(document.getElementById('cf_mport').value);
      p+='&mqtt_user='+encodeURIComponent(document.getElementById('cf_muser').value);
      p+='&mqtt_pass='+encodeURIComponent(document.getElementById('cf_mpass').value);
    }
    p+='&mqtt_dname='+encodeURIComponent(document.getElementById('cf_mdname').value);
    p+='&scan_mode='+encodeURIComponent(document.getElementById('cf_scanmode').value);
    p+='&scan_target='+encodeURIComponent(document.getElementById('cf_scantarget').value);
    p+='&scan_duration='+encodeURIComponent(document.getElementById('cf_scandur').value);
    p+='&scan_interval='+encodeURIComponent(document.getElementById('cf_scanint').value);
    var el=document.getElementById('cfgResult');
    el.textContent='正在保存...';el.style.color='#666';
    var x=new XMLHttpRequest();
    x.open('GET','/api/save-config?'+p,true);
    x.timeout=8000;
    x.onload=function(){
      try{
        var d=JSON.parse(x.responseText);
        if(d.ok){
          var msg='已保存并应用';
          if(d.wifi_changed)msg+=', WiFi正在重连';
          if(d.mqtt_changed)msg+=', MQTT正在重连';
          el.textContent=msg;el.style.color='#27ae60';
        }else{el.textContent='保存失败';el.style.color='#e05c65';}
      }catch(e){el.textContent='响应异常';el.style.color='#e05c65';}
    };
    x.onerror=function(){el.textContent='请求失败';el.style.color='#e05c65';};
    x.ontimeout=function(){el.textContent='请求超时';el.style.color='#e05c65';};
    x.send();
  }
  function updateStatus(){
    var x=new XMLHttpRequest();
    x.open('GET','/api/status.json',true);
    x.timeout=6000;
    x.onload=function(){
      try{
        var d=JSON.parse(x.responseText);
        var e;
        e=document.getElementById('v_time');
        if(e&&d.time_str)e.textContent=d.time_str;
        e=document.getElementById('v_uptime');
        if(e&&d.uptime_ms!==undefined){
          var ms=d.uptime_ms,s=Math.floor(ms/1000)%60,m=Math.floor(ms/60000)%60,h=Math.floor(ms/3600000),dy=Math.floor(h/24);h=h%24;
          e.textContent=(dy>0?dy+'天 ':'')+h+':'+(m<10?'0':'')+m+':'+(s<10?'0':'')+s;
        }
        e=document.getElementById('v_heap');
        if(e&&d.free_heap!==undefined&&d.heap_total){
          var kb=Math.floor(d.free_heap/1024),pct=Math.floor(d.free_heap*100/d.heap_total);
          e.textContent=kb+' KB ('+pct+'%)';
          e.className='val '+(pct>30?'status-ok':(pct>15?'status-warn':'status-err'));
        }
        e=document.getElementById('v_net');
        if(e){
          if(d.net_mode==='wifi'){
            if(d.wifi_connected){
              var r=d.wifi_rssi||0,bn=(r>=-50)?4:((r>=-60)?3:((r>=-70)?2:1)),bar='';
              for(var i=0;i<4;i++)bar+=(i<bn)?'\u2593':'\u2591';
              e.innerHTML='WiFi \u5df2\u8fde\u63a5<br>SSID: <b>'+d.wifi_ssid+'</b><br>IP: '+d.wifi_ip+'<br>\u4fe1\u53f7: '+bar+' '+r+'dBm';
            }else{
              e.innerHTML='WiFi \u672a\u8fde\u63a5<br>SSID: '+d.wifi_ssid+'<br><span class="status-err">\u6b63\u5728\u5c1d\u8bd5\u91cd\u8fde...</span>';
            }
          }else{
            var mLabel=(d.net_mode==='ml307r'?'ML307R':'ML307');
            e.innerHTML=mLabel+' \u6a21\u5757'+(d.mqtt_connected?' <span class="status-ok">\u5df2\u8fde\u63a5</span>':' <span class="status-err">\u672a\u8fde\u63a5</span>');
          }
        }
        e=document.getElementById('v_mqtt');
        if(e&&d.data_mode==='mqtt'){
          var st=d.mqtt_connected?'<span class="status-ok">\u5df2\u8fde\u63a5</span>':'<span class="status-err">\u672a\u8fde\u63a5</span>';
          var md=d.mqtt_auto_mode?'\u6a21\u5f0f: \u81ea\u52a8\u5207\u6362<br>\u5f53\u524d: '+d.mqtt_host+':'+d.mqtt_port:'\u670d\u52a1\u5668: '+d.mqtt_host+':'+d.mqtt_port;
          if(d.mqtt_device_name)md+='<br>\u8bbe\u5907\u540d: <b>'+d.mqtt_device_name+'</b>';
          e.innerHTML=st+'<br>'+md;
        }
        e=document.getElementById('v_lastfound');
        if(e){
          var ago=d.last_found_ago_s,str,cls;
          if(ago<0){str='\u5c1a\u672a\u53d1\u73b0';cls='status-warn';}
          else if(ago<60){str=ago+' \u79d2\u524d';cls='status-ok';}
          else if(ago<3600){str=Math.floor(ago/60)+' \u5206 '+(ago%60)+' \u79d2\u524d';cls=(ago<300)?'status-ok':'';}
          else{str=Math.floor(ago/3600)+' \u5c0f\u65f6 '+Math.floor((ago%3600)/60)+' \u5206\u524d';cls='status-warn';}
          e.className='sec '+cls;
          e.textContent='\u6700\u8fd1\u53d1\u73b0 \u00b7 '+str;
        }
      }catch(ex){}
    };
    x.send();
  }
  setInterval(updateStatus,8000);
  setInterval(function(){if(logOpen)refreshLog();},5000);
  </script>
</body>
</html>
)HTML";

  configServer.send(200, "text/html; charset=utf-8", page);
}

void handleStatusReset() {
  String page = R"HTML(
<!DOCTYPE html>
<html lang='zh-CN'>
<head>
  <meta charset='utf-8'>
  <meta name='viewport' content='width=device-width,initial-scale=1'>
  <title>重新初始化</title>
  <style>
    *{box-sizing:border-box;margin:0;padding:0;}
    body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Arial,sans-serif;background:#f0f2f5;padding:60px 20px;text-align:center;}
    .card{background:#fff;border-radius:12px;box-shadow:0 1px 6px rgba(0,0,0,.07);padding:32px 24px;max-width:360px;margin:0 auto;}
    h2{font-size:17px;color:#c0392b;margin-bottom:10px;font-weight:600;}
    p{font-size:14px;color:#999;line-height:1.6;}
  </style>
</head>
<body>
  <div class='card'>
    <h2>正在重新初始化...</h2>
    <p>设备将重启并进入配置模式</p>
  </div>
</body>
</html>
)HTML";
  
  configServer.send(200, "text/html; charset=utf-8", page);
  delay(1000);
  
  // 执行恢复出厂设置
  factoryReset();
}

void handleStatusSetScan() {
  String mode = configServer.hasArg("mode") ? configServer.arg("mode") : scanTargetMode;
  String uuid = configServer.hasArg("uuid") ? configServer.arg("uuid") : scanTargetUuid;
  String mac = configServer.hasArg("mac") ? configServer.arg("mac") : "";

  if (mode != "uuid") mode = "mac";
  scanTargetMode = mode;
  scanTargetUuid = uuid;

  // 清除旧的目标列表
  targetUuids.clear();
  
  if (scanTargetMode == "uuid") {
    // UUID模式：清除MAC目标列表，解析UUID列表
    targetDevices.clear();
    scannedDevices.clear();
    Serial.println("[STATUS] 切换到UUID模式，清除MAC目标列表");
    
    String s = scanTargetUuid;
    s.replace(",", " ");
    int start = 0;
    while (start < (int)s.length()) {
      while (start < (int)s.length() && (s[start] == ' ' || s[start] == '\t')) start++;
      if (start >= (int)s.length()) break;
      int end = start;
      while (end < (int)s.length() && s[end] != ' ' && s[end] != '\t') end++;
      String tok = s.substring(start, end);
      tok = normalizeUuidToken(tok);
      if (tok.length() > 0) targetUuids.push_back(tok);
      start = end + 1;
    }
    Serial.printf("[STATUS] UUID目标数量: %d\n", (int)targetUuids.size());
  } else {
    // MAC模式：如果提供了新的MAC，更新目标列表
    if (mac.length() > 0) {
      targetDevices.clear();
      scannedDevices.clear();
      Serial.printf("[STATUS] 切换到MAC模式，设置目标: %s\n", mac.c_str());
      
      // 解析MAC地址（支持多个，用空格或逗号分隔）
      mac.replace(",", " ");
      int start = 0;
      while (start < (int)mac.length()) {
        while (start < (int)mac.length() && (mac[start] == ' ' || mac[start] == '\t')) start++;
        if (start >= (int)mac.length()) break;
        int end = start;
        while (end < (int)mac.length() && mac[end] != ' ' && mac[end] != '\t') end++;
        String tok = mac.substring(start, end);
        tok.trim();
        tok.toUpperCase();
        if (tok.length() >= 12) {
          // 添加冒号格式化
          if (tok.length() == 12 && tok.indexOf(':') < 0) {
            String formatted = "";
            for (int i = 0; i < 12; i += 2) {
              if (i > 0) formatted += ":";
              formatted += tok.substring(i, i + 2);
            }
            tok = formatted;
          }
          addTargetDevice(tok, "状态页设置");
        }
        start = end + 1;
      }
    }
    Serial.printf("[STATUS] MAC目标数量: %d\n", (int)targetDevices.size());
  }

  preferences.begin("config", false);
  preferences.putString("scan_mode", scanTargetMode);
  preferences.putString("target_uuid", scanTargetUuid);
  preferences.end();

  String page = R"HTML(
<!DOCTYPE html>
<html lang='zh-CN'>
<head>
  <meta charset='utf-8'>
  <meta http-equiv='refresh' content='1;url=/'>
  <meta name='viewport' content='width=device-width,initial-scale=1'>
  <title>已应用</title>
  <style>*{box-sizing:border-box;margin:0;padding:0;}body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Arial,sans-serif;background:#f0f2f5;padding:60px 20px;text-align:center;}p{font-size:15px;color:#666;}</style>
</head>
<body>
  <p>已应用，正在返回...</p>
</body>
</html>
)HTML";
  configServer.send(200, "text/html; charset=utf-8", page);
}

void handleRoot() {
  configServer.send(200, "text/plain", "O5 Config");
}

void handleNotFound() {
  Serial.printf("[AP] 捕获未匹配请求，重定向到配置页面\n");
  configServer.sendHeader("Location", "/", true);
  configServer.send(302, "text/plain", "");
}

void beginStationWithCurrentConfig() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
}

String getDeviceIdString() {
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(buf);
}

bool parseUrl(const String& url, bool& isHttps, String& host, int& port, String& path) {
  String u = url;
  isHttps = false;
  port = 80;
  if (u.startsWith("https://")) {
    isHttps = true;
    port = 443;
    u = u.substring(8);
  } else if (u.startsWith("http://")) {
    u = u.substring(7);
  } else {
    return false;
  }

  int slash = u.indexOf('/');
  String hostPort = (slash >= 0) ? u.substring(0, slash) : u;
  path = (slash >= 0) ? u.substring(slash) : "/";

  int colon = hostPort.indexOf(':');
  if (colon >= 0) {
    host = hostPort.substring(0, colon);
    port = hostPort.substring(colon + 1).toInt();
  } else {
    host = hostPort;
  }
  return host.length() > 0;
}

bool httpRequest(const String& method, const String& url, const String& contentType, const String& payload, int& outCode, String& outBody, const String& extraHeaders) {
  if (netMode == "ml307" || netMode == "ml307r") {
    return httpRequestMl307(method, url, contentType, payload, outCode, outBody, extraHeaders);
  }
  return httpRequestWiFi(method, url, contentType, payload, outCode, outBody, extraHeaders);
}

bool httpRequestWiFi(const String& method, const String& url, const String& contentType, const String& payload, int& outCode, String& outBody, const String& extraHeaders) {
  if (WiFi.status() != WL_CONNECTED) {
    outCode = -1;
    outBody = "";
    return false;
  }
  HTTPClient http;
  http.setTimeout(10000);
  http.begin(url);
  if (contentType.length() > 0) http.addHeader("Content-Type", contentType);
  {
    int p = 0;
    while (p < extraHeaders.length()) {
      int nl = extraHeaders.indexOf("\n", p);
      String line = (nl >= 0) ? extraHeaders.substring(p, nl) : extraHeaders.substring(p);
      line.trim();
      if (line.length() > 0) {
        int c = line.indexOf(':');
        if (c > 0) {
          String k = line.substring(0, c);
          String v = line.substring(c + 1);
          v.trim();
          http.addHeader(k, v);
        }
      }
      if (nl < 0) break;
      p = nl + 1;
    }
  }

  if (method == "GET") {
    outCode = http.GET();
  } else if (method == "POST") {
    outCode = http.POST(payload);
  } else if (method == "PUT") {
    outCode = http.PUT(payload);
  } else {
    outCode = -2;
    http.end();
    outBody = "";
    return false;
  }
  outBody = http.getString();
  http.end();
  return true;
}

String ml307ReadRaw(unsigned long timeoutMs) {
  unsigned long start = millis();
  String resp;
  while (millis() - start < timeoutMs) {
    bool gotAny = false;
    while (ModemSerial.available()) {
      gotAny = true;
      char c = (char)ModemSerial.read();
      resp += c;
      start = millis();
    }
    if (!gotAny) delay(5);
  }
  return resp;
}

bool ml307WaitForToken(const String& token, unsigned long timeoutMs, String* outResp) {
  unsigned long start = millis();
  String r;
  int tp = -1;
  while (millis() - start < timeoutMs) {
    bool gotAny = false;
    while (ModemSerial.available()) {
      gotAny = true;
      char c = (char)ModemSerial.read();
      r += c;
      if (tp < 0) {
        tp = r.indexOf(token);
      }
      if (tp >= 0) {
        int eol1 = r.indexOf('\n', tp);
        int eol2 = r.indexOf('\r', tp);
        if (eol1 >= 0 || eol2 >= 0) {
          if (outResp) *outResp = r;
          return true;
        }
      }
    }
    if (!gotAny) delay(5);
  }
  if (outResp) *outResp = r;
  return (tp >= 0);
}

String ml307ReadUntil(unsigned long timeoutMs) {
  unsigned long start = millis();
  String resp;
  while (millis() - start < timeoutMs) {
    bool gotAny = false;
    while (ModemSerial.available()) {
      gotAny = true;
      char c = (char)ModemSerial.read();
      resp += c;
      start = millis();
      if (resp.indexOf("\r\nOK\r\n") >= 0 || resp.indexOf("\nOK\r\n") >= 0) {
        return resp;
      }
      if (resp.indexOf("\r\nERROR\r\n") >= 0 || resp.indexOf("\nERROR\r\n") >= 0) {
        return resp;
      }
    }
    if (!gotAny) {
      delay(5);
    }
  }
  return resp;
}

// 从原始数据中提取并缓存MQTT URC行，返回去掉URC后的数据
static String ml307ExtractMqttUrc(const String& raw) {
  String cleaned;
  cleaned.reserve(raw.length());
  int start = 0;
  while (start < (int)raw.length()) {
    int nl = raw.indexOf('\n', start);
    if (nl < 0) nl = raw.length();
    String line = raw.substring(start, nl);
    if (isMqttMsgUrc(line)) {
      // 缓存MQTT消息URC，不放回cleaned
      mqttUrcBuffer.push_back(line);
      Serial.printf("[ML307][URC-CACHE] 缓存MQTT消息: %s\n", line.c_str());
    } else {
      cleaned += line;
      if (nl < (int)raw.length()) cleaned += '\n';
    }
    start = nl + 1;
  }
  return cleaned;
}

bool ml307SendAT(const String& cmd, const char* expect1, const char* expect2, unsigned long timeoutMs, String* outResp) {
  if (modemMutex) {
    xSemaphoreTake(modemMutex, portMAX_DELAY);
  }

  // 清缓冲前先提取可能存在的MQTT URC
  {
    String preFlush;
    while (ModemSerial.available()) {
      char c = (char)ModemSerial.read();
      preFlush += c;
    }
    if (preFlush.length() > 0 && isMqttMsgUrc(preFlush)) {
      ml307ExtractMqttUrc(preFlush);
    }
  }

  if (cmd.length() > 0) {
    ModemSerial.print(cmd);
    ModemSerial.print("\r\n");
  }
  String resp = ml307ReadUntil(timeoutMs);
  if (resp.indexOf("OK") < 0 && resp.indexOf("ERROR") < 0) {
    String more = ml307ReadUntil(3000);
    if (more.length() > 0) resp += more;
  }

  // 从AT响应中提取混入的MQTT URC
  if (isMqttMsgUrc(resp)) {
    resp = ml307ExtractMqttUrc(resp);
  }

  if (outResp) *outResp = resp;
  bool ok1 = (expect1 == nullptr) ? true : (resp.indexOf(expect1) >= 0);
  bool ok2 = (expect2 == nullptr) ? true : (resp.indexOf(expect2) >= 0);
  if (!(ok1 && ok2)) {
    Serial.printf("[ML307][AT][FAIL] %s\n", cmd.c_str());
    Serial.println("[ML307][AT][RESP] >>>");
    Serial.print(resp);
    Serial.println("\n[ML307][AT][RESP] <<<");
  }

  if (modemMutex) {
    xSemaphoreGive(modemMutex);
  }
  return ok1 && ok2;
}

bool ml307Init() {
  ModemSerial.begin(ml307Baud, SERIAL_8N1, ML307_RX_PIN, ML307_TX_PIN);
  delay(200);
  String r;
  if (!ml307SendAT("AT", "OK", nullptr, 800, &r)) {
    Serial.printf("[ML307] AT无响应: %s\n", r.c_str());
    return false;
  }
  // 确保模块处于全功能模式（从深度睡眠恢复后可能处于CFUN=0）
  ml307SendAT("AT+CFUN=1", "OK", nullptr, 5000, nullptr);
  ml307SendAT("ATE0", "OK", nullptr, 800, nullptr);
  ml307SendAT("AT+CMEE=2", "OK", nullptr, 800, nullptr);
  ml307SendAT(String("AT+CGDCONT=1,\"IP\",\"") + ml307Apn + "\"", "OK", nullptr, 1000, nullptr);
  Serial.println("[ML307] 模块初始化完成");
  return true;
}

// 关闭ML307模块以省电（CFUN=0最小功能模式，约18mA）
void ml307PowerOff() {
  Serial.println("[ML307] === 关闭模块以省电 ===");
  
  String r;
  // 断开MQTT连接
  ml307SendAT("AT+MQTTDISC=0", "OK", nullptr, 3000, &r);
  delay(100);
  
  // 关闭TCP/IP连接
  for (int i = 0; i < 4; i++) {
    ml307SendAT(String("AT+MIPCLOSE=") + i, "OK", nullptr, 2000, &r);
  }
  delay(100);
  
  // 关闭网络
  ml307SendAT("AT+NETCLOSE", "OK", nullptr, 5000, &r);
  delay(100);
  
  // 去激活PDP上下文
  ml307SendAT("AT+CGACT=0,1", "OK", nullptr, 5000, &r);
  delay(100);
  
  // 设置为最小功能模式
  if (!ml307SendAT("AT+CFUN=0", "OK", nullptr, 5000, &r)) {
    Serial.printf("[ML307] CFUN=0 失败: %s\n", r.c_str());
    ml307SendAT("AT+CFUN=4", "OK", nullptr, 5000, &r);
  }
  delay(200);
  
  // 关闭串口，TX设输入避免灌电流
  ModemSerial.end();
  pinMode(ML307_TX_PIN, INPUT);
  
  Serial.println("[ML307] 模块已进入省电模式（CFUN=0）");
}

// 重新开启ML307模块
void ml307PowerOn() {
  Serial.println("[ML307] === 重新开启模块 ===");
  
  // 重新初始化串口
  ModemSerial.begin(ml307Baud, SERIAL_8N1, ML307_RX_PIN, ML307_TX_PIN);
  delay(500);
  
  String r;
  // 尝试唤醒模块
  for (int i = 0; i < 5; i++) {
    if (ml307SendAT("AT", "OK", nullptr, 1000, &r)) {
      break;
    }
    Serial.printf("[ML307] 唤醒尝试 %d/5...\n", i + 1);
    delay(500);
  }
  
  // 初始化模块
  ml307SendAT("AT+CFUN=1", "OK", nullptr, 5000, nullptr);
  delay(1000);
  ml307SendAT("ATE0", "OK", nullptr, 800, nullptr);
  ml307SendAT("AT+CMEE=2", "OK", nullptr, 800, nullptr);
  ml307SendAT(String("AT+CGDCONT=1,\"IP\",\"") + ml307Apn + "\"", "OK", nullptr, 1000, nullptr);
  
  Serial.println("[ML307] 模块已开机并初始化");
}

bool ml307EnsureNetwork() {
  static unsigned long lastAttempt = 0;
  static unsigned long lastOk = 0;
  static unsigned long retryInterval = 2000;  // 初始2秒，失败后递增
  unsigned long now = millis();
  if (lastOk > 0 && (now - lastOk) < 10000) {
    return true;
  }
  if (now - lastAttempt < retryInterval) {
    return false;
  }
  lastAttempt = now;

  String resp;
  if (!ml307SendAT("AT", "OK", nullptr, 800, &resp)) {
    Serial.println("[ML307] 模块无响应");
    retryInterval = min(retryInterval * 2, 30000UL);  // 指数退避，最大30秒
    return false;
  }

  ml307SendAT("AT+CSQ", "+CSQ:", nullptr, 800, &resp);
  ml307SendAT("AT+CPIN?", "READY", nullptr, 800, &resp);
  ml307SendAT("AT+CEREG?", "+CEREG:", nullptr, 800, &resp);

  if (!ml307SendAT("AT+CGATT?", "+CGATT: 1", nullptr, 1500, &resp)) {
    ml307SendAT("AT+CGATT=1", "OK", nullptr, 5000, &resp);
    ml307SendAT("AT+CGATT?", "+CGATT: 1", nullptr, 1500, &resp);
  }
  if (resp.indexOf("+CGATT: 1") < 0) {
    Serial.println("[ML307] 网络未附着");
    retryInterval = min(retryInterval * 2, 30000UL);
    return false;
  }

  resp = "";
  ml307SendAT("AT+CGACT?", "+CGACT:", nullptr, 2000, &resp);
  bool pdpActive = (resp.indexOf("+CGACT: 1,1") >= 0 || resp.indexOf("+CGACT:1,1") >= 0);
  if (!pdpActive) {
    ml307SendAT("AT+CGACT=1,1", "OK", nullptr, 5000, &resp);
    ml307SendAT("AT+CGACT?", "+CGACT:", nullptr, 2000, &resp);
    pdpActive = (resp.indexOf("+CGACT: 1,1") >= 0 || resp.indexOf("+CGACT:1,1") >= 0);
  }
  if (!pdpActive) {
    Serial.println("[ML307] PDP未激活");
    retryInterval = min(retryInterval * 2, 30000UL);
    return false;
  }

  (void)ml307TryConfigureDnsServers();

  resp = "";
  ml307SendAT("AT+CGPADDR=1", "+CGPADDR:", nullptr, 2000, &resp);
  if (resp.indexOf("+CGPADDR:") < 0 || resp.indexOf(".") < 0) {
    Serial.println("[ML307] 未获得IP");
    retryInterval = min(retryInterval * 2, 30000UL);
    return false;
  }

  if (ml307NetOpenSupported()) {
    String t;
    ml307SendAT("AT+NETOPEN", nullptr, nullptr, 10000, &t);
  } else {
    if (ml307MdialupSupported()) {
      String t;
      ml307SendAT("AT+MDIALUP=1,1", "OK", nullptr, 10000, &t);
    }
  }

  retryInterval = 2000;  // 成功后恢复初始重试间隔
  lastOk = now;
  return true;
}

static bool ml307ParseMIPOpenResult(const String& resp, int connectId) {
  int p = resp.indexOf("+MIPOPEN:");
  if (p < 0) return false;
  // 仅解析 +MIPOPEN: 所在的这一行，避免后续 +MIPURC: "disconn" 等URC串入导致误判
  int lineEnd = resp.indexOf('\n', p);
  if (lineEnd < 0) lineEnd = resp.indexOf('\r', p);
  if (lineEnd < 0) lineEnd = resp.length();
  String line = resp.substring(p, lineEnd);
  String s = line.substring(String("+MIPOPEN:").length());
  s.trim();
  int c1 = s.indexOf(',');
  if (c1 < 0) return false;
  int c2 = s.indexOf(',', c1 + 1);
  int id = -1;
  int result = -1;
  if (c2 < 0) {
    String a = s.substring(0, c1);
    String b = s.substring(c1 + 1);
    a.trim();
    b.trim();
    id = a.toInt();
    result = b.toInt();
  } else {
    String tail = s;
    tail.trim();
    int lastComma = tail.lastIndexOf(',');
    if (lastComma < 0) return false;
    int prevComma = tail.lastIndexOf(',', lastComma - 1);
    if (prevComma < 0) return false;
    String idStr = tail.substring(prevComma + 1, lastComma);
    String resStr = tail.substring(lastComma + 1);
    idStr.trim();
    resStr.trim();
    id = idStr.toInt();
    result = resStr.toInt();
  }
  if (id != connectId) return false;
  return (result == 0);
}

static bool ml307MIPOpen(int connectId, const String& protoType, const String& host, int port, int timeoutSec, int accessMode, int localPort, unsigned long urcTimeoutMs) {
  String r;
  ml307SendAT(String("AT+MIPCLOSE=") + String(connectId), nullptr, nullptr, 2000, &r);
  String cmd = String("AT+MIPOPEN=") + String(connectId) + ",\"" + protoType + "\",\"" + host + "\"," + String(port) + "," + String(timeoutSec) + "," + String(accessMode) + "," + String(localPort);
  g_ml307LastMIPOPEN = cmd;
  if (!ml307SendAT(cmd, "OK", nullptr, 5000, &r)) {
    g_ml307LastMIPOPEN = cmd + "\n" + r;
    if (r.indexOf("+CME ERROR: 552") >= 0) {
      String t;
      if (ml307NetOpenSupported()) {
        ml307SendAT("AT+NETOPEN", "OK", nullptr, 10000, &t);
      }
      ml307SendAT(String("AT+MIPCLOSE=") + String(connectId), "OK", nullptr, 2000, &t);
      if (!ml307SendAT(cmd, "OK", nullptr, 8000, &t)) {
        g_ml307LastMIPOPEN = cmd + "\n" + t;
        return false;
      }
    } else {
      return false;
    }
  }
  String urc;
  if (!ml307WaitForToken("+MIPOPEN:", urcTimeoutMs, &urc)) {
    g_ml307LastMIPOPEN = cmd + "\n" + r + "\n" + urc;
    if (urc.indexOf("CONNECT") >= 0) {
      return true;
    }
    Serial.printf("[ML307][MIPOPEN] wait URC timeout/unknown. urc='%s'\n", urc.c_str());
    return false;
  }
  g_ml307LastMIPOPEN = cmd + "\n" + r + "\n" + urc;
  bool ok = ml307ParseMIPOpenResult(urc, connectId);
  if (!ok) {
    Serial.printf("[ML307][MIPOPEN] URC indicates failure: %s\n", urc.c_str());
  }
  // 若同一次URC缓冲中出现断开提示，则认为连接不稳定/SSL握手失败
  String disconnToken = String("+MIPURC: \"disconn\",") + String(connectId) + ",";
  int dp = urc.indexOf(disconnToken);
  if (dp >= 0) {
    int lineEnd = urc.indexOf('\n', dp);
    if (lineEnd < 0) lineEnd = urc.indexOf('\r', dp);
    if (lineEnd < 0) lineEnd = urc.length();
    String dline = urc.substring(dp, lineEnd);
    int reason = -1;
    int lastComma = dline.lastIndexOf(',');
    if (lastComma >= 0) {
      String rs = dline.substring(lastComma + 1);
      rs.trim();
      reason = rs.toInt();
    }
    Serial.printf("[ML307][MIPOPEN] immediate disconn after open: %s (reason=%d)\n", dline.c_str(), reason);
    return false;
  }
  return ok;
}

static bool ml307NetOpenSupported() {
  static int supported = -1;
  if (supported >= 0) return supported == 1;
  String t;
  ml307SendAT("AT+NETOPEN?", nullptr, nullptr, 1500, &t);
  supported = (t.indexOf("ERROR") >= 0 || t.indexOf("+CME ERROR") >= 0) ? 0 : 1;
  return supported == 1;
}

static bool ml307MdialupSupported() {
  static int supported = -1;
  if (supported >= 0) return supported == 1;
  String t;
  ml307SendAT("AT+MDIALUP?", nullptr, nullptr, 1500, &t);
  supported = (t.indexOf("ERROR") >= 0 || t.indexOf("+CME ERROR") >= 0) ? 0 : 1;
  return supported == 1;
}

static bool ml307MIPCallSupported() {
  static int supported = -1;
  if (supported >= 0) return supported == 1;
  String t;
  ml307SendAT("AT+MIPCALL?", nullptr, nullptr, 1500, &t);
  supported = (t.indexOf("ERROR") >= 0 || t.indexOf("+CME ERROR") >= 0) ? 0 : 1;
  return supported == 1;
}

static bool ml307TryConfigureDnsServers() {
  static int done = 0;
  if (done) return true;
  done = 1;

  const char* dns1 = "114.114.114.114";
  const char* dns2 = "8.8.8.8";

  {
    String r;
    if (ml307SendAT("AT+MDNSCFG=?", "OK", nullptr, 2000, &r)) {
      String cmdIp = String("AT+MDNSCFG=\"ip\",\"") + dns1 + "\",\"" + dns2 + "\"";
      (void)ml307SendAT(cmdIp, "OK", nullptr, 2000, &r);
      String cmdPri = "AT+MDNSCFG=\"priority\",0";
      (void)ml307SendAT(cmdPri, "OK", nullptr, 2000, &r);
      return true;
    }
  }

  return false;
}

static bool ml307ResolveHostToIp(const String& host, String& outIp) {
  outIp = "";
  bool hasAlpha = false;
  for (size_t i = 0; i < host.length(); i++) {
    char c = host[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
      hasAlpha = true;
      break;
    }
  }
  if (!hasAlpha) {
    return false;
  }

  String resp;
  String cmd = String("AT+MDNSGIP=\"") + host + "\",1";
  if (!ml307SendAT(cmd, "OK", nullptr, 6000, &resp)) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      Serial.println("[ML307][DNS] AT+MDNSGIP 失败；建议先用 PC 解析域名得到 IP，直接把 mqtt_host 配成 IP 进行联通性验证");
    }
    return false;
  }

  String urc;
  if (!ml307WaitForToken("+MDNSGIP:", 8000, &urc)) {
    return false;
  }

  int q1 = urc.indexOf('"');
  while (q1 >= 0) {
    int q2 = urc.indexOf('"', q1 + 1);
    if (q2 < 0) break;
    String token = urc.substring(q1 + 1, q2);
    bool looksIp = true;
    int dots = 0;
    for (size_t i = 0; i < token.length(); i++) {
      char c = token[i];
      if (c == '.') {
        dots++;
        continue;
      }
      if (c < '0' || c > '9') {
        looksIp = false;
        break;
      }
    }
    if (looksIp && dots == 3) {
      outIp = token;
      return true;
    }
    q1 = urc.indexOf('"', q2 + 1);
  }
  return false;
}

static bool ml307MIPClose(int connectId) {
  String r;
  String cmd = String("AT+MIPCLOSE=") + String(connectId);
  ml307SendAT(cmd, nullptr, nullptr, 5000, &r);
  if (r.indexOf("OK") >= 0) return true;
  if (r.indexOf("+CME ERROR: 551") >= 0) return true;
  return false;
}

static bool ml307MIPSend(int connectId, const String& data) {
  String r;
  int len = data.length();
  if (len <= 0) return true;
  String cmd = String("AT+MIPSEND=") + String(connectId) + "," + String(len);
  ml307SendAT(cmd, nullptr, nullptr, 1000, &r);
  if (r.indexOf(">") < 0) {
    String more = ml307ReadUntil(500);
    if (more.length() > 0) r += more;
  }
  if (r.indexOf(">") < 0) {
    Serial.println("[ML307][MIPSEND] no prompt");
    Serial.println(r);
    return false;
  }
  ModemSerial.print(data);
  String r2 = ml307ReadUntil(5000);
  return (r2.indexOf("OK") >= 0);
}

static bool ml307MIPSendRaw(int connectId, const uint8_t* data, size_t len) {
  if (data == nullptr || len == 0) return false;

  auto hexPreview = [&](const uint8_t* p, size_t plen, size_t maxBytes) -> String {
    size_t n = (plen < maxBytes) ? plen : maxBytes;
    String s;
    s.reserve((int)(n * 3 + 16));
    for (size_t i = 0; i < n; i++) {
      static const char* hex = "0123456789ABCDEF";
      uint8_t b = p[i];
      s += hex[(b >> 4) & 0x0F];
      s += hex[b & 0x0F];
      if (i + 1 < n) s += ' ';
    }
    if (plen > n) s += " ...";
    return s;
  };

  if (modemMutex) {
    xSemaphoreTake(modemMutex, portMAX_DELAY);
  }

  String dataHex = hexPreview(data, len, 64);
  g_ml307LastMIPSEND = String("DATAHEX: ") + dataHex;

  ModemSerial.print(String("AT+MIPSEND=") + String(connectId) + "," + String((int)len) + "\r\n");

  unsigned long start = millis();
  bool gotPrompt = false;
  String promptResp;
  while (millis() - start < 2000) {
    while (ModemSerial.available()) {
      int c = ModemSerial.read();
      if (c < 0) break;
      if (promptResp.length() < 160) promptResp += (char)c;
      if (c == '>') {
        gotPrompt = true;
        break;
      }
    }
    if (gotPrompt) break;
    delay(1);
  }

  if (!gotPrompt) {
    g_ml307LastMIPSEND = String("DATAHEX: ") + dataHex + "\n" + promptResp;
    if (modemMutex) xSemaphoreGive(modemMutex);
    Serial.println("[ML307][MIPSEND] no prompt (raw)");
    return false;
  }

  ModemSerial.write(data, len);

  // 等待 OK
  start = millis();
  String resp;
  while (millis() - start < 8000) {
    while (ModemSerial.available()) {
      char ch = (char)ModemSerial.read();
      resp += ch;
      if (resp.indexOf("OK") >= 0) {
        g_ml307LastMIPSEND = String("DATAHEX: ") + dataHex + "\n" + promptResp + "\n" + resp;
        if (modemMutex) xSemaphoreGive(modemMutex);
        return true;
      }
      if (resp.indexOf("ERROR") >= 0) {
        g_ml307LastMIPSEND = String("DATAHEX: ") + dataHex + "\n" + promptResp + "\n" + resp;
        if (modemMutex) xSemaphoreGive(modemMutex);
        return false;
      }
    }
    delay(1);
  }

  g_ml307LastMIPSEND = String("DATAHEX: ") + dataHex + "\n" + promptResp + "\n" + resp;
  if (modemMutex) xSemaphoreGive(modemMutex);
  Serial.println("[ML307][MIPSEND] timeout waiting OK (raw)");
  return false;
}

static bool ml307MIPRDRead(int connectId, int readLen, String& outData, int& outUnreadLeft) {
  outData = "";
  outUnreadLeft = 0;
  String r;
  String cmd = String("AT+MIPRD=") + String(connectId) + "," + String(readLen);

  // 注意：这里不能用 ml307SendAT，因为它会先清空串口缓冲，可能把 +MIPURC: "recv" 等URC冲掉。
  if (modemMutex) {
    xSemaphoreTake(modemMutex, portMAX_DELAY);
  }
  ModemSerial.print(cmd);
  ModemSerial.print("\r\n");
  r = ml307ReadUntil(8000);
  if (modemMutex) {
    xSemaphoreGive(modemMutex);
  }
  g_ml307LastMIPRD = r;
  if (r.indexOf("+CME ERROR: 550") >= 0) {
    outUnreadLeft = 0;
    outData = "";
    return true;
  }
  if (r.indexOf("+MIPRD:") < 0) {
    outUnreadLeft = 0;
    outData = "";
    return (r.indexOf("OK") >= 0);
  }

  int p = r.indexOf("+MIPRD:");
  int lineEnd = r.indexOf("\n", p);
  if (lineEnd < 0) lineEnd = r.indexOf("\r", p);
  if (lineEnd < 0) lineEnd = r.length();
  String header = r.substring(p + String("+MIPRD:").length(), lineEnd);
  header.trim();

  int partsCount = 0;
  String parts[4];
  int start = 0;
  while (partsCount < 4) {
    int comma = header.indexOf(',', start);
    if (comma < 0) {
      parts[partsCount++] = header.substring(start);
      break;
    }
    parts[partsCount++] = header.substring(start, comma);
    start = comma + 1;
  }
  for (int i = 0; i < partsCount; i++) parts[i].trim();
  if (partsCount < 2) return false;

  int id = parts[0].toInt();
  if (id != connectId) return false;

  int dataLen = 0;
  int unread = 0;
  if (partsCount == 2) {
    dataLen = parts[1].toInt();
    unread = 0;
  } else if (partsCount == 3) {
    // 常见格式：+MIPRD: <id>,<dataLen>,<unread>
    dataLen = parts[1].toInt();
    unread = parts[2].toInt();
  } else {
    int a = parts[partsCount - 2].toInt();
    int b = parts[partsCount - 1].toInt();
    dataLen = b;
    unread = a;
    if (dataLen > readLen && unread <= readLen) {
      int tmp = dataLen;
      dataLen = unread;
      unread = tmp;
    }
  }
  outUnreadLeft = unread;
  if (dataLen <= 0) {
    outData = "";
    return true;
  }

  int dataStart = lineEnd + 1;
  while (dataStart < r.length() && (r[dataStart] == '\r' || r[dataStart] == '\n')) dataStart++;
  if (dataStart >= r.length()) return false;

  int dataEnd = dataStart + dataLen;
  if (dataEnd > r.length()) {
    Serial.printf("[ML307][MIPRD] truncated: want=%d have=%d\n", dataLen, (int)(r.length() - dataStart));
    return false;
  }
  outData = r.substring(dataStart, dataEnd);
  return true;
}

static int ml307MIPRDReadRaw(int connectId, int readLen, uint8_t* out, int outCap, int& outUnreadLeft, unsigned long timeoutMs) {
  outUnreadLeft = 0;
  if (out == nullptr || outCap <= 0) return 0;
  if (readLen <= 0) return 0;
  if (readLen > outCap) readLen = outCap;

  if (modemMutex) {
    xSemaphoreTake(modemMutex, portMAX_DELAY);
  }

  ModemSerial.print(String("AT+MIPRD=") + String(connectId) + "," + String(readLen) + "\r\n");

  // 1) 读到 +MIPRD: 这一行
  unsigned long start = millis();
  String line;
  String trace;
  bool gotHeader = false;
  bool noData = false;
  bool disconn = false;
  bool sawOk = false;
  unsigned long okSeenAt = 0;
  while (millis() - start < timeoutMs) {
    while (ModemSerial.available()) {
      int c = ModemSerial.read();
      if (c < 0) break;
      if (c == '\r') continue;
      if (c == '\n') {
        String l = line;
        l.trim();
        if (l.length() > 0) {
          if (trace.length() < 480) {
            if (trace.length() > 0) trace += "\n";
            trace += l;
          }
        }
        if (line.indexOf("+MIPRD:") >= 0) {
          gotHeader = true;
          break;
        }
        if (line.indexOf("+CME ERROR: 550") >= 0) {
          noData = true;
          break;
        }
        if (line == "OK") {
          sawOk = true;
          okSeenAt = millis();
        }
        if (line.indexOf("+CME ERROR") >= 0 || line.indexOf("ERROR") >= 0) {
          disconn = true;
          break;
        }
        if (line.indexOf("disconn") >= 0) {
          disconn = true;
          break;
        }
        line = "";
        continue;
      }
      line += (char)c;
      if (line.length() > 120) {
        line.remove(0, line.length() - 120);
      }
    }
    if (gotHeader || noData || disconn) break;
    if (sawOk && (millis() - okSeenAt) > 200) {
      noData = true;
      break;
    }
    delay(1);
  }

  if (disconn) {
    g_ml307LastMIPRD = (trace.length() > 0) ? trace : line;
    if (modemMutex) xSemaphoreGive(modemMutex);
    return -1;
  }

  if (noData || !gotHeader) {
    if (noData) {
      g_ml307LastMIPRD = (trace.length() > 0) ? trace : line;
    } else {
      g_ml307LastMIPRD = String("[ML307][MIPRD] timeout header trace='") + trace + "' line='" + line + "'";
    }
    if (modemMutex) xSemaphoreGive(modemMutex);
    return 0;
  }

  int p = line.indexOf("+MIPRD:");
  String header = line.substring(p + String("+MIPRD:").length());
  header.trim();

  int partsCount = 0;
  String parts[4];
  int st = 0;
  while (partsCount < 4) {
    int comma = header.indexOf(',', st);
    if (comma < 0) {
      parts[partsCount++] = header.substring(st);
      break;
    }
    parts[partsCount++] = header.substring(st, comma);
    st = comma + 1;
  }
  for (int i = 0; i < partsCount; i++) parts[i].trim();
  if (partsCount < 2) {
    if (modemMutex) xSemaphoreGive(modemMutex);
    return 0;
  }

  int id = parts[0].toInt();
  if (id != connectId) {
    if (modemMutex) xSemaphoreGive(modemMutex);
    return 0;
  }

  int dataLen = 0;
  int unread = 0;
  if (partsCount == 2) {
    dataLen = parts[1].toInt();
    unread = 0;
  } else if (partsCount == 3) {
    dataLen = parts[1].toInt();
    unread = parts[2].toInt();
  } else {
    int a = parts[partsCount - 2].toInt();
    int b = parts[partsCount - 1].toInt();
    dataLen = b;
    unread = a;
    if (dataLen > readLen && unread <= readLen) {
      int tmp = dataLen;
      dataLen = unread;
      unread = tmp;
    }
  }
  outUnreadLeft = unread;
  if (dataLen <= 0) {
    g_ml307LastMIPRD = line;
    unsigned long drainStart = millis();
    while (millis() - drainStart < 200) {
      if (!ModemSerial.available()) {
        delay(1);
        continue;
      }
      char ch = (char)ModemSerial.read();
      (void)ch;
    }
    if (modemMutex) xSemaphoreGive(modemMutex);
    return 0;
  }
  if (dataLen > outCap) dataLen = outCap;

  // 2) 跳过 \r\n，然后精确读取 dataLen 字节（允许包含 0x00）
  start = millis();
  while (millis() - start < timeoutMs) {
    if (ModemSerial.available()) {
      int c = ModemSerial.peek();
      if (c == '\r' || c == '\n') {
        ModemSerial.read();
        continue;
      }
      break;
    }
    delay(1);
  }

  size_t got = 0;
  start = millis();
  while (got < (size_t)dataLen && millis() - start < timeoutMs) {
    int c = ModemSerial.read();
    if (c < 0) {
      delay(1);
      continue;
    }
    out[got++] = (uint8_t)c;
  }

  if (got < (size_t)dataLen) {
    Serial.printf("[ML307][MIPRD] short read: want=%d got=%d\n", dataLen, (int)got);
    g_ml307LastMIPRD = line;
    unsigned long drainStart = millis();
    while (millis() - drainStart < 300) {
      if (!ModemSerial.available()) {
        delay(1);
        continue;
      }
      (void)ModemSerial.read();
    }
    if (modemMutex) xSemaphoreGive(modemMutex);
    return -1;
  }

  // 3) 读到本次 AT+MIPRD 的 OK/ERROR 行（不做 1s 大 drain，避免吞掉后续 URC）
  start = millis();
  String tail;
  while (millis() - start < 500) {
    while (ModemSerial.available()) {
      char ch = (char)ModemSerial.read();
      tail += ch;
      if (tail.length() > 64) {
        tail.remove(0, tail.length() - 64);
      }
      if (tail.indexOf("\nOK") >= 0 || tail.indexOf("\nERROR") >= 0 || tail.indexOf("+CME ERROR") >= 0) {
        start = millis();
        break;
      }
    }
    if (!ModemSerial.available()) break;
    delay(1);
  }

  g_ml307LastMIPRD = line + "\n" + tail;
  if (modemMutex) xSemaphoreGive(modemMutex);
  return (int)got;
}

static int parseHttpStatusCodeFromResponse(const String& resp) {
  int lineEnd = resp.indexOf("\r\n");
  if (lineEnd < 0) lineEnd = resp.indexOf("\n");
  if (lineEnd < 0) return -5;
  String line = resp.substring(0, lineEnd);
  int sp1 = line.indexOf(' ');
  if (sp1 < 0) return -5;
  int sp2 = line.indexOf(' ', sp1 + 1);
  if (sp2 < 0) sp2 = line.length();
  return line.substring(sp1 + 1, sp2).toInt();
}

static String extractHttpBody(const String& resp) {
  int p = resp.indexOf("\r\n\r\n");
  if (p >= 0) return resp.substring(p + 4);
  p = resp.indexOf("\n\n");
  if (p >= 0) return resp.substring(p + 2);
  return "";
}

// ML307R HTTP 实例 ID（使用缓存模式）
static int g_ml307HttpId = -1;

// 删除 HTTP 实例
static void ml307HttpDelete() {
  if (g_ml307HttpId >= 0) {
    String r;
    ml307SendAT(String("AT+MHTTPDEL=") + String(g_ml307HttpId), "OK", nullptr, 1000, &r);
    g_ml307HttpId = -1;
  }
}

// 使用 ML307R 原生 HTTP AT 命令进行 HTTP 请求
bool httpRequestMl307(const String& method, const String& url, const String& contentType, const String& payload, int& outCode, String& outBody, const String& extraHeaders) {
  outBody = "";
  outCode = -1;
  
  if (!ml307EnsureNetwork()) {
    outCode = -1;
    return false;
  }
  
  bool isHttps = false;
  String host, path;
  int port = 0;
  if (!parseUrl(url, isHttps, host, port, path)) {
    outCode = -2;
    return false;
  }
  
  String r;
  const int sslId = 0;
  
  // 删除旧实例（如果存在）
  ml307HttpDelete();
  
  // 构建 host URL（包含协议和端口）
  String hostUrl = isHttps ? "https://" : "http://";
  hostUrl += host;
  if ((isHttps && port != 443) || (!isHttps && port != 80)) {
    hostUrl += ":";
    hostUrl += String(port);
  }
  
  // 先清理所有可能存在的旧 HTTP 实例（ID 0-4）
  for (int i = 0; i < 5; i++) {
    ml307SendAT(String("AT+MHTTPDEL=") + String(i), "OK", nullptr, 500, &r);
  }
  g_ml307HttpId = -1;
  
  // 创建 HTTP 实例
  String createCmd = "AT+MHTTPCREATE=\"" + hostUrl + "\"";
  if (!ml307SendAT(createCmd, "+MHTTPCREATE:", nullptr, 5000, &r)) {
    Serial.println("[ML307-HTTP] 创建实例失败");
    outCode = -1;
    return false;
  }
  
  // 解析 httpid
  int idPos = r.indexOf("+MHTTPCREATE:");
  if (idPos >= 0) {
    g_ml307HttpId = r.substring(idPos + 13).toInt();
  } else {
    g_ml307HttpId = 0;
  }
  Serial.printf("[ML307-HTTP] 创建实例成功, ID=%d\n", g_ml307HttpId);
  
  // 配置缓存模式（接收数据缓存在模块中，请求完成后手动读取）
  ml307SendAT(String("AT+MHTTPCFG=\"cached\",") + String(g_ml307HttpId) + ",1,8192", "OK", nullptr, 500, &r);
  
  // 配置超时（连接60s，响应30s）
  ml307SendAT(String("AT+MHTTPCFG=\"timeout\",") + String(g_ml307HttpId) + ",60,30", "OK", nullptr, 500, &r);
  
  // 如果是 HTTPS，配置 SSL（注意：ML307R 的 SSL 可能与某些服务器不兼容）
  if (isHttps) {
    ml307SendAT(String("AT+MSSLCFG=\"auth\",") + String(sslId) + ",0", "OK", nullptr, 500, &r);
    ml307SendAT(String("AT+MSSLCFG=\"ignoreverify\",") + String(sslId) + ",1", "OK", nullptr, 500, &r);
    ml307SendAT(String("AT+MSSLCFG=\"ignorestamp\",") + String(sslId) + ",1", "OK", nullptr, 500, &r);
    ml307SendAT(String("AT+MHTTPCFG=\"ssl\",") + String(g_ml307HttpId) + ",1," + String(sslId), "OK", nullptr, 500, &r);
  }
  
  // 设置通用报头（使用 AT+MHTTPCFG="header" 命令）
  if (contentType.length() > 0) {
    ml307SendAT(String("AT+MHTTPCFG=\"header\",") + String(g_ml307HttpId) + ",\"Content-Type: " + contentType + "\"", "OK", nullptr, 500, &r);
  }
  
  // 设置额外报头（如 X-LC-Id, X-LC-Key）- 使用 AT+MHTTPCFG="header"
  if (extraHeaders.length() > 0) {
    int p = 0;
    while (p < (int)extraHeaders.length()) {
      int nl = extraHeaders.indexOf('\n', p);
      if (nl < 0) nl = extraHeaders.length();
      String line = extraHeaders.substring(p, nl);
      line.trim();
      line.replace("\r", "");
      if (line.length() > 0 && line.indexOf(':') > 0) {
        ml307SendAT(String("AT+MHTTPCFG=\"header\",") + String(g_ml307HttpId) + ",\"" + line + "\"", "OK", nullptr, 500, &r);
      }
      p = nl + 1;
    }
  }
  
  // 设置 content 数据（POST/PUT）
  if ((method == "POST" || method == "PUT") && payload.length() > 0) {
    // 使用命令直接输入数据
    String contentCmd = String("AT+MHTTPCONTENT=") + String(g_ml307HttpId) + ",0,0,\"" + payload + "\"";
    if (!ml307SendAT(contentCmd, "OK", nullptr, 2000, &r)) {
      Serial.println("[ML307-HTTP] 设置content失败");
      ml307HttpDelete();
      outCode = -3;
      return false;
    }
  }
  
  // 确定请求方法编号
  int methodNum = 1; // GET
  if (method == "POST") methodNum = 2;
  else if (method == "PUT") methodNum = 3;
  else if (method == "DELETE") methodNum = 4;
  else if (method == "HEAD") methodNum = 5;
  
  // 发送请求 - 使用更长超时，让 URC 在同一响应中返回
  Serial.printf("[ML307-HTTP] %s %s%s\n", method.c_str(), hostUrl.c_str(), path.c_str());
  String reqCmd = String("AT+MHTTPREQUEST=") + String(g_ml307HttpId) + "," + String(methodNum) + ",0,\"" + path + "\"";
  
  // 发送请求命令，使用长超时等待 URC 响应
  // 不使用 ml307SendAT，直接操作串口以避免 mutex 问题
  if (modemMutex) {
    xSemaphoreTake(modemMutex, portMAX_DELAY);
  }
  
  while (ModemSerial.available()) ModemSerial.read();
  ModemSerial.print(reqCmd);
  ModemSerial.print("\r\n");
  
  // 等待 OK 和 URC 响应
  String fullResp;
  fullResp.reserve(512);
  unsigned long start = millis();
  bool gotOk = false;
  bool gotResponse = false;
  int headerLen = 0;
  int contentLen = 0;
  
  Serial.println("[ML307-HTTP] 等待响应...");
  
  while (millis() - start < 35000) {
    while (ModemSerial.available()) {
      char c = ModemSerial.read();
      fullResp += c;
    }
    
    // 检查是否收到 OK
    if (!gotOk && fullResp.indexOf("OK") >= 0) {
      gotOk = true;
      Serial.println("[ML307-HTTP] 请求已发送，等待URC...");
    }
    
    // 检查错误 URC
    if (fullResp.indexOf("+MHTTPURC:\"err\"") >= 0 || fullResp.indexOf("+MHTTPURC: \"err\"") >= 0) {
      Serial.printf("[ML307-HTTP] 请求错误: %s\n", fullResp.c_str());
      if (modemMutex) xSemaphoreGive(modemMutex);
      ml307HttpDelete();
      outCode = -5;
      return false;
    }
    
    // 检查成功 URC: +MHTTPURC: "recv",<httpid>,<code>,<header_len>,<content_len>
    int recvPos = fullResp.indexOf("+MHTTPURC:\"recv\"");
    if (recvPos < 0) recvPos = fullResp.indexOf("+MHTTPURC: \"recv\"");
    if (recvPos >= 0) {
      // 解析响应码和长度
      int commaCount = 0;
      for (int i = recvPos; i < (int)fullResp.length(); i++) {
        if (fullResp[i] == ',') {
          commaCount++;
          if (commaCount == 2) {
            int nextComma = fullResp.indexOf(',', i + 1);
            if (nextComma > i) {
              outCode = fullResp.substring(i + 1, nextComma).toInt();
            }
          } else if (commaCount == 3) {
            int nextComma = fullResp.indexOf(',', i + 1);
            if (nextComma > i) {
              headerLen = fullResp.substring(i + 1, nextComma).toInt();
            }
          } else if (commaCount == 4) {
            int endPos = i + 1;
            while (endPos < (int)fullResp.length() && isDigit(fullResp[endPos])) endPos++;
            contentLen = fullResp.substring(i + 1, endPos).toInt();
            break;
          }
        }
      }
      Serial.printf("[ML307-HTTP] 响应完成: code=%d, header=%d, content=%d\n", outCode, headerLen, contentLen);
      gotResponse = true;
      break;
    }
    
    delay(10);
  }
  
  if (modemMutex) {
    xSemaphoreGive(modemMutex);
  }
  
  if (!gotResponse) {
    Serial.println("[ML307-HTTP] 等待响应超时");
    if (fullResp.length() > 0) {
      Serial.printf("[ML307-HTTP] 收到的数据: %s\n", fullResp.c_str());
    }
    ml307HttpDelete();
    outCode = -6;
    return false;
  }
  
  // 读取 content 数据
  if (contentLen > 0) {
    String readCmd = String("AT+MHTTPREAD=") + String(g_ml307HttpId) + ",1," + String(contentLen);
    if (ml307SendAT(readCmd, "+MHTTPREAD:", nullptr, 5000, &r)) {
      // 解析: +MHTTPREAD: <httpid>,<type>,<unread>,<len>,<data>
      int dataPos = r.indexOf("+MHTTPREAD:");
      if (dataPos >= 0) {
        // 找到第4个逗号后的数据
        int commaCount = 0;
        for (int i = dataPos; i < (int)r.length(); i++) {
          if (r[i] == ',') {
            commaCount++;
            if (commaCount == 4) {
              // 数据从这里开始
              int dataStart = i + 1;
              // 找到 OK 或结尾
              int okPos = r.indexOf("\r\nOK", dataStart);
              if (okPos > dataStart) {
                outBody = r.substring(dataStart, okPos);
              } else {
                outBody = r.substring(dataStart);
              }
              outBody.trim();
              break;
            }
          }
        }
      }
    }
  }
  
  Serial.printf("[ML307-HTTP] HTTP %d, body=%d bytes\n", outCode, (int)outBody.length());
  
  // 删除实例
  ml307HttpDelete();
  
  return (outCode >= 200 && outCode < 300);
}

// 恢复出厂（长按GPIO9五秒）
void checkFactoryResetButton() {
  static unsigned long pressedAt = 0;
  static bool wasPressed = false;
  // 增加简单的数字滤波/防抖
  static int lowCount = 0;
  
  if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
    if (lowCount < 5) lowCount++;
  } else {
    lowCount = 0;
  }
  
  bool pressed = (lowCount >= 3); // 连续检测到3次低电平才认为是按下
  
  unsigned long now = millis();
  if (pressed) {
    if (!wasPressed) pressedAt = now;
    if (now - pressedAt >= 5000) {
      Serial.println("[RESET] 检测到GPIO9长按5秒，恢复初始化...");
      factoryReset();
    }
  }
  wasPressed = pressed;
}

void factoryReset() {
  // 清除配置并重启
  preferences.begin("config", false);
  preferences.clear();
  preferences.end();
  Serial.println("[RESET] 配置已清除，正在重启...");
  delay(500);
  esp_restart();
}

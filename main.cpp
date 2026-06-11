#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <WiFiManager.h>
#include <esp_task_wdt.h>
#include <limits.h>

// ==================== 传感器与通信库 ====================
#include <Adafruit_SHT31.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <BH1750_WE.h>
#include <MHZ19.h>
#include <ModbusMaster.h>
#include <PubSubClient.h>
#include <mbedtls/aes.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

// AES-256 加密后的签名结构：16字节IV + 密文（16字节倍数）

#if __has_include("credentials.h")
#include "credentials.h"
#endif

// ==================== 常量与配置 ====================
#define MQTT_TLS_ENABLED 0  // Set to 1 to enable TLS
#define SENSOR_DEBUG_LEVEL 1
const char* mqtt_server = "112.126.27.218";
#if MQTT_TLS_ENABLED
const int mqtt_port = 8883;
#else
const int mqtt_port = 1883;
#endif
#define MQTT_PUB_TOPIC "agri/sensor/data"
const char* DEVICE_NAME = "esp32_s3_n16r8_001";
const char* DEVICE_TYPE = "agriculture_sensor";

// 凭证必须通过 credentials.h 或环境变量提供，禁止硬编码
// 注意：SECRET_TOKEN 现在作为 AES-256 密钥使用，必须是 32 字节字符串
#ifndef SECRET_TOKEN
#error "SECRET_TOKEN must be defined in credentials.h"
#endif
#ifndef WIFI_PASSWORD
#error "WIFI_PASSWORD must be defined in credentials.h"
#endif

const unsigned long MIN_VALID_TIMESTAMP = 1704067200;
const int SOIL_ADC_DRY = 4000;
const int SOIL_ADC_WET = 1200;
constexpr uint32_t WDT_TIMEOUT_SECONDS = 15;

namespace PinConfig {
  constexpr uint8_t I2C_SDA = 21;
  constexpr uint8_t I2C_SCL = 20;
  constexpr uint8_t ONE_WIRE_BUS = 4;
  constexpr uint8_t SOIL_MOIST_PIN = 36;
  constexpr uint8_t RE_DE_PIN = 32;
  constexpr uint8_t CO2_RX_PIN = 16;
  constexpr uint8_t CO2_TX_PIN = 17;
  constexpr uint8_t RS485_RX_PIN = 18;
  constexpr uint8_t RS485_TX_PIN = 19;
}

// ==================== 实例化硬件对象 ====================
Adafruit_SHT31 sht31 = Adafruit_SHT31();
BH1750_WE lightMeter;
OneWire oneWire(PinConfig::ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
MHZ19 mhz19;
ModbusMaster node;
#if MQTT_TLS_ENABLED
WiFiClientSecure espClient;
#else
WiFiClient espClient;
#endif
PubSubClient client(espClient);
HardwareSerial co2Serial(1);
HardwareSerial modbusSerial(2);

// ==================== 全局变量定义 ====================
float airTemp = -999.0, airHum = -999.0, soilTemp = -999.0, lux = -1.0;
int soilMoist = -1, co2 = -1;
double soilEC = -1.0;

bool sht31Available = false, bh1750Available = false, ds18b20Available = false, mhz19Available = false, modbusAvailable = false;
bool sht31PermanentOffline = false, bh1750PermanentOffline = false, ds18b20PermanentOffline = false, mhz19PermanentOffline = false, modbusPermanentOffline = false;

uint8_t sht31ErrorCount = 0, bh1750ErrorCount = 0, ds18b20ErrorCount = 0, mhz19ErrorCount = 0, modbusErrorCount = 0;
const uint8_t MAX_ALLOWED_ERRORS = 3;

unsigned long sht31RetryTime = 0, bh1750RetryTime = 0, ds18b20RetryTime = 0, mhz19RetryTime = 0, modbusRetryTime = 0;
// 重试间隔 1 小时，工业环境可按需调整
const unsigned long SENSOR_RETRY_INTERVAL = 3600000;

unsigned long lastUploadTime = 0;
unsigned long mqttReconnectTime = 0;
unsigned long lastNtpSyncTime = 0;
unsigned long lastWifiReconnectTime = 0;
unsigned long wifiReconnectInterval = 30000;
bool ntpSuccess = false;
bool ntpRequested = false;
bool wasWifiConnected = true;

// ==================== 辅助与基础工具函数 ====================
inline unsigned long timeDiff(unsigned long newer, unsigned long older) {
  return (newer >= older) ? (newer - older) : (ULONG_MAX - older + newer + 1);
}

void preTransmission() {
  digitalWrite(PinConfig::RE_DE_PIN, HIGH);
  delayMicroseconds(50);
}

void postTransmission() {
  delayMicroseconds(50);
  digitalWrite(PinConfig::RE_DE_PIN, LOW);
}

void initRetryTimers() {
  unsigned long now = millis();
  sht31RetryTime = now;
  bh1750RetryTime = now;
  ds18b20RetryTime = now;
  mhz19RetryTime = now;
  modbusRetryTime = now;
}

void printSensorStatus() {
  Serial.println("====== 传感器硬件自检状态 ======");
  Serial.printf("SHT31 (温湿度): %s\n", sht31Available ? "正常" : (sht31PermanentOffline ? "永久离线" : "离线"));
  Serial.printf("BH1750 (光照):  %s\n", bh1750Available ? "正常" : (bh1750PermanentOffline ? "永久离线" : "离线"));
  Serial.printf("DS18B20 (土温): %s\n", ds18b20Available ? "正常" : (ds18b20PermanentOffline ? "永久离线" : "离线"));
  Serial.printf("MH-Z19B (CO2):  %s\n", mhz19Available ? "正常" : (mhz19PermanentOffline ? "永久离线" : "离线"));
  Serial.printf("RS485 (土EC):   %s\n", modbusAvailable ? "正常" : (modbusPermanentOffline ? "永久离线" : "离线"));
  Serial.println("=================================");
}

// ==================== 网络服务逻辑 ====================
void initWiFi() {
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  
  uint64_t mac = ESP.getEfuseMac();
  char apName[28];
  snprintf(apName, sizeof(apName), "ESP32_Agri_%06X", (uint32_t)(mac >> 16));
  
  Serial.println("[WiFiManager] 正在尝试连网或启动配置热点...");
  if (!wm.autoConnect(apName, WIFI_PASSWORD)) {
    Serial.println("[WiFiManager] 连接超时，系统复位重启...");
    delay(1000);
    ESP.restart();
  }
  Serial.println("[WiFiManager] Wi-Fi 连接成功!");
}

void requestTimeSync() {
  configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org", "time.nist.gov");
  Serial.println("[NTP] 已向服务器发送时间同步请求...");
}

bool checkNtpStatus() {
  time_t now;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100)) {
    return false;
  }
  now = time(NULL);
  if (now > MIN_VALID_TIMESTAMP) {
    if (!ntpSuccess) {
      Serial.print("[NTP] 时间同步完成。当前时间: ");
      Serial.println(ctime(&now));
    }
    return true;
  }
  return false;
}

// AES-256 加密 payload，结果以 Base64 编码输出（IV 附在密文前）
void generateAESSignature(const char* payload, const char* key, char* outBase64) {
  // 内部缓冲区根据最大 payload 512 字节 + PKCS7 填充 + IV 预设
  const size_t MAX_PAYLOAD_LEN = 512;
  const size_t MAX_PADDED_LEN  = ((MAX_PAYLOAD_LEN / 16) + 1) * 16; // 528
  const size_t MAX_COMBINED    = MAX_PADDED_LEN + 16;               // 544

  mbedtls_aes_context ctx;
  unsigned char iv[16] = {0};
  unsigned char randBuf[16] = {0};
  mbedtls_ctr_drbg_context ctrDrbg;
  mbedtls_entropy_context entropy;

  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctrDrbg);
  mbedtls_ctr_drbg_seed(&ctrDrbg, mbedtls_entropy_func, &entropy, NULL, 0);
  mbedtls_ctr_drbg_random(&ctrDrbg, randBuf, sizeof(randBuf));
  mbedtls_ctr_drbg_free(&ctrDrbg);
  mbedtls_entropy_free(&entropy);

  for (int i = 0; i < 16; i++) iv[i] = randBuf[i];
  memset(randBuf, 0, sizeof(randBuf));

  size_t payloadLen = strlen(payload);
  if (payloadLen > MAX_PAYLOAD_LEN) {
    payloadLen = MAX_PAYLOAD_LEN;
  }
  size_t paddedLen = ((payloadLen / 16) + 1) * 16;
  unsigned char padded[MAX_PADDED_LEN] = {0};
  memcpy(padded, payload, payloadLen);

  // 先保存 IV，再加密（mbedtls_aes_crypt_cbc 会覆盖 iv）
  unsigned char combined[MAX_COMBINED];
  memcpy(combined, iv, 16);

  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, (const unsigned char*)key, 256);
  mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, paddedLen, iv, padded, padded);
  mbedtls_aes_free(&ctx);

  memcpy(combined + 16, padded, paddedLen);

  static const char* b64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int j = 0;
  for (size_t i = 0; i < paddedLen + 16; i += 3) {
    int b0 = combined[i], b1 = (i + 1 < paddedLen + 16) ? combined[i + 1] : 0;
    int b2 = (i + 2 < paddedLen + 16) ? combined[i + 2] : 0;
    outBase64[j++] = b64chars[(b0 >> 2) & 0x3F];
    outBase64[j++] = b64chars[((b0 << 4) | (b1 >> 4)) & 0x3F];
    outBase64[j++] = (i + 1 < paddedLen + 16) ? b64chars[((b1 << 2) | (b2 >> 6)) & 0x3F] : '=';
    outBase64[j++] = (i + 2 < paddedLen + 16) ? b64chars[b2 & 0x3F] : '=';
  }
  outBase64[j] = '\0';

  // 清零栈上敏感数据
  memset(padded, 0, MAX_PADDED_LEN);
  memset(combined, 0, MAX_COMBINED);
}

// ==================== 硬件安全数据采集 ====================
int safeGetCO2() {
  unsigned long start = millis();
  int val = -1;
  while (timeDiff(millis(), start) < 1500) {
    val = mhz19.getCO2(false);
    if (val > 0) return val;
    delay(50);
  }
  return -1;
}

void readSensors() {
  unsigned long now = millis();

  // 1. SHT31 采集
  if (sht31Available) {
    float t = sht31.readTemperature();
    float h = sht31.readHumidity();
    if (!isnan(t) && !isnan(h) && h > 0.0 && h <= 100.0) {
      airTemp = t; airHum = h;
      sht31ErrorCount = 0;
    } else {
      sht31ErrorCount++;
      if (sht31ErrorCount >= MAX_ALLOWED_ERRORS) {
        Serial.printf("[警告] SHT31 读取失败 (%d/%d)，标记为永久离线。\n", sht31ErrorCount, MAX_ALLOWED_ERRORS);
        sht31Available = false;
        sht31PermanentOffline = true;
        sht31RetryTime = now;
        Serial.println("[警告] SHT31 已永久离线，每小时重试一次。");
      } else {
        if (sht31ErrorCount == 1) {
          Serial.printf("[警告] SHT31 读取失败 (%d/%d)\n", sht31ErrorCount, MAX_ALLOWED_ERRORS);
        }
        sht31RetryTime = now;
      }
    }
  } else if (sht31PermanentOffline && timeDiff(now, sht31RetryTime) > SENSOR_RETRY_INTERVAL) {
    if (sht31.begin(0x44)) {
      sht31Available = true;
      sht31PermanentOffline = false;
      sht31ErrorCount = 0;
      Serial.println("[恢复] SHT31 已恢复在线！");
    } else {
      sht31RetryTime = now;
    }
  }

  // 2. BH1750 采集
  if (bh1750Available) {
    float l = lightMeter.getLux();
    if (l >= 0.0) {
      lux = l;
      bh1750ErrorCount = 0;
    } else {
      bh1750ErrorCount++;
      if (bh1750ErrorCount >= MAX_ALLOWED_ERRORS) {
        Serial.printf("[警告] BH1750 读取失败 (%d/%d)，标记为永久离线。\n", bh1750ErrorCount, MAX_ALLOWED_ERRORS);
        bh1750Available = false;
        bh1750PermanentOffline = true;
        bh1750RetryTime = now;
        Serial.println("[警告] BH1750 已永久离线，每小时重试一次。");
      } else {
        if (bh1750ErrorCount == 1) {
          Serial.printf("[警告] BH1750 读取失败 (%d/%d)\n", bh1750ErrorCount, MAX_ALLOWED_ERRORS);
        }
        bh1750RetryTime = now;
      }
    }
  } else if (bh1750PermanentOffline && timeDiff(now, bh1750RetryTime) > SENSOR_RETRY_INTERVAL) {
    if (lightMeter.init()) {
      bh1750Available = true;
      bh1750PermanentOffline = false;
      bh1750ErrorCount = 0;
      Serial.println("[恢复] BH1750 已恢复在线！");
    } else {
      bh1750RetryTime = now;
    }
  }

  // 3. DS18B20 采集
  if (ds18b20Available) {
    sensors.requestTemperatures();
    float t = sensors.getTempCByIndex(0);
    if (t != -127.0 && t > -40.0 && t < 85.0) {
      soilTemp = t;
      ds18b20ErrorCount = 0;
    } else {
      ds18b20ErrorCount++;
      if (ds18b20ErrorCount >= MAX_ALLOWED_ERRORS) {
        Serial.printf("[警告] DS18B20 读取失败 (%d/%d)，标记为永久离线。\n", ds18b20ErrorCount, MAX_ALLOWED_ERRORS);
        ds18b20Available = false;
        ds18b20PermanentOffline = true;
        ds18b20RetryTime = now;
        Serial.println("[警告] DS18B20 已永久离线，每小时重试一次。");
      } else {
        if (ds18b20ErrorCount == 1) {
          Serial.printf("[警告] DS18B20 读取失败 (%d/%d)\n", ds18b20ErrorCount, MAX_ALLOWED_ERRORS);
        }
        ds18b20RetryTime = now;
      }
    }
  } else if (ds18b20PermanentOffline && timeDiff(now, ds18b20RetryTime) > SENSOR_RETRY_INTERVAL) {
    sensors.begin();
    if (sensors.getDeviceCount() > 0) {
      sensors.requestTemperatures();
      float t = sensors.getTempCByIndex(0);
      if (t != -127.0 && t > -40.0 && t < 85.0) {
        ds18b20Available = true;
        ds18b20PermanentOffline = false;
        ds18b20ErrorCount = 0;
        soilTemp = t;
        Serial.println("[恢复] DS18B20 已恢复在线！");
      } else {
        ds18b20RetryTime = now;
      }
    } else {
      ds18b20RetryTime = now;
    }
  }

  // 4. 土壤湿度 ADC
  int adcVal = analogRead(PinConfig::SOIL_MOIST_PIN);
  if (adcVal >= SOIL_ADC_WET && adcVal <= SOIL_ADC_DRY) {
    soilMoist = map(adcVal, SOIL_ADC_DRY, SOIL_ADC_WET, 0, 100);
    soilMoist = constrain(soilMoist, 0, 100);
  } else {
    soilMoist = -1;
  }

  // 5. MH-Z19B 二氧化碳
  if (mhz19Available) {
    int c = safeGetCO2();
    if (c > 0) {
      co2 = c;
      mhz19ErrorCount = 0;
    } else {
      mhz19ErrorCount++;
      if (mhz19ErrorCount >= MAX_ALLOWED_ERRORS) {
        Serial.printf("[警告] MH-Z19B 读取失败 (%d/%d)，标记为永久离线。\n", mhz19ErrorCount, MAX_ALLOWED_ERRORS);
        mhz19Available = false;
        mhz19PermanentOffline = true;
        mhz19RetryTime = now;
        Serial.println("[警告] MH-Z19B 已永久离线，每小时重试一次。");
      } else {
        if (mhz19ErrorCount == 1) {
          Serial.printf("[警告] MH-Z19B 读取失败 (%d/%d)\n", mhz19ErrorCount, MAX_ALLOWED_ERRORS);
        }
        mhz19RetryTime = now;
      }
    }
  } else if (mhz19PermanentOffline && timeDiff(now, mhz19RetryTime) > SENSOR_RETRY_INTERVAL) {
    co2Serial.end();
    co2Serial.begin(9600, SERIAL_8N1, PinConfig::CO2_RX_PIN, PinConfig::CO2_TX_PIN);
    mhz19.begin(co2Serial);
    int c = safeGetCO2();
    if (c > 0) {
      mhz19Available = true;
      mhz19PermanentOffline = false;
      mhz19ErrorCount = 0;
      co2 = c;
      Serial.println("[恢复] MH-Z19B 已恢复在线！");
    } else {
      mhz19RetryTime = now;
    }
  }

  // 6. RS485 土壤电导率(EC)
  if (modbusAvailable) {
    uint8_t result = node.readHoldingRegisters(0x0002, 1);
    if (result == node.ku8MBSuccess) {
      soilEC = node.getResponseBuffer(0x00) / 100.0;
      modbusErrorCount = 0;
    } else {
      modbusErrorCount++;
      if (modbusErrorCount >= MAX_ALLOWED_ERRORS) {
        Serial.printf("[警告] Modbus 读取失败 (%d/%d)，标记为永久离线。\n", modbusErrorCount, MAX_ALLOWED_ERRORS);
        modbusAvailable = false;
        modbusPermanentOffline = true;
        modbusRetryTime = now;
        Serial.println("[警告] Modbus 已永久离线，每小时重试一次。");
      } else {
        if (modbusErrorCount == 1) {
          Serial.printf("[警告] Modbus 读取失败 (%d/%d)\n", modbusErrorCount, MAX_ALLOWED_ERRORS);
        }
        modbusRetryTime = now;
      }
    }
  } else if (modbusPermanentOffline && timeDiff(now, modbusRetryTime) > SENSOR_RETRY_INTERVAL) {
    modbusSerial.end();
    modbusSerial.begin(9600, SERIAL_8N1, PinConfig::RS485_RX_PIN, PinConfig::RS485_TX_PIN);
    node.begin(1, modbusSerial);
    if (node.readHoldingRegisters(0x0002, 1) == node.ku8MBSuccess) {
      modbusAvailable = true;
      modbusPermanentOffline = false;
      modbusErrorCount = 0;
      soilEC = node.getResponseBuffer(0x00) / 100.0;
      Serial.println("[恢复] Modbus 已恢复在线！");
    } else {
      modbusRetryTime = now;
    }
  }
}

// ==================== 主程序入口 ====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n========== ESP32 启动初始化 ==========");

  // 先配置基础 Wi-Fi 模式
  WiFi.mode(WIFI_STA);
  initWiFi();

  // Wi-Fi 连接成功后再初始化看门狗
  esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true);
  esp_task_wdt_add(NULL);

  // 初始化引脚
  pinMode(PinConfig::RE_DE_PIN, OUTPUT);
  digitalWrite(PinConfig::RE_DE_PIN, LOW);

  // 初始化总线与硬件
  Wire.begin(PinConfig::I2C_SDA, PinConfig::I2C_SCL);
  Wire.setTimeOut(150);

  if (lightMeter.init()) bh1750Available = true;
  if (sht31.begin(0x44)) sht31Available = true;

  sensors.begin();
  if (sensors.getDeviceCount() > 0) ds18b20Available = true;

  co2Serial.begin(9600, SERIAL_8N1, PinConfig::CO2_RX_PIN, PinConfig::CO2_TX_PIN);
  mhz19.begin(co2Serial);
  mhz19.autoCalibration(false);
  if (safeGetCO2() >= 0) mhz19Available = true;

  modbusSerial.begin(9600, SERIAL_8N1, PinConfig::RS485_RX_PIN, PinConfig::RS485_TX_PIN);
  node.begin(1, modbusSerial);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);
  if (node.readHoldingRegisters(0x0002, 1) == node.ku8MBSuccess) modbusAvailable = true;

  requestTimeSync();
  lastNtpSyncTime = millis();
  ntpRequested = true;

  client.setServer(mqtt_server, mqtt_port);
  client.setBufferSize(1500);
  // 缩短 MQTT Keepalive 以匹配 15 秒上传周期，更快发现断连
  client.setKeepAlive(15);
#if MQTT_TLS_ENABLED
  espClient.setInsecure();
#endif

  initRetryTimers();
  printSensorStatus();
  Serial.println("[System] 系统初始化顺利完成！");
}

void loop() {
  esp_task_wdt_reset();

  unsigned long currentMillis = millis();
  bool isWifiConnected = (WiFi.status() == WL_CONNECTED);

  if (isWifiConnected && !wasWifiConnected) {
    Serial.println("[网络] Wi-Fi 自动重新连接成功。");
    requestTimeSync();
    lastNtpSyncTime = currentMillis;
    ntpRequested = true;
    ntpSuccess = false;
  }
  wasWifiConnected = isWifiConnected;

  if (isWifiConnected && !ntpSuccess) {
    if (checkNtpStatus()) {
      ntpSuccess = true;
    } else if (ntpRequested && timeDiff(currentMillis, lastNtpSyncTime) > 15000) {
      Serial.println("[NTP] 超时未同步，重新发送请求...");
      requestTimeSync();
      lastNtpSyncTime = currentMillis;
    }
  }

  if (!isWifiConnected) {
    if (timeDiff(currentMillis, lastWifiReconnectTime) > wifiReconnectInterval) {
      Serial.printf("[网络] 检测到 Wi-Fi 离线，%lu ms 后尝试重连...\n", wifiReconnectInterval);
      WiFi.begin();
      lastWifiReconnectTime = currentMillis;
      wifiReconnectInterval = (wifiReconnectInterval > 150000) ? 300000 : wifiReconnectInterval * 2;
    }
  } else {
    wifiReconnectInterval = 30000;
  }

  if (isWifiConnected && ntpSuccess) {
    if (!client.connected()) {
      if (timeDiff(currentMillis, mqttReconnectTime) > 10000) {
        mqttReconnectTime = currentMillis;
        char clientId[40];
        snprintf(clientId, sizeof(clientId), "%s_%u", DEVICE_NAME, (uint32_t)ESP.getEfuseMac());
        Serial.print("[MQTT] 正在尝试连接 Broker... ");
        if (client.connect(clientId)) {
          Serial.println("成功！");
        } else {
          Serial.printf("失败, rc=%d\n", client.state());
        }
      }
    } else {
      client.loop();
    }
  }

  // 每 15 秒核心周期：采集和上传
  if (timeDiff(currentMillis, lastUploadTime) >= 15000) {
    lastUploadTime = currentMillis;

    readSensors();

    if (isWifiConnected && ntpSuccess && client.connected()) {
      // 优化：定义局部辅助字符缓冲区，避免使用 String 产生临时内存野指针
      char strAirTemp[16] = "null", strAirHum[16] = "null", strSoilTemp[16] = "null";
      char strSoilMoist[16] = "null", strLux[16] = "null", strCo2[16] = "null", strSoilEc[16] = "null";

      if (sht31Available && airTemp > -90.0) snprintf(strAirTemp, sizeof(strAirTemp), "%.2f", airTemp);
      if (sht31Available && airHum > 0.0)    snprintf(strAirHum, sizeof(strAirHum), "%.2f", airHum);
      if (ds18b20Available && soilTemp > -90.0) snprintf(strSoilTemp, sizeof(strSoilTemp), "%.2f", soilTemp);
      if (soilMoist >= 0)                    snprintf(strSoilMoist, sizeof(strSoilMoist), "%d", soilMoist);
      if (bh1750Available && lux >= 0.0)     snprintf(strLux, sizeof(strLux), "%.2f", lux);
      if (mhz19Available && co2 > 0)         snprintf(strCo2, sizeof(strCo2), "%d", co2);
      if (modbusAvailable && soilEC >= 0.0)   snprintf(strSoilEc, sizeof(strSoilEc), "%.2f", soilEC);

      // 第一步：构建用于签名的 Payload 原始串
      char payloadBuffer[512];
      snprintf(payloadBuffer, sizeof(payloadBuffer),
        "{\"device_id\":\"%s\",\"device_type\":\"%s\",\"timestamp\":%lld,"
        "\"data\":{\"air_temperature\":%s,\"air_humidity\":%s,\"soil_temperature\":%s,"
        "\"soil_moisture\":%s,\"lux\":%s,\"co2\":%s,\"soil_ec\":%s}}",
        DEVICE_NAME, DEVICE_TYPE, (long long)time(NULL),
        strAirTemp, strAirHum, strSoilTemp, strSoilMoist, strLux, strCo2, strSoilEc
      );

      // 第二步：生成加密签名（Base64 输出到 768 字节缓冲区）
      char signBuffer[768];
      generateAESSignature(payloadBuffer, SECRET_TOKEN, signBuffer);

      // 第三步：生成最终要发送的完整 JSON
      char finalPublishBuffer[1536];
      snprintf(finalPublishBuffer, sizeof(finalPublishBuffer),
        "{\"device_id\":\"%s\",\"device_type\":\"%s\",\"timestamp\":%lld,"
        "\"data\":{\"air_temperature\":%s,\"air_humidity\":%s,\"soil_temperature\":%s,"
        "\"soil_moisture\":%s,\"lux\":%s,\"co2\":%s,\"soil_ec\":%s},\"sign\":\"%s\"}",
        DEVICE_NAME, DEVICE_TYPE, (long long)time(NULL),
        strAirTemp, strAirHum, strSoilTemp, strSoilMoist, strLux, strCo2, strSoilEc,
        signBuffer
      );

      if (client.publish(MQTT_PUB_TOPIC, finalPublishBuffer)) {
        Serial.printf("[MQTT] 数据成功报送 -> 长度: %d\n", strlen(finalPublishBuffer));
      } else {
        Serial.println("[错误] MQTT 缓冲区溢出或链路断开，发布失败。");
      }
    } else {
      Serial.println("[System] 通信未就绪，本次周期不发送数据。");
    }
  }
}

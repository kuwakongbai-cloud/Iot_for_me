# ESP32-S3 AIoT 边缘智慧农业监控节点

本项目是一款基于 **ESP32-S3 (N16R8)** 芯片构建的工业级边缘智能农业数据采集终端。系统集成多种高精度传感器，采用非 TLS 轻量级 MQTT 通信。为了解决传统 `1883` 端口明文传输容易遭受中间人攻击（MITM）和数据篡改的问题，本项目在边缘端创新性地引入了 **AES-256-CBC 动态加签机制** 与 **NTP 时间戳防重放校验**，在确保单片机低内存与低能耗运行的同时，提供了金融级的安全数据链路保障。

---

## 项目核心特性

* **多维度环境感知**：全面覆盖大气温湿度、光照强度、CO2 浓度、土壤温度、土壤电导率（EC）以及土壤水分等多物理量采集。
* **端侧硬件级安全防御**：
    * **AES-256-CBC 签名机制**：对采集数据及时间戳进行硬件级 AES 加密并转化为 Base64 签名，随报文一同上传，防止数据中间人篡改。
    * **防重放攻击**：强制引入 NTP 毫秒级网络时间戳参与加签，若前后两次报文时间窗口异常或早于特定基准线，服务器可直接拦截。
* **工业级鲁棒性架构**：
    * **硬件看门狗（WDT）**：15秒硬件守护，防止极端环境下突发死机，系统超时自动复位。
    * **传感器容错与永久离线隔离**：独立软件计数器追踪各类传感器健康状态。连续读取失败 3 次即标记为“离线”，并开启 1 小时阶梯式异步重试机制，避免死循环阻塞主线程。
    * **零字符串内存优化**：抛弃 Arduino 传统的 `String` 动态拼接，全流程采用静态字符缓冲区 `snprintf`，彻底杜绝碎片化引起的内存野指针和内存泄漏。
* **网络动态自愈**：利用 `WiFiManager` 动态配网；网络意外中断时，启动动态指数退避算法（30秒~5分钟）尝试重连，避免网络风暴。

---

## 硬件架构与引脚拓扑

### 1. 核心控制器
* **MCU**: ESP32-S3-WROOM-1 (N16R8: 16MB Flash / 8MB PSRAM)

### 2. 传感器总线清单
| 传感器型号 | 监测物理量 | 通信协议/接口 | ESP32-S3 默认引脚 (PinConfig) |
| :--- | :--- | :--- | :--- |
| **SHT-31** | 环境温度、环境湿度 | I2C (0x44) | SDA: `IO21` / SCL: `IO20` |
| **BH1750** | 环境光照强度 | I2C (0x23) | 共享 I2C 总线 |
| **DS18B20** | 土壤温度 | OneWire 单总线 | DATA: `IO4` |
| **MH-Z19B** | 环境二氧化碳 (CO2) | HardwareSerial1 | RX: `IO16` / TX: `IO17` |
| **RS485 传感器** | 土壤电导率 (EC) | HardwareSerial2 + ModbusRTU | RX: `IO18` / TX: `IO19` / RE_DE: `IO32` |
| **土壤水分传感器** | 土壤相对含水量 | 模拟量 (ADC) | A0: `IO36` |

---

## 安全加签与防篡改逻辑

由于系统默认运行在 `1883` 免密或未启用 TLS 的明文 MQTT 端口上，为了抵御中间人攻击，本项目在客户端与服务器端设计了对称加密双向核验机制：

### 加密流程图解
1. **数据打包**：将传感器数值与当前 NTP UTC 时间戳拼接，生成原始明文 JSON 字符串：`Payload_A`。
2. **IV 混合与加密**：通过硬件随机数发生器（RNG）生成 16 字节随机初始化向量（IV），使用 32 字节的 `SECRET_TOKEN` 作为密钥，采用 **AES-256-CBC** 算法对 `Payload_A` 进行加密。
3. **加签输出**：将 `[16字节IV] + [加密密文]` 组合后进行标准 **Base64 编码**，生成最终的签名 `sign`。
4. **最终报文**：将 `Payload_A` 的内容与 `sign` 组合发送。

### 数据报文示例
MQTT 报文每 15 秒上报一次，发布至主题 `agri/sensor/data`：

```json
{
  "device_id": "esp32_s3_n16r8_001",
  "device_type": "agriculture_sensor",
  "timestamp": 1781253400,
  "data": {
    "air_temperature": 26.54,
    "air_humidity": 62.30,
    "soil_temperature": 22.15,
    "soil_moisture": 45,
    "lux": 12500.00,
    "co2": 450,
    "soil_ec": 1.24
  },
  "sign": "u7G3fK9X...[此处为16字节IV与密文混合后的Base64编码签名]..."
}
```

#### 后端服务器核验逻辑
当服务器在 1883 端口收到上述报文后，可按以下步骤进行核验，核验失败则直接丢弃：
1. **时间戳防重放检查：** 判断报文中的 `timestamp` 是否在允许的时间窗口内。若超出范围或小于固定的历史基准线`（1704067200）`，直接视为非法重放报文。
2. **提取并解码签名：** 将最终报文中的` sign `字段进行 `Base64` 反解码，拆分出前` 16 `字节作为后续字节作为密文。
3. **解密并还原：** 服务器使用保存在后端的同款` 32 `字节 `SECRET_TOKEN` 密钥，对密文进行 AES-256-CBC 解密。
4. **强一致性对比：** 核验解密出来的` JSON `字符串内容是否与明文` Data `区段完全一致。若一致，则说明数据未被中间人篡改或截获重放。

## 固件部署指南
### 1. 开发环境准备
本项目基于 PlatformIO 平台开发（强烈推荐），亦可兼容 Arduino IDE。
核心依赖库依赖列表：

`WiFiManager by tzapu`

`PubSubClient by Nick O'Leary`

`Adafruit SHT31 Library`

`DallasTemperature & OneWire`

`BH1750_WE`

`MHZ19 by Wouter van Marle`

`ModbusMaster by Doc Walker`
### 2. 机密凭证存储
为了代码安全，严禁在 main.cpp 中硬编码任何密钥。你必须在项目的`src`目录下创建一个 `credentials.h` 文件，并配置以下内容：
```C++
#ifndef CREDENTIALS_H
#define CREDENTIALS_H

// 强制：SECRET_TOKEN 必须是 32 字节字符串，用作 AES-256 的密钥
#define SECRET_TOKEN "YOUR_32_BYTE_SUPER_SECRET_KEY_!!"

// WiFiManager 的初始化配置热点密码
#define WIFI_PASSWORD "Admin123456"

#endif
```
### 3. 配置编译与烧录
请确认您在在 `platformio.ini` 中确保启用了 `PSRAM` 并正确选择了板型：
```
[env:esp32-s3-devkitc-1]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200
board_build.arduino.memory_type = qio_opi
board_build.f_flash = 80000000L
build_flags =
    -DBOARD_HAS_PSRAM
```
## 运维与调试日志说明
项目内置完整的硬件自检与运行日志输出（波特率 115200）：

1. **启动自检：** 初始化时将自检总线并打印 `====== 传感器硬件自检状态 ======` 表格。

2. **故障隔离：** 任何单路传感器连续读取失败 `3` 次上限后，串口均会发出 `[警告]` 提示并标记为永久离线，阶梯式每小时重试一次。

3. **通信状态监测：** 实时输出`网络`、`NTP` 同步状态 `[NTP]` 时间同步完成，以及数据上报状态 `[MQTT] 数据成功报送 -> 长度: XXX。`











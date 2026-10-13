// Nordic UART Service 桥 —— 本板改用 NimBLE-Arduino 而非内核自带的 Bluedroid。
//
// 换栈的原因（实测数据，非推测）：
//   ESP32-C3 无 PSRAM，启动可用堆约 228.7KB。满屏 16 位精灵图占 153.6KB，
//   Bluedroid 再吃掉约 72KB，剩余仅 2.9KB —— 不足以容纳协议行（单行上限 4KB）
//   与 JSON 解析。NimBLE 在 C3 上只需约 15-20KB。
//
// 本文件与上游 claude-desktop-buddy 的差异（上游用 Bluedroid API）：
//   1. 安全配置：Bluedroid 用 BLESecurity 对象 + BLESecurityCallbacks 类；
//      NimBLE 用 NimBLEDevice 的静态方法，认证回调挂在 NimBLEServerCallbacks 上。
//   2. 配对码来源：NimBLE-Arduino **没有** onPassKeyNotify 回调，拿不到协议栈
//      生成的随机码。改为由本文件生成随机六位码并经 setSecurityPasskey() 设定，
//      同样显示在设备屏幕上让主机输入，对用户而言行为等价。
//   3. 特征权限：NimBLE 2.x 无 setAccessPermissions，加密要求改由创建特征时的
//      NIMBLE_PROPERTY 标志表达。
//   4. CCCD(2902)：NimBLE 自动创建并按特征属性推导其权限，无法像上游那样
//      单独设权限。上游为此还特意 wrap 了 ble_att_svr_register。
//   5. 绑定清除：Bluedroid 要遍历 esp_ble_get_bond_device_list；NimBLE 直接
//      deleteAllBonds()。

#include "ble_bridge.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <string.h>

// Nordic UART Service UUIDs — every BLE serial example uses these, so
// existing tools (nRF Connect, bluefy, Web Bluetooth examples) can talk to
// us without custom UUIDs.
#define NUS_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

// Incoming bytes are buffered in a simple ring for bleRead()/bleAvailable().
// Sized to hold a transcript snapshot JSON plus headroom; the GATT layer
// will flow-control if we fall behind.
static const size_t RX_CAP = 2048;
static uint8_t  rxBuf[RX_CAP];
static volatile size_t rxHead = 0;
static volatile size_t rxTail = 0;

static NimBLEServer*         server = nullptr;
static NimBLECharacteristic* txChar = nullptr;
static NimBLECharacteristic* rxChar = nullptr;
static volatile bool      connected = false;
static volatile bool      secure = false;
static volatile uint32_t  passkey = 0;
static volatile uint16_t  mtu = 23;

static void rxPush(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    size_t next = (rxHead + 1) % RX_CAP;
    if (next == rxTail) return;  // full — drop (upstream should keep up)
    rxBuf[rxHead] = p[i];
    rxHead = next;
  }
}

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    NimBLEAttValue v = c->getValue();
    if (v.size()) rxPush(v.data(), v.size());
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
    (void)s;
    connected = true;

    // 每次连接换一个新配对码。上游由协议栈生成并经 onPassKeyNotify 回调取得；
    // NimBLE-Arduino 无此回调，故在此生成并写入协议栈（见文件头说明第 2 条）。
    // 设备为 DisplayOnly，主机需输入此码。
    uint32_t pk = esp_random() % 1000000u;
    NimBLEDevice::setSecurityPasskey(pk);
    passkey = pk;

    mtu = connInfo.getMTU();
    Serial.printf("[ble] connected, passkey %06lu\n", (unsigned long)pk);
  }

  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& connInfo, int reason) override {
    (void)s; (void)connInfo; (void)reason;
    connected = false;
    secure = false;
    passkey = 0;
    mtu = 23;
    Serial.println("[ble] disconnected");
    // Restart advertising so the next client can find us.
    NimBLEDevice::startAdvertising();
  }

  void onMTUChange(uint16_t newMtu, NimBLEConnInfo&) override {
    mtu = newMtu;
    Serial.printf("[ble] mtu=%u\n", newMtu);
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    passkey = 0;
    // 与上游判据一致：加密 + 认证 + 已绑定 三者齐备才算安全链路。
    secure = connInfo.isEncrypted() && connInfo.isAuthenticated() && connInfo.isBonded();
    Serial.printf("[ble] auth %s (enc=%d authen=%d bonded=%d)\n",
                  secure ? "ok" : "FAIL",
                  (int)connInfo.isEncrypted(), (int)connInfo.isAuthenticated(),
                  (int)connInfo.isBonded());
    if (!secure && server) server->disconnect(connInfo);
  }
};

void bleInit(const char* deviceName) {
  NimBLEDevice::init(deviceName);
  // Request the biggest MTU we can get. macOS negotiates to 185 typically.
  NimBLEDevice::setMTU(517);

  // LE Secure Connections + MITM + 绑定，设备侧 DisplayOnly（显示配对码）。
  // 对应上游的 ESP_LE_AUTH_REQ_SC_MITM_BOND + ESP_IO_CAP_OUT。
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* svc = server->createService(NUS_SERVICE_UUID);

  // 加密要求由属性标志表达（NimBLE 2.x 无 setAccessPermissions）。
  txChar = svc->createCharacteristic(
    NUS_TX_UUID,
    NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC
  );

  rxChar = svc->createCharacteristic(
    NUS_RX_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC
  );
  rxChar->setCallbacks(new RxCallbacks());

  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  // 顺序不能颠倒：NimBLEAdvertising::setName() 会先看扫描响应是否已启用 ——
  // 已启用则把名字放进扫描响应，否则塞进广播包。广播包已被 128 位服务 UUID
  // 占去大半，只剩约 10 字节装不下 "Claude-XXXX"，会静默失败。而 Claude Desktop
  // 的设备列表是按名字前缀 "Claude" 过滤的，丢了名字就选不中设备。
  adv->enableScanResponse(true);
  adv->setName(deviceName);
  adv->setPreferredParams(0x06, 0x12);   // iOS-friendly connection interval
  NimBLEDevice::startAdvertising();
  Serial.printf("[ble] advertising as '%s'\n", deviceName);
}

bool bleConnected() { return connected; }
bool bleSecure()    { return secure; }
uint32_t blePasskey() { return passkey; }

void bleClearBonds() {
  // NimBLE 直接提供清空接口，无需像 Bluedroid 那样遍历 bond 列表。
  int n = NimBLEDevice::getNumBonds();
  NimBLEDevice::deleteAllBonds();
  Serial.printf("[ble] cleared %d bond(s)\n", n);
}

size_t bleAvailable() {
  return (rxHead + RX_CAP - rxTail) % RX_CAP;
}

int bleRead() {
  if (rxHead == rxTail) return -1;
  int b = rxBuf[rxTail];
  rxTail = (rxTail + 1) % RX_CAP;
  return b;
}

size_t bleWrite(const uint8_t* data, size_t len) {
  if (!connected || !txChar) return 0;
  // ATT notify payload is limited to (MTU - 3). macOS negotiates 185, so
  // the 182-byte chunk works there; use the live mtu so a peer that caps
  // at the 23-byte default doesn't get truncated notifies.
  size_t chunk = mtu > 3 ? mtu - 3 : 20;
  if (chunk > 180) chunk = 180;
  size_t sent = 0;
  while (sent < len) {
    size_t n = len - sent;
    if (n > chunk) n = chunk;
    txChar->setValue((uint8_t*)(data + sent), n);
    txChar->notify();
    sent += n;
    // Small yield so the BLE stack flushes before the next chunk.
    delay(4);
  }
  return sent;
}

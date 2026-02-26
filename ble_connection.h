#ifndef BLE_CONNECTION_H
#define BLE_CONNECTION_H

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <BLEAdvertisedDevice.h>

// Отладка BLE: 1 = подробный лог с метками времени, 0 = только ключевые сообщения
#define BLE_DEBUG 0

#define BLE_LOG(fmt, ...) do { Serial.printf("[BLE %lu] ", (unsigned long)millis()); Serial.printf(fmt, ##__VA_ARGS__); } while(0)
#define BLE_DEBUG_LOG(fmt, ...) do { if (BLE_DEBUG) { Serial.printf("[BLE %lu] ", (unsigned long)millis()); Serial.printf(fmt, ##__VA_ARGS__); } } while(0)

// UUID для HID сервиса и характеристик
static BLEUUID serviceUUID("1812");
static BLEUUID charUUID("2A4D");       // HID Report (notify)
static BLEUUID hidControlPointUUID("2A4C");  // HID Control Point: 0x01 = Exit Suspend (начать слать отчёты)

// Внешние переменные для доступа из main
extern boolean connected;
extern uint8_t channelData[20];  // Увеличили до 20 байт
extern void processJoystickData(uint8_t* data, size_t length);

// Внутренние переменные
static boolean doConnect = false;
static boolean scanning = false;
static BLEAddress *pServerAddress = nullptr;
static BLERemoteCharacteristic* pRemoteCharacteristic = nullptr;
static BLEClient* pClient = nullptr;
static unsigned long lastReconnectAttempt = 0;
static unsigned long lastDataReceived = 0;   // время последнего пакета от пульта
static unsigned long connectStartMs = 0;     // момент начала текущего подключения (для отладки)
static int connectAttempts = 0;
static unsigned long cooldownUntil = 0;      // до этого времени не сканируем
static uint32_t notifyPacketCount = 0;       // счётчик пакетов в notifyCallback
#define NO_DATA_TIMEOUT_MS   5000
#define BLE_COOLDOWN_MS      2000

// Состояния подключения
enum ConnectionState {
    CONN_IDLE,
    CONN_WAITING_FOR_SERVICES,
    CONN_GETTING_SERVICE,
    CONN_GETTING_CHARACTERISTIC,
    CONN_SUBSCRIBING,
    CONN_WAITING_FOR_DATA,
    CONN_COMPLETE,
    CONN_FAILED
};

static ConnectionState connState = CONN_IDLE;
static unsigned long connTimer = 0;
static BLERemoteService* pRemoteService = nullptr;

// Callback для уведомлений
void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic,
                    uint8_t* pData, size_t length, bool isNotify) {
    notifyPacketCount++;
    lastDataReceived = millis();
    if (BLE_DEBUG) {
        if (notifyPacketCount <= 3 || (notifyPacketCount % 100 == 0))
            BLE_DEBUG_LOG("NOTIFY #%u len=%u isNotify=%d\n", (unsigned)notifyPacketCount, (unsigned)length, isNotify ? 1 : 0);
        if (notifyPacketCount <= 2 && length > 0) {
            Serial.print("      HEX: ");
            for (size_t i = 0; i < length && i < 20; i++) Serial.printf("%02X ", pData[i]);
            Serial.println();
        }
    }
    if (length >= 20) {
        memcpy(channelData, pData, 20);
        processJoystickData(pData, length);
    } else if (length > 0) {
        memcpy(channelData, pData, length);
        processJoystickData(pData, length);
    }
}

// Callback для поиска устройств
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
        Serial.printf("\n📡 Найдено устройство: %s\n", advertisedDevice.toString().c_str());
        
        if (advertisedDevice.haveName()) {
            Serial.printf("   Имя: \"%s\"\n", advertisedDevice.getName().c_str());
            
            if (strcmp(advertisedDevice.getName().c_str(), "ExpressLRS Joystick") == 0) {
                Serial.println("✅ ЭТО НАШ ПУЛЬТ!");
                Serial.printf("   RSSI: %d dBm\n", advertisedDevice.getRSSI());
                Serial.printf("   MAC: %s\n", advertisedDevice.getAddress().toString().c_str());
                
                if (advertisedDevice.haveServiceUUID()) {
                    Serial.printf("   Сервис: %s\n", advertisedDevice.getServiceUUID().toString().c_str());
                }
                
                advertisedDevice.getScan()->stop();
                scanning = false;
                pServerAddress = new BLEAddress(advertisedDevice.getAddress());
                doConnect = true;
            }
        }
    }
    
    void onScanEnd(bool success) {
        BLE_LOG("Сканирование завершено, success=%d\n", success ? 1 : 0);
        scanning = false;
    }
};

// Функция для получения MAC адреса ESP32
void printESP32Mac() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    Serial.printf("ESP32 MAC (BT): %02X:%02X:%02X:%02X:%02X:%02X\n", 
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Функция начала подключения
void startConnection() {
    connectAttempts++;
    connectStartMs = millis();
    notifyPacketCount = 0;
    lastDataReceived = 0;  // сброс: учитываем только пакеты этой сессии
    BLE_LOG("Попытка подключения #%d, цель %s\n", connectAttempts, pServerAddress->toString().c_str());
    
    if (pClient != nullptr) {
        BLE_DEBUG_LOG("Закрываем предыдущий клиент...\n");
        pClient->disconnect();
        delete pClient;
        pClient = nullptr;
        delay(500);
    }
    
    BLE_DEBUG_LOG("createClient()\n");
    pClient = BLEDevice::createClient();
    if (!pClient) {
        BLE_LOG("Ошибка: createClient вернул NULL\n");
        connState = CONN_FAILED;
        return;
    }
    
    BLE_DEBUG_LOG("connect() к %s\n", pServerAddress->toString().c_str());
    unsigned long t0 = millis();
    bool ok = pClient->connect(*pServerAddress);
    unsigned long dt = millis() - t0;
    BLE_LOG("connect() вернул %s за %lu мс\n", ok ? "true" : "false", dt);
    if (!ok) {
        delete pClient;
        pClient = nullptr;
        connState = CONN_FAILED;
        return;
    }
    
    BLE_DEBUG_LOG("Ожидание сервисов 300 мс\n");
    connState = CONN_WAITING_FOR_SERVICES;
    connTimer = millis();
}

// Функция обновления состояния подключения
void updateConnection() {
    if (!doConnect || scanning) return;
    
    switch (connState) {
        case CONN_IDLE:
            startConnection();
            break;
            
        case CONN_WAITING_FOR_SERVICES:
            if (millis() - connTimer >= 300) {
                BLE_DEBUG_LOG("getService(1812)\n");
                connState = CONN_GETTING_SERVICE;
                connTimer = millis();
            }
            break;
            
        case CONN_GETTING_SERVICE: {
            unsigned long t0 = millis();
            pRemoteService = pClient->getService(serviceUUID);
            unsigned long dt = millis() - t0;
            if (pRemoteService != nullptr) {
                BLE_LOG("getService() OK за %lu мс\n", dt);
                connState = CONN_GETTING_CHARACTERISTIC;
                connTimer = millis();
            } else if (millis() - connTimer >= 8000) {
                BLE_LOG("getService() NULL (таймаут 8 с)\n");
                pClient->disconnect();
                connState = CONN_FAILED;
            }
            break;
        }
            
        case CONN_GETTING_CHARACTERISTIC: {
            static unsigned long lastCharTry = 0;
            if (millis() - connTimer < 200) break;
            unsigned long t0 = millis();
            pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
            unsigned long dt = millis() - t0;
            BLE_DEBUG_LOG("getCharacteristic(2A4D) за %lu мс -> %s\n", dt, pRemoteCharacteristic ? "OK" : "NULL");
            if (pRemoteCharacteristic == nullptr && (lastCharTry == 0 || millis() - lastCharTry > 1500)) {
                lastCharTry = millis();
                BLE_LOG("getCharacteristics() для списка...\n");
                t0 = millis();
                std::map<std::string, BLERemoteCharacteristic*>* pChars = pRemoteService->getCharacteristics();
                dt = millis() - t0;
                BLE_LOG("getCharacteristics() за %lu мс, элементов: %d\n", dt, pChars ? (int)pChars->size() : 0);
                if (pChars && !pChars->empty()) {
                    for (auto it = pChars->begin(); it != pChars->end(); ++it) {
                        BLERemoteCharacteristic* c = it->second;
                        BLE_LOG("   char %s R=%d N=%d\n", c->getUUID().toString().c_str(), c->canRead(), c->canNotify());
                        if (pRemoteCharacteristic == nullptr && c->canNotify()) pRemoteCharacteristic = c;
                        if (c->getUUID().toString() == charUUID.toString()) pRemoteCharacteristic = c;
                    }
                    if (pRemoteCharacteristic)
                        BLE_LOG("Выбрана характеристика %s\n", pRemoteCharacteristic->getUUID().toString().c_str());
                }
            }
            if (pRemoteCharacteristic != nullptr) {
                lastCharTry = 0;
                BLE_LOG("Характеристика: READ=%d NOTIFY=%d -> SUBSCRIBING\n", pRemoteCharacteristic->canRead(), pRemoteCharacteristic->canNotify());
                connState = CONN_SUBSCRIBING;
            } else if (millis() - connTimer >= 8000) {
                lastCharTry = 0;
                BLE_LOG("Характеристика не найдена (таймаут)\n");
                pClient->disconnect();
                connState = CONN_FAILED;
            }
            break;
        }
            
        case CONN_SUBSCRIBING: {
            if (pRemoteCharacteristic->canNotify()) {
                // 1) HID Control Point 0x2A4C: 0x01 = Exit Suspend — просим устройство начать слать отчёты
                BLERemoteCharacteristic* pCtrl = pRemoteService->getCharacteristic(hidControlPointUUID);
                if (pCtrl && pCtrl->canWrite()) {
                    uint8_t exitSuspend = 0x01;
                    BLE_LOG("HID Control Point (0x2A4C): пишем 0x01 (Exit Suspend)...\n");
                    pCtrl->writeValue(&exitSuspend, 1, true);
                    delay(80);
                } else {
                    BLE_DEBUG_LOG("0x2A4C не найден или только read — пропуск\n");
                }
                // 2) Подписка: сначала выключить, пауза, затем включить (переподписка)
                BLERemoteDescriptor* pDesc = pRemoteCharacteristic->getDescriptor(BLEUUID((uint16_t)0x2902));
                if (pDesc) {
                    uint8_t off[2] = {0x00, 0x00};
                    uint8_t on[2]  = {0x01, 0x00};
                    BLE_LOG("CCCD: выкл -> пауза 200 мс -> вкл\n");
                    pDesc->writeValue(off, 2, true);
                    delay(200);
                    pDesc->writeValue(on, 2, true);
                    delay(80);
                }
                BLE_DEBUG_LOG("registerForNotify()\n");
                pRemoteCharacteristic->registerForNotify(notifyCallback);
                if (pRemoteCharacteristic->canRead()) {
                    delay(50);
                    BLE_DEBUG_LOG("readValue()...\n");
                    std::string v = pRemoteCharacteristic->readValue();
                    BLE_LOG("  readValue() = %d байт\n", (int)v.length());
                }
                BLE_LOG("Подписка включена, ждём данные (5 с)\n");
                connState = CONN_WAITING_FOR_DATA;
                connTimer = millis();
            } else {
                BLE_LOG("Характеристика без NOTIFY\n");
                pClient->disconnect();
                connState = CONN_FAILED;
            }
            break;
        }
            
        case CONN_WAITING_FOR_DATA: {
            unsigned long waitMs = millis() - connTimer;
            if (notifyPacketCount > 0) {
                BLE_LOG("Данные получены (NOTIFY #%u), связь установлена\n", (unsigned)notifyPacketCount);
                connState = CONN_COMPLETE;
            } else if (waitMs >= 5000) {
                BLE_LOG("Нет данных за 5 с — пульт не шлёт пакеты.\n");
                Serial.println(F("\n>>> ПЕРЕЗАПУСТИТЕ BLE JOYSTICK НА ПУЛЬТЕ (выкл/вкл в настройках) <<<\n"));
                connState = CONN_COMPLETE;
            } else if (BLE_DEBUG) {
                static unsigned long lastWaitLog = 0;
                unsigned long sec = waitMs / 1000;
                if (sec > 0 && (waitMs - lastWaitLog >= 900 || lastWaitLog == 0)) {
                    lastWaitLog = waitMs;
                    BLE_DEBUG_LOG("ожидание данных... %lu с, пакетов: %u\n", sec, (unsigned)notifyPacketCount);
                }
            }
            break;
        }
            
        case CONN_COMPLETE:
            BLE_LOG("CONN_COMPLETE. Пакетов от пульта: %u, время от connect: %lu мс\n", (unsigned)notifyPacketCount, millis() - connectStartMs);
            connected = true;
            connectStartMs = millis();  // для таймаута «подключено, но 0 пакетов»
            connectAttempts = 0;
            connState = CONN_IDLE;
            doConnect = false;
            lastDataReceived = notifyPacketCount > 0 ? millis() : 0;
            break;
            
        case CONN_FAILED:
            BLE_LOG("CONN_FAILED, попыток: %d\n", connectAttempts);
            if (connectAttempts >= 3) {
                connectAttempts = 0;
                doConnect = false;
            }
            connState = CONN_IDLE;
            delay(2000);
            break;
    }
}

// Функция инициализации BLE
void initBLE() {
    Serial.println("\n🔄 Инициализация BLE...");
    BLEDevice::init("ESP32-C3-Listener");
    printESP32Mac();
    BLEDevice::setPower(ESP_PWR_LVL_P9);
    BLE_LOG("BLE инициализирован (BLE_DEBUG=%d)\n", BLE_DEBUG);
}

// Функция запуска сканирования
void startScanning() {
    if (scanning) return;
    
    BLE_LOG("Сканирование BLE (10 с)...\n");
    scanning = true;
    
    BLEScan* pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(500);
    pBLEScan->setWindow(250);
    pBLEScan->start(10, false);
}

// Функция для вызова в loop()
void handleBLEConnection() {
    if (!connected) {
        if (cooldownUntil > 0 && millis() < cooldownUntil) {
            static unsigned long lastCooldownLog = 0;
            if (BLE_DEBUG && millis() - lastCooldownLog > 2000) {
                lastCooldownLog = millis();
                unsigned long left = (cooldownUntil - millis()) / 1000;
                BLE_DEBUG_LOG("Cooldown: осталось %lu с до сканирования\n", left);
            }
        } else {
            cooldownUntil = 0;
            if (millis() - lastReconnectAttempt > 3000) {
                if (!scanning && !doConnect && connState == CONN_IDLE) {
                    startScanning();
                }
                lastReconnectAttempt = millis();
            }
        }
        
        if (doConnect) {
            updateConnection();
        }
    }
    
    if (connected) {
        static unsigned long lastCheck = 0;
        static unsigned long lastDataLog = 0;
        if (millis() - lastCheck > 5000) {
            if (pClient && !pClient->isConnected()) {
                BLE_LOG("Соединение потеряно (isConnected=false)\n");
                connected = false;
                if (pClient) {
                    pClient->disconnect();
                    delete pClient;
                    pClient = nullptr;
                }
                pRemoteService = nullptr;
                pRemoteCharacteristic = nullptr;
                connState = CONN_IDLE;
                cooldownUntil = millis() + BLE_COOLDOWN_MS;
                BLE_LOG("Cooldown %d с до след. скана\n", BLE_COOLDOWN_MS / 1000);
            } else if ((notifyPacketCount == 0 && (millis() - connectStartMs > NO_DATA_TIMEOUT_MS)) ||
                       (lastDataReceived > 0 && (millis() - lastDataReceived > NO_DATA_TIMEOUT_MS))) {
                if (notifyPacketCount == 0)
                    Serial.println(F("\n>>> Нет пакетов от пульта. Перезапустите BLE Joystick на пульте (выкл/вкл) <<<\n"));
                BLE_LOG("Отключаемся, cooldown %d с\n", BLE_COOLDOWN_MS/1000);
                connected = false;
                if (pClient) {
                    pClient->disconnect();
                    delete pClient;
                    pClient = nullptr;
                }
                pRemoteService = nullptr;
                pRemoteCharacteristic = nullptr;
                connState = CONN_IDLE;
                doConnect = false;
                cooldownUntil = millis() + BLE_COOLDOWN_MS;
            }
            lastCheck = millis();
        }
        if (BLE_DEBUG && millis() - lastDataLog > 10000 && lastDataReceived > 0) {
            lastDataLog = millis();
            unsigned long ago = (millis() - lastDataReceived) / 1000;
            BLE_DEBUG_LOG("Статус: подключен, пакетов %u, последний данные %lu с назад\n", (unsigned)notifyPacketCount, ago);
        }
    }
}

#endif
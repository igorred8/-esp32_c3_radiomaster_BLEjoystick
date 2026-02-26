#include "ble_connection.h"
#include "joystick_handler.h"

// Глобальные переменные
boolean connected = false;
uint8_t channelData[20] = {0};

const unsigned long PRINT_INTERVAL_MS = 200;
unsigned long lastPrintMs = 0;

void setup() {
    Serial.begin(115200);
    delay(2000);
    
    Serial.println(F("\nExpressLRS + EdgeTX HID -> ESP32-C3\n"));
    initBLE();
}

void loop() {
    handleBLEConnection();
    
    if (connected && hasJoystickData()) {
        if (millis() - lastPrintMs >= PRINT_INTERVAL_MS) {
            uint8_t ch[12];
            getChannelValues(ch);
            Serial.printf("CH1:%3d CH2:%3d CH3:%3d CH4:%3d CH5:%3d CH6:%3d CH7:%3d CH8:%3d CH9:%3d CH10:%3d CH11:%3d CH12:%3d\n",
                ch[0], ch[1], ch[2], ch[3], ch[4], ch[5], ch[6], ch[7], ch[8], ch[9], ch[10], ch[11]);
            lastPrintMs = millis();
        }
    }
    
    delay(10);
}
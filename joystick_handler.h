#ifndef JOYSTICK_HANDLER_H
#define JOYSTICK_HANDLER_H

#include <Arduino.h>

// Включить отладочный вывод сырых пакетов (HEX, оси) в processJoystickData (1 = вкл, 0 = выкл)
#define JOYSTICK_DEBUG 0

// Внутренние переменные
static uint8_t lastRawData[20];
static size_t lastRawLen = 0;
static int packetCount = 0;
static bool dataReceived = false;

// Последние значения 12 каналов (0..255), обновляются в processJoystickData
static uint8_t channelValues[12] = {0};

// Реальный формат ExpressLRS BLE Joystick: [0] кнопки, [1] пусто;
// [2-3] CH1, [4-5] CH2, [6-7] CH5, [8-9] CH6, [10-11] CH3, [12-13] CH4, [14-15] CH7, [16-17] CH8 (если пакет ≥18 байт)
// Оси: 16-bit LE, диапазон 0..32767 → вывод 0..255
// Оси: 16-bit LE, 0x0000..0xFFFF → 0..255
#define HID_USE_REPORT_ID_OFFSET 0

uint16_t readUint16LE(uint8_t* data, int index) {
    return (uint16_t)(data[index] | (data[index + 1] << 8));
}

// Оси: пульт шлёт 0..32767 (0x7FFF) → вывод 0..255
uint8_t valueToByte(uint16_t val) {
    return (uint8_t)constrain(map(val, 0, 32767, 0, 255), 0, 255);
}

// Диагностика по реальной схеме: [0]=кнопки, [1]=пусто, [2-3]=CH1..[14-15]=CH7, CH8 нет
void printRawDiagnostic(uint8_t* data, size_t len) {
    packetCount++;
    Serial.printf("\n=== ПАКЕТ #%d (%d байт) ===\n", packetCount, len);
    Serial.print("HEX: ");
    for (size_t i = 0; i < len && i < 16; i++) {
        Serial.printf("%02X ", data[i]);
        if (i == 1 || i == 9) Serial.print("| ");
    }
    Serial.println();
    if (len >= 2) Serial.printf("Кнопки[0]=0x%02X (CH9=1,CH10=2) пустой[1]=0x%02X\n", data[0], data[1]);
    if (len >= 16) {
        uint16_t ch1 = readUint16LE(data, 2), ch2 = readUint16LE(data, 4), ch5 = readUint16LE(data, 6);
        uint16_t ch6 = readUint16LE(data, 8), ch3 = readUint16LE(data, 10), ch4 = readUint16LE(data, 12);
        uint16_t ch7 = readUint16LE(data, 14);
        uint16_t ch8 = (len >= 18) ? readUint16LE(data, 16) : 0;
        Serial.println("Оси (0-32767 -> 0-255): CH1..CH8");
        Serial.printf("  CH1:%5u(%3d) CH2:%5u(%3d) CH5:%5u(%3d) CH6:%5u(%3d)\n",
            ch1, valueToByte(ch1), ch2, valueToByte(ch2), ch5, valueToByte(ch5), ch6, valueToByte(ch6));
        Serial.printf("  CH3:%5u(%3d) CH4:%5u(%3d) CH7:%5u(%3d) CH8:%5u(%3d)\n",
            ch3, valueToByte(ch3), ch4, valueToByte(ch4), ch7, valueToByte(ch7), ch8, valueToByte(ch8));
    }
}

// Заполняет массив из 12 значений каналов (0..255). Вызывать из основного скетча.
void getChannelValues(uint8_t out[12]) {
    memcpy(out, channelValues, 12);
}

// Есть ли хотя бы один принятый пакет от пульта
bool hasJoystickData() {
    return dataReceived;
}

// Функция обработки данных. Обновляет channelValues[] для чтения из скетча.
void processJoystickData(uint8_t* data, size_t length) {
    dataReceived = true;
    if (HID_USE_REPORT_ID_OFFSET && length >= 20) {
        size_t payload = (length >= 21) ? 20 : 19;
        lastRawLen = payload;
        memcpy(lastRawData, data + 1, payload);
        if (payload < 20) lastRawData[19] = 0;
    } else {
        lastRawLen = min((size_t)20, length);
        memcpy(lastRawData, data, lastRawLen);
    }
    // Парсим в channelValues[0..11]
    uint8_t btn = lastRawData[0];
    channelValues[0] = valueToByte(readUint16LE(lastRawData, 2));
    channelValues[1] = valueToByte(readUint16LE(lastRawData, 4));
    channelValues[2] = valueToByte(readUint16LE(lastRawData, 10));
    channelValues[3] = valueToByte(readUint16LE(lastRawData, 12));
    channelValues[4] = valueToByte(readUint16LE(lastRawData, 6));
    channelValues[5] = valueToByte(readUint16LE(lastRawData, 8));
    channelValues[6] = valueToByte(readUint16LE(lastRawData, 14));
    channelValues[7] = (lastRawLen >= 18) ? valueToByte(readUint16LE(lastRawData, 16)) : 0;
    channelValues[8]  = (btn & (1 << 0)) ? 255 : 0;
    channelValues[9]  = (btn & (1 << 1)) ? 255 : 0;
    channelValues[10] = (btn & (1 << 2)) ? 255 : 0;
    channelValues[11] = (btn & (1 << 3)) ? 255 : 0;
#if JOYSTICK_DEBUG
    static int debugCount = 0;
    if (debugCount < 5) {
        printRawDiagnostic(lastRawData, 20);
        debugCount++;
    }
#endif
}

#endif
#ifndef BATTERY_MONITOR_H
#define BATTERY_MONITOR_H

#include <Arduino.h>

struct BatteryReading
{
    uint16_t rawAdc;
    uint16_t senseMillivolts;
    uint16_t batteryMillivolts;
    uint8_t percentage;
};

class BatteryMonitor
{
public:
    BatteryMonitor(uint8_t adcPin);

    void begin();

    BatteryReading readBattery();
    float getBatteryVoltage();
    uint8_t getBatteryPercentage();
    static uint8_t voltageToPercentage(float voltage);

private:
    uint8_t _adcPin;

    uint16_t readAveragedADC(uint8_t samples = 16);
    static uint16_t adcToSenseMillivolts(uint16_t adc);
};

#endif

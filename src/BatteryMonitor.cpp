#include "BatteryMonitor.h"

BatteryMonitor::BatteryMonitor(uint8_t adcPin)
{
    _adcPin = adcPin;
}

void BatteryMonitor::begin()
{
    pinMode(_adcPin, INPUT);
}

uint16_t BatteryMonitor::readAveragedADC(uint8_t samples)
{
    uint32_t sum = 0;

    // nRF52 SAADC settings are global, so set them before every battery read.
    analogReference(AR_INTERNAL_3_0);
    analogReadResolution(12);
    delay(1);

    for (uint8_t i = 0; i < samples; i++)
    {
        sum += analogRead(_adcPin);
        delay(2);
    }

    analogReference(AR_DEFAULT);
    analogReadResolution(10);

    return sum / samples;
}

uint16_t BatteryMonitor::adcToSenseMillivolts(uint16_t adc)
{
    return (uint32_t)adc * 3000UL / 4095UL;
}

BatteryReading BatteryMonitor::readBattery()
{
    BatteryReading reading;
    reading.rawAdc = readAveragedADC();
    reading.senseMillivolts = adcToSenseMillivolts(reading.rawAdc);
    reading.batteryMillivolts = reading.senseMillivolts * 2U;
    reading.percentage = voltageToPercentage(reading.batteryMillivolts / 1000.0f);
    return reading;
}

float BatteryMonitor::getBatteryVoltage()
{
    return readBattery().batteryMillivolts / 1000.0f;
}

uint8_t BatteryMonitor::getBatteryPercentage()
{
    return readBattery().percentage;
}

uint8_t BatteryMonitor::voltageToPercentage(float voltage)
{
    if (voltage >= 4.15f) return 100;
    if (voltage >= 4.05f) return 90;
    if (voltage >= 3.95f) return 80;
    if (voltage >= 3.87f) return 70;
    if (voltage >= 3.80f) return 60;
    if (voltage >= 3.74f) return 50;
    if (voltage >= 3.68f) return 40;
    if (voltage >= 3.60f) return 30;
    if (voltage >= 3.50f) return 20;
    if (voltage >= 3.40f) return 10;

    return 0;
}

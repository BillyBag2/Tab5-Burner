#include "battery_math.h"
#include <math.h>
float battery_bus_voltage(uint16_t raw) { return raw * 0.00125f; }
// INA226 shunt LSB = 2.5 uV, Tab5 R39 = 5 milliohms. Positive = discharge.
float battery_shunt_current(uint16_t raw) { return (int16_t)raw * 0.0005f; }
int battery_voltage_percent(float volts)
{
    if (!isfinite(volts) || volts < 1.0f || volts > 9.0f) return -1;
    float level = (volts - 6.0f) / 2.4f * 100.0f;
    if (level < 0) return 0;
    if (level > 100) return 100;
    return (int)(level + 0.5f);
}

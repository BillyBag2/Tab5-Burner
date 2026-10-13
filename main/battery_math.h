#pragma once
#include <stdint.h>
float battery_bus_voltage(uint16_t raw);
float battery_shunt_current(uint16_t raw);
int battery_voltage_percent(float volts);

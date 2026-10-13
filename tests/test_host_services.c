#include "battery_math.h"
#include "storage_gate.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void)
{
    assert(fabsf(battery_bus_voltage(5920) - 7.4f) < 0.001f);
    assert(fabsf(battery_shunt_current(2000) - 1.0f) < 0.001f);
    assert(fabsf(battery_shunt_current((uint16_t)-1000) + 0.5f) < 0.001f);
    assert(battery_voltage_percent(6.0f) == 0);
    assert(battery_voltage_percent(7.2f) == 50);
    assert(battery_voltage_percent(8.4f) == 100);
    assert(battery_voltage_percent(8.6f) == 100);
    assert(battery_voltage_percent(0) == -1);
    assert(battery_voltage_percent(NAN) == -1);
    assert(battery_voltage_percent(INFINITY) == -1);
    storage_gate_t gate = { .mode = STORAGE_LOCAL };
    assert(storage_gate_claim(&gate)); // Flashing or open directory holds local ownership.
    assert(storage_gate_claim(&gate)); // Nested browser lease.
    assert(!storage_gate_switch(&gate, true));
    assert(!storage_gate_remount(&gate));
    storage_gate_release(&gate);
    assert(!storage_gate_switch(&gate, true));
    storage_gate_release(&gate);
    assert(storage_gate_switch(&gate, true));
    assert(!storage_gate_claim(&gate)); // No access while switching.
    assert(!storage_gate_remount(&gate));
    assert(!storage_gate_switch(&gate, false));
    gate.mode = STORAGE_USB;
    assert(!storage_gate_claim(&gate)); // PC exclusively owns the media.
    assert(!storage_gate_remount(&gate));
    assert(!storage_gate_switch(&gate, true));
    assert(storage_gate_switch(&gate, false));
    gate.mode = STORAGE_ERROR; // Failed remount must not grant app access.
    assert(!storage_gate_claim(&gate));
    assert(!storage_gate_switch(&gate, true));
    assert(storage_gate_remount(&gate));
    gate.mode = STORAGE_LOCAL;
    assert(storage_gate_claim(&gate)); // Remount succeeded.
    storage_gate_release(&gate);
    storage_gate_release(&gate); // No unsigned underflow.
    assert(gate.readers == 0);
    puts("PASS: battery conversion and exclusive SD ownership across flash, USB, remount and failures");
    return 0;
}

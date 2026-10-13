#include "storage_gate.h"
bool storage_gate_claim(storage_gate_t *gate)
{
    if (gate->mode != STORAGE_LOCAL) return false;
    ++gate->readers;
    return true;
}
void storage_gate_release(storage_gate_t *gate) { if (gate->readers) --gate->readers; }
bool storage_gate_switch(storage_gate_t *gate, bool to_usb)
{
    if (gate->readers || gate->mode != (to_usb ? STORAGE_LOCAL : STORAGE_USB)) return false;
    gate->mode = STORAGE_SWITCHING;
    return true;
}
bool storage_gate_remount(storage_gate_t *gate)
{
    if (gate->readers || gate->mode == STORAGE_USB || gate->mode == STORAGE_SWITCHING) return false;
    gate->mode = STORAGE_SWITCHING;
    return true;
}

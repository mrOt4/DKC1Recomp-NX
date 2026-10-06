#ifndef DKC1_SWITCH_CLOCK_H
#define DKC1_SWITCH_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* Thread placement (Switch only). Horizon starts every thread on the
 * process's default core, so without this the HD workers would share the
 * emulation thread's core. PinMainThread keeps the emulation on the first
 * core the process may use; PinWorker(n) puts worker n (1-based) on the
 * n-th other allowed core, wrapping. */
void Dkc1SwitchPinMainThread(void);
void Dkc1SwitchPinWorker(int index);

/* CPU clock control (Switch only). Boost raises the CPU to 1785 MHz, the
 * rate Horizon itself uses for its CPU boost mode; the GPU clock is left
 * alone. The rate found at the first call is restored by Dkc1SwitchClockExit
 * (and by Boost(false)). Returns the CPU rate in MHz now in effect, or 0
 * when the clock service is unavailable. */
uint32_t Dkc1SwitchClockBoost(bool enabled);
/* Re-apply the boost if the system reset the clock (dock change, HOME
 * menu); cheap, call every few seconds. */
void Dkc1SwitchClockMaintain(void);
void Dkc1SwitchClockExit(void);

#endif

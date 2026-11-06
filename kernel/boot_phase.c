#include "boot_phase.h"
#include "debug.h"
#include "printf.h"
#include "arch_barrier.h"

static enum boot_phase current_phase;
static const char *const phase_names[] = {
	"early",
	"memory",
	"bootstrap-tests",
	"devices",
	"runtime",
	"smp",
	"services",
};

enum boot_phase boot_phase_current(void)
{
	return __atomic_load_n(&current_phase, __ATOMIC_ACQUIRE);
}

void boot_phase_require(enum boot_phase expected)
{
	if (boot_phase_current() != expected) {
		panic("boot phase mismatch: current=%u expected=%u", (unsigned)boot_phase_current(),
			(unsigned)expected);
	}
}

void boot_phase_complete(enum boot_phase phase)
{
	if (phase <= BOOT_PHASE_EARLY || phase > BOOT_PHASE_SERVICES_READY) {
		panic("invalid boot phase: %u", (unsigned)phase);
	}
	boot_phase_require(phase - 1);
	if (interrupts_enabled()) {
		panic("boot phase %s completed with IRQs enabled", phase_names[phase]);
	}
	kprintf("[boot] %s complete\n", phase_names[phase]);
	__atomic_store_n(&current_phase, phase, __ATOMIC_RELEASE);
}

void boot_phase_wait_for(enum boot_phase phase)
{
	while (boot_phase_current() < phase) {
		cpu_relax();
	}
}

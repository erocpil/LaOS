#include "../kernel/cpu.h"

void arm64_smp_probe_mark(uint32_t cpu);

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	int cpu = cpu_get_ctx()->id;
	arm64_smp_probe_mark((uint32_t)cpu);

	return 0;
}

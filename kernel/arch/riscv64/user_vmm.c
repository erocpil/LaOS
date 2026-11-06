#include "user_vmm.h"

int arch_user_vmm_init(uint64_t *root_phys)
{
	(void)root_phys;
	return 0;
}

void arch_user_vmm_destroy(uint64_t *root_phys)
{
	(void)root_phys;
}

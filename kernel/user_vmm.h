#ifndef __USER_VMM_H__
#define __USER_VMM_H__

#include <stdint.h>

/* Install/remove supervisor-only platform mappings required while a user
 * translation root is active. The root argument is a physical address. */
int arch_user_vmm_init(uint64_t *root_phys);
void arch_user_vmm_destroy(uint64_t *root_phys);

#endif

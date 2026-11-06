#ifndef __USER_VMM_H__
#define __USER_VMM_H__

#include <stdint.h>

/* 用户页表根处于活动状态时，安装或移除所需的特权级映射。
 * root 参数为页表根的物理地址。 */
int arch_user_vmm_init(uint64_t *root_phys);
void arch_user_vmm_destroy(uint64_t *root_phys);

#endif

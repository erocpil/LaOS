#pragma once
static inline void cpu_relax(void)
{
	__builtin_trap();
}

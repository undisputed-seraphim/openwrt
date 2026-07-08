/* Math function stubs for Broadcom DHD driver */
#include <typedefs.h>

uint32 math_uint64_multiple_add(uint64 *acc, uint32 multiplicand, uint32 multiplier)
{
	*acc += (uint64)multiplicand * multiplier;
	return 0;
}

uint32 math_uint64_divide(uint64 *dividend, uint32 divisor)
{
	uint32 result;
	if (divisor == 0)
		return 0;
	result = (uint32)(*dividend / divisor);
	*dividend -= (uint64)result * divisor;
	return result;
}

uint32 math_uint64_right_shift(uint64 *val, uint32 shift)
{
	uint32 saved = (uint32)(*val & ((1ULL << shift) - 1));
	*val >>= shift;
	return saved;
}

/* Provide __aeabi_uldivmod needed by ARM EABI (not in kernel libgcc) */
uint64 __aeabi_uldivmod(uint64 n, uint64 d)
{
	if (d == 0)
		return 0;
	return n / d;
}

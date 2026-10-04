/*
 * <sys/bitops.h> stand-in for hosts that do not have NetBSD's.
 *
 * nvmm_x86.h includes this before anything from the OS layer, so it also
 * carries the handful of <sys/cdefs.h> attributes that header relies on.
 */
#ifndef _NVMM_COMPAT_SYS_BITOPS_H_
#define _NVMM_COMPAT_SYS_BITOPS_H_

#ifndef __unused
#define __unused		__attribute__((__unused__))
#endif
#ifndef __packed
#define __packed		__attribute__((__packed__))
#endif
#ifndef __aligned
#define __aligned(x)		__attribute__((__aligned__(x)))
#endif

/* NetBSD's bit-field helpers. __BIT(64) is 0, so __BITS(63, n) works. */
#undef __BIT
#undef __BITS
#undef __LOWEST_SET_BIT
#undef __SHIFTOUT
#undef __SHIFTIN
#undef __SHIFTOUT_MASK
#define __BIT(n)		\
	(((n) >= 64) ? 0ULL : (1ULL << ((n) & 63)))
#define __BITS(m, n)		\
	((__BIT(((m) > (n) ? (m) : (n)) + 1) - 1) ^ \
	 (__BIT(((m) < (n) ? (m) : (n))) - 1))
#define __LOWEST_SET_BIT(mask)	((((mask) - 1) & (mask)) ^ (mask))
#define __SHIFTOUT(x, mask)	(((x) & (mask)) / __LOWEST_SET_BIT(mask))
#define __SHIFTIN(x, mask)	((x) * __LOWEST_SET_BIT(mask))
#define __SHIFTOUT_MASK(mask)	__SHIFTOUT((mask), (mask))

#endif /* _NVMM_COMPAT_SYS_BITOPS_H_ */

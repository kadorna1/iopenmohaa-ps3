/*
 * PS3 wrapper for <setjmp.h>.
 *
 * The toolchain's libc setjmp()/longjmp() are built with AltiVec: setjmp
 * aligns the buffer up to 16 bytes, then saves r1/r2/r13-r31, LR, CR, the
 * FPRs f14-f31, VRSAVE and the vector registers v20-v31 (roughly 470 bytes).
 *
 * This project is compiled with -mno-altivec, and for that configuration
 * newlib's machine/setjmp.h sizes jmp_buf at only 256 bytes (it keys on
 * __ALTIVEC__ at the point where the header is parsed). Every setjmp() in the
 * engine therefore overran its jmp_buf by ~200 bytes and silently corrupted
 * the neighbouring variables: fbuffer in the JPEG loaders (libjpeg then saw
 * a NULL input buffer: "Empty input file"), the globals next to abortframe
 * in common.c, and the members after env in puff.c.
 *
 * Pretend __ALTIVEC__ is defined only while the real header is parsed so
 * jmp_buf gets the 512-byte size the library code actually needs.
 */
#ifndef OPENMOHAA_PS3_SETJMP_WRAPPER_H
#define OPENMOHAA_PS3_SETJMP_WRAPPER_H

#if defined(__PS3__) && !defined(__ALTIVEC__)
#  define __ALTIVEC__ 1
#  include_next <setjmp.h>
#  undef __ALTIVEC__
#else
#  include_next <setjmp.h>
#endif

#endif /* OPENMOHAA_PS3_SETJMP_WRAPPER_H */

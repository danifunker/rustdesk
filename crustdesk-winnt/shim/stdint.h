/* MSVC 4 has no <stdint.h>. Windows NT is ILP32 on every processor it ran
 * on (x86, MIPS, Alpha, PowerPC): int and long are 32 bits, and so is a
 * pointer, even on the Alpha. */
#ifndef CDV_NT_STDINT_H
#define CDV_NT_STDINT_H

typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef __int64 int64_t;
typedef unsigned __int64 uint64_t;
typedef signed char int_least8_t;
typedef unsigned char uint_least8_t;
typedef short int_least16_t;
typedef unsigned short uint_least16_t;
typedef int int_least32_t;
typedef unsigned int uint_least32_t;
typedef int int_fast8_t;
typedef unsigned int uint_fast8_t;
typedef int int_fast16_t;
typedef unsigned int uint_fast16_t;
typedef int int_fast32_t;
typedef unsigned int uint_fast32_t;
typedef __int64 int_fast64_t;
typedef unsigned __int64 uint_fast64_t;
typedef __int64 intmax_t;
typedef unsigned __int64 uintmax_t;
typedef long intptr_t;
typedef unsigned long uintptr_t;

#define INT8_MAX 127
#define INT16_MAX 32767
#define INT32_MAX 2147483647
#define INT32_MIN (-INT32_MAX - 1)
#define INT64_MAX 9223372036854775807i64
#define UINT8_MAX 255
#define UINT16_MAX 65535
#define UINT32_MAX 0xffffffffu
#define UINT64_MAX 0xffffffffffffffffui64
#ifndef SIZE_MAX
#define SIZE_MAX 0xffffffffu
#endif
#define INT64_C(x) x##i64
#define UINT64_C(x) x##ui64

#endif

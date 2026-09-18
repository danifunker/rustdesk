/* VENDORED from ~/repos/mogrix/compat/runtime/soft_float_stubs.c, which mogrix's
 * .gitignore excludes (compat/runtime/), so a fresh clone of mogrix -- which is
 * what scripts/toolchain.sh starts from -- does not have it. irix-ld links
 * -lsoft_float_stubs into every executable, so the archive must exist even
 * though neither the agent nor the panel takes a symbol from it (Rust's
 * compiler_builtins supplies the real __*tf3 routines first). Keep this in step
 * with the mogrix copy; delete it here once mogrix tracks the file. */

/*
 * soft_float_stubs.c — the 128-bit soft-float helpers clang references on
 * MIPS n32, which nothing on IRIX defines.
 *
 * WHAT THE GAP IS. The MIPS n32 ABI makes `long double` 128-bit IEEE 754
 * (binary128), and no MIPS CPU implements it in hardware, so every operation
 * is a library call. LLVM emits the compiler-rt names -- __addtf3, __fixtfsi,
 * __extenddftf2 and the rest. IRIX has the operations, but under MIPSpro's own
 * names: __q_add, __q_sub, __q_mul, __q_div, __q_neg, __q_ext (float->quad),
 * __q_extd (double->quad), __q_floti/__q_flotj/__q_flotju/__q_flotk/__q_flotku
 * (integer->quad) and __q_eq/__q_ne/__q_lt/__q_le/__q_gt/__q_ge, all exported
 * from /usr/lib32/libc.so.1. libm adds the transcendentals (__qsin, __qexp...).
 * So the two halves exist and simply do not know each other's names.
 *
 * WHY THESE ABORT RATHER THAN FORWARD. Forwarding is the obviously better
 * implementation and should be done -- but only by someone who has confirmed
 * on hardware that LLVM's fp128 argument passing matches what MIPSpro's
 * __q_* routines expect. Both claim the n32 convention; if they disagree in
 * any detail the result is not a crash, it is arithmetic that is quietly
 * wrong, which is the single worst outcome available here. A named abort is
 * strictly better than a plausible wrong number.
 *
 * WHY THEY EXIST AT ALL, THEN. Almost nothing reaches these. A package
 * references them because some header declared a `long double` overload, or a
 * printf path handles %Lf, on a branch it never executes for us -- but the
 * reference still has to resolve or the executable does not link. That is the
 * whole job: let the link succeed, and if one is ever genuinely called, say
 * which one and stop.
 *
 * TO IMPLEMENT PROPERLY: replace a stub body with a call to its __q_*
 * equivalent, and verify against a value computed by MIPSpro's cc on the same
 * machine. Do it one operation at a time; do not convert the file wholesale.
 *
 * No printf: IRIX libc's printf does not understand %zu and can itself route
 * through these helpers. write(2) only.
 *
 * ASCII only, C89-compatible declarations.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
soft_float_unsupported(const char *name)
{
    static const char pre[] = "mogrix: 128-bit soft float is not implemented on IRIX: ";
    static const char post[] = "()\nmogrix: see compat/runtime/soft_float_stubs.c\n";

    (void)write(2, pre, sizeof(pre) - 1);
    (void)write(2, name, strlen(name));
    (void)write(2, post, sizeof(post) - 1);
    abort();
}

/* Each stub aborts before returning, so no fp128 value is ever materialised
 * and the declared return type costs nothing at runtime. */
#define TF_STUB(ret, name, args)            \
    ret name args;                          \
    ret name args                           \
    {                                       \
        soft_float_unsupported(#name);      \
        for (;;) { }                        \
    }

/* Arithmetic */
TF_STUB(long double, __addtf3,  (long double a, long double b))
TF_STUB(long double, __subtf3,  (long double a, long double b))
TF_STUB(long double, __multf3,  (long double a, long double b))
TF_STUB(long double, __divtf3,  (long double a, long double b))
TF_STUB(long double, __negtf2,  (long double a))
TF_STUB(long double, __powitf2, (long double a, int b))

/* Widening and narrowing */
TF_STUB(long double, __extendsftf2, (float a))
TF_STUB(long double, __extenddftf2, (double a))
TF_STUB(float,       __trunctfsf2,  (long double a))
TF_STUB(double,      __trunctfdf2,  (long double a))

/* Quad -> integer */
TF_STUB(int,                __fixtfsi,    (long double a))
TF_STUB(long long,          __fixtfdi,    (long double a))
TF_STUB(unsigned int,       __fixunstfsi, (long double a))
TF_STUB(unsigned long long, __fixunstfdi, (long double a))

/* Integer -> quad */
TF_STUB(long double, __floatsitf,  (int a))
TF_STUB(long double, __floatditf,  (long long a))
TF_STUB(long double, __floatunsitf, (unsigned int a))
TF_STUB(long double, __floatunditf, (unsigned long long a))

/* Comparisons. Their return values are the compiler-rt convention (negative,
 * zero, positive; __unordtf2 non-zero for NaN), not C's. */
TF_STUB(int, __eqtf2,    (long double a, long double b))
TF_STUB(int, __netf2,    (long double a, long double b))
TF_STUB(int, __lttf2,    (long double a, long double b))
TF_STUB(int, __letf2,    (long double a, long double b))
TF_STUB(int, __gttf2,    (long double a, long double b))
TF_STUB(int, __getf2,    (long double a, long double b))
TF_STUB(int, __unordtf2, (long double a, long double b))

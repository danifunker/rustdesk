/*
 * compat_shim.c -- the two Solaris 11 entry points the libc crate binds to,
 * on a machine that predates them.
 *
 * `libc` declares `sysconf` and `pthread_kill` with `link_name` pointing at
 * the XPG7 variants for every `target_os = "solaris"`:
 *
 *     unix/mod.rs:1560           link_name = "__sysconf_xpg7"
 *     unix/solarish/mod.rs:2885  link_name = "__pthread_kill_xpg7"
 *
 * Those arrived in Solaris 11.4. On 10 the link simply fails --
 * "Undefined symbol __sysconf_xpg7" -- and nothing before the link says so,
 * because the crate compiles perfectly well.
 *
 * Forwarding is the right answer rather than a stopgap: the XPG7 variants
 * differ from the classic ones only in conformance details this agent does not
 * depend on (a few `sysconf` names report the newer standard's version number,
 * and XPG7 `pthread_kill` is specified to fail with ESRCH where the older one
 * is silent). What matters is that both names mean the same call here.
 *
 * If a third turns up, it belongs beside these two. `nm` on this machine's
 * libc will not answer the question -- ask the linker, which is what found
 * these: declare the symbol in a two-line C file and try to link it.
 */

#include <pthread.h>
#include <signal.h>
#include <unistd.h>

long __sysconf_xpg7(int name)
{
    return sysconf(name);
}

int __pthread_kill_xpg7(pthread_t thread, int sig)
{
    return pthread_kill(thread, sig);
}

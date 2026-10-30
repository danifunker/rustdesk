#ifndef CDV_NT_SELFTEST_H
#define CDV_NT_SELFTEST_H

/* Run every check, writing a line for each through `out`; 0 if all pass. */
int selftest(void (*out)(const char *));

#endif

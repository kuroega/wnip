/* selftest.h - optional GUI smoke test (--gui-selftest). */
#ifndef WNIP_SELFTEST_H
#define WNIP_SELFTEST_H

/* Runs the desktop smoke test and returns the number of failed checks. */
int selftest_run(void);

/* Number of failed checks from the most recent run. */
int selftest_failures(void);

#endif /* WNIP_SELFTEST_H */

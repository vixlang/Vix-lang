#ifndef VIX_WINCOMPAT_H
#define VIX_WINCOMPAT_H

#ifdef _WIN32

/* Windows compatibility layer for POSIX functions */

#include <stdlib.h>
#include <process.h>
#include <io.h>

/* setenv replacement */
#define setenv(name, value, overwrite) _putenv_s(name, value)

/* getopt - simplified implementation for Windows */
extern int opterr;
extern int optind;
extern int optopt;
extern char *optarg;

/* Access constants */
#ifndef F_OK
#define F_OK 0
#endif
#ifndef X_OK
#define X_OK 1
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef R_OK
#define R_OK 4
#endif

/* Sleep replacement */
#define sleep(seconds) Sleep((seconds) * 1000)
#define usleep(usec) Sleep((usec) / 1000)

#else

/* Linux/Unix - include normal unistd.h */
#include <unistd.h>

#endif /* _WIN32 */

#endif /* VIX_WINCOMPAT_H */

/* newlib <-> bionic errno translation. */

#ifndef BSNX_ERRNO_SHIM_H
#define BSNX_ERRNO_SHIM_H

int errno_host_to_bionic(int value);
int errno_bionic_to_host(int value);

/* The guest's __errno(). Returns a per-thread slot kept in sync with the
 * host errno in both directions. */
int *bionic_errno_location(void);

#endif

/* The generated binding table between guest imports and host code. */

#ifndef BSNX_IMPORTS_H
#define BSNX_IMPORTS_H

#include <stddef.h>
#include <stdint.h>

#include "so_util.h"

extern DynLibFunction dynlib_functions[];
extern const size_t dynlib_function_count;

uintptr_t dynlib_find_export(const char *name);

#endif

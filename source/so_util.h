/* so_util.h -- load, relocate and patch Android arm64 .so modules
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef BSNX_SO_UTIL_H
#define BSNX_SO_UTIL_H

#include <elf.h>
#include <stddef.h>
#include <stdint.h>

#define ALIGN_MEM(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

#define SO_MAX_SEGMENTS 8

typedef struct {
  const char *symbol;
  uintptr_t func;
} DynLibFunction;

typedef struct so_module {
  struct so_module *next;
  char name[64];

  /* The whole PT_LOAD span, twice: load_base is the writable staging copy we
   * relocate into, load_virtbase is the executable mapping the guest runs
   * from once so_finalize donates the pages. */
  void *load_base, *load_virtbase;
  size_t load_size;
  void *load_memrv; /* VirtmemReservation * */

  /* Unmodified program headers (link-time vaddrs) for dl_iterate_phdr, which
   * the libc++ unwinder inside the guest walks during exception handling. */
  Elf64_Phdr phdr[SO_MAX_SEGMENTS * 2];
  int phnum;

  void *so_base; /* temporary whole-file image */
  size_t so_size;

  Elf64_Ehdr *elf_hdr;
  Elf64_Phdr *prog_hdr;
  Elf64_Shdr *sec_hdr;
  Elf64_Sym *syms;
  int num_syms;
  char *shstrtab;
  char *dynstrtab;
} so_module;

/* A 16-byte absolute branch installed over a guest function, plus the bytes
 * it displaced so the original can still be reached. */
typedef struct {
  uintptr_t addr;
  uintptr_t thumb_addr; /* unused on aarch64; kept for source parity */
  uint8_t patch_instr[16];
  uint8_t orig_instr[16];
} so_hook;

int so_load(so_module *mod, const char *filename, void *base, size_t max_size);
int so_relocate(so_module *mod);
void so_resolve(so_module *mod, DynLibFunction *funcs, int num_funcs);
void so_finalize(so_module *mod);
void so_flush_caches(so_module *mod);
void so_execute_init_array(so_module *mod);
void so_free_temp(so_module *mod);

uintptr_t so_symbol(so_module *mod, const char *symbol);
/* Same lookup, but into the staging copy. Valid only between so_load and
 * so_finalize, which is the window where the live mapping does not exist
 * yet but relocations have already been applied. */
void *so_symbol_staging(so_module *mod, const char *symbol);
void *so_resolve_external(const char *name);

int so_patch_code(void *dst, const void *src, size_t len);
/* Redirect `symbol` (or `addr`) to `dst`; the returned hook can call through
 * to the displaced original with so_continue_*. */
so_hook so_hook_addr(uintptr_t addr, uintptr_t dst);
so_hook so_hook_symbol(so_module *mod, const char *symbol, uintptr_t dst);
/* Temporarily restore the original prologue, so the caller can invoke the
 * unhooked function, then reinstall the branch. Not reentrant per hook. */
void so_hook_disable(so_hook *hook);
void so_hook_enable(so_hook *hook);

int so_dl_iterate_phdr(int (*callback)(void *info, size_t size, void *data), void *data);
int so_dump_maps(char *buf, size_t cap);

#endif

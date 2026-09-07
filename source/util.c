/* Small helpers shared across the loader. */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

void install_bionic_tls(void *buf) {
  memset(buf, 0, BIONIC_TLS_SIZE);
  /* Bionic's thread pointer sits 0x200 bytes into its TLS block; slots the
   * guest reads (the canary at +0x28) live above it. */
  void *tp = (uint8_t *)buf + 0x200;
  __asm__ volatile("msr tpidr_el0, %0" : : "r"(tp));
}

int path_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

int is_directory(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int mkpath(const char *path) {
  char buf[512];
  size_t len = strlen(path);
  if (len == 0 || len >= sizeof buf) return 0;
  memcpy(buf, path, len + 1);
  while (len > 1 && buf[len - 1] == '/') buf[--len] = '\0';

  /* Skip past any "device:" prefix before walking separators. */
  char *cursor = strchr(buf, ':');
  cursor = cursor ? cursor + 1 : buf;
  if (*cursor == '/') cursor++;

  for (; *cursor; cursor++) {
    if (*cursor != '/') continue;
    *cursor = '\0';
    if (mkdir(buf, 0777) != 0 && errno != EEXIST) { *cursor = '/'; return 0; }
    *cursor = '/';
  }
  if (mkdir(buf, 0777) != 0 && errno != EEXIST) return 0;
  return 1;
}

int copy_file(const char *from, const char *to) {
  FILE *in = fopen(from, "rb");
  if (!in) return 0;
  FILE *out = fopen(to, "wb");
  if (!out) { fclose(in); return 0; }

  static char buffer[64 * 1024];
  int ok = 1;
  for (;;) {
    size_t got = fread(buffer, 1, sizeof buffer, in);
    if (got == 0) break;
    if (fwrite(buffer, 1, got, out) != got) { ok = 0; break; }
  }
  if (ferror(in)) ok = 0;
  fclose(in);
  if (fclose(out) != 0) ok = 0;
  if (!ok) unlink(to);
  return ok;
}

void remove_tree(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return;
  if (!S_ISDIR(st.st_mode)) { unlink(path); return; }

  DIR *dir = opendir(path);
  if (dir) {
    struct dirent *entry;
    char child[512];
    while ((entry = readdir(dir)) != NULL) {
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
      snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
      remove_tree(child);
    }
    closedir(dir);
  }
  rmdir(path);
}

int move_tree(const char *from, const char *to) {
  if (rename(from, to) == 0) return 1;

  struct stat st;
  if (stat(from, &st) != 0) return 0;
  if (!S_ISDIR(st.st_mode)) {
    if (!copy_file(from, to)) return 0;
    unlink(from);
    return 1;
  }

  if (!mkpath(to)) return 0;
  DIR *dir = opendir(from);
  if (!dir) return 0;

  int ok = 1;
  struct dirent *entry;
  char src[512], dst[512];
  while ((entry = readdir(dir)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    snprintf(src, sizeof src, "%s/%s", from, entry->d_name);
    snprintf(dst, sizeof dst, "%s/%s", to, entry->d_name);
    if (!move_tree(src, dst)) { ok = 0; break; }
  }
  closedir(dir);
  if (ok) rmdir(from);
  return ok;
}

int count_tree_entries(const char *path) {
  DIR *dir = opendir(path);
  if (!dir) return 0;
  int total = 0;
  struct dirent *entry;
  char child[512];
  while ((entry = readdir(dir)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    total++;
    snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
    if (is_directory(child)) total += count_tree_entries(child);
  }
  closedir(dir);
  return total;
}

const char *strip_device(const char *path) {
  if (!path) return path;
  const char *colon = strchr(path, ':');
  if (colon && colon[1] == '/') return colon + 1;
  return path;
}

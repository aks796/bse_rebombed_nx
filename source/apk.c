/* Unpacking the player's own APK.
 *
 * An APK is a zip, so this is a small zip reader: find the central
 * directory, walk it, and inflate the handful of entries this port needs
 * straight onto the SD card. Nothing is held in memory beyond a pair of
 * buffers, since the asset tree runs to a hundred megabytes.
 *
 * Every entry is checked against the CRC the archive recorded for it. A
 * silently truncated or garbled file here is worse than a failed install:
 * the engine would run for minutes before tripping over it, and the reason
 * would be nowhere near the cause.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <zlib.h>

#include "apk.h"
#include "config.h"
#include "error.h"
#include "setup.h"
#include "util.h"

/* What this port takes out of the archive. Everything else in there -- the
 * other ABIs, the Java side, Android resources -- has nothing to run it. */
#define APK_LIB_ENTRY "lib/arm64-v8a/libmain.so"
#define APK_ASSET_PREFIX "assets/ballistica_files/"

#define ZIP_EOCD_SIGNATURE 0x06054b50u
#define ZIP_CENTRAL_SIGNATURE 0x02014b50u
#define ZIP_LOCAL_SIGNATURE 0x04034b50u

/* The end-of-central-directory record sits at the very end unless there is a
 * zip comment, which may run to 64 KB. */
#define ZIP_EOCD_MAX_SEARCH (66u * 1024u)

#define COPY_BUFFER_BYTES (64u * 1024u)
/* Writing the asset tree is thousands of small files; a generous stream
 * buffer keeps that from becoming thousands of tiny card writes. */
#define WRITE_BUFFER_BYTES (256u * 1024u)

#define MAX_ENTRY_NAME 512

typedef struct {
  FILE *file;
  long central_offset;
  unsigned long central_size;
  unsigned entry_count;
} Archive;

typedef struct {
  char name[MAX_ENTRY_NAME];
  unsigned method;
  unsigned long compressed_size;
  unsigned long uncompressed_size;
  unsigned long crc;
  long local_offset;
} Entry;

/* ------------------------------------------------------------- little ends */

static unsigned read16(const unsigned char *p) {
  return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned long read32(const unsigned char *p) {
  return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
         ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* ------------------------------------------------------------ finding one */

#define MAX_CANDIDATES 8
#define CANDIDATE_PATH_MAX 512

/* Copying to an SD card from a desktop leaves sidecars next to what you
 * actually copied: macOS writes "._name.apk" alongside "name.apk", and it
 * sorts first. Anything starting with a dot is one of those. */
static bool looks_like_apk(const char *name) {
  const size_t length = strlen(name);
  if (length < 5 || name[0] == '.') return false;
  return strcasecmp(name + length - 4, ".apk") == 0;
}

/* Fills paths with the .apk files in the game folder, name order, and
 * returns how many. */
static unsigned collect_archives(char paths[][CANDIDATE_PATH_MAX],
                                 unsigned max) {
  DIR *dir = opendir(GAME_ROOT);
  if (!dir) return 0;

  unsigned count = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (!looks_like_apk(entry->d_name)) continue;

    char path[CANDIDATE_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", GAME_ROOT, entry->d_name);
    if (is_directory(path)) continue;

    /* Insertion sort, so which one gets tried first does not depend on the
     * order the card happens to hand them back in. */
    unsigned at = count;
    while (at > 0 && strcasecmp(paths[at - 1], path) > 0) {
      if (at < max) memcpy(paths[at], paths[at - 1], CANDIDATE_PATH_MAX);
      at--;
    }
    if (at < max) {
      snprintf(paths[at], CANDIDATE_PATH_MAX, "%s", path);
      if (count < max) count++;
    }
  }
  closedir(dir);
  return count;
}

static const char *base_name(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

/* ------------------------------------------------------- central directory */

static bool open_archive(const char *path, Archive *archive) {
  memset(archive, 0, sizeof *archive);
  archive->file = fopen(path, "rb");
  if (!archive->file) return false;

  if (fseek(archive->file, 0, SEEK_END) != 0) return false;
  const long size = ftell(archive->file);
  if (size < 22) return false;

  long search = size < (long)ZIP_EOCD_MAX_SEARCH ? size : (long)ZIP_EOCD_MAX_SEARCH;
  unsigned char *tail = malloc((size_t)search);
  if (!tail) return false;
  if (fseek(archive->file, size - search, SEEK_SET) != 0 ||
      fread(tail, 1, (size_t)search, archive->file) != (size_t)search) {
    free(tail);
    return false;
  }

  bool found = false;
  for (long i = search - 22; i >= 0; i--) {
    if (read32(tail + i) != ZIP_EOCD_SIGNATURE) continue;
    archive->entry_count = read16(tail + i + 10);
    archive->central_size = read32(tail + i + 12);
    archive->central_offset = (long)read32(tail + i + 16);
    found = true;
    break;
  }
  free(tail);

  if (!found) return false;
  /* A zip64 archive writes these as all-ones and puts the real values
   * elsewhere. No APK this port can use is anywhere near that big. */
  if (archive->central_offset == (long)0xFFFFFFFFu) return false;
  return true;
}

static void close_archive(Archive *archive) {
  if (archive->file) fclose(archive->file);
  archive->file = NULL;
}

/* Reads the next central-directory entry, leaving the file positioned on the
 * one after it. Returns false at the end or on damage. */
static bool next_entry(Archive *archive, Entry *entry) {
  unsigned char header[46];
  if (fread(header, 1, sizeof header, archive->file) != sizeof header)
    return false;
  if (read32(header) != ZIP_CENTRAL_SIGNATURE) return false;

  entry->method = read16(header + 10);
  entry->crc = read32(header + 16);
  entry->compressed_size = read32(header + 20);
  entry->uncompressed_size = read32(header + 24);
  const unsigned name_length = read16(header + 28);
  const unsigned extra_length = read16(header + 30);
  const unsigned comment_length = read16(header + 32);
  entry->local_offset = (long)read32(header + 42);

  if (name_length >= sizeof entry->name) {
    /* Nothing this port wants has a name that long; skip past it. */
    if (fseek(archive->file, name_length + extra_length + comment_length,
              SEEK_CUR) != 0)
      return false;
    entry->name[0] = '\0';
    return true;
  }

  if (fread(entry->name, 1, name_length, archive->file) != name_length)
    return false;
  entry->name[name_length] = '\0';
  if (fseek(archive->file, extra_length + comment_length, SEEK_CUR) != 0)
    return false;
  return true;
}

/* ------------------------------------------------------------- extracting */

/* Seeks past the local header so the file is positioned on the entry's data.
 * The local header repeats the name and carries its own extra field, whose
 * length routinely differs from the central one thanks to alignment
 * padding, so it has to be read rather than assumed. */
static bool seek_to_data(Archive *archive, const Entry *entry) {
  unsigned char header[30];
  if (fseek(archive->file, entry->local_offset, SEEK_SET) != 0) return false;
  if (fread(header, 1, sizeof header, archive->file) != sizeof header)
    return false;
  if (read32(header) != ZIP_LOCAL_SIGNATURE) return false;
  const unsigned name_length = read16(header + 26);
  const unsigned extra_length = read16(header + 28);
  return fseek(archive->file, name_length + extra_length, SEEK_CUR) == 0;
}

typedef struct {
  FILE *file;
  unsigned long crc;
  unsigned long written;
  bool failed;
} Sink;

static void sink_write(Sink *sink, const unsigned char *data, size_t length) {
  if (sink->failed || length == 0) return;
  if (fwrite(data, 1, length, sink->file) != length) {
    sink->failed = true;
    return;
  }
  sink->crc = crc32(sink->crc, data, (unsigned)length);
  sink->written += length;
}

static bool extract_stored(Archive *archive, const Entry *entry, Sink *sink) {
  unsigned char buffer[COPY_BUFFER_BYTES];
  unsigned long left = entry->compressed_size;
  while (left > 0) {
    const size_t want = left < sizeof buffer ? (size_t)left : sizeof buffer;
    const size_t got = fread(buffer, 1, want, archive->file);
    if (got != want) return false;
    sink_write(sink, buffer, got);
    if (sink->failed) return false;
    left -= got;
  }
  return true;
}

static bool extract_deflated(Archive *archive, const Entry *entry, Sink *sink) {
  z_stream stream;
  memset(&stream, 0, sizeof stream);
  /* Negative window bits: a zip member is a raw deflate stream with no
   * zlib wrapper around it. */
  if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return false;

  unsigned char *in = malloc(COPY_BUFFER_BYTES);
  unsigned char *out = malloc(COPY_BUFFER_BYTES);
  bool ok = in && out;
  unsigned long left = entry->compressed_size;

  while (ok) {
    if (stream.avail_in == 0 && left > 0) {
      const size_t want = left < COPY_BUFFER_BYTES ? (size_t)left : COPY_BUFFER_BYTES;
      const size_t got = fread(in, 1, want, archive->file);
      if (got != want) { ok = false; break; }
      left -= got;
      stream.next_in = in;
      stream.avail_in = (unsigned)got;
    }

    stream.next_out = out;
    stream.avail_out = COPY_BUFFER_BYTES;
    const int status = inflate(&stream, Z_NO_FLUSH);
    if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) {
      ok = false;
      break;
    }

    sink_write(sink, out, COPY_BUFFER_BYTES - stream.avail_out);
    if (sink->failed) { ok = false; break; }
    if (status == Z_STREAM_END) break;
    /* No input left and nothing more coming means a truncated member. */
    if (status == Z_BUF_ERROR && stream.avail_in == 0 && left == 0) {
      ok = false;
      break;
    }
  }

  inflateEnd(&stream);
  free(in);
  free(out);
  return ok;
}

/* Entries come out of the archive grouped by directory, so remembering the
 * last one turns a walk-and-mkdir per file into one per directory. On a card
 * this slow that is most of the difference. */
static bool ensure_parent(const char *destination) {
  static char last[640];
  char parent[640];
  snprintf(parent, sizeof parent, "%s", destination);
  char *slash = strrchr(parent, '/');
  if (!slash) return true;
  *slash = '\0';
  if (!strcmp(parent, last)) return true;
  if (!mkpath(parent)) return false;
  snprintf(last, sizeof last, "%s", parent);
  return true;
}

/* Writes one entry to destination, replacing whatever was there. */
static bool extract_entry(Archive *archive, const Entry *entry,
                          const char *destination) {
  if (!ensure_parent(destination)) return false;

  if (!seek_to_data(archive, entry)) return false;

  Sink sink;
  memset(&sink, 0, sizeof sink);
  sink.crc = crc32(0, NULL, 0);
  sink.file = fopen(destination, "wb");
  if (!sink.file) return false;

  char *buffer = malloc(WRITE_BUFFER_BYTES);
  if (buffer) setvbuf(sink.file, buffer, _IOFBF, WRITE_BUFFER_BYTES);

  bool ok = entry->method == 0 ? extract_stored(archive, entry, &sink)
                               : extract_deflated(archive, entry, &sink);
  if (fflush(sink.file) != 0) ok = false;
  if (fclose(sink.file) != 0) ok = false;
  free(buffer);

  if (ok && sink.written != entry->uncompressed_size) {
    trace("apk: %s came out %lu bytes, expected %lu", entry->name, sink.written,
          entry->uncompressed_size);
    ok = false;
  }
  if (ok && sink.crc != entry->crc) {
    trace("apk: %s failed its checksum", entry->name);
    ok = false;
  }

  /* A file that is there but wrong is worse than one that is missing: the
   * engine would read it much later and blame something else. */
  if (!ok) remove(destination);
  return ok;
}

/* ------------------------------------------------------------------ driver */

static bool wanted(const Entry *entry, bool need_library, bool need_assets,
                   char *destination, size_t destination_size) {
  const size_t length = strlen(entry->name);
  if (length == 0 || entry->name[length - 1] == '/') return false;

  if (need_library && !strcmp(entry->name, APK_LIB_ENTRY)) {
    snprintf(destination, destination_size, "%s", SO_PATH);
    return true;
  }

  const size_t prefix = sizeof APK_ASSET_PREFIX - 1;
  if (need_assets && !strncmp(entry->name, APK_ASSET_PREFIX, prefix)) {
    snprintf(destination, destination_size, "%s/%s", ASSETS_PATH,
             entry->name + prefix);
    return true;
  }
  return false;
}

/* Remembered so the caller can clear the archive away once everything it
 * supplied has been checked over. */
static char g_installed[CANDIDATE_PATH_MAX];

const char *apk_installed_path(void) {
  return g_installed[0] ? g_installed : NULL;
}

bool apk_needed(void) {
  if (path_exists(SO_PATH) && is_directory(ASSETS_PATH)) return false;
  char candidates[MAX_CANDIDATES][CANDIDATE_PATH_MAX];
  return collect_archives(candidates, MAX_CANDIDATES) > 0;
}

bool apk_install(void) {
  const bool need_library = !path_exists(SO_PATH);
  const bool need_assets = !is_directory(ASSETS_PATH);
  if (!need_library && !need_assets) return false;

  char candidates[MAX_CANDIDATES][CANDIDATE_PATH_MAX];
  const unsigned count = collect_archives(candidates, MAX_CANDIDATES);
  if (count == 0) return false;

  /* Which file to use is decided by opening them, not by their names: an
   * APK that will not read is not the one the player meant, whatever it is
   * called. */
  Archive archive;
  const char *path = NULL;
  for (unsigned i = 0; i < count; i++) {
    if (open_archive(candidates[i], &archive)) {
      /* Kept here rather than as a pointer into the caller's array, so it
       * outlives this function for whoever clears the archive away. Both
       * buffers are the same fixed size, so this is a straight copy. */
      memcpy(g_installed, candidates[i], sizeof g_installed);
      g_installed[sizeof g_installed - 1] = '\0';
      path = g_installed;
      break;
    }
    close_archive(&archive);
    trace("apk: %s does not read as an archive; skipping it",
          base_name(candidates[i]));
  }

  if (!path) {
    g_installed[0] = '\0';
    fatal_error("%s is not a readable APK.\n\n"
                "  Copy the file to the SD card again -- a partial copy and\n"
                "  a zip64 archive both land here.",
                base_name(candidates[0]));
  }

  trace("apk: unpacking %s", path);
  startup_status_printf("Unpacking %s", base_name(path));

  if (fseek(archive.file, archive.central_offset, SEEK_SET) != 0) {
    close_archive(&archive);
    fatal_error("%s has a damaged directory.", base_name(path));
  }

  /* The central directory has to be read through in order, but extracting
   * seeks away from it, so each entry's position is remembered and restored. */
  unsigned extracted = 0;
  unsigned long bytes = 0;
  bool got_library = false;
  const unsigned total = archive.entry_count;

  for (unsigned i = 0; i < total; i++) {
    Entry entry;
    if (!next_entry(&archive, &entry)) break;
    const long resume = ftell(archive.file);

    char destination[640];
    if (!wanted(&entry, need_library, need_assets, destination,
                sizeof destination))
      continue;

    if (entry.method != 0 && entry.method != 8) {
      close_archive(&archive);
      fatal_error("%s is compressed in a way this port cannot read.",
                  entry.name);
    }

    if (!extract_entry(&archive, &entry, destination)) {
      close_archive(&archive);
      fatal_error("Could not unpack %s.\n\n"
                  "  The APK may be damaged, or the SD card may be full.\n"
                  "  There is a note in trace.txt.",
                  entry.name);
    }

    if (!strcmp(entry.name, APK_LIB_ENTRY)) got_library = true;
    extracted++;
    bytes += entry.uncompressed_size;

    /* Thousands of files go by, so the count only goes on screen now and
     * then; repainting per file would cost more than the writing does. */
    if ((extracted % 64) == 0)
      startup_status_printf("Unpacking: %u files, %lu MB", extracted,
                            bytes / (1024ul * 1024ul));

    if (fseek(archive.file, resume, SEEK_SET) != 0) break;
  }

  close_archive(&archive);

  if (need_library && !got_library) {
    fatal_error("%s has no arm64 library.\n\n"
                "  This port needs an APK containing\n  %s\n\n"
                "  A split APK for another processor will not work.",
                base_name(path), APK_LIB_ENTRY);
  }
  if (extracted == 0) {
    fatal_error("%s does not look like an Explodinary APK.\n\n"
                "  Nothing inside it matched what this port needs.",
                base_name(path));
  }

  trace("apk: unpacked %u files, %lu bytes", extracted, bytes);
  startup_status_printf("Unpacked %u files", extracted);
  return true;
}

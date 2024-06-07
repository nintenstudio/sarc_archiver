/*
 * High-level PhysicsFS archiver for simple Nintendo SARC archives. Based on
 * physfs_archiver_unpacked.c by Ryan C. Gordon. The original source file is
 * in the ext/physfs/src folder.
 *
 * RULES: Archive entries must be uncompressed. Dirs and files allowed, but no
 *  symlinks, etc. We can relax some of these rules as necessary.
 *
 * ZSTD compression can be handled using a custom PHYSFS_Io interface.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file originally written by Ryan C. Gordon.
 */

#include <stdint.h>
#include <stdlib.h>

#include <physfs.h>
#define __PHYSICSFS_INTERNAL__
#include <physfs_internal.h>

#include "sarc.h"
#include "sarc_io.h"
#include "archiver_sarc.h"
#include "archiver_sarc_internal.h"
#include "vmem.h"

// TODO: See if we can track the number of open write handles, then rebuild the
// SARC and turn it back into a normal read-only archive? It looks like calling
// PHYSFS_close() may call our io->flush() which will let us know when a write
// handle is closed.
const PHYSFS_Archiver archiver_sarc_default = {
  .version = 0,
  .info = {
    .extension = "pack",
    .description = "An extension to support uncompressed SARC files with a .pack extension",
    .author = "Torphedo",
    .url = "https://github.com/Torphedo",
    .supportsSymlinks = false
  },
  .openArchive = SARC_openArchive,
  .enumerate = __PHYSFS_DirTreeEnumerate,
  .openRead = SARC_openRead,
  .openWrite = SARC_openWrite,
  .openAppend = SARC_openAppend,
  .remove = SARC_remove,
  .mkdir = SARC_mkdir,
  .stat = SARC_stat,
  .closeArchive = SARC_closeArchive
};

typedef struct {
  char **list;
  PHYSFS_uint32 size;
  PHYSFS_ErrorCode errcode;
}EnumStringListCallbackData;


// TODO: Call SARC_flush() here.
void SARC_closeArchive(void *opaque) {
  SARC_ctx *info = ((SARC_ctx *) opaque);
  if (info) {
    __PHYSFS_DirTreeDeinit(&info->tree);

    if (info->io) {
      info->io->destroy(info->io);
    }

    allocator.Free(info);
  } /* if */
} /* SARC_closeArchive */

void SARC_abandonArchive(void *opaque) {
  SARC_ctx *info = ((SARC_ctx *) opaque);
  if (info)
  {
      info->io = NULL;
      SARC_closeArchive(info);
  } /* if */
} /* SARC_abandonArchive */

SARCentry *findEntry(SARC_ctx* ctx, const char *path) {
  return (SARCentry *) __PHYSFS_DirTreeFind(&ctx->tree, path);
} /* findEntry */

PHYSFS_Io *SARC_openRead(void *opaque, const char *name) {
  PHYSFS_Io *retval = NULL;
  SARC_ctx *info = (SARC_ctx *) opaque;
  SARC_file_ctx* file = NULL;
  SARCentry *entry = findEntry(info, name);

  BAIL_IF_ERRPASS(!entry, NULL);
  BAIL_IF(entry->tree.isdir, PHYSFS_ERR_NOT_A_FILE, NULL);

  retval = (PHYSFS_Io *) allocator.Malloc(sizeof (PHYSFS_Io));
  GOTO_IF(!retval, PHYSFS_ERR_OUT_OF_MEMORY, SARC_openRead_failed);

  file = (SARC_file_ctx *) allocator.Malloc(sizeof (SARC_file_ctx));
  GOTO_IF(!file, PHYSFS_ERR_OUT_OF_MEMORY, SARC_openRead_failed);

  file->io = info->io->duplicate(info->io);
  GOTO_IF_ERRPASS(!file->io, SARC_openRead_failed);

  if (!file->io->seek(file->io, entry->startPos)) {
    goto SARC_openRead_failed;
  }

  file->curPos = 0;
  file->entry = entry;
  
  // Give IO our archiver info
  file->arc_info = opaque;

  // Set SARC_Io as the I/O handler for this archiver
  memcpy(retval, &SARC_Io, sizeof (*retval));
  retval->opaque = file;
  return retval;

SARC_openRead_failed:
  if (file != NULL) {
    if (file->io != NULL) {
      file->io->destroy(file->io);
    }
    allocator.Free(file);
  } /* if */

  if (retval != NULL) {
    allocator.Free(retval);
  }

  return NULL;
} /* SARC_openRead */

// Copy all file contents to newly allocated buffers
PHYSFS_EnumerateCallbackResult callback_copy_files(void *data, const char *origdir, const char *fname) {
  SARC_ctx* info = (SARC_ctx*)data;
  char* full_path = __PHYSFS_smallAlloc(strlen(origdir) + strlen(fname) + 1);
  if (full_path == NULL) {
    return PHYSFS_ENUM_ERROR;
  }

  // It doesn't want a leading slash, but we need a slash between directories.
  if (origdir[0] == 0) {
    // No containing dir.
    strcpy(full_path, fname);
  }
  else {
    sprintf(full_path, "%s/%s", origdir, fname);
  }

  PHYSFS_Stat statbuf = {0};
  PHYSFS_stat(full_path, &statbuf);
  if  (statbuf.filetype == PHYSFS_FILETYPE_DIRECTORY){
    __PHYSFS_DirTreeEnumerate(&info->tree, full_path, callback_copy_files, full_path, data);
  }
  else {
    // We've finally got a full filename.
    SARCentry* entry = findEntry(info, full_path);

    // Store the file in a new buffer and store the pointer in the entry.
    entry->data_ptr = (uint64_t) allocator.Malloc(entry->size);
    uint64_t pos = info->io->tell(info->io); // Save position
    info->io->seek(info->io, entry->startPos);
    info->io->read(info->io, (void*)entry->data_ptr, entry->size);
    info->io->seek(info->io, pos); // Go back to saved position
    printf("%s %s\n", module_name_info, full_path);
  }

  __PHYSFS_smallFree(full_path);
  return PHYSFS_ENUM_OK;
}

PHYSFS_Io* SARC_openWrite(void *opaque, const char *name) {
  SARC_ctx* info = (SARC_ctx*) opaque;

  if (findEntry(info, name) == NULL) {
     // File doesn't exist, early exit
     return NULL;
  }

  // Copy file data to their own buffers for more expansion
  __PHYSFS_DirTree* tree = (__PHYSFS_DirTree *) &info->tree;
  __PHYSFS_DirTreeEnumerate(tree, "", callback_copy_files, "", opaque);

  info->open_write_handles++;

  SARC_file_ctx* file_info = allocator.Malloc(sizeof(SARC_file_ctx));
  if (file_info == NULL) {
    printf("%s SARC_openWrite(): Failed to allocate %li bytes for SARC_file_ctx.\n", module_name_err, sizeof(SARC_file_ctx));
    return NULL;
  }
  else {
    file_info->curPos = 0;
    file_info->entry = findEntry(info, name);
    file_info->io = info->io->duplicate(info->io);
    file_info->arc_info = opaque;
  }

  PHYSFS_Io* handle = allocator.Malloc(sizeof(PHYSFS_Io));
  if (handle == NULL) {
    printf("%s SARC_openWrite(): Failed to allocate %li bytes for PHYSFS_Io return value.\n", module_name_err, sizeof(PHYSFS_Io));
  }
  else {
    // Use SARC_Io as our I/O handler.
    *handle = SARC_Io;
    handle->opaque = file_info;
  }
  return handle;
} /* SARC_openWrite */

PHYSFS_Io *SARC_openAppend(void *opaque, const char *name) {
  PHYSFS_Io* io = SARC_openWrite(opaque, name);
  io->seek(io, io->length(io)); // Move position to end of file
  return io;
} /* SARC_openAppend */

int SARC_remove(void *opaque, const char *name) {
  BAIL(PHYSFS_ERR_READ_ONLY, 0);
} /* SARC_remove */

int SARC_mkdir(void *opaque, const char *name) {
  BAIL(PHYSFS_ERR_READ_ONLY, 0);
} /* SARC_mkdir */

int SARC_stat(void *opaque, const char *path, PHYSFS_Stat *stat) {
  SARC_ctx *info = (SARC_ctx *) opaque;
  const SARCentry *entry = findEntry(info, path);

  BAIL_IF_ERRPASS(!entry, 0);

  if (entry->tree.isdir) {
    stat->filetype = PHYSFS_FILETYPE_DIRECTORY;
    stat->filesize = 0;
  } /* if */
  else {
    stat->filetype = PHYSFS_FILETYPE_REGULAR;
    stat->filesize = entry->size;
  } /* else */

  stat->modtime = 0;
  stat->createtime = 0;
  stat->accesstime = -1;
  stat->readonly = 1;

  return 1;
} /* SARC_stat */

void* SARC_addEntry(void* opaque, char* name, const int isdir,
                    const PHYSFS_sint64 ctime, const PHYSFS_sint64 mtime,
                    const PHYSFS_uint64 pos, const PHYSFS_uint64 len) {
  SARC_ctx* info = (SARC_ctx*) opaque;
  SARCentry* entry;

  entry = (SARCentry*) __PHYSFS_DirTreeAdd(&info->tree, name, isdir);
  BAIL_IF_ERRPASS(!entry, NULL);

  entry->startPos = isdir ? 0 : pos;
  entry->size = isdir ? 0 : len;

  return entry;
} /* SARC_addEntry */

void* SARC_init_archive(PHYSFS_Io* io) {
  SARC_ctx* info = (SARC_ctx *) allocator.Malloc(sizeof (SARC_ctx));
  BAIL_IF(!info, PHYSFS_ERR_OUT_OF_MEMORY, NULL);

  bool case_sensitive = true;
  bool only_us_ascii = false;

  if (!__PHYSFS_DirTreeInit(&info->tree, sizeof (SARCentry), case_sensitive, only_us_ascii)) {
    allocator.Free(info);
    return NULL;
  }
  info->io = io;

  return info;
}

bool SARC_loadEntries(PHYSFS_Io* io, uint32_t count, uint32_t files_offset, SARC_ctx* archive) {
  uint32_t name_pos = sizeof(sarc_header) + sizeof(sarc_sfat_header);
  name_pos += (sizeof(sarc_sfat_node) * count) + sizeof(sarc_sfnt_header);
  uint32_t name_buf_size = files_offset - name_pos;

  char* name_buffer = allocator.Malloc(name_buf_size);
  if (name_buffer == NULL) {
    PHYSFS_setErrorCode(PHYSFS_ERR_OUT_OF_MEMORY);
    return false;
  }

  // Save our place, jump to the list of names
  uint32_t read_pos = io->tell(io);
  io->seek(io, name_pos);
  io->read(io, name_buffer, name_buf_size);
  io->seek(io, read_pos);

  name_pos = 0; // Reset name position offset so we start reading from the first filename.

  for (uint32_t i = 0; i < count; i++) {
    sarc_sfat_node node = {0};
    io->read(io, &node, sizeof(node));
    uint32_t size = node.file_end_offset - node.file_start_offset;

    // Jump to the next 4-byte alignment boundary
    while((name_pos % 4) != 0) {
      name_pos++;
    }

    uint32_t file_pos = node.file_start_offset + files_offset;

    char* name = name_buffer + name_pos;
    SARC_addEntry(archive, name, 0, -1, -1, file_pos, size);
    name_pos += strlen(name) + 1;
  }
  allocator.Free(name_buffer);

  return true;
}

void* SARC_openArchive(PHYSFS_Io* io, const char* name, int forWriting, int* claimed) {
  assert(io != NULL); // Sanity check.

  static const char magic[] = "SARC";
  sarc_header header = {0};
  io->read(io, &header, sizeof(header));

  if (strncmp((char*) &header.magic, magic, 4) != 0) {
    BAIL(PHYSFS_ERR_UNSUPPORTED, NULL);
  }
  // Claim the archive, because it's a valid SARC
  *claimed = 1;

  sarc_sfat_header sfat_header = {0};
  io->read(io, &sfat_header, sizeof(sfat_header));

  SARC_ctx* archive = SARC_init_archive(io);
  BAIL_IF_ERRPASS(!archive, NULL);

  archive->arc_filename = allocator.Malloc(strlen(name) + 1);
  strcpy(archive->arc_filename, name);

  SARC_loadEntries(io, sfat_header.node_count, header.data_offset, archive);
  return archive;
}

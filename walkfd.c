#include <dirent.h>   // for DT_DIR, DT_LNK, DT_REG, DT_UNKNOWN
#include <errno.h>
#include <fcntl.h>    // for openat, O_DIRECTORY, O_RDONLY
#include <glib.h>     // for g_hash_table_contains, GHashTable
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>  // for SYS_getdents
#include <unistd.h>

#include "walkfd.h"


/* A method that walks an open directory file descriptor, checks whether
 * traversed files are in a hashset, and if so, prints them. Returns 0
 * unless an error occurred, otherwise -1. Leaves errno set on error.
 *
 * Parameters:
 *  -> fd: open file descriptor to traverse
 *  -> root: path that the hashset files are relative to (used for printing)
 *  -> rel_path: the path (relative to root) of the currently open directory
 *       note: the hashset only contains the relative path
 *  -> symlinks: whether to print unexpected symlinks (or only files)
 *  -> silent: whether to print permission errors on directories
 *  -> hs: the GHashTable containing all file paths to check against
 *
 * Why use a custom tree walker instead of <fts.h> or <ftw.h>? When using
 *   <fts.h> with FTS_NOSTAT you can't use the DIRENT to distinguish symlinks
 *   from real files. <ftw.h> calls stat on each file.
 *
 * FIXME: we naively traverse the directories and hold open a file descriptor
 *   at each recursion. On sensible modern Arch systems this shouldn't be a
 *   problem, but in theory we could run out.
 */
int walkfd(int fd, char* root, char* rel_path, bool symlinks,
            bool silent, GHashTable* hs) {
    if(fd == -1) {
        // don't fail on access errors, print a warning and continue instead
        if (errno == EACCES) {
            if (!silent) {
                fprintf(stderr,
                        "Cannot open directory '%s%s': permission denied\n",
                        root, rel_path);
            }
            errno = 0;
            return 0;
        } else {
            fprintf(stderr, "Cannot open directory '%s%s': error %d\n",
                    root, rel_path, errno);
            return -1; // treat unknown errors as fatal
        }
    }

    // read through every entry in directory
    size_t path_length = strlen(rel_path);
    long nread;
    char buf[8192];
    struct linux_dirent* entry;
    while (true) {
        // using a syscall is ugly, but since we already can't use fts/ftw
        // it's not that much worse than readdir; it's also ~30% faster ;-)
        nread = syscall(SYS_getdents, fd, buf, 8192);
        if (nread == -1) {
            fprintf(stderr, "Failed to get directory entries!\n");
            return -1;
        }
        if (nread == 0)
            break;

        for (long bpos = 0; bpos < nread;) {
            entry = (struct linux_dirent *) (buf + bpos);
            unsigned char type = *(buf + bpos + entry->d_reclen - 1);
            bpos += entry->d_reclen;

            /* Reconstruct the full path
             * Not wasteful, we have to check the hashmap for the path anyway.
             * Note: for efficiency, we reuse the same string in memory
             *   repeatedly. So we store the length of the previous path and
             *   append the new entry to it.
             */
            strcpy(rel_path + path_length, "/");
            strcpy(rel_path + (path_length + 1), entry->d_name);

            // verify that we have something sane
            // FIXME: this arises for (rare) file systems; call stat instead
            if (type == DT_UNKNOWN) {
                fprintf(stderr, "FAIL: could not get file type of %s%s\n",
                        root, rel_path);
                return -1;
            }

            // recurse
            else if (type == DT_DIR) {
                // readdir returns POSIX dot files
                if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                    continue;

                int nextfd = openat(fd, entry->d_name, O_DIRECTORY | O_RDONLY);
                int wd_err = walkfd(nextfd, root, rel_path, symlinks, silent, hs);
                close(nextfd);
                if (wd_err) {
                    return wd_err;
                }
            }

            // handle regular files
            else if (type == DT_REG || (type == DT_LNK && symlinks)) {
                if (!g_hash_table_contains(hs, rel_path)) {
                    printf("%s%s\n", root, rel_path);
                }
            }

            // if we get this far, it isn't a file type we care about, so continue
        }
    }

    // if all directory entries have been handled, then there's no error
    return 0;
}

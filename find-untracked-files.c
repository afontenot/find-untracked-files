#include <alpm.h>              // for alpm_db_get_pkgcache, alpm_file_t, alp...
#include <alpm_list.h>         // for alpm_list_next, alpm_list_t
#include <bits/getopt_core.h>
#include <dirent.h>            // for DT_DIR, DT_LNK, DT_REG, DT_UNKNOWN
#include <errno.h>
#include <fcntl.h>             // for openat, O_DIRECTORY, O_RDONLY, S_IFBLK
#include <getopt.h>            // for getopt_long
#include <glib.h>              // for GHashTable
#include <limits.h>            // for PATH_MAX
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>           // for SYS_getdents
#include <unistd.h>

struct linux_dirent {
    unsigned long  d_ino;
    off_t          d_off;
    unsigned short d_reclen;
    char           d_name[];
};

/* Why use a custom tree walker instead of <fts.h> or <ftw.h>? When using
 *   <fts.h> with FTS_NOSTAT you can't use the DIRENT to distinguish symlinks
 *   from real files. <ftw.h> calls stat on each file.
 *
 * FIXME: we naively traverse the directories and hold open a file descriptor
 *   at each recursion. On sensible modern Arch systems this shouldn't be a
 *   problem, but in theory we could run out.
 *
 * Parameters:
 *  -> fd: open file descriptor to traverse
 *  -> root: path that the alpm database is relative to
 *  -> alpm_path: the path (relative to root) of the currently open directory
 *  -> symlinks: whether to print unexpected symlinks (or only files)
 *  -> silent: whether to print permission errors on directories
 *  -> hs: the hashset containing all file paths known to the alpm db
 */
int walkfd(int fd, char* root, char* alpm_path, bool symlinks,
            bool silent, GHashTable* hs) {
    if(fd == -1) {
        // don't fail on access errors, print a warning and continue instead
        if (errno == EACCES) {
            if (!silent) {
                fprintf(stderr, "find-untracked-files: cannot open "
                        "directory '%s%s': Permission denied\n",
                        root, alpm_path);
            }
            errno = 0;
            return 0;
        } else {
            fprintf(stderr, "cannot open directory '%s%s': error %d\n",
                    root, alpm_path, errno);
            return -1; // treat unknown errors as fatal
        }
    }

    // read through every entry in directory
    size_t path_length = strlen(alpm_path);
    long nread;
    char buf[8192];
    struct linux_dirent* entry;
    while (true) {
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
             * Note: for efficiency, we create the path using the same string
             *   in memory repeatedly. So we store the length of the previous
             *   path and append the new entry to it.
             */
            strcpy(alpm_path + path_length, "/");
            strcpy(alpm_path + (path_length + 1), entry->d_name);

            // verify that we have something sane
            if (type == DT_UNKNOWN) {
                fprintf(stderr, "FAIL: could not get file type of %s%s\n",
                        root, alpm_path);
                return -1;
            }

            // recurse
            if (type == DT_DIR) {
                // readdir returns POSIX dot files
                if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                    continue;

                int nextfd = openat(fd, entry->d_name, O_DIRECTORY | O_RDONLY);
                int wd_err = walkfd(nextfd, root, alpm_path, symlinks, silent, hs);
                close(nextfd);
                if (wd_err) {
                    return wd_err;
                }
            }

            // handle regular files
            if (type == DT_REG || (type == DT_LNK && symlinks)) {
                // in the db, paths are missing the root dir, so remove it
                if (!g_hash_table_contains(hs, alpm_path)) {
                    printf("%s%s\n", root, alpm_path);
                }
            }

            // if we get this far, it isn't a file type we care about, so continue
        }
    }

    // if all directory entries have been handled, then there's no error
    return 0;
}

void print_help(char exec_path[]){
    static const char* const helptext =
        "Usage: %s [OPTION]... [DIR]...\n"
        "Search DIRs for any files not tracked by a Pacman database.\n\n"
        "One or more DIR may be specified and will be searched sequentially.\n\n"
        "Mandatory arguments to long options are mandatory for short options too.\n"
        "  -r, --root=DIR       Specifies the root directory for package installations\n"
        "                         (default DIR: /)\n"
        "  -d, --db=DIR         Specifies the location of the Pacman database\n"
        "                         (default DIR: /var/lib/pacman)\n"
        "  -n, --no-symlinks    Disables checking the package database for symlinks\n"
        "  -q, --quiet          Disables printing an error upon access failures\n\n\n"
        "Issue tracker: https://github.com/afontenot/find-untracked-files\n"
        "License: GPL-3.0-or-greater https://www.gnu.org/licenses/gpl-3.0.en.html\n";
    printf(helptext, exec_path);
}

int main(int argc, char* argv[]) {
    // default arguments
    char default_root[] = "/";
    char* root = malloc(strlen(default_root) + 1);
    strcpy(root, default_root);
    char default_db[] = "/var/lib/pacman";
    char* db = malloc(strlen(default_db) + 1);
    strcpy(db, default_db);
    bool nosymlinks = false;
    bool silent = false;

    // parse arguments
    while (true) {
        int opt;
        int option_index = 0;
        static struct option long_options[] = {
            {"root",        required_argument, NULL, 'r'},
            {"db",          required_argument, NULL, 'd'},
            {"no-symlinks", no_argument,       NULL, 'n'},
            {"quiet",       no_argument,       NULL, 'q'},
            {"help",        no_argument,       NULL, 'h'},
            {NULL,          0,                 NULL,  0 }
        };

        opt = getopt_long(argc, argv, "r:d:nqh", long_options, &option_index);
        if (opt == -1)
            break;

        switch (opt) {
        case 'r':
            root = realloc(root, strlen(optarg) + 1);
            if (root == NULL) {
                fprintf(stderr, "failed to realloc string for root argument\n");
                exit(EXIT_FAILURE);
            }
            strcpy(root, optarg);
            break;

        case 'd':
            db = realloc(db, strlen(optarg) + 1);
            if (db == NULL) {
                fprintf(stderr, "failed to realloc string for db argument\n");
                exit(EXIT_FAILURE);
            }
            strcpy(db, optarg);
            break;

        case 'n':
            nosymlinks = true;
            break;

        case 'q':
            silent = true;
            break;

        case 'h':
            print_help(argv[0]);
            exit(EXIT_SUCCESS);

        case '?':
            print_help(argv[0]);
            exit(EXIT_FAILURE);

        default:
            fprintf(stderr, "?? getopt returned character code 0%o ??\n", opt);
            exit(EXIT_FAILURE);
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "No directory specified to search.\n\n");
        print_help(argv[0]);
        exit(EXIT_FAILURE);
    }

    // initialize hash set
    GHashTable* hs = g_hash_table_new(g_str_hash, g_str_equal);

    // get handle to local database
    alpm_errno_t alpm_err;
    alpm_handle_t* handle = alpm_initialize(root, db, &alpm_err);
    if (!handle) {
        fprintf(stderr, "cannot initialize alpm: %s\n",
                alpm_strerror(alpm_err));
        exit(EXIT_FAILURE);
    }

    /* FIXME: figure out why alpm_initialize is setting errno
     *
     * alpm_initialize apparently sets errno for some correctable failures; we
     * have to reset it to zero here because readdir does not unambiguously
     * signal failure, thus requiring explicit errno checks
     */
    errno = 0;

    alpm_db_t* localdb = alpm_get_localdb(handle);

    // get a list of local packages
    alpm_list_t* pkgs = alpm_db_get_pkgcache(localdb);

    // loop over local packages, add to set
    for (alpm_list_t* lp = pkgs; lp; lp = alpm_list_next(lp)) {
        alpm_pkg_t* pkg = lp->data;
        alpm_filelist_t* filelist = alpm_pkg_get_files(pkg);
        for(size_t i = 0; i < filelist->count; i++) {
            const alpm_file_t* file = filelist->files + i;
            g_hash_table_add(hs, file->name);
        }
    }

    // remaining args are all user-chosen paths to search
    for (size_t i = optind; i < argc; i++) {
        char* path = malloc(PATH_MAX);
        strcpy(path, *(argv + i));

        // because we match path strings exactly, delete trailing slash
        if (path[strlen(path)-1] == '/')
            path[strlen(path)-1] = '\0';

        // sanity check: path must be inside the specified root
        if (strncmp(root, path, strlen(root))) {
            fprintf(stderr, "Error: path '%s' not in the root '%s'\n",
                    path, root);
            exit(EXIT_FAILURE);
        }

        // get the path we expect to find in the database (without root)
        char* alpm_path = path + strlen(root);

        // walk through file system
        int fd = open(path, O_DIRECTORY | O_RDONLY);
        int walkerr = walkfd(fd, root, alpm_path, !nosymlinks, silent, hs);
        close(fd);
        if (walkerr) {
            if (errno)
                fprintf(stderr, "Error: %d\n", errno);

            exit(EXIT_FAILURE);
        }
        free(path);
    }

    // clean up alpm
    alpm_release(handle);

    // clean up hashset
    g_hash_table_destroy(hs);

    // free strings for arguments
    free(root);
    free(db);

    exit(EXIT_SUCCESS);
}

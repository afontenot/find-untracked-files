#include <alpm.h>
#include <alpm_list.h>
#include <bits/getopt_core.h>  // for optarg, optind
#include <errno.h>
#include <fcntl.h>             // for openat, O_DIRECTORY, O_RDONLY
#include <getopt.h>            // for getopt_long, etc
#include <glib.h>              // for GHashTable, etc
#include <limits.h>            // for PATH_MAX
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "walkfd.h"


/* Program structure:
 *
 *  1. Open a handle to an alpm database at a user specified location.
 *  2. Creates a hashset with every filepath part of an installed package.
 *  3. Given a list of user specified paths, the program recursively walks
 *     the file system for each path, and for each file (and optionally
 *     symlink) checks whether it is part of an installed package, and if
 *     not, prints it.
 */


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
            printf(helptext, argv[0]);
            exit(EXIT_SUCCESS);

        case '?':
            printf(helptext, argv[0]);
            exit(EXIT_FAILURE);

        default:
            fprintf(stderr, "?? getopt returned character code 0%o ??\n", opt);
            exit(EXIT_FAILURE);
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "No directory specified to search.\n\n");
        printf(helptext, argv[0]);
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

    // FIXME: figure out why alpm_initialize is setting errno
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
                fprintf(stderr, "errno %d\n", errno);

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

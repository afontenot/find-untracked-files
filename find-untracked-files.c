#include <alpm.h>              // for alpm_db_get_pkgcache, alpm_file_t, alp...
#include <alpm_list.h>         // for alpm_list_next, alpm_list_t
#include <bits/struct_stat.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>            // for getopt_long
#include <glib.h>              // for GHashTable
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// fallback method that calls lstat to get file type
int getfiletype(char* path) {
    struct stat sb;
    int stat_err = lstat(path, &sb);

    // bubble up errors
    if (stat_err == -1)
        return -1;

    // get the file type flags
    int fmt = (sb.st_mode & S_IFMT);

    // return dirent.h type from stat.h type
    return (fmt == S_IFREG) ? DT_REG :
           (fmt == S_IFDIR) ? DT_DIR :
           (fmt == S_IFLNK) ? DT_LNK :
           (fmt == S_IFBLK) ? DT_BLK :
           (fmt == S_IFCHR) ? DT_CHR :
           (fmt == S_IFIFO) ? DT_FIFO :
           (fmt == S_IFSOCK) ? DT_SOCK :
           DT_UNKNOWN;
}

/* Why use a custom tree walker instead of <fts.h> or <ftw.h>? When using
 *   <fts.h> with FTS_NOSTAT you can't use the DIRENT to distinguish symlinks
 *   from real files. <ftw.h> calls stat on each file.
 * FIXME: we naively traverse the directories and hold open a file descriptor
 *   at each recursion. On sensible modern Arch systems this shouldn't be a
 *   problem, but in theory we could run out.
 * Arguments: requires a callback function to be passed that takes a string
 *   and a GHashTable; we pass the latter on directly, and it determines
 *   whether to print the string based on whether it is in the hashset.
 */
int walkdir(char* path, int symlinks, bool silent,
            int callback(char*, GHashTable*), GHashTable* hs) {
    DIR* dir = opendir(path);
    if(!dir) {
        // don't fail on access errors, print a warning and continue instead
        if (errno == EACCES) {
            if (!silent) {
                fprintf(stderr,
                        "find-untracked-files: cannot open "
                        "directory '%s': Permission denied\n",
                        path);
            }
            errno = 0;
            return 0;
        } else {
            fprintf(stderr,
                    "cannot open directory '%s': error %d\n",
                    path,
                    errno);
            return -1; // treat unknown errors as fatal
        }
    }

    // read through every entry in directory
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        /* reconstruct the full path
         * not too wasteful, we have to check the hashmap for the path anyway
         * +2: account for \0 termination and additional '/'
         */
        size_t pathlen = strlen(path) + strlen(entry->d_name) + 2;
        char fullpath[pathlen];
        strcpy(fullpath, path);
        strcat(fullpath, "/"); // we know that path is not terminated with /
        strcat(fullpath, entry->d_name);

        /* If readdir adds d_type to our dirent we can avoid calling stat.
         * Most systems support this, but an unusual FS may return DT_UNKNOWN.
         */
#ifdef _DIRENT_HAVE_D_TYPE
        unsigned char type = entry->d_type;
        if (type == DT_UNKNOWN)
            type = getfiletype(fullpath);
#else
        unsigned char type = getfiletype(fullpath);
#endif

        // verify that we have something sane
        if (type == DT_UNKNOWN) {
            fprintf(stderr, "FAIL: could not get "
                    "file type of %s\n", fullpath);
            return -1;
        }

        // recurse
        if (type == DT_DIR) {
            // readdir returns POSIX dot files
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                continue;

            int wd_err = walkdir(fullpath, symlinks, silent, callback, hs);
            if (wd_err)
                return wd_err;
        }

        // handle regular files
        if (type == DT_REG || (type == DT_LNK && symlinks)) {
            int cb_error = callback(fullpath, hs);
            if (cb_error)
                return cb_error;
        }

        // if we get this far, it isn't a file type we care about, so continue
    }

    /* readdir() exits with NULL on error AND after returning the last file,
     * so we have to check for errors explicitly. See `man 3 readdir`
     */
    if (errno)
        return -1;

    // clean up
    closedir(dir);
    return 0;
}

// decides whether to print a file path; function passed to walkdir()
int printdir(char* filepath, GHashTable* hs) {
    if (!g_hash_table_contains(hs, filepath)) {
        printf("%s\n", filepath);
    }
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

        opt = getopt_long(argc, argv, "r:d:nqh",
                          long_options, &option_index);
        if (opt == -1)
            break;

        switch (opt) {
        case 'r':
            root = realloc(root, strlen(optarg) + 1);
            if (root == NULL) {
                printf("failed to realloc string for root argument\n");
                exit(EXIT_FAILURE);
            }
            strcpy(root, optarg);
            break;

        case 'd':
            db = realloc(db, strlen(optarg) + 1);
            if (db == NULL) {
                printf("failed to realloc string for db argument\n");
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
            printf("?? getopt returned character code 0%o ??\n", opt);
            exit(EXIT_FAILURE);
        }
    }

    if (optind >= argc) {
        printf("No directory specified to search.\n\n");
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

    /* we modify the file paths to add the root file path, so we have to keep
     * the pointers in scope for the lifetime of the hash table
     */
    size_t filepaths_len = 1000;
    char** filepaths;
    filepaths = malloc(filepaths_len * sizeof(void*));

    // cache root length
    size_t root_len = strlen(root);

    // loop over local packages, add to set
    size_t filepath_i = 0;
    for (alpm_list_t* lp = pkgs; lp; lp = alpm_list_next(lp)) {
        alpm_pkg_t* pkg = lp->data;
        alpm_filelist_t* filelist = alpm_pkg_get_files(pkg);
        for(size_t i = 0; i < filelist->count; i++) {
            if (filepath_i >= filepaths_len) {
                filepaths_len *= 2;
                filepaths = realloc(filepaths, filepaths_len * sizeof(void*));
                if (filepaths == NULL) {
                    fprintf(stderr, "realloc of file path array failed!\n");
                    exit(EXIT_FAILURE);
                }
            }

            const alpm_file_t* file = filelist->files + i;
            filepaths[filepath_i] = malloc(root_len + strlen(file->name) + 1);
            strcpy(filepaths[filepath_i], root);
            strcat(filepaths[filepath_i], file->name);
            g_hash_table_add(hs, filepaths[filepath_i]);
            filepath_i++;
        }
    }

    // clean up alpm since we're caching modified path names
    alpm_release(handle);

    // remaining args are all user-chosen paths to search
    for (size_t i = optind; i < argc; i++) {
        char* path = strdup(*(argv + i));

        // because we match path strings exactly, delete trailing slash
        if (path[strlen(path)-1] == '/')
            path[strlen(path)-1] = '\0';

        // walk through file system
        int rd_error = walkdir(path, !nosymlinks, silent, printdir, hs);
        if (rd_error) {
            if (errno)
                fprintf(stderr, "Error: %d\n", errno);

            exit(EXIT_FAILURE);
        }

        free(path);
    }

    // deallocate file path array
    for (size_t i = 0; i <= filepath_i; i++) {
        free(filepaths[i]);
    }
    free(filepaths);

    // clean up hashset
    g_hash_table_destroy(hs);

    // free strings for arguments
    free(root);
    free(db);

    exit(EXIT_SUCCESS);
}

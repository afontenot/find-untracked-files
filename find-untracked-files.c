#include <alpm.h>              // for alpm_db_get_pkgcache, alpm_file_t, alp...
#include <alpm_list.h>         // for alpm_list_next, alpm_list_t
#include <bits/struct_stat.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "lib/argparse.h"      // for argparse_describe, argparse_init, argp...
#include "lib/cc_hashset.h"    // for CC_HashSet, cc_hashset_add, cc_hashset...

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

// Why use a custom tree walker instead of <fts.h>?
//   With FTS_NOSTAT set, you can't use the FTSENT
//   to distinguish symlinks from real files.
//   <ftw.h> unavoidably calls stat on each file.
int walkdir(char* path, int symlinks, int (* callback)(char*, CC_HashSet*), CC_HashSet* hs) {
    DIR* dir = opendir(path);
    if(!dir) {
        if (errno == EACCES) {
            fprintf(stderr, 
                    "find-untracked-files: cannot open "
                    "directory '%s': Permission denied\n", 
                    path);
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
    // on supported systems, we avoid calling stat
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        // reconstruct the full path
        // account for \0 termination and additional '/'
        int pathlen = strlen(path) + strlen(entry->d_name) + 2;
        char fullpath[pathlen];
        snprintf(fullpath, pathlen, "%s/%s", path, entry->d_name);

        // on supported systems, we avoid calling stat, which is slow
        // most systems should have d_type
        // but weird FS may ret DT_UNKNOWN
#ifdef _DIRENT_HAVE_D_TYPE
        unsigned char type = entry->d_type;
        if (type == DT_UNKNOWN) {
            type = getfiletype(fullpath);
        }
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
            if (!strcmp(entry->d_name, ".") || 
                !strcmp(entry->d_name, ".."))
                continue;

            int wd_err = walkdir(fullpath, symlinks, callback, hs);
            if (wd_err)
                return wd_err;
        }

        // handle regular files
        if (type == DT_REG || (type == DT_LNK && symlinks)) {
            int cb_error = callback(fullpath, hs);
            if (cb_error)
                return cb_error;
        }

        // we ignore anything else (e.g. block devices, etc)
        // so if we get to this point, we're done
    }

    // readdir() exits with NULL on error or finishing,
    // so we have to check for errors explicitly
    // see `man 3 readdir`
    if (errno)
        return -1;

    // clean up
    closedir(dir);
    return 0;
}

int printdir(char *filepath, CC_HashSet* hs) {
    void* path_ptr = filepath + 1; 
    if (!cc_hashset_contains(hs, path_ptr)) {
        printf("%s\n", filepath);
    }
    return 0;
}

int main(int argc, const char* argv[]) {
    // usage data for argparse
    static const char* const usage[] = {
        "find-untracked-files [options] "
        "/path/to/search [/other/paths]",
        NULL,
    };

    // parse arguments - initialize with defaults
    char root[] = "/";
    char db[] = "/var/lib/pacman";
    int nosymlinks = 0;
    struct argparse_option options[] = {
        OPT_HELP(),
        OPT_STRING('r', "root", &root,
                   "path to the root dir for pkg install "
                   "(default: '/')"),
        OPT_STRING('d', "db", &db,
                   "path to the pkg database "
                   "(default: '/var/lib/pacman')"),
        OPT_BOOLEAN('s', "no-symlinks", &nosymlinks,
                    "disable checking symbolic links "),
        OPT_END(),
    };
    struct argparse argparse;
    argparse_init(&argparse, options, usage, 0);
    argparse_describe(&argparse, 
                      "\nFind files not part of any Pacman package",
                      "");
    argc = argparse_parse(&argparse, argc, argv);

    // handle additional arguments (file paths)
    if (argc == 0) {
        fprintf(stderr, "No directory specified to search.\n");
        exit(EXIT_FAILURE);
    } 

    // initialize hash set
    CC_HashSet* hs;
    cc_hashset_new(&hs);
    
    // get handle to local database
    alpm_errno_t alpm_err;
    alpm_handle_t* handle = alpm_initialize(root, db, &alpm_err);
    if (!handle) {
        fprintf(stderr, "cannot initialize alpm: %s\n", 
                alpm_strerror(alpm_err));
        exit(EXIT_FAILURE);
    }

    // FIXME: figure out why alpm_initialize is setting errno
    //
    // alpm_initialize apparently sets errno for some correctable 
    // failures; we have to reset it to zero here because readdir 
    // does not unambiguously signal failure, thus requiring 
    // explicit errno checks
    errno = 0;

    alpm_db_t* localdb = alpm_get_localdb(handle);

    // get a list of local packages
    alpm_list_t* pkgs = alpm_db_get_pkgcache(localdb);

    // loop over local packages, add to set
    for (alpm_list_t* lp = pkgs; lp; lp = alpm_list_next(lp)) {
        alpm_pkg_t* pkg = lp->data;
        alpm_filelist_t* filelist = alpm_pkg_get_files(pkg);
        for(size_t i = 0; i < filelist->count; i++) {
            const alpm_file_t *file = filelist->files + i;
            cc_hashset_add(hs, file->name);
        }
    }
    
    // remaining args are all user-chosen paths to search
    for (int i = 0; i < argc; i++) {
        char* path = strdup(*(argv + i));

        // because we match path strings exactly, delete trailing slash
        if (path[strlen(path)-1] == '/')
            path[strlen(path)-1] = '\0';
        
        // walk through file system
        int rd_error = walkdir(path, (1-nosymlinks), printdir, hs);
        if (rd_error) {
            if (errno)
                fprintf(stderr, "Error: %d\n", errno);
        
            exit(EXIT_FAILURE);
        }

        free(path);
    }

    cc_hashset_destroy(hs);
    alpm_release(handle);
    exit(EXIT_SUCCESS);
}

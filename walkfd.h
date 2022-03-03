#include <fcntl.h>    // for off_t
#include <glib.h>     // for GHashTable
#include <stdbool.h>

// struct representing an entry returned by a getdents syscall
struct linux_dirent {
    unsigned long  d_ino;
    off_t          d_off;
    unsigned short d_reclen;
    char           d_name[];
};

int walkfd(int fd, char* root, char* rel_path, bool symlinks,
            bool silent, GHashTable* hs);

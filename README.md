# find-untracked-files
Find files in a directory that are not owned by any Arch Linux package

## What is it for?
Have you ever encountered this problem?

    $ sudo pacman -S maven
    error: failed to commit transaction (conflicting files)
    maven: /usr/bin/mvn exists in filesystem

As you probably know, on most Linux systems, including Arch Linux,
you're not really supposed to install things manually to system 
directories (`/usr/local` excepted). Files placed in these locations
may become outdated or get left behind, and they conflict with the
official repository packages for the same program (or others).

But it's easy to make this mistake. I wanted to try to clean up my 
`/usr` by finding and selectively removing untracked files. This 
turned out to be harder than expected. You might consider this 
approach, for example:

    find /usr -type f -exec pacman -Qo {} \;

This proved to be unusably slow for me. The obvious problem is that 
this runs pacman once for every file in `/usr`. Fortunately, 
`pacman -Qo` accepts multiple file paths, so next you try:

    find /usr/ -type f -exec echo {} \; | parallel -X pacman -Qo

This too, is unusably slow. As it turns out, the real problem is
that the way pacman checks for file ownership is to check the file 
list of every single package installed to see if it contains the
file you're asking about. It does this for every single file,
even if you request ownership data for multiple files.

    for(i = packages; i && (!found || is_dir); i = alpm_list_next(i)) {
        if(alpm_filelist_contains(alpm_pkg_get_files(i->data), rel_path)) {
            print_query_fileowner(rpath, i->data);
            found = 1;
        }
    }

On my computer, I have over 750,000 files in `/usr`. This approach 
clearly won't work.

**Instead** I wrote a C program that requests the file lists 
directly from ALPM and *caches* them using a hash table, making the
search for each file very fast. (Hash table lookups have O(1) 
average complexity.)

Note that the C version of the program (as opposed to the default
Python implementation, which is only about twice as slow) is
experimental.

## How do I use the program?

First, install 
[`meson`](https://archlinux.org/packages/extra/any/meson/).

Build the program with

    meson build && ninja -C build

The resulting binary is in the `build` directory.

Basic usage is simple:

    ./find-untracked-files /path/to/search

This will use the default location for installed packages (the root
directory, '/') and for the database location ('/var/lib/pacman').
If for some reason you have different settings, you can set them with
`--root` and `--db`.

You can also specify multiple search paths:

    ./find-untracked-files /path1 /path2

You can disable searching for symbolic links in Arch packages:

    ./find-untracked-files -s /path/to/search

Basic help is available in the program:

    ./find-untracked-files -h

## How fast is it?

Scanning my 750,000 file directory and checking for unowned files 
takes my computer only ~1.5 seconds.

The find command utilizing parallel took more than 420 times longer
to run! This is slow enough that it's difficult to re-run it to see
changes after you've removed certain files or packages.

`libalpm` is smart enough not to touch the disk twice to get the 
file list for a package as long as its handle stays alive, so the
parallel approach is actually *vastly* better than the naive `find`.
If you call `pacman` once for every file, obviously this caching
doesn't happen. A rough calculation suggests that the 
non-parallelized version would take nearly a week to run.

## Alternatives

The [Arch Wiki](https://wiki.archlinux.org/title/Pacman/Tips_and_tricks)
suggests a program called `pacreport` to find unowned files.
Unfortunately, this program is rather flawed. It doesn't allow you to
scan particular directories, instead the whole file system is scanned
on every run (with certain directories hardcoded as exceptions).

Also, if you check the 
[code](https://github.com/andrewgregory/pacutils/blob/master/src/pacreport.c#L484)
for `pacreport`, you'll see that despite being written to solve this 
exact problem, it does the same thing that `pacman` does. It scans
your entire package database for every single file on your system
to see if anything owns the file. For that reason, `pacreport` is
extremely slow, similar to the approaches I mentioned in the 
discussion above.

`pacreport` does allow you to exclude certain files and directories
from being scanned, which I have not yet implemented for this
program.

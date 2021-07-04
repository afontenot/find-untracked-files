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

**Instead** I wrote a simple program in Python that requests the 
file lists from ALPM (technically, the pyalpm bindings for Python,
available in the `extra` repository) and *caches* them, doing the 
search for each file only once.

## How do I use the program?

First, install 
[`pyalpm`](https://archlinux.org/packages/extra/x86_64/pyalpm/) 
using Pacman.

Basic usage is simple:

    python find-untracked-files.py /path/to/search

This will use the default location for installed packages (the root
directory, '/') and for the database location ('/var/lib/pacman').
If for some reason you have different settings, you can set them with
`--root` and `--db`.

You can also specify multiple search paths:

    python find-untracked-files.py /path1 /path2

Basic help is available in the program:

    python find-untracked-files.py -h

## How fast is it?

Scanning my 750,000 file directory and checking for unowned files 
takes my computer 4 seconds (on an SSD). I don't know exactly how
much faster this is than the `find` approach above, because the 
latter still hasn't finished running while I was writing this README.

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

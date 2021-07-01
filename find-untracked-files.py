from os import walk

import argparse
from pyalpm import Handle

parser = argparse.ArgumentParser(description="Find files not owned by any Pacman package")
parser.add_argument("--root", default="/", help="the path to the directory your packages get installed to")
parser.add_argument("--db", default="/var/lib/pacman", help="the path to the directory your pkg database is in")
parser.add_argument("paths", nargs="+", help="paths to search for untracked files")
args = parser.parse_args()

handle = Handle(args.root, args.db)
localdb = handle.get_localdb()

# get a set containing every installed file path
filelist = []
for pkg in localdb.pkgcache:
    filelist.extend([args.root + x[0] for x in pkg.files])
filelist = set(filelist)

# walk every search path given by the user, printing untracked files
for searchpath in args.paths:
    for dirpath, _, files in walk(searchpath):
        for file in [dirpath + "/" + x for x in files]:
            if not file in filelist:
                print(file)

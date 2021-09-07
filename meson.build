project('find-untracked-files', 'c')
src = ['find-untracked-files.c', 
       'lib/argparse.c']
alpm = [dependency('libalpm')]
glib = [dependency('glib-2.0')]
cc = ['-O3', '-Wall', '-Wpedantic']
executable('find-untracked-files', sources : src, c_args : cc, dependencies : [alpm, glib])

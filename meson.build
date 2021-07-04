project('find-untracked-files', 'c')
src = ['find-untracked-files.c', 
       'lib/argparse.c',
       'lib/cc_array.c',
       'lib/cc_common.c',
       'lib/cc_hashset.c',
       'lib/cc_hashtable.c']
alpm = [dependency('libalpm')]
cc = ['-O3', '-Wall', '-Wpedantic']
executable('find-untracked-files', sources : src, c_args : cc, dependencies : alpm)

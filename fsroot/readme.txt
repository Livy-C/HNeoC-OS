HNeoFS
======

This file lives on a filesystem that HNeoC OS reads over ATA PIO.

On-disk layout (see include/hneofs.h):

  LBA 2048        superblock (512 bytes, magic 'HNFS')
  LBA 2049-2056   file table, 8 sectors = 64 entries of 64 bytes
  LBA 2057-...    file data, packed back to back

Directories are a tree, but a very simple one: every entry has a
"parent" field pointing at its containing directory, and the root has
no entry of its own. Looking up "/bin/ls" walks the tree one name
component at a time.

There are no inodes and no allocation bitmap. A file is just a starting
LBA plus a length, and the whole file table is read into memory once at
mount time. Free space is derived from the extents of the files that
exist, so deleting a file frees its space without any bookkeeping.

Deleting leaves a hole in the table rather than shifting entries,
because shifting would invalidate every parent link after it.

Try the shell commands:

  ls              list the root directory
  ls /bin         list a subdirectory
  cat readme.txt  print a file
  fsstat          show the superblock and disk usage

**FS Dev by jaes16**
=====
## Overview:
A log-structure file system, built on top of FUSE. This project is mainly a hobby. The file system provides the basic functionalities of a file system, and even a rudimentary garbage collection system, although the garbage collection has a very long way to go.

## Requirements for running:
- [macFUSE](https://osxfuse.github.io/): FUSE, or Filesystem in Userspace, is a software interface that allows users create their own file systems without editing kernel code. macFUSE is a version for the mac, but tweaking the makefile should allow for the file system to be made with [FUSE](https://github.com/libfuse/libfuse) on linux. 


## Running the FS:

1. Call `make` to create the kernel. The makefile will need tweaking according to how FUSE was installed.
2. Create a new directory in the `build` directory (ex. `build/test`.)
3. Run the file system on the new directory (ex. `build/fs build/test`)
4. Now the new directory is mounted with the file system.

## Supported FUSE operations:
```
.init
.destroy
.getattr
.fgetattr
.access
.readdir
.mkdir
.rmdir
.chmod
.mknod
.unlink
.truncate
.open
.read
.write
.statfs
```
The rest are still provided by FUSE

## To do:
- Improve Garbage Collection
- Symbolic Links
- More debugging...

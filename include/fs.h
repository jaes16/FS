/*  2020 (c) jms
    a file system loosely based on the log structured file system, assuming a SSD for hard drive.
    utilizing FUSE (file system in userspace, how that acronym works is beyond me) to implement infrastructure for translation of instructions to data instructions
    erase in segments, program and read in blocks. (perase - segment, pwrite (have to be an empty block) - block, pread - block)

    assuming that we are using a system where dirname and basename do not change pathname

*/


#ifndef FS_H
#define FS_H


#define FUSE_USE_VERSION 26

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <time.h>





////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// SIZES ///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// All sizes subject to possible change in the future



#define DRIVE_SIZE 10485760 // 10 mbytes
#define BLOCK_SIZE 4096 // 4 kbytes
#define SEGMENT_SIZE 131072 // 128 kbytes
#define DATA_SIZE 10223616 // DRIVE_SIZE - (2*SEGMENT_SIZE)
#define INODE_SIZE 100
#define ITABLE_ENTRY_SIZE 104 // INODE_SIZE + int
#define ITABLE_MAX_SIZE 259584 // ITABLE_ENTRY_SIZE * MAX_INODES
#define DT_ENTRY_SIZE 36 // 36 bytes
#define DIR_TABLE_SIZE 113 // BLOCK_SIZE/sizeof(dir_table)

#define MAX_BLOCKS 2496 // DATA_SIZE/BLOCK_SIZE
#define DATA_BLOCKS_IN_SEGMENT 32 // SEGMENT_SIZE/BLOCK_SIZE
#define MAX_SEGMENTS 78 // DATA_SIZE/SEGMENT_SIZE
#define MAX_INODES 2496 // = MAX_BLOCKS (sort of overshooting)
#define MAX_ITABLE_BLOCKS 64 // ITABLE_MAX_SIZE/BLOCK_SIZE rounded up
#define ITABLE_ENT_IN_BLOCK 39 // BLOCK_SIZE/ITABLE_ENTRY_SIZE
#define SEG_SUM_BLOCKS 2 // (MAX_BLOCKS * sizeof(short+char))/BLOCK_SIZE, rounded up
#define SEG_SUM_IN_BLOCK 42 // (BLOCK_SIZE/(sizeof(short+char))) /DATA_BLOCKS_IN_SEGMENT rounded down

#define DATA_START 131072 // = SEGMENT_SIZE
#define SECOND_CR 10354688 // DRIVE_SIZE - SEGMENT_SIZE

#define MAX_PATH 4096 // 4 kbytes
#define MAX_FILE_NAME 32




////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// DATA STRUCTURES /////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////




typedef struct                  // size: 100 bytes. Roughly following simplified ext2 structure.
{
  int mode; // permission bits
  int uid; // owner user id
  int size; // size in bytes
  int time; // last access time
  int ctime; // creation time
  int mtime; // last modified time
  int dtime; // deletion time
  int gid; // owning group id
  int links_count; // number of hard links to this file
  int block_count; // number of blocks allocated for this file
  int blocks[15]; // pointers to blocks (absolute locations). pointer 0 - 11 direct, 12 indirect, 13 doubly indirect, 14 triply indirect
} inode;




typedef struct // size: BLOCK_SIZE
{
  // if uninitialized
  int checkpoint_location;
  int itable_size;
  int data_location;
  int data_size;
  char pad[(BLOCK_SIZE-16)]; // to make sure the size of this struct is 4096
} superblock;

typedef struct // size: BLOCK_SIZE
{
  time_t timestamp; // 8bytes

  int num_itable_blocks;
  int num_inodes;
  char inode_numbers[MAX_INODES+1]; // because we don't use 0, because it doesn't help with checkin if dir_entries are empty. 2497 bytes

  int pointers[MAX_ITABLE_BLOCKS]; // 256 bytes, pointing to inode table blocks

  int segsum_loc[SEG_SUM_BLOCKS];
  // a list of which segments are being used. could use only bits, but we have an abundance of space left. 312 bytes
  int segments_usage[MAX_SEGMENTS]; // 0 for unused segment, 1 for used segment, 2 for segments just written out by garbage collection

  char pad[BLOCK_SIZE-3097];
} checkpoint;

typedef struct // size: SEGMENT_SIZE
{
  superblock sb;
  checkpoint cp[DATA_BLOCKS_IN_SEGMENT-1]; // to make sure the size of the region is a segment long
} checkpoint_region;




typedef struct // size: BLOCK_SIZE
{
  char liveness[SEG_SUM_IN_BLOCK][DATA_BLOCKS_IN_SEGMENT];
  short inode_num[SEG_SUM_IN_BLOCK][DATA_BLOCKS_IN_SEGMENT];
  char pad[BLOCK_SIZE - (3 * SEG_SUM_IN_BLOCK * DATA_BLOCKS_IN_SEGMENT)];
} seg_summary;




typedef struct // size: 104 = ITABLE_ENTRY_SIZE
{
  int inode_num;
  inode inode;
} itable_entry;



typedef struct // size: BLOCK_SIZE
{
  itable_entry num_to_inodes[ITABLE_ENT_IN_BLOCK];
  char pad[BLOCK_SIZE-(ITABLE_ENTRY_SIZE * ITABLE_ENT_IN_BLOCK)];
} i_table;


typedef struct // size: 36 bytes = 32 char + 1 int.
{
  char file_name[MAX_FILE_NAME];
  int inode_num;
} dir_table;


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// GLOBAL VARIABLES ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////




extern void *segment[DATA_BLOCKS_IN_SEGMENT]; // might not always be filled, depending on the size of the data the pointers point to
extern seg_summary ss[SEG_SUM_BLOCKS];
extern int which_seg;

extern i_table *itable; // table of inodes. Can grow and shrink as needed, in sizes of blocks. Must malloc realloc and free

extern dir_table d_table[DIR_TABLE_SIZE+1]; // table of dir entries,

extern checkpoint cp;
extern superblock sb;


extern FILE *drive;
extern int fd_drive;




////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE /
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////




void* fs_init(struct fuse_conn_info *conn);


void fs_destroy(void *privatedata);


// return file attributes, for path name, should fill in the elements of the stat struct
// if a field is meaningless then it should be set to 0
int fs_getattr(const char *path, struct stat *stbuf);


int fs_access(const char *path, int mask);


int fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi);


int fs_mkdir(const char* path, mode_t mode);


int fs_rmdir(const char* path);


int fs_chmod(const char* path, mode_t mode);


int fs_fgetattr(const char* path, struct stat* stbuf, struct fuse_file_info *fi);


int fs_mknod(const char* path, mode_t mode, dev_t rdev);


int fs_unlink(const char* path);


int fs_truncate(const char* path, off_t size);


int fs_open(const char* path, struct fuse_file_info* fi);


int fs_read(const char* path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi);


int fs_write(const char* path, const char *buf, size_t size, off_t offset, struct fuse_file_info* fi);


int fs_statfs(const char* path, struct statvfs* stbuf);


static struct fuse_operations fs_filesystem_operations = {
    .init       = fs_init,
    .destroy    = fs_destroy,
    .getattr    = fs_getattr,
    .fgetattr   = fs_fgetattr,
    .access     = fs_access,
    .readdir    = fs_readdir,
    .mkdir      = fs_mkdir,
    .rmdir      = fs_rmdir,
    .chmod      = fs_chmod,
    .mknod      = fs_mknod,
    .unlink     = fs_unlink,
    .truncate    = fs_truncate,
    .open        = fs_open,
    .read        = fs_read,
    .write       = fs_write,
    .statfs      = fs_statfs,

    /*
    .readlink    = fs_readlink,
    .symlink     = fs_symlink,
    .rename      = fs_rename,
    .link        = fs_link,
    .chown       = fs_chown,
    .ftruncate   = fs_ftruncate,
    .utimens     = fs_utimens,
    .create      = fs_create,
    .release     = fs_release,
    .opendir     = fs_opendir,
    .releasedir  = fs_releasedir,
    .fsync       = fs_fsync,
    .flush       = fs_flush,
    .fsyncdir    = fs_fsyncdir,
    .lock        = fs_lock,
    .bmap        = fs_bmap,
    .ioctl       = fs_ioctl,
    .poll        = fs_poll,
#ifdef HAVE_SETXATTR
    .setxattr    = fs_setxattr,
    .getxattr    = fs_getxattr,
    .listxattr   = fs_listxattr,
    .removexattr = fs_removexattr,
#endif
  */
    .flag_nullpath_ok = 0,

};


#endif

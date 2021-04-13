
#ifndef FS_HELPER_H
#define FS_HELPER_H


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
#include "fs.h"




////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE /
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////



// Just to make sure we're erasing in segment sizes. Just writing out 0's to segments
// making sure we're ignoring the first segment, because its off limits
void perase(int size_in_segments, int seg_num);


void write_checkpoint();


int write_seg_summary(int seg);


void flush_segment();


int get_segment();


void free_segment();


// return -1 if unable to allocate more space, otherwise returns the absolute address of the where the block was added
int add_to_segment(void *block, int address, int inode_num);


int read_block(void *buf, int address);


// When theres no drive, create generic drive
void create_init_drive();


// returns 0 if the inode was found, -1 if not
int find_inode(int inode_num, inode *id);


// changes the inode specified by inode_num. if add is 0, then we are adding a new inode to the table
// returns -1 for ENOSPC, 0 for success
int change_inode(int inode_num, inode *id, int add);


// returns -1 if the inode number couldn't be found, -2 if file_name is too long, and inode number otherwise
int find_inode_num(char *file_name, inode parent_id);


// returns -1 for enospc, -2 for nametoolong
int add_dir_entry(char *file_name, int inode_num, int parent_inode_num, inode *parent_id);


// returns 1 if it is unflushed memory in segment. otherwise returns 0
int is_unflushed_memory(int address);


// returns -1 for enospc
int remove_dir_entry(int inode_num, int parent_inode_num, inode *parent_id);


// saves the appropriate inode info in the inode pointer
// return inode_num if path exists, -1 if path doesn't exist, -2 if path name is too long, -3 if component of path is file
// later, can remove last and just find it from path in this function
int get_inode(const char *path, inode *id);


int garbage_collection();

#endif

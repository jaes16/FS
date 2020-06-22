/*  2020 (c) jms
    a file system loosely based on the log structured file system, assuming a SSD for hard drive.
    utilizing FUSE (file system in userspace, how that acronym works is beyond me) to implement infrastructure for translation of instructions to data instructions
    erase in segments, program and read in blocks. (perase - segment, pwrite (have to be an empty block) - block, pread - block)

    assuming that we are using a system where dirname and basename do not change pathname

*/

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
#include "fs.h"
#include "fs_helper.h"





/*
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
#define MAX_DATA_BLOCKS 2418 // MAX_SEGMENTS * DATA_BLOCKS_IN_SEGMENT
#define DATA_BLOCKS_IN_SEGMENT 31 // SEGMENT_SIZE/BLOCK_SIZE -1
#define MAX_SEGMENTS 78 // DATA_SIZE/SEGMENT_SIZE
#define MAX_INODES 2496 // = MAX_BLOCKS (sort of overshooting)
#define MAX_ITABLE_BLOCKS 64 // ITABLE_MAX_SIZE/BLOCK_SIZE rounded up
#define ITABLE_ENT_IN_BLOCK 39 // BLOCK_SIZE/ITABLE_ENTRY_SIZE

#define DATA_START 131072 // = SEGMENT_SIZE
#define SECOND_CR 10354688 // DRIVE_SIZE - SEGMENT_SIZE

#define MAX_PATH 4096 // 4 kbytes
#define MAX_FILE_NAME 32
*/




////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// GLOBAL VARIABLES ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////



void *segment[DATA_BLOCKS_IN_SEGMENT]; // might not always be filled, depending on the size of the data the pointers point to
seg_summary ss;
int which_seg;

i_table *itable; // table of inodes. Can grow and shrink as needed, in sizes of blocks. Must malloc realloc and free

dir_table d_table[DIR_TABLE_SIZE+1]; // table of dir entries,

checkpoint cp;
superblock sb;


FILE *drive;
int fd_drive;



////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE /
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


void* fs_init(struct fuse_conn_info *conn){

  char drive_path[MAX_PATH];

  // find where "drive" file should be
  memset(drive_path, '\0', MAX_PATH);
  getcwd(drive_path, (MAX_PATH - sizeof("/FS_SSD")));
  strcat(drive_path, "/FS_SSD");

  // see if "drive" file is initialized
  if((drive = fopen(drive_path, "r+"))){
    fd_drive = fileno(drive);

    // if it is, read in checkpoint_regions and find latest checkpoint
    checkpoint_region cr;

    // checking first checkpoint_region
    memset(&cr, 0, SEGMENT_SIZE);
    pread(fd_drive,&cr,SEGMENT_SIZE,0);

    int i = DATA_BLOCKS_IN_SEGMENT - 2;
    for(; cr.cp[i].num_inodes == 0; i--){} // find the last valid cp
    memset(&cp, 0, BLOCK_SIZE);
    memcpy(&cp, &cr.cp[i], BLOCK_SIZE);

    // checking second checkpoint_region
    memset(&cr, 0, SEGMENT_SIZE);
    pread(fd_drive,&cr,SEGMENT_SIZE,SECOND_CR);

    i = DATA_BLOCKS_IN_SEGMENT - 2;
    for(; cr.cp[i].num_inodes == 0; i--){} // find the last valid cp
    if(difftime(cp.timestamp, cr.cp[i].timestamp) < 0){ // if this cp was written later
      memset(&cp, 0, BLOCK_SIZE);
      memcpy(&cp, &cr.cp[i], BLOCK_SIZE);
    }

    // now that we have the checkpoint_region, read in latest itable
    itable = (i_table *) malloc(cp.num_itable_blocks * BLOCK_SIZE);
    for(i = 0; i < cp.num_itable_blocks; i++){
      pread(fd_drive, &itable[i], BLOCK_SIZE, cp.pointers[i]);
    }

    memset(&ss, 0, BLOCK_SIZE);
    pread(fd_drive, &ss, BLOCK_SIZE, cp.segsum_loc);

    get_segment();


  }else{
    // if it isn't, create file and write out initial state
    drive = fopen(drive_path, "w+");
    fseek(drive, DRIVE_SIZE-1, SEEK_SET);
    fputc('\0', drive);
    for(int i = -1; i < 79; i++) perase(1,i);
    // write out initialized
    create_init_drive();

  }

  return NULL;
}







void fs_destroy(void *privatedata)
{
  flush_segment();
  write_seg_summary();
  write_checkpoint();
  flush_segment();
  free_segment();
  fclose(drive);
}







// return file attributes, for path name, should fill in the elements of the stat struct
// if a field is meaningless then it should be set to 0
int fs_getattr(const char *path, struct stat *stbuf)
{
  int call_rslt = 0;
  memset(stbuf, 0, sizeof(struct stat));
  inode id;

  // see if this file exists
  call_rslt = get_inode(path, &id);
  if(call_rslt == -1) return -ENOENT;
  if(call_rslt == -2) return -ENAMETOOLONG;
  if(call_rslt == -3) return -ENOTDIR;

  // fill out stat
  stbuf -> st_mode = id.mode;
  stbuf -> st_nlink = id.links_count;
  stbuf -> st_ino = call_rslt;
  stbuf -> st_uid = id.uid;
  //stbuf -> st_attimespc =
  stbuf -> st_size = id.size;
  stbuf -> st_blocks = id.block_count;

  return 0;
}








int fs_access(const char *path, int mask)
{

  inode id;

  int call_rslt = get_inode(path, &id);
  if (call_rslt == -1) return -ENOENT;
  if (call_rslt == -2) return -ENAMETOOLONG;
  if (call_rslt == -3) return -ENOTDIR;

  if((id.mode & mask) == mask) return 0;
  else return -EACCES;

  return 0;
}







int fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi)
{

  inode id;

  int call_rslt = get_inode(path, &id);
  if (call_rslt == -1) return -ENOENT;
  if (call_rslt == -2) return -ENAMETOOLONG;
  if ((call_rslt == -3) || ((id.mode & S_IFDIR) == 0)) return -ENOTDIR;

  if( (offset/DIR_TABLE_SIZE) > id.block_count) return 0;
  // find the offset block and offset size we should start reading from
  int offset_block = offset / DIR_TABLE_SIZE;
  int offset_num = offset % DIR_TABLE_SIZE;

  for(int i = offset_block; i < id.block_count; i++){
    // read in current directory table if it is not in memory
    memset(&d_table, 0, BLOCK_SIZE);
    if(read_block(&d_table, id.blocks[i]) == -1) return -EIO;

    for(int j = offset_num; j < DIR_TABLE_SIZE; j++){
      if(d_table[j].inode_num != 0){
        if (filler(buf, d_table[j].file_name, NULL, (i*DIR_TABLE_SIZE) + j + 1)) return 0;
      }
    }
    offset_num = 0;
  }
  return 0;
}





int fs_mkdir(const char* path, mode_t mode)
{
  inode id;
  inode parent_id;
  int inode_number = 0;
  int parent_inode_num;

  // check if file exists
  int call_rslt = get_inode(path, &id);
  if (call_rslt == 1) return -ENOENT; // can't make root
  if (call_rslt == -2) return -ENAMETOOLONG; // name too long!
  if (call_rslt == -3) return -ENOTDIR; // a component of the path is not a directory
  if (call_rslt > 0) return -EEXIST; // can't kill whats already dead, can't create what already exists

  // get file name and whatnot
  char *dup = strdup(path);
  char *dup1 = basename(dup);
  char file_name[MAX_FILE_NAME];
  char path_parent[MAX_PATH];
  memset(file_name, 0, MAX_FILE_NAME);
  memset(path_parent, 0, MAX_PATH);
  strncpy(file_name, dup1, strlen(dup1));
  free(dup);
  dup = strdup(path);
  dup1 = dirname(dup);
  strncpy(path_parent, dup1, strlen(dup1));
  free(dup);


  // see if parent directory exists
  memset(&id, 0, INODE_SIZE);
  memset(&parent_id, 0, INODE_SIZE);
  parent_inode_num = get_inode(path_parent, &parent_id);
  if(parent_inode_num == -1) return -ENOENT;

  // see if there is a free inode number for this new directory
  for(int i = 0; i < MAX_INODES; i++){
    if(cp.inode_numbers[i] == 0){
      inode_number = i;
      cp.inode_numbers[i] = 1;
      cp.num_inodes++;
      break;
    }
  }

  // see if we can write out a block for directory data
  dir_table *dir_data = (dir_table *) malloc(BLOCK_SIZE);
  memset(dir_data, 0, BLOCK_SIZE);
  dir_data[0].inode_num = inode_number;
  strcpy(dir_data[0].file_name, ".");
  dir_data[1].inode_num = parent_inode_num;
  strcpy(dir_data[1].file_name, "..");
  id.blocks[0] = add_to_segment(dir_data, 0);
  if(id.blocks[0] == -1) return -ENOSPC;

  // create inode
  id.mode = mode | S_IFDIR;
  id.uid = 0;
  id.size = DT_ENTRY_SIZE * 2;
  id.time = time(NULL);
  id.ctime = time(NULL);
  id.mtime = time(NULL);
  id.gid = 0;
  id.links_count = 1;
  id.block_count = 1;

  // add to parent directory data
  call_rslt = add_dir_entry(file_name, inode_number, parent_inode_num, &parent_id);
  if(call_rslt == -1) return -ENOSPC;
  else if(call_rslt == -2) return -ENAMETOOLONG;

  // add inode and inode number pair to itable
  if(change_inode(inode_number, &id, 0) == -1) return -ENOSPC;

  //flush_segment();
  return 0;
}



int fs_rmdir(const char *path)
{
  inode id;
  inode parent_id;

  // check if directory exists
  int inode_num = get_inode(path, &id);
  if (inode_num == -1) return -ENOENT;
  if (inode_num == -2) return -ENAMETOOLONG;
  if ((inode_num == -3) || ((id.mode & S_IFDIR) == 0)) return -ENOTDIR;

  // get file name and whatnot
  char *dup = strdup(path);
  char *dup1 = basename(dup);
  char path_parent[MAX_PATH];
  dup = strdup(path);
  dup1 = dirname(dup);
  strncpy(path_parent, dup1, strlen(dup1));
  free(dup);

  // check if empty

  // get inode and inode number
  int parent_inode_num = get_inode(path_parent, &parent_id);

  // remove inode from itable
  if(change_inode(inode_num, &id, -1) == -1) return -ENOSPC;

  // remove dir entry from parent directory data
  if(remove_dir_entry(inode_num, parent_inode_num, &parent_id) == -1) return -ENOSPC;

  // update checkpoint
  cp.num_inodes--;
  cp.inode_numbers[inode_num] = 0;


  return 0;
}








int fs_chmod(const char* path, mode_t mode)
{
  inode id;
  int inode_num = 0;

  // check if file exists
  inode_num = get_inode(path, &id);
  if (inode_num == 1) return -ENOENT; // can't change root
  if (inode_num == -1) return -ENOENT; // ain't not no nothing of nonfile
  if (inode_num == -2) return -ENAMETOOLONG; // name too long!
  if (inode_num == -3) return -ENOTDIR; // a component of the path is not a directory

  if((id.mode & mode) == mode) return 0;

  // find inode in itable and change it
  int changed = 0;
  for(int i = 0; (i < cp.num_itable_blocks) && (changed!=1); i++){
    for(int j = 0; (j < ITABLE_ENT_IN_BLOCK) && (changed!=1); j++){
      // change parent inode
      if(itable[i].num_to_inodes[j].inode_num == inode_num){
        itable[i].num_to_inodes[j].inode.mode = mode;
        // write out changes
        i_table *block = (i_table *) malloc(BLOCK_SIZE);
        memcpy(block, &itable[i], BLOCK_SIZE);
        cp.pointers[i] = add_to_segment(block, cp.pointers[i]);
        if(cp.pointers[i] == -1) return -ENOSPC;
        changed = 1;
      }
    }
  }
  return 0;
}








int fs_fgetattr(const char* path, struct stat* stbuf, struct fuse_file_info *fi)
{
  return fs_getattr(path, stbuf);
}








int fs_mknod(const char* path, mode_t mode, dev_t rdev)
{
  inode id;
  inode parent_id;
  int inode_number = 0;
  int parent_inode_num;

  // check if file exists
  int call_rslt = get_inode(path, &id);
  if (call_rslt == 1) return -ENOENT; // can't make root
  if (call_rslt == -2) return -ENAMETOOLONG; // name too long!
  if (call_rslt == -3) return -ENOTDIR; // a component of the path is not a directory
  if (call_rslt > 0) return -EEXIST; // can't kill whats already dead, can't create what already exists

  // get file name and whatnot
  char *dup = strdup(path);
  char *dup1 = basename(dup);
  char file_name[MAX_FILE_NAME];
  char path_parent[MAX_PATH];
  memset(file_name, 0, MAX_FILE_NAME);
  memset(path_parent, 0, MAX_PATH);
  strncpy(file_name, dup1, strlen(dup1));
  free(dup);
  dup = strdup(path);
  dup1 = dirname(dup);
  strncpy(path_parent, dup1, strlen(dup1));
  free(dup);


  // see if parent directory exists
  memset(&id, 0, INODE_SIZE);
  memset(&parent_id, 0, INODE_SIZE);
  parent_inode_num = get_inode(path_parent, &parent_id);
  if(parent_inode_num == -1) return -ENOENT;



  // see if there is a free inode number for this new directory
  for(int i = 0; i < MAX_INODES; i++){
    if(cp.inode_numbers[i] == 0){
      inode_number = i;
      cp.inode_numbers[i] = 1;
      cp.num_inodes++;
      break;
    }
  }

  // create inode
  id.mode = mode | S_IFREG | S_IWGRP | S_IWOTH;
  id.uid = 0;
  id.size = 0;
  id.time = time(NULL);
  id.ctime = time(NULL);
  id.mtime = time(NULL);
  id.gid = 0;
  id.links_count = 1;
  id.block_count = 0;

  // add to parent directory data
  call_rslt = add_dir_entry(file_name, inode_number, parent_inode_num, &parent_id);
  if(call_rslt == -1) return -ENOSPC;
  else if(call_rslt == -2) return -ENAMETOOLONG;

  // add inode and inode number pair to itable
  if(change_inode(inode_number, &id, 0) == -1) return -ENOSPC;

  //flush_segment();
  return 0;
}







int fs_unlink(const char* path)
{
  inode id;
  inode parent_id;

  // check if directory exists
  int inode_num = get_inode(path, &id);
  if (inode_num == -1) return -ENOENT;
  if (inode_num == -2) return -ENAMETOOLONG;
  if (inode_num == -3) return -ENOTDIR;
  if ((id.mode & S_IFREG) == 0) return -EISDIR;

  // get file name and whatnot
  char *dup = strdup(path);
  char *dup1 = basename(dup);
  char path_parent[MAX_PATH];
  dup = strdup(path);
  dup1 = dirname(dup);
  strncpy(path_parent, dup1, strlen(dup1));
  free(dup);

  // get inode and inode number
  int parent_inode_num = get_inode(path_parent, &parent_id);

  // remove inode from itable
  if(change_inode(inode_num, &id, -1) == -1) return -ENOSPC;

  // remove dir entry from parent directory data
  if(remove_dir_entry(inode_num, parent_inode_num, &parent_id) == -1) return -ENOSPC;

  // update checkpoint
  cp.num_inodes--;
  cp.inode_numbers[inode_num] = 0;


  return 0;
}







int fs_truncate(const char* path, off_t size)
{
  inode id;
  // see if inode exists
  int inode_num = get_inode(path, &id);
  if(inode_num == -1) return -ENOENT;
  if(inode_num == -2) return -ENAMETOOLONG;
  if(inode_num == -3) return -ENOTDIR;
  if((id.mode & S_IFREG) == 0) return -EISDIR;

  // check how many blocks the file needs
  int last_block = size / BLOCK_SIZE;
  int last_byte = size % BLOCK_SIZE;
  if(last_byte != 0) last_block++;

  // check if the file needs to be truncated or extended
  if(id.size == size) return 0;
  else if(id.size > size){ // truncate

    // get rid of any blocks that beyond the new size

    for(int i = last_block; i < id.block_count; i++){
      id.blocks[i] = 0;
    }

    // zero out any bytes that occur after size, if there is a need
    if(last_byte != 0){
      char *block = (char *) malloc(BLOCK_SIZE);
      pread(fd_drive, block, BLOCK_SIZE, id.blocks[last_block-1]);
      memset((block + last_byte), 0, (BLOCK_SIZE - last_byte));
      id.blocks[last_block-1] = add_to_segment(block, id.blocks[last_block-1]);
      if(id.blocks[last_block-1] == -1) return -ENOSPC;
    }


  }
  else if(last_block > id.block_count){ // extend only if more blocks need to be added. If just more bytes in the same block, nothing to do

    // allocate new blocks as needed
    for(int i = id.block_count; i < last_block; i++){
      if(i > 14) return -ENOSPC; // NEED TO CHANGE, BECAUSE OF INDIRECT BLOCKS

      void *block = (void *) malloc(BLOCK_SIZE);
      memset(block, 0, BLOCK_SIZE);
      id.blocks[i] = add_to_segment(block, id.blocks[i]); // id.blocks will be 0 anyways if its empty
      if(id.blocks[i] == -1) return -ENOSPC;
    }

  }

  id.block_count = last_block;
  id.size = size;
  if(change_inode(inode_num, &id, 1) == -1) return -ENOSPC;
  return 0;

}







int fs_open(const char* path, struct fuse_file_info* fi)
{
  inode id;
  // see if inode exists
  int inode_num = get_inode(path, &id);
  if(inode_num == -1) return -ENOENT;
  if(inode_num == -2) return -ENAMETOOLONG;
  if(inode_num == -3) return -ENOTDIR;
  if((id.mode & S_IFREG) == 0) return -EISDIR;

  return 0;
}







int fs_read(const char* path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi)
{
  inode id;
  // see if inode exists
  int inode_num = get_inode(path, &id);
  if(inode_num == -1) return -ENOENT;
  if(inode_num == -2) return -ENAMETOOLONG;
  if(inode_num == -3) return -ENOTDIR;
  if((id.mode & S_IFREG) == 0) return -EISDIR;

  // see which block we need to start reading from
  int first_block = offset/BLOCK_SIZE;
  int first_byte = offset%BLOCK_SIZE;
  if(first_byte != 0) first_block++;

  char temp_buf[BLOCK_SIZE];

  // check how to read, based on size and offset
  if(offset >= id.size) return 0;
  else if((offset+size) >= id.size){ // if offset is within file, but size is beyond it
    for(int i = first_block; i < id.block_count; i++){ // read all the contents after offset
      memset(temp_buf, 0, BLOCK_SIZE);
      if(read_block(temp_buf, id.blocks[i]) == -1) return -EIO;
      if(i == id.block_count-1) memcpy(buf +(BLOCK_SIZE*i), temp_buf+first_byte, (id.size%BLOCK_SIZE)-first_byte);
      else memcpy(buf + (BLOCK_SIZE*i), temp_buf+first_byte, BLOCK_SIZE-first_byte);
      first_byte = 0;
    }

    return id.size-offset;
  }
  else{ // if offset and offset+size are both within file
    int last_block = (offset+size)/BLOCK_SIZE;
    if(((offset+size) % BLOCK_SIZE) != 0) last_block++;

    for(int i = first_block; i < last_block; i++){ // read all the contents between offset and size
      memset(temp_buf, 0, BLOCK_SIZE);
      if(read_block(temp_buf, id.blocks[i]) == -1) return -EIO;
      if(i == last_block-1) memcpy(buf + (BLOCK_SIZE*i), temp_buf+first_byte, ((offset+size)%BLOCK_SIZE)-first_byte);
      else memcpy(buf + (BLOCK_SIZE*i), temp_buf+first_byte, BLOCK_SIZE-first_byte);
      first_byte = 0;
    }

    return size;
  }


  return 0;
}







int fs_write(const char* path, const char *buf, size_t size, off_t offset, struct fuse_file_info* fi)
{
  inode id;
  // see if inode exists
  int inode_num = get_inode(path, &id);
  if(inode_num == -1) return -ENOENT;
  if(inode_num == -2) return -ENAMETOOLONG;
  if(inode_num == -3) return -ENOTDIR;
  if((id.mode & S_IFREG) == 0) return -EISDIR;

  // see which block we need to start writing from
  int first_block = offset/BLOCK_SIZE;
  int first_byte = offset%BLOCK_SIZE;
  if(first_byte != 0) first_block++;

  // check how to read, based on size and offset
  if(offset >= id.size){
    if(fs_truncate(path, offset+size) == -ENOSPC) return -ENOSPC;
    memset(&id, 0, INODE_SIZE);
    inode_num = get_inode(path, &id);
  }
  if((offset+size) >= id.size){ // if offset is within file, but size is beyond it
    for(int i = first_block; i < id.block_count; i++){ // read all the contents after offset
      char *temp_buf = (char *) malloc(BLOCK_SIZE);
      memset(temp_buf, 0, BLOCK_SIZE);

      // copy from buf to temp buf, the appropriate amount and at the proper offset
      if(i == id.block_count-1) memcpy(temp_buf+first_byte, buf +(BLOCK_SIZE*i), (id.size%BLOCK_SIZE)-first_byte);
      else memcpy(temp_buf+first_byte, buf + (BLOCK_SIZE*i), BLOCK_SIZE-first_byte);

      // add to segment
      id.blocks[i] = add_to_segment(temp_buf, id.blocks[i]);
      if(id.blocks[i] == -1) return -ENOSPC;
      first_byte = 0;
    }

    return id.size-offset;
  }
  else{ // if offset and offset+size are both within file
    int last_block = (offset+size)/BLOCK_SIZE;
    if(((offset+size) % BLOCK_SIZE) != 0) last_block++;

    for(int i = first_block; i < last_block; i++){ // read all the contents between offset and size
      char *temp_buf = (char *) malloc(BLOCK_SIZE);
      memset(temp_buf, 0, BLOCK_SIZE);

      // copy from buf to temp buf, the appropriate amount and at teh proper offset
      if(i == last_block-1) memcpy(temp_buf+first_byte, buf + (BLOCK_SIZE*i), ((offset+size)%BLOCK_SIZE)-first_byte);
      else memcpy(temp_buf+first_byte, buf + (BLOCK_SIZE*i), BLOCK_SIZE-first_byte);

      // add to segment
      id.blocks[i] = add_to_segment(temp_buf, id.blocks[i]);
      if(id.blocks[i] == -1) return -ENOSPC;
      first_byte = 0;
    }

    return size;
  }

  return 0;
}








/*
int fs_statfs(const char* path, struct statvfs* stbuf)
{
  stbuf->f_namemax = MAX_FILE_NAME;
  stbuf->f_frsize = BLOCK_SIZE;
  stbuf->f_bsize = BLOCK_SIZE;
  stbuf->f_files = MAX_INODES - cp.num_inodes;
  stbuf->f_favail = MAX_INODES - cp.num_inodes;
  stbuf->f_ffree = MAX_INODES - cp.num_inodes;
  int freeblocks = 0;
  for(int i = 0; i < MAX_SEGMENTS; i++){
    if(cp.segments_usage[i] == 0) freeblocks = freeblocks + DATA_BLOCKS_IN_SEGMENT;
  }
  for(int i = 0; i < DATA_BLOCKS_IN_SEGMENT; i++){
    if(ss.liveness[i] == 0) freeblocks++;
  }
  stbuf->f_blocks = freeblocks;
  stbuf->f_bavail = freeblocks;
  stbuf->f_bfree = freeblocks;
  return 0;
}*/









int main(int argc, char **argv)
{
    return fuse_main(argc, argv, &fs_filesystem_operations, NULL);
}

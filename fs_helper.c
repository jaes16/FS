/*  2020 (c) jms
    helper methods for file system saved in fs.c

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
// CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE CODE /
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


// Just to make sure we're erasing in segment sizes. Just writing out 0's to segments
// making sure we're ignoring the first segment, because its off limits
void perase(int size_in_segments, int seg_num)
{
  char buf[size_in_segments*SEGMENT_SIZE];
  memset(buf, 0, SEGMENT_SIZE * size_in_segments);
  pwrite(fd_drive, &buf, size_in_segments * SEGMENT_SIZE, (seg_num+1) * SEGMENT_SIZE);
}




void write_checkpoint()
{
  checkpoint check;

  // go through checkpoint region and find an empty spot to write this checkpoint to
  for(int i = 0; i < DATA_BLOCKS_IN_SEGMENT; i++){
    memset(&check, 0, BLOCK_SIZE);
    pread(fd_drive, &check, BLOCK_SIZE, BLOCK_SIZE*(i+1));
    if(check.num_itable_blocks == 0){
      cp.timestamp = time(NULL);
      pwrite(fd_drive, &cp, BLOCK_SIZE, BLOCK_SIZE*(i+1));
      return;
    }
    memset(&check, 0, BLOCK_SIZE);
    pread(fd_drive, &check, BLOCK_SIZE, (BLOCK_SIZE*(i+1))+SECOND_CR);
    if(check.num_itable_blocks == 0){
      cp.timestamp = time(NULL);
      pwrite(fd_drive, &cp, BLOCK_SIZE, (BLOCK_SIZE*(i+1))+SECOND_CR);
      return;
    }
  }

  // if we never returned in the for loop, means that there are no empty spots, so empty out the two segments
  perase(1,-1);
  pwrite(fd_drive, &sb, BLOCK_SIZE, 0);
  pwrite(fd_drive, &cp, BLOCK_SIZE, BLOCK_SIZE);
  perase(1,MAX_SEGMENTS+1);
  pwrite(fd_drive, &sb, BLOCK_SIZE, SECOND_CR);
  return;
}



// returns -1 for enospc
int write_seg_summary()
{
  if(is_unflushed_memory(cp.segsum_loc) == 1){
    memset(segment[((cp.segsum_loc%SEGMENT_SIZE)/BLOCK_SIZE)], 0, BLOCK_SIZE);
    memcpy(segment[((cp.segsum_loc%SEGMENT_SIZE)/BLOCK_SIZE)], &ss, BLOCK_SIZE);
    return 0;
  }
  // check if a block is available in segment
  for(int i = 0; i < DATA_BLOCKS_IN_SEGMENT; i++){
    if(ss.liveness[which_seg][i] == 0){
      ss.liveness[which_seg][i] = 1;
      if(cp.segsum_loc !=  0) ss.liveness[(cp.segsum_loc/SEGMENT_SIZE)-1][(cp.segsum_loc%SEGMENT_SIZE)/BLOCK_SIZE] = 0; // might have to change this later (0 -> -1)
      
      seg_summary *block = (seg_summary *) malloc(BLOCK_SIZE);
      memset(block, 0, BLOCK_SIZE);
      memcpy(block, &ss, BLOCK_SIZE);
      segment[i] = block;

      cp.segsum_loc = (((which_seg + 1) * SEGMENT_SIZE) + (i * BLOCK_SIZE));
      return 0;
    }
  }

  // if we reach this part, never found an empty block in segment, so we have to write this segment out and get a new segment
  flush_segment();
  free_segment();
  if(get_segment() == -1) return -1;
  ss.liveness[which_seg][0] = 1;
  if(cp.segsum_loc != 0) ss.liveness[(cp.segsum_loc/SEGMENT_SIZE)-1][(cp.segsum_loc%SEGMENT_SIZE)/BLOCK_SIZE] = 0; // might have to change this later (0 -> -1)

  seg_summary *block = (seg_summary *) malloc(BLOCK_SIZE);
  memset(block, 0, BLOCK_SIZE);
  memcpy(block, &ss, BLOCK_SIZE);
  segment[0] = block;

  cp.segsum_loc = ((which_seg + 1) * SEGMENT_SIZE);
  return 0;
}





void flush_segment()
{

  for(int i = 0; i < DATA_BLOCKS_IN_SEGMENT; i++){
    if(ss.liveness[which_seg][i] == 1){ // if live and unflushed
      pwrite(fd_drive, segment[i], BLOCK_SIZE, ((which_seg+1) * SEGMENT_SIZE) + (i * BLOCK_SIZE));
      ss.liveness[which_seg][i] = 2;
    }
  }

  return;
}




int get_segment()
{
  which_seg = -1;

  for(int i = 0; i < MAX_SEGMENTS; i++){
    if(cp.segments_usage[i] == 0){
      which_seg = i;
      cp.segments_usage[i] = 1;
      perase(1, which_seg);
      break;
    }
  }
  if(which_seg == -1) return -1;
  else return 0;
}



void free_segment()
{
  for(int i = 0; i < DATA_BLOCKS_IN_SEGMENT; i++){
    free(segment[i]);
  }
  memset(segment, 0, sizeof(void *) * DATA_BLOCKS_IN_SEGMENT);
}


// under the assumption that there is some sort of data at address
// returns 1 if it is unflushed memory in segment. otherwise returns 0
int is_unflushed_memory(int address)
{
  // see if this address is within the segment, and whether the particular block hasn't been written out to drive
  if((address >= ((which_seg+1)*SEGMENT_SIZE)) && (address < ((which_seg+2)*SEGMENT_SIZE))) {
    if(ss.liveness[which_seg][(address%SEGMENT_SIZE)/BLOCK_SIZE] == 1) {
      return 1;
    }
    return 0;
  }
  else return 0;
  return 0;
}




// receives a malloced pointer to a block size load of data,
// return -1 if unable to allocate more space, otherwise returns the absolute address of the where the block was added
int add_to_segment(void *block, int address)
{
  if((address != 0) && (is_unflushed_memory(address) == 1)){
    memset(segment[((address%SEGMENT_SIZE)/BLOCK_SIZE)], 0, BLOCK_SIZE);
    memcpy(segment[((address%SEGMENT_SIZE)/BLOCK_SIZE)], block, BLOCK_SIZE);
    free(block);
    return address;
  }
  // check if a block is available in segment
  for(int i = 0; i < DATA_BLOCKS_IN_SEGMENT; i++){
    if(ss.liveness[which_seg][i] == 0){
      segment[i] = block;
      ss.liveness[which_seg][i] = 1;
      if(address !=  0) ss.liveness[(address/SEGMENT_SIZE)-1][(address%SEGMENT_SIZE)/BLOCK_SIZE] = 0; // might have to change this later (0 -> -1)
      write_seg_summary();
      return(((which_seg + 1) * SEGMENT_SIZE) + (i * BLOCK_SIZE));
    }
  }

  // if we reach this part, never found an empty block in segment, so we have to write this segment out and get a new segment
  flush_segment();
  free_segment();
  if(get_segment() == -1) return -1;

  ss.liveness[which_seg][0] = 1;
  segment[0] = block;

  if(address != 0) ss.liveness[(address/SEGMENT_SIZE)-1][(address%SEGMENT_SIZE)/BLOCK_SIZE] = 0; // might have to change this later (0 -> -1)
  write_seg_summary();
  return((which_seg + 1) * SEGMENT_SIZE);
}




int read_block(void *buf, int address){
  if((address >= ((which_seg+1)*SEGMENT_SIZE)) && (address < ((which_seg+2)*SEGMENT_SIZE))){
    // read in appropriate block in memory
    memcpy(buf, segment[(address%SEGMENT_SIZE)/BLOCK_SIZE], BLOCK_SIZE);
  }
  else{
    if(pread(fd_drive, buf, BLOCK_SIZE, address) == -1) return -1;
  }

  return 0;
}




// When theres no drive, create generic drive
void create_init_drive()
{
  fd_drive = fileno(drive);
  inode rootnode;

  // prepare first segment with segment summary, rootdir, itable
  memset(&ss, 0, BLOCK_SIZE);
  memset(&rootnode, 0, INODE_SIZE);
  memset(d_table, 0, sizeof(d_table));

  seg_summary *block = malloc(BLOCK_SIZE);
  ss.liveness[0][0] = 1;
  memcpy(block, &ss, BLOCK_SIZE);
  segment[0] = block;

  // rootdir data preparation. set up . and .. in root
  d_table[0].inode_num = 1; // using first inode number, 1
  d_table[1].inode_num = 1;
  d_table[0].file_name[0] = '.';
  d_table[1].file_name[0] = '.';
  d_table[1].file_name[1] = '.';

  // prepare for inode in inode table
  rootnode.mode = S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO;
  rootnode.uid = 0;
  rootnode.size = sizeof(dir_table) * 2;
  time_t t = time(NULL);
  struct tm tm = *localtime(&t);
  rootnode.time = tm.tm_sec;
  rootnode.ctime = tm.tm_sec;
  rootnode.mtime = tm.tm_sec;
  rootnode.dtime = 0;
  rootnode.links_count = 1;
  rootnode.block_count = 1;
  rootnode.blocks[0] = SEGMENT_SIZE+BLOCK_SIZE;

  // initialize inode table and place the first inode
  itable = (i_table *) malloc(BLOCK_SIZE);
  memset(itable, 0, BLOCK_SIZE);
  itable[0].num_to_inodes[0].inode_num = 1;
  memcpy(&itable[0].num_to_inodes[0].inode, &rootnode, INODE_SIZE);

  // prepare superblock
  sb.checkpoint_location = BLOCK_SIZE;
  sb.itable_size = BLOCK_SIZE;
  sb.data_location = SEGMENT_SIZE;
  sb.data_size = DATA_SIZE;

  // prepare checkpoint
  cp.timestamp = time(NULL);
  cp.num_itable_blocks = 1;
  cp.num_inodes = 1;
  cp.inode_numbers[0] = 32;
  cp.inode_numbers[1] = 1;
  cp.pointers[0] = SEGMENT_SIZE+(BLOCK_SIZE*2); // the first segment is for cr, then one block for segsum, one for rootdir data
  cp.segments_usage[0] = 1;
  cp.segsum_loc = SEGMENT_SIZE;

  // finally write out in block sizes
  // write out sb and checkpoints, to both checkpoint regions
  pwrite(fd_drive, &sb, BLOCK_SIZE, 0);
  pwrite(fd_drive, &sb, BLOCK_SIZE, SECOND_CR);
  pwrite(fd_drive, &cp, BLOCK_SIZE, BLOCK_SIZE);
  pwrite(fd_drive, &cp, BLOCK_SIZE, SECOND_CR + BLOCK_SIZE);
  // write out rootdir and itable

  // make sure its in memory
  dir_table *block1 = (dir_table *) malloc(BLOCK_SIZE);
  memset(block1, 0, sizeof(dir_table) * (DIR_TABLE_SIZE+1));
  memcpy(block1, d_table, BLOCK_SIZE);
  add_to_segment(block1,0);

  i_table *block2 = (i_table *) malloc(BLOCK_SIZE);
  memset(block2, 0, BLOCK_SIZE);
  memcpy(block2, itable, BLOCK_SIZE);
  add_to_segment(block2,0);

}



// returns 0 if the inode was found, -1 if not
int find_inode(int inode_num, inode *id)
{
  // go through itable to find the inode
  for(int i = 0; i < cp.num_itable_blocks; i++){
    for(int j = 0; j < ITABLE_ENT_IN_BLOCK; j++){
      if(itable[i].num_to_inodes[j].inode_num == inode_num){
        memset(id, 0, INODE_SIZE);
        memcpy(id, &itable[i].num_to_inodes[j].inode, INODE_SIZE);
        return 0;
      }
    }
  }
  // couldn't find
  return -1;
}



/*
  changes the inode specified by inode_num.
  if update is 0, then we are adding a new inode to the table; if update is 1, then we are chaning an inode, if update -1, then we are removing the inode
  returns -1 for ENOSPC, 0 for success
*/
int change_inode(int inode_num, inode *id, int update)
{
  for(int i = 0; (i < cp.num_itable_blocks); i++){
    for(int j = 0; (j < ITABLE_ENT_IN_BLOCK); j++){
      if(update == 1){
        // change appropriate inode
        if(itable[i].num_to_inodes[j].inode_num == inode_num){
          memset(&itable[i].num_to_inodes[j].inode, 0, INODE_SIZE);
          memcpy(&itable[i].num_to_inodes[j].inode, id, INODE_SIZE);

          // write out changes
          i_table *block = (i_table *) malloc(BLOCK_SIZE);
          memcpy(block, &itable[i], BLOCK_SIZE);
          // make appropriate changes to checkpoint
          cp.pointers[i] = add_to_segment(block, cp.pointers[i]);
          if(cp.pointers[i] == -1) return -1;

          return 0;
        }
      }
      else if(update == 0){
        // add inode
        if(itable[i].num_to_inodes[j].inode_num == 0){
          memset(&itable[i].num_to_inodes[j].inode, 0, INODE_SIZE);
          memcpy(&itable[i].num_to_inodes[j].inode, id, INODE_SIZE);
          itable[i].num_to_inodes[j].inode_num = inode_num;

          // write out changes
          i_table *block = (i_table *) malloc(BLOCK_SIZE);
          memcpy(block, &itable[i], BLOCK_SIZE);
          // make appropriate changes to checkpoint
          cp.pointers[i] = add_to_segment(block, cp.pointers[i]);
          if(cp.pointers[i] == -1) return -1;

          return 0;
        }
      }
      else{ // remove inode
        // find appropriate inode
        if(itable[i].num_to_inodes[j].inode_num == inode_num){
          memset(&itable[i].num_to_inodes[j].inode, 0, INODE_SIZE);
          itable[i].num_to_inodes[j].inode_num = 0;

          // write out changes
          i_table *block = (i_table *) malloc(BLOCK_SIZE);
          memcpy(block, &itable[i], BLOCK_SIZE);
          // make appropriate changes to checkpoint
          cp.pointers[i] = add_to_segment(block, cp.pointers[i]);
          if(cp.pointers[i] == -1) return -1;

          return 0;
        }
      }
    }
  }

  // if we made it here, there's no space in itable
  // add a block to itable
  i_table temp[cp.num_itable_blocks];
  memset(temp, 0, cp.num_itable_blocks * BLOCK_SIZE);
  memcpy(temp, itable, cp.num_itable_blocks * BLOCK_SIZE);
  itable = (i_table *) realloc(itable, (cp.num_itable_blocks+1) * BLOCK_SIZE);
  memset(itable, 0, (cp.num_itable_blocks+1) * BLOCK_SIZE);
  memcpy(itable, temp, cp.num_itable_blocks * BLOCK_SIZE);

  // put in inode
  itable[cp.num_itable_blocks].num_to_inodes[0].inode_num = inode_num;
  memset(&itable[cp.num_itable_blocks].num_to_inodes[0].inode, 0, INODE_SIZE);
  memcpy(&itable[cp.num_itable_blocks].num_to_inodes[0].inode, id, INODE_SIZE);

  // write out to segment and update checkpoint
  i_table *newblock = (i_table *) malloc(BLOCK_SIZE);
  memcpy(newblock, &itable[cp.num_itable_blocks], BLOCK_SIZE);
  cp.num_itable_blocks++;
  cp.pointers[cp.num_itable_blocks-1] = add_to_segment(newblock, cp.pointers[cp.num_itable_blocks-1]);
  if(cp.pointers[cp.num_itable_blocks-1] == -1) return -ENOSPC;
  return 0;
}



// returns -1 if the inode number couldn't be found, -2 if file_name is too long, and inode number otherwise
int find_inode_num(char *file_name, inode parent_id)
{
  if(strlen(file_name) > 32) return -2;

  dir_table dtable[DIR_TABLE_SIZE+1];
  // look through every directory data block of this directory
  for(int i = 0; i < parent_id.block_count; i++){

    memset(dtable, 0, BLOCK_SIZE);
    // read in current directory table if it is not in memory
    if(read_block(&dtable, parent_id.blocks[i]) == -1) return -EIO;

    // look in current directory for file name
    for(int j = 0; j < DIR_TABLE_SIZE; j++){
      if(strcmp(dtable[j].file_name,file_name) == 0) return dtable[j].inode_num;
    }
  }

  // couldn't find
  return -1;
}



/*
    returns -1 for enospc, -2 for nametoolong
*/
int add_dir_entry(char *file_name, int inode_num, int parent_inode_num, inode *parent_id)
{
  if(strlen(file_name) > 32) return -2;

  dir_table dtable[DIR_TABLE_SIZE+1];

  for(int i = 0; i < parent_id->block_count; i++){

    // read in current directory table if it is not in memory
    memset(&dtable, 0, BLOCK_SIZE);
    if(read_block(&dtable, parent_id->blocks[i]) == -1) return -EIO;

    // check in parent data block if there are free spaces
    for(int j = 0; j < DIR_TABLE_SIZE; j++){
      // if we find a free space
      if(dtable[j].inode_num == 0){

        // make changes in the table
        dtable[j].inode_num = inode_num;
        strncpy(dtable[j].file_name, file_name, strlen(file_name));
        parent_id->size = parent_id->size + DT_ENTRY_SIZE;

        // make sure the changes in the table are gonna be made in drive
        dir_table *block = (dir_table *) malloc(BLOCK_SIZE);
        memcpy(block, &dtable, BLOCK_SIZE);
        parent_id->blocks[i] = add_to_segment(block, parent_id->blocks[i]);
        if(parent_id->blocks[i] == -1) return -1;


        return change_inode(parent_inode_num, parent_id, 1);
      }
    }
  }

  // if no more space in parent directory data, add new block
  // make a new block to write out
  memset(dtable, 0, BLOCK_SIZE);

  // add this inode and file name pair
  dtable[0].inode_num = inode_num;
  strncpy(dtable[0].file_name, file_name, strlen(file_name));

  // write out
  dir_table *block = (dir_table *) malloc(BLOCK_SIZE);
  memcpy(block, &dtable, BLOCK_SIZE);
  parent_id->blocks[parent_id->block_count] = add_to_segment(block, parent_id->blocks[parent_id->block_count]);
  if(parent_id->blocks[parent_id->block_count] == -1) return -1;
  parent_id->block_count++;
  parent_id->size = parent_id->size + DT_ENTRY_SIZE;

  return change_inode(parent_inode_num, parent_id, 1);

}




int remove_dir_entry(int inode_num, int parent_inode_num, inode *parent_id)
{
  dir_table dtable[DIR_TABLE_SIZE+1];

  for(int i = 0; i < parent_id->block_count; i++){
    // read in data block
    memset(&dtable, 0, BLOCK_SIZE);
    // read in current directory table if it is not in memory
    if(read_block(&dtable, parent_id->blocks[i]) == -1) return -EIO;

    // check in parent data block for info
    for(int j = 0; j < DIR_TABLE_SIZE; j++){
      // if we find the target inode number
      if(dtable[j].inode_num == inode_num){

        // make changes in the table
        dtable[j].inode_num = 0;
        memset(dtable[j].file_name, 0, MAX_FILE_NAME);
        parent_id->size = parent_id->size - DT_ENTRY_SIZE;

        // make sure the changes in the table are gonna be made in drive
        dir_table *block = (dir_table *) malloc(BLOCK_SIZE);
        memcpy(block, &dtable, BLOCK_SIZE);
        parent_id->blocks[i] = add_to_segment(block, parent_id->blocks[i]);
        if(parent_id->blocks[i] == -1) return -1;


        return change_inode(parent_inode_num, parent_id, 1);
      }
    }
  }

  return 0;
}





// saves the appropriate inode info in the inode pointer
// return inode_num if path exists, -1 if path doesn't exist, -2 if path name is too long, -3 if component of path is file
// later, can remove last and just find it from path in this function
int get_inode(const char *path, inode *id)
{
  if(strcmp(path,"/") == 0){
    memset(id, 0, INODE_SIZE);
    memcpy(id, &itable[0].num_to_inodes[0].inode, INODE_SIZE);
    return itable[0].num_to_inodes[0].inode_num;
  }

  if(strlen(path) > PATH_MAX) return -2;

  int inode_num = 1; // have to think about whether path should always be an absolute path
  memset(id, 0, INODE_SIZE);

  // get current component of the path
  char *current;
  inode parent_id;
  char dup[PATH_MAX];
  memset(dup, 0, PATH_MAX);

  if(path[0] == '/') strncpy(dup, path+1, PATH_MAX-1);
  else strncpy(dup, path, PATH_MAX);

  // find inode of rootdir
  memcpy(&parent_id, &itable[0].num_to_inodes[0].inode, INODE_SIZE);

  // until we've found the inode for the file or can't find the file
  for(current = strtok(dup, "/"); current != NULL; current = strtok(NULL, "/")){
    // check how long the file name is
    if(strlen(current) > MAX_FILE_NAME) return -2;

    // check that it is a directory
    if((parent_id.mode & S_IFDIR) == 0) return -3;

    // try to find inode number of this component
    inode_num = find_inode_num(current, parent_id);
    if(inode_num == -1) return -1;
    else if(inode_num == -2) return -2;

    // find inode in itable and copy into parent_id
    if(find_inode(inode_num, &parent_id) == -1) return -1;

    // check if this was the last component
    if(strchr(strstr(path,current), '/') == NULL){
      memcpy(id, &parent_id, INODE_SIZE);
      return inode_num;
    }

    // if it wasn't, continue through
  }

  // didn't find
  return -1;
}






/*
int garbage_collection()
{
  // to keep track of segment we are writing out
  // go through
  for(int i = 0; i < MAX_SEGMENTS; i++){
    // check if valid segment
    if((cp.segments_usage[i] != 0) && (i != which_seg)){

      // read segment summary
      memset(&ss, 0, BLOCK_SIZE);
      pread(fd_drive, &ss, BLOCK_SIZE, (i+1) * SEGMENT_SIZE);

      // go through all blocks
      for(int j = 0; j < DATA_BLOCKS_IN_SEGMENT; j++){
        if(ss.liveness[j] != 0){
          // read block
          memset(block,0,)
          read_block()
          // check if live block
          if()
        }
      }
    }
  }

}
*/

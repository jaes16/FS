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
#define DATA_BLOCKS_IN_SEGMENT 31 // SEGMENT_SIZE/BLOCK_SIZE -1
#define MAX_SEGMENTS 78 // DATA_SIZE/SEGMENT_SIZE
#define MAX_INODES 2496 // = MAX_BLOCKS (sort of overshooting)
#define MAX_ITABLE_BLOCKS 64 // ITABLE_MAX_SIZE/BLOCK_SIZE rounded up
#define ITABLE_ENT_IN_BLOCK 39 // BLOCK_SIZE/ITABLE_ENTRY_SIZE

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
  time_t timestamp;
  int num_itable_blocks;
  int num_inodes;
  char inode_numbers[MAX_INODES];
  int pointers[MAX_ITABLE_BLOCKS];
  int segments_usage[MAX_SEGMENTS]; // a list of which segments are being used. could use only bits, but we have an abundance of space left.
  char pad[(BLOCK_SIZE - ((MAX_ITABLE_BLOCKS+MAX_SEGMENTS+4) * 4)) - MAX_INODES];
} checkpoint;

typedef struct // size: SEGMENT_SIZE
{
  superblock sb;
  checkpoint cp[DATA_BLOCKS_IN_SEGMENT]; // to make sure the size of the region is a segment long
} checkpoint_region;




typedef struct // size: BLOCK_SIZE
{
  int num_filled;
  short liveness[DATA_BLOCKS_IN_SEGMENT]; // 64 bytes in total. 0 for empty, 1 for live, 2 for flushed, -1 for dead. Update later for activeness
  char pad[BLOCK_SIZE - ((DATA_BLOCKS_IN_SEGMENT+2) * sizeof(short))];
} seg_summary;




typedef struct // size: 104 = ITABLE_ENTRY_SIZE
{
  int inode_num;
  inode inode;
} itable_entry;



typedef struct
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


// Just to make sure we're erasing in segment sizes. Just writing out 0's to segments
// making sure we're ignoring the first segment, because its off limits
void perase(int size_in_segments, int seg_num)
{
  char *buf = (char *) malloc(size_in_segments*SEGMENT_SIZE);
  memset(buf, 0, SEGMENT_SIZE * size_in_segments);
  pwrite(fd_drive, buf, size_in_segments * SEGMENT_SIZE, (seg_num+1) * SEGMENT_SIZE);
  free(buf);
}




void write_checkpoint()
{
  checkpoint check;

  // go through checkpoint region and find an empty spot to write this checkpoint to
  for(int i = 1; i < DATA_BLOCKS_IN_SEGMENT; i++){
    memset(&check, 0, BLOCK_SIZE);
    pread(fd_drive, &check, BLOCK_SIZE, BLOCK_SIZE*i);
    if(check.num_itable_blocks == 0){
      cp.timestamp = time(NULL);
      pwrite(fd_drive, &cp, BLOCK_SIZE, BLOCK_SIZE*i);
      return;
    }
    memset(&check, 0, BLOCK_SIZE);
    pread(fd_drive, &check, BLOCK_SIZE, (BLOCK_SIZE*i)+SECOND_CR);
    if(check.num_itable_blocks == 0){
      cp.timestamp = time(NULL);
      pwrite(fd_drive, &cp, BLOCK_SIZE, BLOCK_SIZE*i);
      return;
    }
  }

  // if we never returned in the for loop, means that there are no empty spots, so empty out the two segments
  perase(1,-1);
  pwrite(fd_drive, &sb, BLOCK_SIZE, 0);
  cp.timestamp = time(NULL);
  pwrite(fd_drive, &cp, BLOCK_SIZE, BLOCK_SIZE);
  perase(1,MAX_SEGMENTS+1);
  pwrite(fd_drive, &sb, BLOCK_SIZE, SECOND_CR);
  return;
}




void write_seg_summary()
{
  pwrite(fd_drive, &ss, BLOCK_SIZE, which_seg*SEGMENT_SIZE);
}





void flush_segment()
{

  for(int i = 0; i < DATA_BLOCKS_IN_SEGMENT; i++){
    if(ss.liveness[i] == 1){ // if live and unflushed
      pwrite(fd_drive, segment[i], BLOCK_SIZE, (which_seg+1) * SEGMENT_SIZE);
      ss.liveness[i] = 2;
    }
  }

  write_checkpoint();
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
    }
  }
  if(which_seg == -1) return -1;
  else return 0;
}



void free_segment()
{
  memset(&ss, 0, BLOCK_SIZE);

  for(int i = 0; i < DATA_BLOCKS_IN_SEGMENT; i++){
    free(segment[i]);
  }
  memset(segment, 0, sizeof(void *) * DATA_BLOCKS_IN_SEGMENT);
}



// return -1 if unable to allocate more space, otherwise returns the absolute address of the where the block was added
int add_to_segment(void *block){
  // check if a block is available in segment
  if(ss.num_filled == DATA_BLOCKS_IN_SEGMENT){
    // if this segment is full, flush it out, write out its segment summary and get a new segment
    flush_segment();
    free_segment();
    write_seg_summary();
    if(get_segment() == -1) return -ENOSPC;
  }
  segment[ss.num_filled] = block;
  ss.liveness[ss.num_filled] = 1;
  ss.num_filled++;
  return (which_seg);
}




// When theres no drive, create generic drive
void create_init_drive()
{
  fd_drive = fileno(drive);

  // prepare first segment with segment summary, rootdir, itable
  memset(&ss, 0, BLOCK_SIZE);
  memset(d_table, 0, sizeof(d_table));

  ss.liveness[0] = 1;
  ss.liveness[1] = 1;

  // rootdir data preparation. set up . and .. in root
  d_table[0].inode_num = 1; // using first inode number, 1
  d_table[1].inode_num = 1;
  d_table[0].file_name[0] = '.';
  d_table[1].file_name[0] = '.';
  d_table[1].file_name[1] = '.';

  // prepare for inode in inode table
  // initialize inode table and place the first inode
  itable = (i_table *) malloc(BLOCK_SIZE);
  memset(itable, 0, BLOCK_SIZE);
  itable[0].num_to_inodes[0].inode_num = 1;
  itable[0].num_to_inodes[0].inode.mode = S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO;
  itable[0].num_to_inodes[0].inode.uid = 0;
  itable[0].num_to_inodes[0].inode.size = sizeof(dir_table) * 2;
  time_t t = time(NULL);
  struct tm tm = *localtime(&t);
  itable[0].num_to_inodes[0].inode.time = tm.tm_sec;
  itable[0].num_to_inodes[0].inode.ctime = tm.tm_sec;
  itable[0].num_to_inodes[0].inode.mtime = tm.tm_sec;
  itable[0].num_to_inodes[0].inode.dtime = 0;
  itable[0].num_to_inodes[0].inode.links_count = 1;
  itable[0].num_to_inodes[0].inode.block_count = 1;
  itable[0].num_to_inodes[0].inode.blocks[0] = SEGMENT_SIZE+BLOCK_SIZE;

  // prepare superblock
  sb.checkpoint_location = BLOCK_SIZE;
  sb.itable_size = BLOCK_SIZE;
  sb.data_location = SEGMENT_SIZE;
  sb.data_size = DATA_SIZE;

  // prepare checkpoint
  cp.timestamp = time(NULL);
  cp.num_itable_blocks = 1;
  cp.num_inodes = 1;
  cp.pointers[0] = SEGMENT_SIZE+(BLOCK_SIZE*2); // the first segment is for cr, then two blocks for seg_summary and rootdir data. Then there's the itable
  cp.segments_usage[0] = 1;

  // clear up segments
  perase(2, 0);

  // finally write out in block sizes
  // write out sb and checkpoints, to both checkpoint regions
  pwrite(fd_drive, &sb, BLOCK_SIZE, 0);
  pwrite(fd_drive, &sb, BLOCK_SIZE, SECOND_CR);
  pwrite(fd_drive, &cp, BLOCK_SIZE, BLOCK_SIZE);
  pwrite(fd_drive, &cp, BLOCK_SIZE, SECOND_CR + BLOCK_SIZE);
  // write out segment summary, rootdir data, and itable
  pwrite(fd_drive, &ss, BLOCK_SIZE, SEGMENT_SIZE);
  pwrite(fd_drive, d_table, BLOCK_SIZE, SEGMENT_SIZE + BLOCK_SIZE);
  pwrite(fd_drive, itable, BLOCK_SIZE, SEGMENT_SIZE + (2 * BLOCK_SIZE));

  // make sure its in memory
  dir_table *block1 = (dir_table *) malloc(sizeof(dir_table) * (DIR_TABLE_SIZE+1));
  memset(block1, 0, sizeof(dir_table) * (DIR_TABLE_SIZE+1));
  memcpy(block1, d_table, BLOCK_SIZE);
  segment[0] = block1;

  i_table *block2 = (i_table *) malloc(BLOCK_SIZE);
  memset(block2, 0, BLOCK_SIZE);
  //pread(fd_drive, block2, )
  memcpy(block2, itable, BLOCK_SIZE);
  segment[1] = block2;


}



// saves the appropriate inode info in the inode pointer
// return inode_num if path exists, -1 if path doesn't exist, -2 if path name is too long, -3 if component of path is file
// later, can remove last and just find it from path in this function
int get_inode(const char *path, inode *id)
{

  // if root, return root inode
  if(strcmp(path,"/") == 0){
    memset(id, 0, INODE_SIZE);
    memcpy(id, &itable[0].num_to_inodes[0].inode, INODE_SIZE);
    return itable[0].num_to_inodes[0].inode_num;
  }

  // check path length
  if(strlen(path) > PATH_MAX) return -2;

  int inode_num = 1; // have to think about whether path should always be an absolute path
  memset(id, 0, INODE_SIZE);

  // get current component of the path
  char dup[PATH_MAX];
  memset(dup, 0, PATH_MAX);
  if(path[0] == '/') strncpy(dup, path+1, PATH_MAX-1);
  else strncpy(dup, path, PATH_MAX);
  char *current;
  inode parent;

  // find inode of the first component we're searching
  for(int i = 0; i < cp.num_itable_blocks; i++){
    for(int j = 0; j < ITABLE_ENT_IN_BLOCK; j++){
      if(itable[i].num_to_inodes[j].inode_num == inode_num) memcpy(&parent, &itable[i].num_to_inodes[j].inode, INODE_SIZE);
    }
  }

  int go_on = 0;

  // until we've found the inode for the file or can't find the file
  for(current = strtok(dup, "/"); current != NULL; current = strtok(NULL, "/")){
    // check how long the file name is
    if(strlen(current) > 32) return -2;

    // check that it is a directory
    if((parent.mode & S_IFDIR) == 0) return -3;

    // look through every directory data block of this directory
    for(int i = 0; (i < parent.block_count) && (go_on != 1); i++){
      // read in current directory table
      memset(d_table, 0, BLOCK_SIZE);
      pread(fd_drive, &d_table, BLOCK_SIZE, BLOCK_SIZE * parent.blocks[i]);

      // check in current directory table if that component exists
      for(int j = 0; (j < DIR_TABLE_SIZE) && (d_table[j].inode_num != 0) && (go_on != 1); j++){

        if(strcmp(d_table[j].file_name,current) == 0){
          // if the component does exist, get inode from inode table
          inode_num = d_table[j].inode_num;
          for(int k = 0; k < cp.num_itable_blocks; i++){
            for(int l = 0; l < ITABLE_ENT_IN_BLOCK; l++){
              if(itable[l].num_to_inodes[k].inode_num == inode_num){
                memset(&parent, 0, INODE_SIZE);
                memcpy(&parent, &itable[l].num_to_inodes[k].inode, INODE_SIZE);
              }
            }
          }
          // check if this is the last component
          if(strchr(strstr(path,current), '/') != NULL){
            // if not, continue with next component
            go_on = 1;
          }
          else{
            // if it is the target file, return inode
            memcpy(id, &parent, INODE_SIZE);
            return inode_num;
          }
        }
      }
    }
    // check if we have found the component and can continue or should return an error
    if(go_on == 1) go_on = 0;
    else return -1;
  }
  return -1;
}




void* fs_init(struct fuse_conn_info *conn)
{

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

    int i;
    for(i = (DATA_BLOCKS_IN_SEGMENT-1) /*there are DATA_BLOCKS_IN_SEGMENT cp's in a cr*/; cr.cp[i].num_inodes == 0; i++){} // find the last valid cp
    cp.timestamp = cr.cp[i].timestamp;
    memcpy(&cp.pointers, &cr.cp[i].pointers, sizeof(cp.pointers));

    // checking second checkpoint_region
    memset(&cr, 0, SEGMENT_SIZE);
    pread(fd_drive,&cr,SEGMENT_SIZE,0);

    for(i = (DATA_BLOCKS_IN_SEGMENT-1) /*as above*/; cr.cp[i].num_inodes == 0; i++){} // find the last valid cp
    if(difftime(cp.timestamp, cr.cp[i].timestamp) < 0){ // if this cp was written later
      cp.timestamp = cr.cp[i].timestamp;
      memcpy(&cp.pointers, &cr.cp[i].pointers, sizeof(cp.pointers));
    }

    // now that we have the checkpoint_region, read in latest itable
    itable = (i_table *) malloc(cp.num_itable_blocks * BLOCK_SIZE);
    for(i = 0; i < cp.num_itable_blocks; i++){
      pread(fd_drive, &itable + (BLOCK_SIZE*i), BLOCK_SIZE, BLOCK_SIZE*cp.pointers[i]);
    }

    get_segment();


  }else{
    // if it isn't, create file and write out initial state
    drive = fopen(drive_path, "w+");
    fseek(drive, DRIVE_SIZE-1, SEEK_SET);
    fputc('\0', drive);

    // write out initialized
    create_init_drive();

  }

  return NULL;
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
  if ((call_rslt == -3) && ((id.mode & S_IFDIR) == 0)) return -ENOTDIR;

  if( (offset/DIR_TABLE_SIZE) > id.block_count) return 0;
  // find the offset block and offset size we should start reading from
  int offset_block = offset / DIR_TABLE_SIZE;
  int offset_num = offset % DIR_TABLE_SIZE;

  for(int i = offset_block; i < id.block_count; i++){
    memset(&d_table, 0, sizeof(d_table));
    pread(fd_drive, &d_table, BLOCK_SIZE, id.blocks[offset_block]);
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


  int call_rslt = get_inode(path, &id);
  if (call_rslt == 1) return -ENOENT; // can't make root
  if (call_rslt == 0) return -EEXIST; // can't kill whats already dead, can't create something that already exists
  if (call_rslt == -2) return -ENAMETOOLONG; // name too long!
  if (call_rslt == -3) return -ENOTDIR; // a component of the path is not a directory

  char *dup = strdup(path);
  char *dup1 = basename(dup);
  char *dup2;
  char file_name[MAX_FILE_NAME];
  char path_parent[MAX_PATH];
  char parent_name[MAX_FILE_NAME];
  memset(parent_name, 0, MAX_FILE_NAME);
  strncpy(file_name, dup1, strlen(dup1));
  free(dup);
  dup = strdup(path);
  dup1 = dirname(dup);
  strncpy(path_parent, dup1, strlen(dup1));
  dup2 = basename(dup1);
  strncpy(parent_name, dup2, strlen(dup2));
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
  strncpy(dir_data[0].file_name, file_name, strlen(file_name));
  dir_data[1].inode_num = parent_inode_num;
  strncpy(dir_data[1].file_name, path_parent, strlen(path_parent));
  id.blocks[0] = add_to_segment(dir_data);
  if(id.blocks[0] == -1) return -ENOSPC;

  // create inode
  id.mode = mode & S_IFDIR;
  id.uid = 0;
  id.size = DT_ENTRY_SIZE * 2;
  id.time = time(NULL);
  id.ctime = time(NULL);
  id.mtime = time(NULL);
  id.gid = 0;
  id.links_count = 1;
  id.block_count = 1;


  // see if there is space in parent directory data to place this file's name and inumber. If not, allocate another block and update parent inode
  int enough_space = 0;
  for(int i = 0; (i < parent_id.block_count) && (enough_space == 0); i++){
    // read in data block
    memset(d_table, 0, BLOCK_SIZE);
    pread(fd_drive, d_table, BLOCK_SIZE, parent_id.blocks[i]);
    // check in data block if there are free spaces
    for(int j = 0; j < DIR_TABLE_SIZE; j++){
      // if we find a free space
      if(d_table[j].inode_num == 0){
        // make changes in the table in memory
        d_table[j].inode_num = inode_number;
        strncpy(d_table[j].file_name, file_name, strlen(file_name));
        // make sure the changes in the table are gonna be made in drive
        dir_table *block = (dir_table *) malloc(BLOCK_SIZE);
        memcpy(block, &d_table, BLOCK_SIZE);
        cp.pointers[j] = add_to_segment(block);
        if(cp.pointers[j] == -1) return -ENOSPC;
        enough_space = 1;
        break;
      }
    }
  }
  // if no more space in parent directory data, add new block
  if(enough_space == 0){

    // make a new block to write out
    memset(d_table, 0, BLOCK_SIZE);
    // add this inode and file name pair
    d_table[0].inode_num = inode_number;
    strncpy(d_table[0].file_name, file_name, strlen(file_name));
    // write out
    dir_table *block = (dir_table *) malloc(BLOCK_SIZE);
    memcpy(block, &d_table, BLOCK_SIZE);
    parent_id.blocks[parent_id.block_count] = add_to_segment(block);
    if(parent_id.blocks[parent_id.block_count] == -1) return -ENOSPC;
    parent_id.block_count++;

  }

  parent_id.size =+ DT_ENTRY_SIZE;
  // see if there is enough space to allocate this inode in itable
  enough_space = 0;
  for(int i = 0; i < cp.num_itable_blocks; i++){
    for(int j = 0; j < ITABLE_ENT_IN_BLOCK; j++){
      // change parent inode
      if(itable[i].num_to_inodes[j].inode_num == parent_inode_num){
        memset(&itable[i].num_to_inodes[j].inode, 0, INODE_SIZE);
        memcpy(&itable[i].num_to_inodes[j].inode, &parent_id, INODE_SIZE);
        // write out changes
        i_table *block = (i_table *) malloc(BLOCK_SIZE);
        memcpy(block, &itable[i], BLOCK_SIZE);
        cp.pointers[i] = add_to_segment(block);
        if(cp.pointers[i] == -1) return -ENOSPC;
      }
      if((itable[i].num_to_inodes[j].inode_num == 0) && (enough_space == 0)){
        // make changes in the table in memory
        itable[i].num_to_inodes[j].inode_num = inode_number;
        itable[i].num_to_inodes[j].inode = id;
        // make sure the changes in the table are gonna be made in drive
        i_table *block = (i_table *) malloc(BLOCK_SIZE);
        memcpy(block, &itable[i], BLOCK_SIZE);
        cp.pointers[i] = add_to_segment(block);
        if(cp.pointers[i] == -1) return -ENOSPC;
        enough_space = 1;
        break;
      }
    }
  }
  // if not, allocate another block
  if(enough_space == 0){

    // make a new block to write out
    i_table temp[cp.num_itable_blocks];
    memset(temp, 0, cp.num_itable_blocks * BLOCK_SIZE);
    memcpy(temp, itable, cp.num_itable_blocks * BLOCK_SIZE);
    itable = (i_table *) realloc(itable, (cp.num_itable_blocks+1) * BLOCK_SIZE);
    memset(itable, 0, (cp.num_itable_blocks+1) * BLOCK_SIZE);
    memcpy(itable, temp, cp.num_itable_blocks * BLOCK_SIZE);

    // put in inode
    itable[cp.num_itable_blocks-1].num_to_inodes[0].inode_num = inode_number;
    itable[cp.num_itable_blocks-1].num_to_inodes[0].inode = id;

    // write out to disk and update checkpoint
    i_table *newblock = (i_table *) malloc(BLOCK_SIZE);
    memcpy(newblock, &itable[cp.num_itable_blocks-1], BLOCK_SIZE);
    cp.num_itable_blocks++;
    cp.pointers[cp.num_itable_blocks-1] = add_to_segment(newblock);
    if(cp.pointers[cp.num_itable_blocks-1] == -1) return -ENOSPC;
  }


  return 0;
}






void destroy(const void *privatedata)
{
  flush_segment();
  free_segment();
  write_seg_summary();
  fclose(drive);
}





static struct fuse_operations fs_filesystem_operations = {
    .init       = fs_init,
    .getattr    = fs_getattr,
    .access     = fs_access,
    .readdir    = fs_readdir,
    //.mkdir      = fs_mkdir,
    /*.rmdir      = fs_rmdir,
    .fgetattr   = fs_fgetattr,
    .mknod      = fs_mknod,
    .unlink     = fs_unlink,
    .open       = fs_open,
    .statfs     = fs_statfs,
    .create     = fs_create,
    .release    = fs_release,
    .read       = fs_read,
    .write      = fs_write,
    .truncate   = fs_truncate,
*/
};




int main(int argc, char **argv)
{
    return fuse_main(argc, argv, &fs_filesystem_operations, NULL);
}

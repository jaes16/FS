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
  int segments_usage[MAX_SEGMENTS]; // a list of which segments are being used. could use only bits, but we have an abundance of space left. 312 bytes

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


char segment[32][4096];
dir_table d_table[DIR_TABLE_SIZE+1];
i_table *itable;
FILE *drive;
int fd_drive;
seg_summary ss[2];




int main(){
  char drive_path[MAX_PATH];
  // find where "drive" file should be
  memset(drive_path, '\0', MAX_PATH);
  getcwd(drive_path, (MAX_PATH - sizeof("/FS_SSD")));
  strcat(drive_path, "/FS_SSD");
  drive = fopen(drive_path, "r");
  fd_drive = fileno(drive);

  // if it is, read in checkpoint_regions and find latest checkpoint
  checkpoint_region cr;
  checkpoint cp;

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
  for(i = 0; i < SEG_SUM_BLOCKS; i++){
    pread(fd_drive, &ss[i], BLOCK_SIZE, cp.segsum_loc[i]);
  }

  for(i = 0; i < SEG_SUM_BLOCKS; i++){
    int s = ((cp.segsum_loc[i]/SEGMENT_SIZE) - 1); // segment the segsum is in
    ss[s/SEG_SUM_IN_BLOCK].liveness[s%SEG_SUM_IN_BLOCK][((cp.segsum_loc[i]/4096)-1)%DATA_BLOCKS_IN_SEGMENT] = 2;
  }

  memset(segment, 0, SEGMENT_SIZE);
  for(int i = 0; i < 20; i++){

    pread(fd_drive, segment, SEGMENT_SIZE, SEGMENT_SIZE*(i+1));

    memset(segment, 0, SEGMENT_SIZE - BLOCK_SIZE);

  }

  free(itable);
  return 0;
}

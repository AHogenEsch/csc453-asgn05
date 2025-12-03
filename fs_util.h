#ifndef FS_UTIL_H
#define FS_UTIL_H

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <libgen.h>

// --- Constants ---
#define DIRECT_ZONES 7
#define INODE_SIZE 64 // Size of an inode in bytes
#define DIR_ENTRY_SIZE 64 // Size of a directory entry in bytes
#define SECTOR_SIZE 512
#define PARTITION_TABLE_OFFSET 0x1BE

// Use this attribute to prevent compiler padding for on-disk structures
#define PACKED __attribute__((__packed__))

// --- Data Structures ---

// Partition Table Entry (Figure 2)
typedef struct PACKED {
    uint8_t bootind;
    uint8_t start_head;
    uint8_t start_sec;
    uint8_t start_cyl;
    uint8_t type;         // 0x81 is MINIX
    uint8_t end_head;
    uint8_t end_sec;
    uint8_t end_cyl;
    uint32_t lFirst;      // First sector (LBA addressing)
    uint32_t size;        // Size of partition (in sectors)
} partition_entry_t;

// MINIX Version 3 Superblock (Figure 3)
typedef struct PACKED {
    uint32_t ninodes;
    uint16_t pad1;
    int16_t i_blocks;           // # of blocks used by inode bit map
    int16_t z_blocks;           // # of blocks used by zone bit map
    uint16_t firstdata;         // number of first data zone
    int16_t log_zone_size;      // log2 of blocks per zone
    int16_t pad2;
    uint32_t max_file;          // maximum file size
    uint32_t zones;             // number of zones on disk
    int16_t magic;              // magic number (0x4D5A for MINIX v3)
    int16_t pad3;
    uint16_t blocksize;         // block size in bytes
    uint8_t subversion;         // filesystem sub-version
} minix_superblock_t;

// MINIX Inode (Figure 4)
typedef struct PACKED {
    uint16_t mode;                      // mode (file type and permissions)
    uint16_t links;                     // number or links
    uint16_t uid;
    uint16_t gid;
    uint32_t size;                      // file size in bytes
    int32_t atime;                      // access time
    int32_t mtime;                      // modification time
    int32_t ctime;                      // status change time
    uint32_t zone[DIRECT_ZONES];        // direct zones
    uint32_t indirect;                  // single indirect zone
    uint32_t two_indirect;              // double indirect zone
    uint32_t unused;
} minix_inode_t;

// MINIX Directory Entry (Figure 5)
typedef struct PACKED {
    uint32_t inode;             // inode number (0 is deleted/invalid)
    unsigned char name[60];     // filename string
} minix_dir_entry_t;

// --- Global State Declarations (Externally accessible by minls/minget) ---

extern FILE *image_fp;
extern long fs_offset;
extern minix_superblock_t current_sb;
extern uint32_t zone_size;
extern int is_verbose;

// --- Function Prototypes (Exposed API for minls/minget) ---

// File System Initialization
int init_filesystem(const char *image_file, int p_num, int s_num,\
     int verbose_flag);
void cleanup_filesystem(void);

// Low-Level I/O
int read_fs_bytes(off_t offset_from_fs_start, void *buffer, size_t nbytes);

// Inode and Block Access
int read_inode(uint32_t inode_num, minix_inode_t *inode_out);
uint32_t get_file_block(const minix_inode_t *inode, uint32_t logical_block);

// Path Traversal
char *canonicalize_path(const char *path);
uint32_t get_inode_by_path(const char *canonical_path);

// Utility/Formatting
void get_permissions_string(uint16_t mode, char *perm_str);

// Verbose Output (minls -v)
void print_verbose_superblock(const char *image_file, int p_num, int s_num);
void print_verbose_inode(uint32_t inode_num, const minix_inode_t *inode);


#endif // FS_UTIL_H
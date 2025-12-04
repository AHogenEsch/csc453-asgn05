#include "fs_util.h"

// ~~~ Global State Definitions (Shared with minls and minget)

FILE *image_fp = NULL;
// Byte offset from the start of the image to the filesystem
long fs_offset = 0; 
minix_superblk_t curr_sb;
uint32_t zone_size = 0;
uint32_t blks_per_zone = 0; // Calculated from log_zone_size
int verbose = 0; // Set by init_filesystem

// ~~~ Reads and validates the Master Boot Record
// Returns 0 on success (buffer filled, magic good), -1 on failure.
static int read_mbr_and_check_magic(off_t table_addr, uint8_t mbr_buffer[512]) {
    // The MBR is 512 bytes long (1 sector)
    if (fseek(image_fp, table_addr - PARTITION_TABLE_OFFSET, SEEK_SET) != 0) {
        fprintf(stderr, 
        "Error: fseek to MBR offset %ld failed.\n", 
        table_addr - PARTITION_TABLE_OFFSET);
        return -1;
    }
    
    // Read the entire 512-byte MBR block
    if (fread(mbr_buffer, 1, SECTOR_SIZE, image_fp) != SECTOR_SIZE) {
        fprintf(stderr, 
        "Error: Failed to read MBR at offset %ld.\n", 
        table_addr - PARTITION_TABLE_OFFSET);
        return -1;
    }
    
    // Check the MBR signature (Magic Number) at offsets 510 and 511
    // The required value is 0x55AA
    if (mbr_buffer[510] != 0x55 || mbr_buffer[511] != 0xAA) {
        // This is the check that was failing the tests (117, 118)
        fprintf(stderr, "Partition table with bad magic: 0x%02x%02x\n", 
            mbr_buffer[511], mbr_buffer[510]);
        return -1;
    }
    
    return 0; // Magic check passed
}

// ~~~ Reads Partition Entry from absolute disk offset
// table_addr is the offset to the partition table entries (0x1BE)
static uint32_t get_partition_start(int part_num, off_t table_addr) {
    uint8_t mbr_buffer[SECTOR_SIZE]; // 512 bytes for MBR

    // Read MBR and validate magic
    if (read_mbr_and_check_magic(table_addr, mbr_buffer) != 0) {
        return 0; // Failure handled inside read_mbr_and_check_magic
    }

    // Now, the partition entries start at table_addr (offset 0x1BE or 446)
    // The table_addr we pass is relative to the start of the disk/partition,
    // so we get the entry pointer relative to the buffer start.
    partition_entry_t *pt = 
        (partition_entry_t *)(mbr_buffer + PARTITION_TABLE_OFFSET);
    
    // Check partition number validity
    if (part_num < 0 || part_num >= 4) {
        fprintf(stderr, 
    "Partition number %d is out of range (0-3).\n", part_num);
        return 0;
    }

    // Check MINIX type (0x81)
    if (pt[part_num].type != 0x81) {
        fprintf(stderr, "Partition %d is type 0x%02x, \
        not a MINIX partition (0x81).\n", part_num, pt[part_num].type);
        return 0;
    }
    
    // Return the LBA sector number of the partition start
    return pt[part_num].lFirst; 
}


// ~~~ 1. Low-Level I/O

/** * Reads bytes from the disk image relative to the 
* filesystem start (fs_offset).
* Returns 0 on success, -1 on failure.
*/
int read_fs_bytes(off_t offset_from_fs_start, void *buffer, size_t nbytes) {
    off_t abs_offset = fs_offset + offset_from_fs_start;
    
    if (fseek(image_fp, abs_offset, SEEK_SET) != 0) {
        if (verbose) fprintf(stderr, "read_fs_bytes: \
        fseek to offset %ld failed (errno: %d).\n", abs_offset, errno);
        return -1;
    }
    
    if (fread(buffer, 1, nbytes, image_fp) != nbytes) {
        if (verbose) fprintf(stderr, "read_fs_bytes: \
        fread %zu bytes at offset %ld failed.\n", nbytes, abs_offset);
        return -1;
    }
    return 0;
}


// ~~~ 2. Filesystem Initialization

/**
* Initializes the global state (fs_offset, superblk).
* Returns 0 on success, -1 on failure.
*/
int init_filesystem(const char *image_file, int p_num, int s_num, \
    int verbose_flag) {
    verbose = verbose_flag;
    
    // Open the image file
    image_fp = fopen(image_file, "r");
    if (!image_fp) {
        perror("Error opening image file");
        return -1;
    }

    // 1) Determine fs_offset from partitioning (if requested)
    fs_offset = 0; // Default to unpartitioned
    
    if (p_num != -1) {
        // Primary partition table is at disk offset 0 + 0x1BE
        uint32_t p_start_sector = get_partition_start(p_num, \
            PARTITION_TABLE_OFFSET);
        if (p_start_sector == 0) {
            // Error already printed in get_partition_start
            return -1; 
        }
        
        fs_offset = (long)p_start_sector * SECTOR_SIZE;
        
        if (s_num != -1) {
// Subpartition table starts relative to the containing partition's MBR block
// MBR block is at fs_offset, and the table is at MBR_blk + 0x1BE
            off_t sub_pt_addr = fs_offset + PARTITION_TABLE_OFFSET;
            uint32_t s_start_sector_rel_disk = \
            get_partition_start(s_num, sub_pt_addr);
            
            if (s_start_sector_rel_disk == 0) {
                // Error already printed in get_partition_start
                return -1; 
            }
            
            // The subpartition's LBA is relative to the disk start, 
            // so we update fs_offset
            fs_offset = (long)s_start_sector_rel_disk * SECTOR_SIZE;
        }
    }
    
    // 2) Read Superblk (always at offset 1024 bytes from FS start)
    if (read_fs_bytes(1024, &curr_sb, sizeof(minix_superblk_t)) != 0) {
        fprintf(stderr, \
            "Error reading superblk (offset %ld).\n", fs_offset + 1024);
        return -1;
    }
    
    // 3) Validate Magic Number
    if (curr_sb.magic != 0x4D5A) { // MINIX v3 magic number
        fprintf(stderr, "Bad magic number. (0x%04x)\n", curr_sb.magic);
        fprintf(stderr, "This doesn't look like a MINIX filesystem.\n");
        return -1;
    }
    
    // 4) Calculate disk geometry
    blks_per_zone = 1 << curr_sb.log_zone_size;
    zone_size = (uint32_t)curr_sb.blksize * blks_per_zone;
    
    if (verbose) {
        print_verbose_superblk(image_file, p_num, s_num);
    }
    
    return 0;
}

/**
* Cleans up global state, closing the file pointer.
*/
void cleanup_filesystem(void) {
    if (image_fp) {
        fclose(image_fp);
        image_fp = NULL;
    }
}


// ~~~ 3. Inode and blk Access

/**
* Reads an inode into the provided structure.
* Returns 0 on success, -1 on failure.
*/
int read_inode(uint32_t inode_num, minix_inode_t *inode_out) {
    if (inode_num == 0 || inode_num > curr_sb.ninodes) {
        return -1;
    }

    // Inodes start at blk 2 + B_imap + B_zmap
    uint32_t inode_start_blk = 2 + \
    curr_sb.i_blks + curr_sb.z_blks;
    
    // Inodes are numbered 1-based, array i is 0-based
    uint32_t i = inode_num - 1;

    // Offset calculation: (blk * blksize) + (i * Inode_Size)
    off_t offset = (off_t)inode_start_blk * curr_sb.blksize;
    offset += (off_t)i * INODE_SIZE;

    return read_fs_bytes(offset, inode_out, sizeof(minix_inode_t));
}

/**
* Converts a logical block number (from the start of the file) to an
* absolute block number on disk (relative to the FS start).
* Returns the absolute block number on disk (0 for holes/invalid).
* NOTE: For MINIX v3, zones are typically 1 block (log_zone_size=0).
*/
uint32_t get_file_blk(const minix_inode_t *inode, uint32_t logical_blk) {
    uint32_t zone_num = 0;
    uint32_t blks_per_zone_val = 1 << curr_sb.log_zone_size;
    uint32_t ptrs_per_blk = curr_sb.blksize / sizeof(uint32_t);
    
    uint32_t logical_zone = logical_blk / blks_per_zone_val;
    uint32_t blk_in_zone = logical_blk % blks_per_zone_val;

    // Direct Zones (0 to DIRECT_ZONES-1)
    if (logical_zone < DIRECT_ZONES) { 
        zone_num = inode->zone[logical_zone];
    }
    // Single indirect Zone
    else if (logical_zone < DIRECT_ZONES + ptrs_per_blk) {
        uint32_t indir_zone_i = logical_zone - DIRECT_ZONES;
        
        // Check if the indir zone itself exists
        if (inode->indirect != 0) {
            uint32_t indir_ptrs[ptrs_per_blk];
            off_t indir_blk_offset = (off_t)inode->indirect * zone_size;

            // Read the blk containing the list of zone ptrs
            if (read_fs_bytes(indir_blk_offset, \
                indir_ptrs, curr_sb.blksize) == 0) {
                // Get the actual data zone number from the list
                zone_num = indir_ptrs[indir_zone_i];
            }
        }
    }
    // Double indir Zone
    else {
        // Calculate the logical zone i relative to the double indir
        uint32_t ptrs_in_single = ptrs_per_blk;
        uint32_t double_indir_start = DIRECT_ZONES + ptrs_in_single;
        uint32_t offset_in_double = logical_zone - double_indir_start;
        zone_num = 0; // Default to file hole

        // Check if the double indir blk itself exists
        if (inode->two_indirect != 0) {
            uint32_t first_level_ptrs[ptrs_in_single];
            off_t double_indir_offset = \
                (off_t)inode->two_indirect * zone_size;

            // Read the first level (ptrs to single indir blks)
            if (read_fs_bytes(double_indir_offset, \
                first_level_ptrs, curr_sb.blksize) == 0) {
                
                uint32_t first_level_i = \
                    offset_in_double / ptrs_in_single;
                
                // Check if the i is valid
                if (first_level_i < ptrs_in_single) {
                    uint32_t second_level_zone = \
                        first_level_ptrs[first_level_i];

                    if (second_level_zone != 0) {
                        uint32_t second_level_ptrs[ptrs_in_single];
                        off_t second_indir_offset = \
                            (off_t)second_level_zone * zone_size;
                        
                        // Read the second level (ptrs to data zones)
                        if (read_fs_bytes(second_indir_offset, \
          second_level_ptrs, curr_sb.blksize) == 0) {

                            uint32_t second_level_i = \
                                offset_in_double % ptrs_in_single;
                            
                            zone_num = \
                                second_level_ptrs[second_level_i];
                        }
                    }
                }
            }
        }
    }
    
    // Check for file hole (zone 0)
    if (zone_num == 0) return 0; 
    
    // blk number = Zone number * blks_per_zone_val + blk_in_zone
    uint32_t disk_blk_num = (zone_num * blks_per_zone_val) + blk_in_zone;

    // This blk number is now ready to be multiplied by curr_sb.blksize
    return disk_blk_num;
}


// ~~~ 4. Path Traversal

/**
* Removes duplicate slashes and ensures a single leading slash.
* Returns a newly allocated, canonicalized path string. Caller must free.
*/
char *canonicalize_path(const char *path) {
    if (!path || path[0] == '\0') {
        char *p = malloc(2);
        if (p) strcpy(p, "/");
        return p;
    }

    // Allocate slightly more than needed, just in case
    size_t len = strlen(path);
    char *temp_copy = malloc(len + 2); 
    char *new_path = malloc(len + 2);
    if (!temp_copy || !new_path) {
        free(temp_copy); // Free if only one succeeded
        free(new_path);
        return NULL;
    }

    char *p = new_path;

    strcpy(temp_copy, path);
    *p++ = '/'; // Always start with one slash

    char *token;
    char *saveptr;

    // Tokenize on '/'
    token = strtok_r(temp_copy, "/", &saveptr);
    while (token != NULL) {
        size_t token_len = strlen(token);
        // Copy the token
        strncpy(p, token, token_len);
        p += token_len;
        // Add a slash if there are more components
        if ((token = strtok_r(NULL, "/", &saveptr)) != NULL) {
            *p++ = '/';
        }
    }

    *p = '\0'; // Null terminate the final string
    free(temp_copy);

    // If result is just a "/" (e.g., input was "//" or ""), done.
    if (new_path[0] == '/' && new_path[1] == '\0') {
        return new_path;
    }

    // Remove trailing slash if it's not the root itself
    if (p > new_path + 1 && *(p - 1) == '/') {
        *(p - 1) = '\0';
    }
    
    return new_path;
}

/**
* Finds the inode number for a given canonicalized path.
* Returns inode number on success (1-based), 0 on failure.
*/
uint32_t get_inode_by_path(const char *canonical_path) {
    uint32_t curr_inode_num = 1;
    
    // Handle the root path quickly
    if (strcmp(canonical_path, "/") == 0) {
        return curr_inode_num;
    }

    char path_copy[1024];
    // Copy the path, skipping the leading '/'
    strncpy(path_copy, canonical_path + 1, 1023);
    path_copy[1023] = '\0';
    
    char *token;
    char *saveptr = NULL; // Initialize saveptr
    
    // Start tokenizing from the first component
    token = strtok_r(path_copy, "/", &saveptr);

    // Loop through path components
    while (token != NULL) {
        minix_inode_t dir_inode;
        uint32_t i;
        uint32_t j;
        if (read_inode(curr_inode_num, &dir_inode) != 0) return 0;
        
        // saveptr now points to the character *after* the delimiter,
        // or to the NULL terminator if this is the last token.
        char *next_token_start = saveptr;
        
        uint32_t target_inode = 0;
        size_t token_len = strlen(token);
        
        // Loop through all blks of the directory file
        for (i= 0; i * curr_sb.blksize < dir_inode.size; i++) {
            uint32_t disk_blk = get_file_blk(&dir_inode, i);
            if (disk_blk == 0) continue; 
            
            off_t blk_offset = (off_t)disk_blk * curr_sb.blksize;
            
            uint8_t dir_blk_buf[curr_sb.blksize];
            if (read_fs_bytes(blk_offset, 
                dir_blk_buf, curr_sb.blksize) != 0) continue;
            
            for (j = 0; j*DIR_ENTRY_SIZE < curr_sb.blksize;j++){
                minix_dir_entry_t *entry = 
                (minix_dir_entry_t *)(dir_blk_buf + j * DIR_ENTRY_SIZE);
                
                if (entry->inode == 0) continue; 
                
                // Compare token to entry->name
                // Assuming minix dir entry name size is 60 or less
                if (token_len > 60) continue; 

                if (strncmp(token, (char*)entry->name, token_len) != 0) {
                    continue; 
                }

            // Ensure the match is exact (prevent "a" matching "abc")
                if (token_len < 60 && entry->name[token_len] != '\0') {
                    continue; 
                }
                
                // Match found!
                target_inode = entry->inode;
                goto found_component;
            }
        }
        
        found_component:
        
        // CHECK 1: Component not found
        if (target_inode == 0) {
             return 0; 
        }
        
    // CHECK 2: Traversal Error (Attempting to descend into a non-directory)
        
        minix_inode_t target_inode_data;
        if (read_inode(target_inode, &target_inode_data) != 0) return 0;

        // If this is NOT the last component (*saveptr != '\0') 
        // AND the target inode is NOT a directory (mode & 0170000) != 0040000
        if (*next_token_start != '\0' && 
            (target_inode_data.mode & 0170000) != 0040000) {
            // File is not a directory or doesn't exist
            fprintf(stderr, "Not a directory: trying to traverse file: %s\n", \
                    canonical_path);
            return 0; // Traversal failed
        }
          
        curr_inode_num = target_inode;

        // Get the next path component: This is the only call to 
        // strtok_r that advances state.
        token = strtok_r(NULL, "/", &saveptr);
    }
    
    return curr_inode_num;
}


// ~~~ 5. Utility/Formatting

/**
* Populates a 11-character string (10 chars + null terminator) with
* Unix-style permission bits based on the inode mode.
*/
void get_permissions_string(uint16_t mode, char *perm_str) {
    // File Type (0170000 mask)
    perm_str[0] = ((mode & 0170000) == 0040000) ? 'd' : '-'; 
    // Directory (0040000) or file (0100000)
    
    // Owner permissions
    perm_str[1] = (mode & 0000400) ? 'r' : '-'; 
    perm_str[2] = (mode & 0000200) ? 'w' : '-'; 
    perm_str[3] = (mode & 0000100) ? 'x' : '-'; 
    
    // Group permissions
    perm_str[4] = (mode & 0000040) ? 'r' : '-'; 
    perm_str[5] = (mode & 0000020) ? 'w' : '-'; 
    perm_str[6] = (mode & 0000010) ? 'x' : '-';
    
    // Other permissions
    perm_str[7] = (mode & 0000004) ? 'r' : '-';
    perm_str[8] = (mode & 0000002) ? 'w' : '-';
    perm_str[9] = (mode & 0000001) ? 'x' : '-';
    perm_str[10] = '\0';
}

// ~~~ 6. Verbose Output

/**
* Prints Superblock and Partition info to stderr for -v flag.
*/
void print_verbose_superblk(const char *image_file, int p_num, int s_num) {
    fprintf(stderr, "\n=== VERBOSE MODE (fs_util.c) ===\n");
    fprintf(stderr, "Image File: %s\n", image_file);
    fprintf(stderr, "Partition: %d, Subpartition: %d\n", p_num, s_num);
    fprintf(stderr, "FS Start (Disk Offset): %ld bytes (Sector: %ld)\n", \
        fs_offset, fs_offset / SECTOR_SIZE);
    
    fprintf(stderr, "\nSuperblk Contents:\n");
    fprintf(stderr, "  ninodes:    %u\n", curr_sb.ninodes);
    fprintf(stderr, "  i_blks:    %d\n", curr_sb.i_blks);
    fprintf(stderr, "  z_blks:    %d\n", curr_sb.z_blks);
    fprintf(stderr, "  firstdata:   %u\n", curr_sb.firstdata);
    fprintf(stderr, "  log_zone_size: %d (zone size: %u)\n", \
        curr_sb.log_zone_size, zone_size);
    fprintf(stderr, "  max_file:    %u\n", curr_sb.max_file);
    fprintf(stderr, "  zones:     %u\n", curr_sb.zones);
    fprintf(stderr, "  magic:     0x%x\n", curr_sb.magic);
    fprintf(stderr, "  blksize:   %u\n", curr_sb.blksize);
    fprintf(stderr, "  subversion:   %u\n", curr_sb.subversion);
    fprintf(stderr, "==================================\n");
}

/**
* Prints Inode data to stderr for -v flag.
*/
void print_verbose_inode(uint32_t inode_num, const minix_inode_t *inode) {
    char perm_str[11];
    int i;
    get_permissions_string(inode->mode, perm_str);
    
    fprintf(stderr, "\nFile inode #%u:\n", inode_num);
    fprintf(stderr, "  mode:      0x%x (%s)\n", inode->mode, perm_str);
    fprintf(stderr, "  links:     %u\n", inode->links);
    fprintf(stderr, "  uid:      %u\n", inode->uid);
    fprintf(stderr, "  gid:      %u\n", inode->gid);
    fprintf(stderr, "  size:      %u\n", inode->size);
    fprintf(stderr, "  atime:     %u ~~~%s", inode->atime, \
        ctime((time_t *)&inode->atime));
    fprintf(stderr, "  mtime:     %u ~~~%s", inode->mtime, \
        ctime((time_t *)&inode->mtime));
    fprintf(stderr, "  ctime:     %u ~~~%s", inode->ctime, \
        ctime((time_t *)&inode->ctime));

    fprintf(stderr, "  Direct zones:\n");
    for (i = 0; i < DIRECT_ZONES; i++) {
        fprintf(stderr, "   zone[%d] = %u\n", i, inode->zone[i]);
    }
    fprintf(stderr, "  indir:    %u\n", inode->indirect);
    fprintf(stderr, "  two_indir:  %u\n", inode->two_indirect);
}
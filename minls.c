#include "fs_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

// Global State from fs_util.c (needed for blocksize, zone_size, etc.)
extern uint32_t zone_size;
extern minix_superblock_t current_sb;
extern int is_verbose;

// Function prototypes
void print_usage(const char *progname);
void list_single_entry(uint32_t entry_inode_num, const char *name);
int list_directory_contents(uint32_t dir_inode_num, const char *dir_path);


/**
 * Prints the usage message for minls.
 */
void print_usage(const char *progname) {
    fprintf(stderr, \
    "usage: %s [-v] [-p part [-s subpart]] imagefile [path]\n", progname);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, \
    "  -p <num>   select primary partition for filesystem (default: none)\n");
    fprintf(stderr, \
    "  -s <num>   select subpartition for filesystem (default: none)\n");
    fprintf(stderr, "  -v         verbose. Print partition table(s), \
    superblock, and source inode to stderr.\n");
    fprintf(stderr, "  -h         print usage information and exit\n");
}

/**
 * Lists the information for a single file or directory entry.
 * This is used for listing the target file itself (if it's not a directory),
 * or for each entry inside a directory.
 */
void list_single_entry(uint32_t entry_inode_num, const char *name) {
    minix_inode_t entry_inode;
    char perm_str[11];

    if (read_inode(entry_inode_num, &entry_inode) != 0) {
        fprintf(stderr, "Error: Could not read inode %u for entry %s.\n", \
            entry_inode_num, name);
        return;
    }

    // Get the formatted permissions string
    get_permissions_string(entry_inode.mode, perm_str);

    // Output format: [permissions] [size] [filename]
    // The size field must be right-justified to 9 bits, 
    // with a space on either side.
    printf("%s %9u %s\n", perm_str, entry_inode.size, name);
}

/**
 * Iterates through the blocks of a directory inode and prints the contents.
 * Returns 0 on success, -1 on failure.
 */
int list_directory_contents(uint32_t dir_inode_num, const char *dir_path) {
    minix_inode_t dir_inode;
    if (read_inode(dir_inode_num, &dir_inode) != 0) {
        fprintf(stderr, "minls: Failed to read directory inode %u.\n", \
            dir_inode_num);
        return -1;
    }

    printf("%s:\n", dir_path);
    
    // Check if it's actually a directory
    if ((dir_inode.mode & 0170000) != 0040000) { 
        fprintf(stderr, "minls: %s is not a directory.\n", dir_path);
        return -1;
    }

    // Loop through all blocks of the directory file
    for (uint32_t i = 0; i * current_sb.blocksize < dir_inode.size; i++) {
        uint32_t disk_block = get_file_block(&dir_inode, i);
        if (disk_block == 0) continue; // Skip file holes

        off_t block_offset = (off_t)disk_block * current_sb.blocksize;

        // Read block content into a buffer for directory entries
        uint8_t dir_block_buf[current_sb.blocksize];
        if (read_fs_bytes(block_offset, dir_block_buf, \
            current_sb.blocksize) != 0) {
            fprintf(stderr, \
            "minls: Error reading directory data block %u.\n", disk_block);
            continue;
        }

        // Loop through directory entries in the block
        uint32_t entries_per_block = current_sb.blocksize / DIR_ENTRY_SIZE;
        for (uint32_t j = 0; j < entries_per_block; j++) {
            // Cast the buffer section to the entry structure
            minix_dir_entry_t *entry = \
            (minix_dir_entry_t *)(dir_block_buf + j * DIR_ENTRY_SIZE);

            if (entry->inode == 0) continue; // Skip deleted/invalid entries

// Directory entry names are 60 bytes, often not null-terminated perfectly
// Copy the name to ensure it is null-terminated before 
// passing to list_single_entry
            char entry_name[61];
            strncpy(entry_name, (char*)entry->name, 60);
            entry_name[60] = '\0';

            list_single_entry(entry->inode, entry_name);
        }
    }

    return 0;
}

/**
 * Main function for the minls utility.
 */
int main(int argc, char *argv[]) {
    int p_num = -1, s_num = -1, verbose_flag = 0;
    char *image_file = NULL;
    char *src_path = "/"; // Default path to root directory
    int opt;

    // --- 1. Parse Arguments ---
    while ((opt = getopt(argc, argv, "p:s:vh")) != -1) {
        switch (opt) {
            case 'p':
                p_num = atoi(optarg);
                break;
            case 's':
                s_num = atoi(optarg);
                break;
            case 'v':
                verbose_flag = 1;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    // Check for required argument: imagefile
    if (argc - optind < 1) {
        fprintf(stderr, "Error: Missing required argument (imagefile).\n");
        print_usage(argv[0]);
        return 1;
    }

    image_file = argv[optind++];
    
    // Optional argument: path
    if (argc - optind >= 1) {
        src_path = argv[optind];
    }
    
    // --- 2. Filesystem Initialization ---
    if (init_filesystem(image_file, p_num, s_num, verbose_flag) != 0) {
        cleanup_filesystem();
        return 1;
    }

    // --- 3. Canonicalize Path and Find Inode ---
    char *canonical_src_path = canonicalize_path(src_path);
    if (!canonical_src_path) {
        fprintf(stderr, "Error: Failed to canonicalize path: %s\n", src_path);
        cleanup_filesystem();
        return 1;
    }
    
    uint32_t src_inode_num = get_inode_by_path(canonical_src_path);
    if (src_inode_num == 0) {
        fprintf(stderr, "minls: Can't find %s\n", canonical_src_path);
        free(canonical_src_path);
        cleanup_filesystem();
        return 1;
    }

    // --- 4. Read Inode and Check File Type ---
    minix_inode_t src_inode;
    if (read_inode(src_inode_num, &src_inode) != 0) {
        fprintf(stderr, "minls: Failed to read inode %u.\n", src_inode_num);
        free(canonical_src_path);
        cleanup_filesystem();
        return 1;
    }

    if (verbose_flag) {
        print_verbose_inode(src_inode_num, &src_inode);
    }
    
    // --- 5. List Contents or Single File ---
    int status = 0;
    
    // Check if it's a directory (0040000 mask)
    if ((src_inode.mode & 0170000) == 0040000) {
        // List the contents of the directory
        status = list_directory_contents(src_inode_num, canonical_src_path);
    } else {
        // List the single file or non-directory item itself
        char *filename = canonical_src_path;
// Strip path to get just the filename for display, unless it's the root
        if (strcmp(filename, "/") != 0) {
            char *last_slash = strrchr(filename, '/');
            if (last_slash) {
                filename = last_slash + 1;
            }
        } else {
             // For the root directory listing itself as a file, use "."
             filename = ".";
        }
        
// Use a simple version of the list_single_entry logic, passing the basename
        list_single_entry(src_inode_num, filename);
    }

    // --- 6. Cleanup ---
    free(canonical_src_path);
    cleanup_filesystem();

    return (status == 0) ? 0 : 1;
}
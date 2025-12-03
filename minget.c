#include "fs_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <getopt.h>

// Function prototypes
void print_usage(const char *progname);
int copy_file_data(const minix_inode_t *inode, FILE *dest_fp);

// Global State from fs_util.c (needed for blocksize, zone_size, etc.)
extern uint32_t zone_size;
extern minix_superblock_t current_sb;

/**
 * Prints the usage message for minget.
 */
void print_usage(const char *progname) {
    fprintf(stderr, "usage: %s [-v] [-p part [-s subpart]] \
    imagefile srcpath [dstpath]\n", progname);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -p <num>   select primary partition \
    for filesystem (default: none)\n");
    fprintf(stderr, "  -s <num>   select subpartition for \
    filesystem (default: none)\n");
    fprintf(stderr, "  -v         verbose. Print partition \
    table(s), superblock, and source inode to stderr.\n");
    fprintf(stderr, "  -h         print usage information and exit\n");
}

/**
 * Copies the contents of the file described by the inode to the 
 * destination file pointer. Handles block translation, file size, 
 * and potential holes (zone 0).
 * Returns 0 on success, -1 on failure.
 */
int copy_file_data(const minix_inode_t *inode, FILE *dest_fp) {
    // The total file size determines how many bytes we need to copy
    uint32_t remaining_size = inode->size;
    uint32_t current_logical_block = 0;
    
    // Allocate a buffer the size of a block for efficient I/O
    uint8_t *block_buf = (uint8_t *)malloc(current_sb.blocksize);
    if (!block_buf) {
        perror("Error allocating buffer");
        return -1;
    }

    if (is_verbose) {
        fprintf(stderr,
            "Starting copy. File size: %u bytes. Block size: %u.\n", 
            remaining_size, current_sb.blocksize);
    }
    
    // Loop until all bytes are copied
    while (remaining_size > 0) {
        // 1. Get the disk block number for the current logical block
        uint32_t disk_block_num = \
        get_file_block(inode, current_logical_block);

        // Calculate how many bytes to read/write in this iteration
        uint32_t bytes_to_copy = current_sb.blocksize;
        if (bytes_to_copy > remaining_size) {
            bytes_to_copy = remaining_size;
        }

        if (disk_block_num == 0) {
            // Zone 0 indicates a file hole: skip reading, write zeros.
            if (is_verbose) {
                fprintf(stderr, 
                    "  [LBlock %u] Hole found. Writing %u zeros.\n",
                    current_logical_block, bytes_to_copy);
            }
            
            // Set the buffer to zeros for the hole's content
            memset(block_buf, 0, bytes_to_copy);
            
            // Write the zeroed data to the destination
            if (fwrite(block_buf,1, bytes_to_copy, dest_fp) != bytes_to_copy){
                perror("Error writing zero data for file hole");
                free(block_buf);
                return -1;
            }
        } else {
            // Normal data block: read from disk and write to destination.
            
            // Calculate the disk offset (relative to FS start)
            off_t disk_offset = (off_t)disk_block_num * current_sb.blocksize;
            
            if (is_verbose) {
                fprintf(stderr, \
            "  [LBlock %u] Disk Block %u (Offset %ld). Copying %u bytes.\n",
                    current_logical_block, disk_block_num,
                    fs_offset + disk_offset, bytes_to_copy);
            }
            
            // Read the block from the disk image
            if (read_fs_bytes(disk_offset, block_buf, \
                current_sb.blocksize) != 0) {
                fprintf(stderr, "Error reading data block %u from image.\n", \
                    disk_block_num);
                free(block_buf);
                return -1;
            }

            // Write the data to the destination
            if (fwrite(block_buf, 1, bytes_to_copy, \
                dest_fp) != bytes_to_copy) {
                perror("Error writing file data to destination");
                free(block_buf);
                return -1;
            }
        }

        // Update loop variables
        remaining_size -= bytes_to_copy;
        current_logical_block++;
    }

    free(block_buf);
    return 0;
}

/**
 * Main function for the minget utility.
 */
int main(int argc, char *argv[]) {
    int p_num = -1, s_num = -1, verbose_flag = 0;
    char *image_file = NULL;
    char *src_path = NULL;
    char *dst_path = NULL;
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

    // Check for required arguments: imagefile and srcpath
    if (argc - optind < 2) {
        fprintf(stderr, "Error: Missing required arguments \
            (imagefile, srcpath).\n");
        print_usage(argv[0]);
        return 1;
    }

    image_file = argv[optind++];
    src_path = argv[optind++];
    
    // Optional argument: dstpath
    if (argc - optind >= 1) {
        dst_path = argv[optind];
    }

    // --- 2. Filesystem Initialization ---
    if (init_filesystem(image_file, p_num, s_num, verbose_flag) != 0) {
        cleanup_filesystem();
        return 1;
    }

    // --- 3. Canonicalize Path and Find Inode ---
    char *canonical_src_path = canonicalize_path(src_path);
    if (!canonical_src_path) {
        fprintf(stderr, "Error: Failed to canonicalize path: %s\n",src_path);
        cleanup_filesystem();
        return 1;
    }
    
    uint32_t src_inode_num = get_inode_by_path(canonical_src_path);
    if (src_inode_num == 0) {
        fprintf(stderr, "minget: Can't find %s\n", canonical_src_path);
        free(canonical_src_path);
        cleanup_filesystem();
        return 1;
    }

    // --- 4. Read Inode and Check File Type ---
    minix_inode_t src_inode;
    if (read_inode(src_inode_num, &src_inode) != 0) {
        fprintf(stderr, "minget: Failed to read inode %u.\n", src_inode_num);
        free(canonical_src_path);
        cleanup_filesystem();
        return 1;
    }

    // Check if it's a regular file (0100000 mask)
    if ((src_inode.mode & 0170000) != 0100000) {
        fprintf(stderr, \
        "minget: %s is not a regular file.\n", canonical_src_path);
        free(canonical_src_path);
        cleanup_filesystem();
        return 1;
    }

    if (verbose_flag) {
        print_verbose_inode(src_inode_num, &src_inode);
    }
    
    // --- 5. Open Destination ---
    FILE *dest_fp = stdout; // Default to stdout
    int file_descriptor = -1;

    if (dst_path) {
// Open file for writing, create if it doesn't exist, truncate if it does.
        file_descriptor = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (file_descriptor < 0) {
            perror("Error opening destination file");
            free(canonical_src_path);
            cleanup_filesystem();
            return 1;
        }
        dest_fp = fdopen(file_descriptor, "w");
        if (!dest_fp) {
            perror("Error associating file descriptor with stream");
            close(file_descriptor);
            free(canonical_src_path);
            cleanup_filesystem();
            return 1;
        }
    }
    
    // --- 6. Copy Data ---
    int copy_status = copy_file_data(&src_inode, dest_fp);

    // --- 7. Cleanup ---
    if (dst_path) {
        fclose(dest_fp);
    }
    free(canonical_src_path);
    cleanup_filesystem();

    return (copy_status == 0) ? 0 : 1;
}
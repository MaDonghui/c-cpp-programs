#ifndef SFS_H
#define SFS_H

#include <stdint.h>

/*
 * This file defines all data structures and other information about the SFS
 * filesystem. Instead of hardcoding any sizes or offset, use the constants of
 * this file.
 *
 * The overall filesystem has the following layout:
 * +------------------------+
 * | Magic numbers          |
 * |  16 bytes              |
 * +------------------------+
 * | Root directory entries |
 * |  4096 bytes            |
 * +------------------------+
 * | Block table            |
 * |  32 Kbyte              |
 * +------------------------+
 * | Data area              |
 * |  remainder (~8 Mbyte)  |
 * +------------------------+
 *
 *
 * Note about the defines below:
 *  - The _SIZE values represent the size in bytes for each area in the
 *    filesystem.
 *  - The _OFF values represent the offset in bytes from the start of the
 *    partition (image).
 *  - The _NENTRIES values represent the logical number of entries in that area
 *    (e.g., number of directory entries, number of blockidx values).
 */

#define SFS_MAGIC_SIZE 16u

#define SFS_ROOTDIR_NENTRIES  64u
#define SFS_ROOTDIR_SIZE      (sizeof(struct sfs_entry) * SFS_ROOTDIR_NENTRIES)
#define SFS_ROOTDIR_OFF       SFS_MAGIC_SIZE

#define SFS_BLOCKTBL_NENTRIES 0x4000
#define SFS_BLOCKTBL_SIZE     (sizeof(blockidx_t) * SFS_BLOCKTBL_NENTRIES)
#define SFS_BLOCKTBL_OFF      (SFS_ROOTDIR_OFF + SFS_ROOTDIR_SIZE)

#define SFS_DATA_OFF          (SFS_BLOCKTBL_OFF + SFS_BLOCKTBL_SIZE)

/* "Normal" (sub)directories (everything except rootdir) */
#define SFS_DIR_NENTRIES      16u
#define SFS_DIR_SIZE         (SFS_DIR_NENTRIES * sizeof(struct sfs_entry))

#define SFS_BLOCK_SIZE      512u

/* Special blockidx values (that may not be used normally) */
#define SFS_BLOCKIDX_EMPTY  0x0     /* Block unused */
#define SFS_BLOCKIDX_END    0xfffe  /* End of chain */

/* Bitsmasks in the size field of directory entries. */
#define SFS_SIZEMASK        ((1u << 28) - 1) /* Mask away top 4 bits (flags) */
#define SFS_DIRECTORY       (1u << 31)

#define SFS_FILENAME_MAX    58u

__attribute__((used))
static const char sfs_magic[SFS_MAGIC_SIZE] = "**VUOS SFS IMG**";

/* Special type for blockindices, which explicitly indicate in your code that
 * you're dealing with special blockidx values (e.g., that
 * SFS_BLOCKIDX_{EMPTY,END} may occur and that you will need to subtract 1 to
 * obtain the block index within the data area. */
typedef uint16_t blockidx_t;

/* Directory entry (in rootdir or subdir), referring to a file or subdir. */
struct sfs_entry {
    char filename[SFS_FILENAME_MAX];
    blockidx_t first_block;
    uint32_t size;
} __attribute__((__packed__));

#endif

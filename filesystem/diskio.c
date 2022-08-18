#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>

#include "diskio.h"
#include "sfs.h"


const size_t disk_size = SFS_DATA_OFF + SFS_BLOCKTBL_NENTRIES * SFS_BLOCK_SIZE;

static int img_fd = -1;


void disk_open_image(const char *filename)
{
    if (img_fd != -1) {
        fprintf(stderr, "Opening disk image when one is already open.\n");
        exit(1);
    }
    img_fd = open(filename, O_RDWR);

    if (img_fd == -1) {
        perror("Could not open disk image");
        exit(1);
    }

    disk_verify_magic();
}


void disk_read(void *buf, size_t size, off_t offset)
{
    ssize_t ret;

    ret = pread(img_fd, buf, size, offset);
    if (ret == -1) {
        perror("Error reading from disk");
        exit(1);
    }

    if ((size_t)ret != size) {
        fprintf(stderr, "Could not read %zu bytes from disk, only got %zd\n",
                size, ret);
        exit(1);
    }
}


void disk_write(const void *buf, size_t size, off_t offset)
{
    ssize_t ret;

    assert(offset >= 0);
    if ((size_t)offset >= disk_size) {
        fprintf(stderr, "Error: write to disk outside of range of addressable "
                "blocks: offset=%#lx size=%zu\n", offset, size);
        assert((size_t)offset < disk_size);
    }

    ret = pwrite(img_fd, buf, size, offset);
    if (ret == -1) {
        perror("Error writing to disk");
        exit(1);
    }

    if ((size_t)ret != size) {
        fprintf(stderr, "Could not write %zu bytes to disk, only wrote %zd\n",
                size, ret);
        exit(1);
    }
}

void disk_verify_magic(void)
{
    char buf[SFS_MAGIC_SIZE];
    disk_read(buf, sizeof(buf), 0);
    if (memcmp(buf, sfs_magic, SFS_MAGIC_SIZE)) {
        fprintf(stderr, "Invalid signature '%.*s', expected '%.*s'\n",
                SFS_MAGIC_SIZE, buf, SFS_MAGIC_SIZE, sfs_magic);
        exit(1);
    }
}

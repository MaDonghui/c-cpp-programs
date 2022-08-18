#ifndef DISKIO_H
#define DISKIO_H

/* Open a disk image for future disk operations. */
void disk_open_image(const char *filename);

/* Read `size` bytes from address `offset` of the disk, into `buf`. */
void disk_read(void *buf, size_t size, off_t offset);

/* Write `size` bytes from `buf` to disk at address `offset`. */
void disk_write(const void *buf, size_t size, off_t offset);

/* Verify this is an SFS partitiion by checking the magic bytes at the start. */
void disk_verify_magic(void);

#endif

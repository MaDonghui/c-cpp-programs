#define FUSE_USE_VERSION 26

#include <errno.h>
#include <fuse.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <stdbool.h>

#include "sfs.h"
#include "diskio.h"


static const char default_img[] = "test.img";

/* Options passed from commandline arguments */
struct options {
    const char *img;
    int background;
    int verbose;
    int show_help;
    int show_fuse_help;
} options;


#define log(fmt, ...) \
    do { \
        if (options.verbose) \
            printf(" # " fmt, ##__VA_ARGS__); \
    } while (0)


/* libfuse2 leaks, so let's shush LeakSanitizer if we are using Asan. */
const char *__asan_default_options() { return "detect_leaks=0"; }

int get_entry(const char *path, struct sfs_entry *result) {
    struct sfs_entry buffer = {};

    char *path_copy = strdup(path);
    char *token = strtok(path_copy, "/");
    if (strlen(token) >= SFS_FILENAME_MAX) {
        printf("Error: endpoint name too long: %s", token);
        return -ENAMETOOLONG;
    }

    // root searching
    struct sfs_entry root_entries[SFS_ROOTDIR_NENTRIES];
    disk_read(root_entries, SFS_ROOTDIR_SIZE, SFS_ROOTDIR_OFF);

    bool hit = false;
    for (unsigned i = 0; i < SFS_ROOTDIR_NENTRIES; i++) {
        if (strcmp(root_entries[i].filename, token) == 0) {
            hit = true;

            token = strtok(NULL, "/");
            if (token == NULL) {
                // done
                *result = root_entries[i];
                return 0;
            } else {
                // more searching
                buffer = root_entries[i];
                break;
            }
        }
    }
    if (!hit) {
        return -ENOENT;
    }

    // sub dir searching
    while (token != NULL) {
        if (strlen(token) > SFS_FILENAME_MAX) {
            // name issue
            printf("Error: endpoint name too long: %s", token);
            return -ENAMETOOLONG;
        }

        struct sfs_entry sub_entries[SFS_DIR_NENTRIES];
        disk_read(sub_entries, SFS_DIR_SIZE, SFS_DATA_OFF + SFS_BLOCK_SIZE * (buffer.first_block - 1));

        hit = false;
        for (unsigned int i = 0; i < SFS_DIR_NENTRIES; ++i) {
            if (strcmp(sub_entries[i].filename, token) == 0) {
                hit = true;

                token = strtok(NULL, "/");
                if (token == NULL) {
                    // done
                    *result = sub_entries[i];
                    return 0;   // index of target entry in parent table
                } else {
                    //
                    hit = true;
                    buffer = sub_entries[i];
                    break;
                }
            }
        }

        if (!hit) {
            //printf("No such file or directory: %s\n", token);
            return -ENOENT;
        }
    }

    log("entry found on %p", result);   // honestly do nothing, just to kill the warning
    return -ENOENT;
}

char *get_path_name(const char *path) {
    char *result = NULL;
    char *buffer = strdup(path);

    char *token = strtok(buffer, "/");
    while (token != NULL) {
        result = token;
        token = strtok(NULL, "/");
    }

    return result;
}

int get_parent_info(const char *path, size_t *parent_size, off_t *parent_offset) {
    char path_copy[256];
    strcpy(path_copy, path);
    char *this_token = strtok(path_copy, "/");
    char *next_token = strtok(NULL, "/");

    // parent is root
    if (next_token == NULL) {
        *parent_size = SFS_ROOTDIR_SIZE;
        *parent_offset = SFS_ROOTDIR_OFF;
        return 0;
    }

    // parent is a subdir
    char parent_path[256] = "";
    while (next_token != NULL) {
        strcat(parent_path, "/");
        strcat(parent_path, this_token);
        this_token = next_token;
        next_token = strtok(NULL, "/");
    }

    struct sfs_entry parent_entry;
    unsigned int result = get_entry(parent_path, &parent_entry);
    if (result != 0) {
        return result;
    } else {
        *parent_size = SFS_DIR_SIZE;
        *parent_offset = SFS_DATA_OFF + SFS_BLOCK_SIZE * (parent_entry.first_block - 1);
    }

    return 0;
}

blockidx_t alloc_dir_blocks() {
    blockidx_t block_table[SFS_BLOCKTBL_NENTRIES];
    disk_read(block_table, SFS_BLOCKTBL_SIZE, SFS_BLOCKTBL_OFF);

    blockidx_t index = 0;
    bool hit = false;

    while (index < SFS_BLOCKTBL_NENTRIES - 1) {
        if (block_table[index] != SFS_BLOCKIDX_EMPTY || block_table[index + 1] != SFS_BLOCKIDX_EMPTY) {
            index++;
            continue;
        } else {
            hit = true;
            break;
        }
    }

    if (hit) {
        blockidx_t value_1 = index + 2;
        blockidx_t value_2 = SFS_BLOCKIDX_END;

        // write block_table
        disk_write(&value_1, sizeof(blockidx_t), SFS_BLOCKTBL_OFF + sizeof(blockidx_t) * index);
        disk_write(&value_2, sizeof(blockidx_t), SFS_BLOCKTBL_OFF + sizeof(blockidx_t) * (index + 1));

        // write empty entry in data
        struct sfs_entry empty_entries[SFS_DIR_NENTRIES];
        memset(empty_entries, 0, SFS_DIR_SIZE);
        disk_write(empty_entries, SFS_DIR_SIZE, SFS_DATA_OFF + SFS_BLOCK_SIZE * index);

        return index + 1;
    } else {
        return -1;
    }
}

// trash
//char *get_parent_path(char *path) {
//    char *result;
//
//    char temp[256];
//    strcpy(temp, path);
//
//    char *this_token = strtok(temp, "/");
//    char *next_token = strtok(NULL, "/");
//
//    while (next_token != NULL) {
//        strcat(result, "/");
//        strcat(result, this_token);
//        this_token = next_token;
//        next_token = strtok(NULL, "/");
//    }
//
//    return result;
//}

/*
 * Retrieve information about a file or directory.
 * You should populate fields of `stbuf` with appropriate information if the
 * file exists and is accessible, or return an error otherwise.
 *
 * For directories, you should at least set st_mode (with S_IFDIR) and st_nlink.
 * For files, you should at least set st_mode (with S_IFREG), st_nlink and
 * st_size.
 *
 * Return 0 on success, < 0 on error.
 */
static int sfs_getattr(const char *path,
                       struct stat *st) {
    int res = 0;

    log("getattr %s\n", path);

    memset(st, 0, sizeof(struct stat));
    /* Set owner to user/group who mounted the image */
    st->st_uid = getuid();
    st->st_gid = getgid();
    /* Last accessed/modified just now */
    st->st_atime = time(NULL);
    st->st_mtime = time(NULL);

    if (strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
    } else {
        struct sfs_entry entry;
        int result = get_entry(path, &entry);

        if (result == -ENAMETOOLONG) {
            log("Error: name too long");
            return -ENAMETOOLONG;
        };
        if (result == -ENOENT) {
            log("Error: file or directory not found\n");
            return -ENOENT;
        };

        // read target entry
        if (entry.size & SFS_DIRECTORY) {
            st->st_mode = S_IFDIR | 0755;
            st->st_nlink = 2;
            res = 0;
        } else {
            st->st_mode = S_IFREG | 0755;
            st->st_nlink = 1;
            st->st_size = entry.size & SFS_SIZEMASK;
            res = 0;
        }
    }

    return res;
}


/*
 * Return directory contents for `path`. This function should simply fill the
 * filenames - any additional information (e.g., whether something is a file or
 * directory) is later retrieved through getattr calls.
 * Use the function `filler` to add an entry to the directory. Use it like:
 *  filler(buf, <dirname>, NULL, 0);
 * Return 0 on success, < 0 on error.
 */

static int sfs_readdir(const char *path,
                       void *buf,
                       fuse_fill_dir_t filler,
                       off_t offset,
                       struct fuse_file_info *fi) {
    (void) offset, (void) fi;
    log("readdir %s\n", path);

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    if (strcmp(path, "/") == 0) {
        struct sfs_entry entries[SFS_ROOTDIR_NENTRIES];
        disk_read(entries, SFS_ROOTDIR_SIZE, SFS_ROOTDIR_OFF);

        for (unsigned i = 0; i < SFS_ROOTDIR_NENTRIES; i++) {
            if (strlen(entries[i].filename) > 0) {
                filler(buf, entries[i].filename, NULL, 0);
            }
        }
    } else {
        struct sfs_entry entry;
        int result = get_entry(path, &entry);

        if (result != 0) return result;

        struct sfs_entry entries[SFS_DIR_NENTRIES];
        disk_read(entries, SFS_DIR_SIZE, SFS_DATA_OFF + SFS_BLOCK_SIZE * (entry.first_block - 1));

        for (unsigned i = 0; i < SFS_DIR_NENTRIES; i++) {
            if (strlen(entries[i].filename) > 0) {
                filler(buf, entries[i].filename, NULL, 0);
            }
        }
    }

    return 0;
}


/*
 * Read contents of `path` into `buf` for  up to `size` bytes.
 * Note that `size` may be bigger than the file actually is.
 * Reading should start at offset `offset`; the OS will generally read your file
 * in chunks of 4K byte.
 * Returns the number of bytes read (writting into `buf`), or < 0 on error.
 */
static int sfs_read(const char *path,
                    char *buf,
                    size_t size,
                    off_t offset,
                    struct fuse_file_info *fi) {
    (void) fi;
    log("read %s size=%zu offset=%ld\n", path, size, offset);

    // get file entry
    struct sfs_entry file_entry;
    int result = get_entry(path, &file_entry);
    if (result != 0) return result;
    if (file_entry.size & SFS_DIRECTORY) return -EISDIR;

    // get block table
    blockidx_t block_table[SFS_BLOCKTBL_NENTRIES];
    disk_read(block_table, SFS_BLOCKTBL_SIZE, SFS_BLOCKTBL_OFF);

    // initial
    blockidx_t this_block = file_entry.first_block - 1;

    size_t position = 0;
    off_t remain_offset = offset;

    // ignore blocks
    unsigned int ignore_blocks_num = remain_offset / SFS_BLOCK_SIZE;
    for (; ignore_blocks_num > 0; ignore_blocks_num--) {
        this_block = block_table[this_block] - 1;
        if (this_block == 0xfffd) this_block++;

        remain_offset -= SFS_BLOCK_SIZE;
    }
    // ignore left overs
    if (remain_offset > 0) {
        disk_read(buf + position,
                  SFS_BLOCK_SIZE - remain_offset,
                  SFS_DATA_OFF + this_block * SFS_BLOCK_SIZE + remain_offset);

        position = SFS_BLOCK_SIZE - remain_offset;
        remain_offset = 0;

        this_block = block_table[this_block] - 1;
        if (this_block == 0xfffd) this_block++;
    }

    while (this_block != SFS_BLOCKIDX_END) {
        disk_read(buf + position,
                  SFS_BLOCK_SIZE,
                  SFS_DATA_OFF + this_block * SFS_BLOCK_SIZE);

        position += SFS_BLOCK_SIZE;

        this_block = block_table[this_block] - 1;
        if (this_block == 0xfffd) this_block++;
    }

    return size;
}


/*
 * Create directory at `path`.
 * The `mode` argument describes the permissions, which you may ignore for this
 * assignment.
 * Returns 0 on success, < 0 on error.
 */
static int sfs_mkdir(const char *path,
                     mode_t mode) {
    log("mkdir %s mode=%o\n", path, mode);

    struct sfs_entry target;
    int result = get_entry(path, &target);

    // check existence
    if (result == 0) return -EEXIST;
    if (result == -ENAMETOOLONG) return -ENAMETOOLONG;

    // alloc 2 data block
    blockidx_t first_block = alloc_dir_blocks();


    // get parent and find free entry slot
    size_t parent_size;
    off_t parent_offset;
    get_parent_info(path, &parent_size, &parent_offset);
    struct sfs_entry parent_entries[SFS_ROOTDIR_NENTRIES];
    disk_read(parent_entries, SFS_ROOTDIR_SIZE, parent_offset);

    unsigned int entries_num = 0;
    if (parent_size == SFS_ROOTDIR_SIZE) {
        entries_num = SFS_ROOTDIR_NENTRIES;
    } else {
        entries_num = SFS_DIR_NENTRIES;
    }
    unsigned int index = NULL;
    for (unsigned i = 0; i < entries_num; i++) {
        if (parent_entries[i] == SFS_BLOCKIDX_EMPTY) {
            index = i;
            break;
        }
    }
    if(index == NULL) return -1;    // all full

    return 0;
}


/*
 * Remove directory at `path`.
 * Directories may only be removed if they are empty, otherwise this function
 * should return -ENOTEMPTY.
 * Returns 0 on success, < 0 on error.
 */
static int sfs_rmdir(const char *path) {
    log("rmdir %s\n", path);

    struct sfs_entry target;
    int result = get_entry(path, &target);
    if (result != 0) {
        return result;
    }

    struct sfs_entry target_entries[SFS_DIR_NENTRIES];
    disk_read(target_entries, SFS_DIR_SIZE, SFS_DATA_OFF + SFS_BLOCK_SIZE * (target.first_block - 1));
    for (unsigned i = 0; i < SFS_DIR_NENTRIES; ++i) {
        if (target_entries[i].first_block != SFS_BLOCKIDX_EMPTY) {
            return -ENOTEMPTY;
        }
    }

    // unlink from parent
    size_t parent_size;
    off_t parent_offset;
    get_parent_info(path, &parent_size, &parent_offset);

//    if(parent_size == SFS_ROOTDIR_SIZE) {
//        struct sfs_entry parent_entries[SFS_ROOTDIR_NENTRIES];
//        disk_read(parent_entries, SFS_ROOTDIR_SIZE, parent_offset);
//    } else if (parent_size == SFS_DIR_SIZE) {
//        struct sfs_entry parent_entries[SFS_DIR_NENTRIES];
//        disk_read(parent_entries, SFS_DIR_SIZE, parent_offset);
//    }

    struct sfs_entry parent_entries[SFS_ROOTDIR_NENTRIES];
    disk_read(parent_entries, SFS_ROOTDIR_SIZE, parent_offset);
    unsigned int entries_num = 0;
    if (parent_size == SFS_ROOTDIR_SIZE) {
        entries_num = SFS_ROOTDIR_NENTRIES;
    } else {
        entries_num = SFS_DIR_NENTRIES;
    }

    char *name = get_path_name(path);
    for (unsigned i = 0; i < entries_num; i++) {
        if (strcmp(name, parent_entries[i].filename) == 0) {
            struct sfs_entry empty_entry;
            memset(&empty_entry, 0, sizeof(struct sfs_entry));

            parent_entries[i] = empty_entry;
            break;
        }
    }   // parent_entries is now ready
    disk_write(parent_entries, parent_size, parent_offset);

    // unlink from block table
    blockidx_t block_table[SFS_BLOCKTBL_NENTRIES];
    disk_read(block_table, SFS_BLOCKTBL_SIZE, SFS_BLOCKTBL_OFF);
    blockidx_t first_block = target.first_block - 1;

    block_table[first_block] = SFS_BLOCKIDX_EMPTY;
    block_table[first_block + 1] = SFS_BLOCKIDX_EMPTY;
    disk_write(block_table, SFS_BLOCKTBL_SIZE, SFS_BLOCKTBL_OFF);

    return 0;
}


/*
 * Remove file at `path`.
 * Can not be used to remove directories.
 * Returns 0 on success, < 0 on error.
 */
static int sfs_unlink(const char *path) {
    log("unlink %s\n", path);

    // find if exists
    struct sfs_entry target;
    int result = get_entry(path, &target);
    if (result != 0) {
        return result;
    }

    // unlink from parent
    size_t parent_size;
    off_t parent_offset;
    get_parent_info(path, &parent_size, &parent_offset);

    struct sfs_entry parent_entries[SFS_ROOTDIR_NENTRIES];
    disk_read(parent_entries, SFS_ROOTDIR_SIZE, parent_offset);
    unsigned int entries_num = 0;
    if (parent_size == SFS_ROOTDIR_SIZE) {
        entries_num = SFS_ROOTDIR_NENTRIES;
    } else {
        entries_num = SFS_DIR_NENTRIES;
    }

    char *name = get_path_name(path);
    for (unsigned i = 0; i < entries_num; i++) {
        if (strcmp(name, parent_entries[i].filename) == 0) {
            struct sfs_entry empty_entry;
            memset(&empty_entry, 0, sizeof(struct sfs_entry));

            parent_entries[i] = empty_entry;
            break;
        }
    }   // parent_entries is now ready
    disk_write(parent_entries, parent_size, parent_offset);

    // unlink from block table
    blockidx_t block_table[SFS_BLOCKTBL_NENTRIES];
    disk_read(block_table, SFS_BLOCKTBL_SIZE, SFS_BLOCKTBL_OFF);

    blockidx_t first_block = target.first_block;
    blockidx_t prev_block;

    while (first_block != SFS_BLOCKIDX_END) {
        prev_block = first_block - 1;
        first_block = block_table[prev_block];
        block_table[prev_block] = SFS_BLOCKIDX_EMPTY;
    }
    disk_write(block_table, SFS_BLOCKTBL_SIZE, SFS_BLOCKTBL_OFF);


    return 0;
}


/*
 * Create an empty file at `path`.
 * The `mode` argument describes the permissions, which you may ignore for this
 * assignment.
 * Returns 0 on success, < 0 on error.
 */
static int sfs_create(const char *path,
                      mode_t mode,
                      struct fuse_file_info *fi) {
    (void) fi;
    log("create %s mode=%o\n", path, mode);

    return -ENOSYS;
}


/*
 * Shrink or grow the file at `path` to `size` bytes.
 * Excess bytes are thrown away, whereas any bytes added in the process should
 * be nil (\0).
 * Returns 0 on success, < 0 on error.
 */
static int sfs_truncate(const char *path, off_t size) {
    log("truncate %s size=%ld\n", path, size);

    return -ENOSYS;
}


/*
 * Write contents of `buf` (of `size` bytes) to the file at `path`.
 * The file is grown if nessecary, and any bytes already present are overwritten
 * (whereas any other data is left intact). The `offset` argument specifies how
 * many bytes should be skipped in the file, after which `size` bytes from
 * buffer are written.
 * This means that the new file size will be max(old_size, offset + size).
 * Returns the number of bytes written, or < 0 on error.
 */
static int sfs_write(const char *path,
                     const char *buf,
                     size_t size,
                     off_t offset,
                     struct fuse_file_info *fi) {
    (void) fi;
    log("write %s data='%.*s' size=%zu offset=%ld\n", path, (int) size, buf,
        size, offset);

    return -ENOSYS;
}


/*
 * Move/rename the file at `path` to `newpath`.
 * Returns 0 on succes, < 0 on error.
 */
static int sfs_rename(const char *path,
                      const char *newpath) {
    /* Implementing this function is optional, and not worth any points. */
    log("rename %s %s\n", path, newpath);

    return -ENOSYS;
}


static const struct fuse_operations sfs_oper = {
        .getattr    = sfs_getattr,
        .readdir    = sfs_readdir,
        .read       = sfs_read,
        .mkdir      = sfs_mkdir,
        .rmdir      = sfs_rmdir,
        .unlink     = sfs_unlink,
        .create     = sfs_create,
        .truncate   = sfs_truncate,
        .write      = sfs_write,
        .rename     = sfs_rename,
};


#define OPTION(t, p)                            \
    { t, offsetof(struct options, p), 1 }
#define LOPTION(s, l, p)                        \
    OPTION(s, p),                               \
    OPTION(l, p)
static const struct fuse_opt option_spec[] = {
        LOPTION("-i %s", "--img=%s", img),
        LOPTION("-b", "--background", background),
        LOPTION("-v", "--verbose", verbose),
        LOPTION("-h", "--help", show_help),
        OPTION("--fuse-help", show_fuse_help),
        FUSE_OPT_END
};

static void show_help(const char *progname) {
    printf("usage: %s mountpoint [options]\n\n", progname);
    printf("By default this FUSE runs in the foreground, and will unmount on\n"
           "exit. If something goes wrong and FUSE does not exit cleanly, use\n"
           "the following command to unmount your mountpoint:\n"
           "  $ fusermount -u <mountpoint>\n\n");
    printf("common options (use --fuse-help for all options):\n"
           "    -i, --img=FILE      filename of SFS image to mount\n"
           "                        (default: \"%s\")\n"
           "    -b, --background    run fuse in background\n"
           "    -v, --verbose       print debug information\n"
           "    -h, --help          show this summarized help\n"
           "        --fuse-help     show full FUSE help\n"
           "\n", default_img);
}

int main(int argc, char **argv) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    options.img = strdup(default_img);

    fuse_opt_parse(&args, &options, option_spec, NULL);

    if (options.show_help) {
        show_help(argv[0]);
        return 0;
    }

    if (options.show_fuse_help) {
        assert(fuse_opt_add_arg(&args, "--help") == 0);
        args.argv[0][0] = '\0';
    }

    if (!options.background)
        assert(fuse_opt_add_arg(&args, "-f") == 0);

    disk_open_image(options.img);

    return fuse_main(args.argc, args.argv, &sfs_oper, NULL);
}

#include "userfs.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <secure/_string.h>

enum {
    /** Увеличил для улучшения производительности */
    BLOCK_SIZE = 8192,
    MAX_FILE_SIZE = 1024 * 1024 * 100,
};

/** Global error code. Set from any function on any error. */
static enum ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
    /** Block memory. */
    char *memory;
    /** How many bytes are occupied. */
    int occupied;
    /** Next block in the file. */
    struct block *next;
    /** Previous block in the file. */
    struct block *prev;
};

struct file {
    /** Double-linked list of file blocks. */
    struct block *block_list;
    /**
     * Last block in the list above for fast access to the end
     * of file.
     */
    struct block *last_block;
    /** How many file descriptors are opened on the file. */
    int refs;
    /** File name. */
    char *name;
    /** Files are stored in a double-linked list. */
    struct file *next;
    struct file *prev;

    size_t size;
    int deleted;
};

/** List of all files. */
static struct file *file_list = NULL;

struct filedesc {
    struct file *file;
    size_t pos;
};

/**
 * An array of file descriptors. When a file descriptor is
 * created, its pointer drops here. When a file descriptor is
 * closed, its place in this array is set to NULL and can be
 * taken by next ufs_open() call.
 */
static struct filedesc **file_descriptors = NULL;
static int file_descriptor_capacity = 0;

static void free_file(struct file *f) {
    struct block *blk = f->block_list;
    while (blk) {
        struct block *next = blk->next;
        free(blk->memory);
        free(blk);
        blk = next;
    }
    free(f->name);
    free(f);
}

enum ufs_error_code
ufs_errno() {
    return ufs_error_code;
}

int
ufs_open(const char *filename, int flags) {
    struct file *file = NULL;
    int is_new_file = 0;

    for (struct file *current = file_list; current != NULL; current = current->next) {
        if (strcmp(current->name, filename) == 0) {
            file = current;
            break;
        }
    }

    if (!file) {
        if (!(flags & UFS_CREATE)) {
            ufs_error_code = UFS_ERR_NO_FILE;
            return -1;
        }

        file = malloc(sizeof(struct file));
        if (!file) {
            ufs_error_code = UFS_ERR_NO_MEM;
            return -1;
        }
        file->name = strdup(filename);
        if (!file->name) {
            free(file);
            ufs_error_code = UFS_ERR_NO_MEM;
            return -1;
        }
        file->block_list = NULL;
        file->last_block = NULL;
        file->refs = 0;
        file->size = 0;
        file->deleted = 0;
        file->next = file_list;
        file->prev = NULL;
        if (file_list)
            file_list->prev = file;
        file_list = file;

        is_new_file = 1;
    }

    struct filedesc *fdesc = malloc(sizeof(struct filedesc));
    if (!fdesc) {
        ufs_error_code = UFS_ERR_NO_MEM;
        if (is_new_file) {
            if (file->prev)
                file->prev->next = file->next;
            else
                file_list = file->next;
            if (file->next)
                file->next->prev = file->prev;
            free_file(file);
        } else if (file->refs == 0 && file->deleted) {
            free_file(file);
        }
        return -1;
    }
    fdesc->file = file;
    fdesc->pos = 0;

    int fd = -1;
    for (int i = 0; i < file_descriptor_capacity; ++i) {
        if (file_descriptors && file_descriptors[i] == NULL) {
            fd = i;
            break;
        }
    }

    if (fd == -1) {
        int new_capacity = file_descriptor_capacity + 16;
        struct filedesc **new_array = realloc(file_descriptors,
                                              new_capacity * sizeof(*new_array));
        if (!new_array) {
            free(fdesc);
            if (is_new_file) {
                if (file->prev)
                    file->prev->next = file->next;
                else
                    file_list = file->next;
                if (file->next)
                    file->next->prev = file->prev;
                free_file(file);
            }
            ufs_error_code = UFS_ERR_NO_MEM;
            return -1;
        }
        for (int i = file_descriptor_capacity; i < new_capacity; ++i)
            new_array[i] = NULL;
        file_descriptors = new_array;
        fd = file_descriptor_capacity;
        file_descriptor_capacity = new_capacity;
    }

    file_descriptors[fd] = fdesc;
    file->refs++;
    return fd;
}

ssize_t ufs_write(int fd, const char *buf, size_t size) {
    if (fd < 0 || fd >= file_descriptor_capacity || !file_descriptors || !file_descriptors[fd]) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    struct filedesc *fdesc = file_descriptors[fd];
    struct file *file = fdesc->file;
    size_t pos = fdesc->pos;

    if (pos > MAX_FILE_SIZE || size > MAX_FILE_SIZE - pos) {
        ufs_error_code = UFS_ERR_NO_MEM;
        return -1;
    }

    size_t written = 0;
    while (written < size) {
        int block_index = (pos + written) / BLOCK_SIZE;
        int block_offset = (pos + written) % BLOCK_SIZE;

        struct block *blk = file->block_list;
        int current_index = 0;
        while (blk && current_index < block_index) {
            blk = blk->next;
            current_index++;
        }

        if (!blk) {
            size_t last_index = 0;
            if (file->last_block) {
                struct block *tmp = file->block_list;
                last_index = 0;
                while (tmp != file->last_block) {
                    tmp = tmp->next;
                    last_index++;
                }
                last_index++;
            } else {
                last_index = 0;
            }
            while (last_index <= (size_t) block_index) {
                struct block *new_blk = malloc(sizeof(struct block));
                if (!new_blk) {
                    ufs_error_code = UFS_ERR_NO_MEM;
                    return written ? (ssize_t) written : -1;
                }
                new_blk->memory = malloc(BLOCK_SIZE);
                if (!new_blk->memory) {
                    free(new_blk);
                    ufs_error_code = UFS_ERR_NO_MEM;
                    return written ? (ssize_t) written : -1;
                }
                new_blk->occupied = 0;
                new_blk->next = NULL;
                new_blk->prev = file->last_block;

                if (file->last_block)
                    file->last_block->next = new_blk;
                else
                    file->block_list = new_blk;
                file->last_block = new_blk;
                blk = new_blk;
                last_index++;
            }
        }

        size_t to_write = size - written;
        size_t available = BLOCK_SIZE - block_offset;
        if (to_write > available)
            to_write = available;

        memcpy(blk->memory + block_offset, buf + written, to_write);
        if (block_offset + to_write > (size_t) blk->occupied)
            blk->occupied = block_offset + to_write;

        written += to_write;
    }

    fdesc->pos += written;
    if (file->size < fdesc->pos)
        file->size = fdesc->pos;

    return (ssize_t) written;
}

ssize_t
ufs_read(int fd, char *buf, size_t size) {
    if (fd < 0 || fd >= file_descriptor_capacity || !file_descriptors || !file_descriptors[fd]) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    struct filedesc *fdesc = file_descriptors[fd];
    struct file *file = fdesc->file;
    size_t pos = fdesc->pos;

    if (pos >= file->size) {
        return 0;
    }

    size_t bytes_read = 0;
    while (bytes_read < size && pos < file->size) {
        size_t block_index = pos / BLOCK_SIZE;
        int block_offset = pos % BLOCK_SIZE;

        struct block *blk = file->block_list;
        size_t current_index = 0;
        while (blk && current_index < block_index) {
            blk = blk->next;
            current_index++;
        }

        if (!blk || block_offset >= blk->occupied)
            break;

        size_t available = blk->occupied - block_offset;
        size_t to_read = size - bytes_read;
        if (to_read > available)
            to_read = available;

        memcpy(buf + bytes_read, blk->memory + block_offset, to_read);
        bytes_read += to_read;
        pos += to_read;
    }

    fdesc->pos = pos;
    return bytes_read;
}

int ufs_close(int fd) {
    if (fd < 0 || fd >= file_descriptor_capacity || !file_descriptors || !file_descriptors[fd]) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    struct filedesc *fdesc = file_descriptors[fd];
    struct file *file = fdesc->file;

    file_descriptors[fd] = NULL;
    free(fdesc);

    file->refs--;

    if (file->deleted && file->refs == 0) {
        free_file(file);
    }

    return 0;
}

int
ufs_delete(const char *filename) {
    struct file *f = NULL;
    for (struct file *current = file_list; current != NULL; current = current->next) {
        if (strcmp(current->name, filename) == 0) {
            f = current;
            break;
        }
    }

    if (!f) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    f->deleted = 1;

    if (f->prev)
        f->prev->next = f->next;
    else
        file_list = f->next;

    if (f->next)
        f->next->prev = f->prev;

    if (f->refs == 0) {
        free_file(f);
    }

    return 0;
}

#if NEED_RESIZE

int
ufs_resize(int fd, size_t new_size)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)fd;
	(void)new_size;
	ufs_error_code = UFS_ERR_NOT_IMPLEMENTED;
	return -1;
}

#endif

void
ufs_destroy(void) {
    for (int i = 0; i < file_descriptor_capacity; ++i) {
        if (file_descriptors && file_descriptors[i]) {
            ufs_close(i);
        }
    }

    struct file *current = file_list;
    while (current) {
        struct file *next = current->next;
        free_file(current);
        current = next;
    }

    free(file_descriptors);
    file_descriptors = NULL;
    file_descriptor_capacity = 0;
}

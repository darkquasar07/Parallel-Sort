#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define RECORD_SIZE 100

// Same 100-byte record layout used by the sorter.
typedef struct
{
    uint32_t key;
    unsigned char payload[RECORD_SIZE - sizeof(uint32_t)];
} Record;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(Record) == RECORD_SIZE, "Record must be exactly 100 bytes");
#endif

static void die(const char *msg)
{
    perror(msg);
    exit(1);
}

static int compare_records(const void *a, const void *b)
{
    const Record *ra = (const Record *)a;
    const Record *rb = (const Record *)b;

    if (ra->key < rb->key)
        return -1;
    if (ra->key > rb->key)
        return 1;
    return memcmp(ra->payload, rb->payload, sizeof(ra->payload));
}

static Record *read_records(const char *path, size_t *count_out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        die("open");

    struct stat st;
    if (fstat(fd, &st) < 0)
        die("fstat");
    if (st.st_size < 0 || (st.st_size % RECORD_SIZE) != 0)
    {
        fprintf(stderr, "%s: file size is not a multiple of %d bytes\n", path, RECORD_SIZE);
        exit(1);
    }

    size_t bytes = (size_t)st.st_size;
    size_t count = bytes / RECORD_SIZE;
    Record *records = NULL;
    if (bytes > 0)
    {
        records = malloc(bytes);
        if (!records)
            die("malloc");

        size_t got = 0;
        while (got < bytes)
        {
            ssize_t rc = read(fd, (unsigned char *)records + got, bytes - got);
            if (rc < 0)
            {
                if (errno == EINTR)
                    continue;
                die("read");
            }
            if (rc == 0)
                break;
            got += (size_t)rc;
        }
        if (got != bytes)
        {
            fprintf(stderr, "%s: short read\n", path);
            exit(1);
        }
    }

    if (close(fd) < 0)
        die("close");
    *count_out = count;
    return records;
}

// Verify nondecreasing key order.
static void check_sorted_keys(const char *path, const Record *records, size_t count)
{
    for (size_t i = 1; i < count; i++)
    {
        if (records[i - 1].key > records[i].key)
        {
            fprintf(stderr, "%s: not sorted at record %zu: %u > %u\n",
                    path, i, records[i - 1].key, records[i].key);
            exit(1);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3)
    {
        fprintf(stderr, "usage: %s output\n", argv[0]);
        fprintf(stderr, "   or: %s input output\n", argv[0]);
        return 1;
    }

    if (argc == 2)
    {
        size_t out_count = 0;
        Record *out = read_records(argv[1], &out_count);
        check_sorted_keys(argv[1], out, out_count);
        printf("OK: %s is sorted (%zu records)\n", argv[1], out_count);
        free(out);
        return 0;
    }

    size_t in_count = 0;
    size_t out_count = 0;
    Record *in = read_records(argv[1], &in_count);
    Record *out = read_records(argv[2], &out_count);

    if (in_count != out_count)
    {
        fprintf(stderr, "record count mismatch: input=%zu output=%zu\n", in_count, out_count);
        return 1;
    }

    check_sorted_keys(argv[2], out, out_count);

    // Sort both copies to confirm no records were lost or changed.
    qsort(in, in_count, sizeof(Record), compare_records);
    qsort(out, out_count, sizeof(Record), compare_records);

    if (in_count > 0 && memcmp(in, out, in_count * sizeof(Record)) != 0)
    {
        fprintf(stderr, "output is sorted but does not contain the same records as input\n");
        return 1;
    }

    printf("OK: %s is sorted and matches %s (%zu records)\n", argv[2], argv[1], out_count);
    free(in);
    free(out);
    return 0;
}

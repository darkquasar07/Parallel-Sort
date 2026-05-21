#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define RECORD_SIZE 100

// Same 100-byte record format expected by psort.
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

static void write_all(int fd, const unsigned char *buf, size_t bytes)
{
    size_t written = 0;
    while (written < bytes)
    {
        ssize_t rc = write(fd, buf + written, bytes - written);
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            die("write");
        }
        if (rc == 0)
        {
            fprintf(stderr, "write returned 0 unexpectedly\n");
            exit(1);
        }
        written += (size_t)rc;
    }
}

// Tiny deterministic PRNG for repeatable test files.
static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4)
    {
        fprintf(stderr, "usage: %s record-count output [seed]\n", argv[0]);
        return 1;
    }

    char *end = NULL;
    errno = 0;
    unsigned long long n_ull = strtoull(argv[1], &end, 10);
    if (errno != 0 || end == argv[1] || *end != '\0')
    {
        fprintf(stderr, "invalid record count: %s\n", argv[1]);
        return 1;
    }
    size_t n = (size_t)n_ull;

    uint32_t seed = (uint32_t)time(NULL);
    if (argc == 4)
    {
        char *seed_end = NULL;
        errno = 0;
        unsigned long seed_ul = strtoul(argv[3], &seed_end, 10);
        if (errno != 0 || seed_end == argv[3] || *seed_end != '\0')
        {
            fprintf(stderr, "invalid seed: %s\n", argv[3]);
            return 1;
        }
        seed = (uint32_t)seed_ul;
    }
    if (seed == 0)
        seed = 1;

    int fd = open(argv[2], O_CREAT | O_TRUNC | O_WRONLY, 0666);
    if (fd < 0)
        die("open output");

    enum
    {
        BATCH = 4096
    };
    Record *records = malloc(BATCH * sizeof(Record));
    if (!records)
        die("malloc");

    size_t left = n;
    // Write records in batches to avoid large memory use.
    while (left > 0)
    {
        size_t cur = left < BATCH ? left : BATCH;
        for (size_t i = 0; i < cur; i++)
        {
            records[i].key = xorshift32(&seed);
            for (size_t j = 0; j < sizeof(records[i].payload); j += 4)
            {
                uint32_t value = xorshift32(&seed);
                size_t copy = sizeof(value);
                if (j + copy > sizeof(records[i].payload))
                {
                    copy = sizeof(records[i].payload) - j;
                }
                memcpy(records[i].payload + j, &value, copy);
            }
        }
        write_all(fd, (const unsigned char *)records, cur * sizeof(Record));
        left -= cur;
    }

    free(records);
    if (close(fd) < 0)
        die("close output");
    return 0;
}

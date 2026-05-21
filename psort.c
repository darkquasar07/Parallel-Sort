#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__) && defined(__MACH__)
#include <sys/sysctl.h>
#endif

#define RECORD_SIZE 100

// One OSTEP record: 4-byte key + 96-byte payload.
typedef struct
{
    uint32_t key;
    unsigned char payload[RECORD_SIZE - sizeof(uint32_t)];
} Record;

typedef struct
{
    Record *base;
    size_t start;
    size_t len;
} SortTask;

typedef struct
{
    size_t start;
    size_t len;
} Chunk;

typedef struct
{
    size_t chunk;
    size_t index;
} HeapNode;

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

// Each worker sorts one independent chunk.
static void *sort_worker(void *arg)
{
    SortTask *task = (SortTask *)arg;
    qsort(task->base + task->start, task->len, sizeof(Record), compare_records);
    return NULL;
}

static long detect_cpu_count(void)
{
#if defined(_SC_NPROCESSORS_ONLN)
    long threads = sysconf(_SC_NPROCESSORS_ONLN);
    if (threads > 0)
        return threads;
#endif

#if defined(__APPLE__) && defined(__MACH__)
    int cpu_count = 1;
    size_t len = sizeof(cpu_count);
    if (sysctlbyname("hw.ncpu", &cpu_count, &len, NULL, 0) == 0 && cpu_count > 0)
    {
        return cpu_count;
    }
#endif

    return 1;
}

static long get_thread_count(size_t records)
{
    long threads = detect_cpu_count();

    // Optional override for testing/performance experiments.
    const char *env = getenv("PSORT_THREADS");
    if (env && *env)
    {
        char *end = NULL;
        errno = 0;
        long requested = strtol(env, &end, 10);
        if (errno == 0 && end != env && *end == '\0' && requested > 0)
        {
            threads = requested;
        }
    }

    if (records == 0)
        return 1;
    if ((size_t)threads > records)
        threads = (long)records;
    return threads;
}

static int heap_less(const HeapNode *a, const HeapNode *b, const Record *records, const Chunk *chunks)
{
    const Record *ra = records + chunks[a->chunk].start + a->index;
    const Record *rb = records + chunks[b->chunk].start + b->index;
    return compare_records(ra, rb) < 0;
}

static void heap_swap(HeapNode *a, HeapNode *b)
{
    HeapNode tmp = *a;
    *a = *b;
    *b = tmp;
}

static void heap_push(HeapNode *heap, size_t *heap_size, HeapNode node, const Record *records, const Chunk *chunks)
{
    size_t i = (*heap_size)++;
    heap[i] = node;

    while (i > 0)
    {
        size_t parent = (i - 1) / 2;
        if (!heap_less(&heap[i], &heap[parent], records, chunks))
            break;
        heap_swap(&heap[i], &heap[parent]);
        i = parent;
    }
}

static HeapNode heap_pop(HeapNode *heap, size_t *heap_size, const Record *records, const Chunk *chunks)
{
    HeapNode min_node = heap[0];
    heap[0] = heap[--(*heap_size)];

    size_t i = 0;
    while (1)
    {
        size_t left = 2 * i + 1;
        size_t right = 2 * i + 2;
        size_t smallest = i;

        if (left < *heap_size && heap_less(&heap[left], &heap[smallest], records, chunks))
        {
            smallest = left;
        }
        if (right < *heap_size && heap_less(&heap[right], &heap[smallest], records, chunks))
        {
            smallest = right;
        }
        if (smallest == i)
            break;

        heap_swap(&heap[i], &heap[smallest]);
        i = smallest;
    }

    return min_node;
}

// K-way merge of already sorted chunks using a min-heap.
static void merge_chunks(const Record *records, Record *out, const Chunk *chunks, size_t chunk_count, size_t total_records)
{
    HeapNode *heap = malloc(chunk_count * sizeof(HeapNode));
    if (!heap)
        die("malloc heap");

    size_t heap_size = 0;
    for (size_t i = 0; i < chunk_count; i++)
    {
        if (chunks[i].len > 0)
        {
            HeapNode node = {.chunk = i, .index = 0};
            heap_push(heap, &heap_size, node, records, chunks);
        }
    }

    for (size_t out_idx = 0; out_idx < total_records; out_idx++)
    {
        HeapNode node = heap_pop(heap, &heap_size, records, chunks);
        const Chunk *chunk = &chunks[node.chunk];
        out[out_idx] = records[chunk->start + node.index];

        size_t next_index = node.index + 1;
        if (next_index < chunk->len)
        {
            HeapNode next = {.chunk = node.chunk, .index = next_index};
            heap_push(heap, &heap_size, next, records, chunks);
        }
    }

    free(heap);
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

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: %s input output\n", argv[0]);
        return 1;
    }

    const char *input_path = argv[1];
    const char *output_path = argv[2];

    int in_fd = open(input_path, O_RDONLY);
    if (in_fd < 0)
        die("open input");

    struct stat st;
    if (fstat(in_fd, &st) < 0)
        die("fstat input");
    if (st.st_size < 0 || (st.st_size % RECORD_SIZE) != 0)
    {
        fprintf(stderr, "input file size must be a multiple of %d bytes\n", RECORD_SIZE);
        close(in_fd);
        return 1;
    }

    size_t input_bytes = (size_t)st.st_size;
    size_t record_count = input_bytes / RECORD_SIZE;

    Record *records = NULL;
    if (record_count > 0)
    {
        // Map input, then copy so qsort can safely reorder records.
        void *mapped = mmap(NULL, input_bytes, PROT_READ, MAP_PRIVATE, in_fd, 0);
        if (mapped == MAP_FAILED)
            die("mmap input");

        records = malloc(record_count * sizeof(Record));
        if (!records)
            die("malloc records");
        memcpy(records, mapped, input_bytes);

        if (munmap(mapped, input_bytes) < 0)
            die("munmap input");
    }

    if (close(in_fd) < 0)
        die("close input");

    long thread_count_long = get_thread_count(record_count);
    size_t thread_count = (size_t)thread_count_long;

    pthread_t *threads = calloc(thread_count, sizeof(pthread_t));
    SortTask *tasks = calloc(thread_count, sizeof(SortTask));
    Chunk *chunks = calloc(thread_count, sizeof(Chunk));
    if (!threads || !tasks || !chunks)
        die("calloc");

    size_t base = record_count / thread_count;
    size_t rem = record_count % thread_count;
    size_t start = 0;

    // Split records as evenly as possible across threads.
    for (size_t i = 0; i < thread_count; i++)
    {
        size_t len = base + (i < rem ? 1 : 0);
        tasks[i].base = records;
        tasks[i].start = start;
        tasks[i].len = len;
        chunks[i].start = start;
        chunks[i].len = len;

        if (pthread_create(&threads[i], NULL, sort_worker, &tasks[i]) != 0)
        {
            errno = EAGAIN;
            die("pthread_create");
        }
        start += len;
    }

    for (size_t i = 0; i < thread_count; i++)
    {
        if (pthread_join(threads[i], NULL) != 0)
        {
            errno = EINVAL;
            die("pthread_join");
        }
    }

    Record *sorted = records;
    if (thread_count > 1 && record_count > 0)
    {
        // Multi-threaded sort needs a final merge step.
        sorted = malloc(record_count * sizeof(Record));
        if (!sorted)
            die("malloc sorted");
        merge_chunks(records, sorted, chunks, thread_count, record_count);
    }

    int out_fd = open(output_path, O_CREAT | O_TRUNC | O_WRONLY, 0666);
    if (out_fd < 0)
        die("open output");

    if (input_bytes > 0)
    {
        write_all(out_fd, (const unsigned char *)sorted, input_bytes);
    }

    // Required by the assignment: force output to disk.
    if (fsync(out_fd) < 0)
        die("fsync output");
    if (close(out_fd) < 0)
        die("close output");

    if (sorted != records)
        free(sorted);
    free(records);
    free(threads);
    free(tasks);
    free(chunks);

    return 0;
}

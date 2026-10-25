/* Opt-in full 5,000,000 -> 50,000,000 byte tail trace. Real socket/broker/file IO;
 * no game data, no credentials, no console, no claim of uncached SSD throughput. */
#define main writebroker_unit_suite_main
#include "test-writebroker-space.c"
#undef main

#define open(...) server_open(__VA_ARGS__)
#define pwrite(fd, bytes, length, offset) server_pwrite(fd, bytes, length, offset)
#include "writebroker-before.inc"
#undef pwrite
#undef open

enum { BENCH_BEGIN = 5000000, BENCH_END = 50000000, BENCH_STEP = 520, BENCH_BASE = 6302400 };
static void bench_arm(const char *name, int before)
{
    const size_t capacity = BENCH_BASE + BENCH_END;
    const uint64_t stages[] = {BENCH_BEGIN, 16u << 20, (16u << 20) + BENCH_STEP, 32u << 20, BENCH_END};
    const size_t rounds_capacity = (BENCH_END - BENCH_BEGIN + BENCH_STEP - 1) / BENCH_STEP;
    unsigned char *tail = malloc(BENCH_END), prefix[65536];
    uint64_t *latency = malloc(rounds_capacity * sizeof(*latency)), sum = 0, logical = 0;
    size_t length = BENCH_BEGIN, rounds = 0, stage = 1;
    uint64_t expected_anchors = 0, expected_small_existing = 0, expected_small_bytes = 0;
    int fd;
    unsigned char *view;
    struct stat status;

    assert(tail && latency && !thread_count);
    reset_write_faults();serve_write_before = before ? serve_write_baseline : NULL;
    fd = open(osrs_data_path, O_RDWR | O_CREAT | O_TRUNC, 0600);assert(fd >= 0);
    memset(prefix, 0x55, sizeof(prefix));memset(tail, 0xa7, BENCH_END);
    for (size_t offset = 0; offset < BENCH_BASE; ) {
        size_t count = BENCH_BASE - offset;
        if (count > sizeof(prefix)) count = sizeof(prefix);
        assert(pwrite(fd, prefix, count, offset) == (ssize_t)count);offset += count;
    }
    assert(pwrite(fd, tail, BENCH_BEGIN, BENCH_BASE) == BENCH_BEGIN);
    view = mmap(NULL, capacity, PROT_READ, MAP_SHARED, fd, 0);assert(view != MAP_FAILED);
    assert(lseek(fd, 137, SEEK_SET) == 137);
    __atomic_store_n(&physical_write_bytes, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&physical_write_calls, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&physical_read_bytes, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&physical_read_calls, 0, __ATOMIC_SEQ_CST);
    memset(&write_metrics_total, 0, sizeof(write_metrics_total));served_writes = served_bytes = 0;
    broker_mode = 1;current_thread_id = 64;
    printf("BROKER_DIFF_STAGE route=%s tail=%u logical=0 pwrite=0 pread=0\n", name, BENCH_BEGIN);fflush(stdout);
    uint64_t wall_start = host_now_ns();
    while (length < BENCH_END) {
        size_t growth = BENCH_END - length;
        if (growth > BENCH_STEP) growth = BENCH_STEP;
        size_t previous = length;
        memset(tail + length, (unsigned char)(rounds * 17 + 1), growth);length += growth;
        expected_anchors += previous / WB_MAX_WRITE;
        size_t final_request = length % WB_MAX_WRITE;
        if (final_request && final_request <= 4096) {
            size_t request_start = length - final_request;
            expected_small_bytes += final_request;
            if (previous > request_start) expected_small_existing += previous - request_start;
        }
        uint64_t started = host_now_ns();
        assert(broker_pwrite(3, tail, length, BENCH_BASE, 0, NULL) == (ssize_t)length);
        latency[rounds] = host_now_ns() - started;sum += latency[rounds];logical += length;rounds++;
        /* Every logical ACK exposes the exact full tail through an already-live
         * shared mapping; also check prefix edges, EOF and caller position. */
        assert(!fstat(fd, &status) && status.st_size == (off_t)(BENCH_BASE + length));
        assert(!memcmp(view + BENCH_BASE, tail, length));
        assert(view[0] == 0x55 && view[BENCH_BASE - 1] == 0x55);
        assert(lseek(fd, 0, SEEK_CUR) == 137);
        while (stage < sizeof(stages) / sizeof(stages[0]) && length >= stages[stage]) {
            printf("BROKER_DIFF_STAGE route=%s threshold=%llu tail=%zu rounds=%zu logical=%llu pwrite=%llu pread=%llu\n",
                   name, (unsigned long long)stages[stage], length, rounds,
                   (unsigned long long)logical, (unsigned long long)__atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST),
                   (unsigned long long)__atomic_load_n(&physical_read_bytes, __ATOMIC_SEQ_CST));
            fflush(stdout);stage++;
        }
    }
    uint64_t wall_ns = host_now_ns() - wall_start;
    close_connections();assert(!thread_count);
    qsort(latency, rounds, sizeof(*latency), timing_compare);
    uint64_t written = __atomic_load_n(&physical_write_bytes, __ATOMIC_SEQ_CST);
    uint64_t read_bytes = __atomic_load_n(&physical_read_bytes, __ATOMIC_SEQ_CST);
    assert(rounds == 86539 && logical == UINT64_C(2379857115320));
    printf("BROKER_DIFF_FULL route=%s begin=%u end=%u step=%u final_growth=240 rounds=%zu logical_bytes=%llu "
           "pwrite_bytes=%llu pwrite_calls=%llu pread_bytes=%llu pread_calls=%llu eligible_bytes=%llu skipped_bytes=%llu "
           "hole_bytes=%llu unknown_bytes=%llu read_error_bytes=%llu write_only_bytes=%llu small_bytes=%llu anchor_bytes=%llu "
           "mean_us=%.3f p50_us=%.3f p95_us=%.3f max_us=%.3f wall_s=%.3f "
           "verification=full-tail+prefix-edges+EOF+position-after-every-logical-ACK-and-live-map transport=socketpair cached_no_fsync=1 console_estimate=unavailable\n",
           name, BENCH_BEGIN, BENCH_END, BENCH_STEP, rounds, (unsigned long long)logical,
           (unsigned long long)written, (unsigned long long)__atomic_load_n(&physical_write_calls, __ATOMIC_SEQ_CST),
           (unsigned long long)read_bytes, (unsigned long long)__atomic_load_n(&physical_read_calls, __ATOMIC_SEQ_CST),
           (unsigned long long)write_metrics_total.eligible_bytes, (unsigned long long)write_metrics_total.skipped_bytes,
           (unsigned long long)write_metrics_total.hole_bytes, (unsigned long long)write_metrics_total.unknown_bytes,
           (unsigned long long)write_metrics_total.read_error_bytes, (unsigned long long)write_metrics_total.write_only_bytes,
           (unsigned long long)write_metrics_total.small_bytes, (unsigned long long)write_metrics_total.anchor_bytes,
           sum / (double)rounds / 1000.0,
           latency[rounds / 2] / 1000.0, latency[rounds * 95 / 100] / 1000.0, latency[rounds - 1] / 1000.0, wall_ns / 1e9);
    fflush(stdout);
    if (before) assert(written == logical && !read_bytes);
    else {
        assert(write_metrics_total.anchor_bytes == expected_anchors);
        assert(write_metrics_total.small_bytes == expected_small_bytes);
        assert(written == BENCH_END - BENCH_BEGIN + expected_anchors + expected_small_existing);
        assert(write_metrics_total.pwrite_bytes == written && write_metrics_total.pread_bytes == read_bytes);
        assert(!write_metrics_total.hole_bytes && !write_metrics_total.unknown_bytes && !write_metrics_total.read_error_bytes);
        assert(write_metrics_total.eligible_bytes > UINT64_C(2000000000000));
    }
    assert(!munmap(view, capacity) && !close(fd));assert(!unlink(osrs_data_path));
    free(latency);free(tail);serve_write_before = NULL;broker_mode = connected = 0;
}
int main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "--full") ||
        (strcmp(argv[2], "both") && strcmp(argv[2], "before") && strcmp(argv[2], "after"))) {
        fputs("Explicit --full both|before|after required: each arm runs all 86,539 writes (~2.38 TB logical).\n", stderr);return 2;
    }
    wine_ps5_title_critical_signals(&ps5_title_block_set);assert(mkdtemp(directory));
    snprintf(token_path, sizeof(token_path), "%s/token", directory);
    snprintf(writes_path, sizeof(writes_path), "%s/writes", directory);
    snprintf(osrs_data_path, sizeof(osrs_data_path), "%s/full-pattern.dat2", directory);
    int token_fd = open(token_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(token_fd >= 0 && write(token_fd, token, sizeof(token)) == sizeof(token));assert(!close(token_fd));
    if (strcmp(argv[2], "after")) bench_arm("original-serve-write", 1);
    if (strcmp(argv[2], "before")) bench_arm("synchronous-diff", 0);
    for (unsigned i = 0; i < BOUNCE_SLOTS; i++) if (bounce_buffer[i]) assert(!munmap(bounce_buffer[i], BOUNCE_SIZE));
    assert(!unlink(token_path) && !rmdir(directory));
    assert(!failures);puts("BROKER_DIFF_FULL_PASS scope=HOST_CACHED_BACKING_IO_NOT_PS5_STORAGE");return 0;
}

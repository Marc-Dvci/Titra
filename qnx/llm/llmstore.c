/*
 * llmstore: puts the model's weights in RAM once, at boot, outside the process that runs them.
 *
 *   llmstore -i /models/dory.gguf -n dory.gguf [-s SHA256_HEX]
 *
 * It creates a shared memory object /dev/shmem/NAME, physically contiguous when the kernel can give it, copies the file
 * in, checks its SHA-256, makes the object read-only for everyone, and exits. The object outlives it. So the model
 * process (llm) never holds a writable copy of its own weights, and when the High Availability Manager restarts llm the
 * new process maps them in under a second instead of reading and checking 3.67 GB again.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "titra/sha256.h"
#include "titra_qnx.h"

#define CHUNK (4u * 1024u * 1024u)

int main(int argc, char **argv) {
    const char *in = NULL, *name = NULL, *want = NULL;
    int opt, contig = 1;
    while ((opt = getopt(argc, argv, "i:n:s:C")) != -1) {
        switch (opt) {
            case 'i': in = optarg; break;
            case 'n': name = optarg; break;
            case 's': want = optarg; break;
            case 'C': contig = 0; break;
            default:
                fprintf(stderr, "usage: llmstore -i FILE -n NAME [-s SHA256] [-C]\n");
                return 2;
        }
    }
    if (in == NULL || name == NULL) {
        fprintf(stderr, "usage: llmstore -i FILE -n NAME [-s SHA256] [-C]\n");
        return 2;
    }
    const double t0 = titra_now();
    int src = open(in, O_RDONLY);
    if (src < 0) {
        fprintf(stderr, "llmstore: cannot open %s: %s\n", in, strerror(errno));
        return 1;
    }
    struct stat st;
    if (fstat(src, &st) != 0 || st.st_size <= 0) {
        fprintf(stderr, "llmstore: cannot read the size of %s\n", in);
        return 1;
    }
    const size_t size = (size_t)st.st_size;

    char shm_name[128];
    snprintf(shm_name, sizeof shm_name, "/%s", name);
    shm_unlink(shm_name);
    int fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0444);
    if (fd < 0) {
        fprintf(stderr, "llmstore: cannot create %s: %s\n", shm_name, strerror(errno));
        return 1;
    }
    int contiguous = 0;
    if (contig && shm_ctl(fd, SHMCTL_ANON | SHMCTL_PHYS, 0, (uint64_t)size) == 0) {
        contiguous = 1;
    } else if (ftruncate(fd, (off_t)size) != 0) {
        fprintf(stderr, "llmstore: cannot size %s: %s\n", shm_name, strerror(errno));
        shm_unlink(shm_name);
        return 1;
    }
    unsigned char *dst = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (dst == MAP_FAILED) {
        fprintf(stderr, "llmstore: cannot map %s: %s\n", shm_name, strerror(errno));
        shm_unlink(shm_name);
        return 1;
    }
    titra_sha256_t sha;
    titra_sha256_init(&sha);
    size_t done = 0;
    while (done < size) {
        const size_t want_bytes = size - done < CHUNK ? size - done : CHUNK;
        const ssize_t n = read(src, dst + done, want_bytes);
        if (n <= 0) {
            fprintf(stderr, "llmstore: read failed at %zu: %s\n", done, strerror(errno));
            shm_unlink(shm_name);
            return 1;
        }
        titra_sha256_update(&sha, dst + done, (size_t)n);
        done += (size_t)n;
    }
    close(src);
    char hex[65];
    titra_sha256_final_hex(&sha, hex);
    if (want != NULL && strcmp(want, hex) != 0) {
        fprintf(stderr, "llmstore: SHA-256 mismatch: file %s, expected %s; object removed\n", hex, want);
        shm_unlink(shm_name);
        return 1;
    }
    munmap(dst, size);
    /* The layout is final and only the creator could change it; everyone else can only map it for reading. */
    shm_ctl(fd, SHMCTL_SEAL, 0, 0);
    fchmod(fd, 0444);
    close(fd);
    printf("{\"object\":\"/dev/shmem/%s\",\"bytes\":%zu,\"contiguous\":%d,\"sha256\":\"%s\",\"verified\":%d,\"seconds\":%.2f}\n", name, size,
           contiguous, hex, want != NULL, titra_now() - t0);
    return 0;
}

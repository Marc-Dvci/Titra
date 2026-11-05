/* SHA-256 against the FIPS 180-4 test vectors. */
#include <string.h>

#include "check.h"
#include "titra/sha256.h"

static void digest(const char *text, char hex[65]) {
    titra_sha256_t s;
    titra_sha256_init(&s);
    titra_sha256_update(&s, text, strlen(text));
    titra_sha256_final_hex(&s, hex);
}

TEST(test_sha256_of_the_empty_string) {
    char hex[65];
    digest("", hex);
    CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
}

TEST(test_sha256_of_abc) {
    char hex[65];
    digest("abc", hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
}

TEST(test_sha256_of_two_blocks) {
    char hex[65];
    digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", hex);
    CHECK(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
}

TEST(test_sha256_does_not_depend_on_how_the_data_is_split) {
    static char buf[1000000];
    memset(buf, 'a', sizeof buf);
    titra_sha256_t s;
    titra_sha256_init(&s);
    size_t done = 0;
    size_t step = 1;
    while (done < sizeof buf) {
        const size_t n = step < sizeof buf - done ? step : sizeof buf - done;
        titra_sha256_update(&s, buf + done, n);
        done += n;
        step = step % 977 + 3;
    }
    char hex[65];
    titra_sha256_final_hex(&s, hex);
    CHECK(strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
}

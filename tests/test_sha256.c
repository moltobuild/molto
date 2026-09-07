#include <moltest.h>

#include <molto/util/sha256.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * SHA-256 against the vectors FIPS 180-4 publishes, plus the two things a
 * streaming implementation gets wrong: the block boundary, and a file read in
 * chunks. The digest molto declares is what the registry makes immutable, so
 * "probably right" is not a state this can be left in.
 */

#define EMPTY_DIGEST "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
#define ABC_DIGEST "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
#define TWO_BLOCK_DIGEST "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
#define MILLION_A_DIGEST "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"

static const char *TWO_BLOCK_MESSAGE =
    "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";

static void digest_of(const void *data, size_t length, char *hex_out) {
    sha256_state state;
    sha256_init(&state);
    sha256_update(&state, data, length);
    sha256_finish(&state, hex_out);
}

MOLTEST(sha256_answers_the_published_vectors) {
    char hex[SHA256_HEX_SIZE];

    digest_of("", 0, hex);
    EXPECT_STREQ(EMPTY_DIGEST, hex);

    digest_of("abc", 3, hex);
    EXPECT_STREQ(ABC_DIGEST, hex);

    digest_of(TWO_BLOCK_MESSAGE, strlen(TWO_BLOCK_MESSAGE), hex);
    EXPECT_STREQ(TWO_BLOCK_DIGEST, hex);
}

/* The expensive vector, and the one that exercises the length counter past
   anything a single update carries. */
MOLTEST(sha256_answers_the_long_vector) {
    sha256_state state;
    sha256_init(&state);

    char chunk[1000];
    memset(chunk, 'a', sizeof chunk);
    for (size_t i = 0; i < 1000; i++)
        sha256_update(&state, chunk, sizeof chunk);

    char hex[SHA256_HEX_SIZE];
    sha256_finish(&state, hex);
    EXPECT_STREQ(MILLION_A_DIGEST, hex);
}

/*
 * The same message fed in one byte at a time must hash to the same thing as
 * the message fed in whole. This is the property the buffering exists to hold,
 * and the one a chunked file read depends on.
 */
MOLTEST(sha256_does_not_care_how_the_message_is_divided) {
    char whole[SHA256_HEX_SIZE];
    digest_of(TWO_BLOCK_MESSAGE, strlen(TWO_BLOCK_MESSAGE), whole);

    sha256_state state;
    sha256_init(&state);
    for (const char *cursor = TWO_BLOCK_MESSAGE; *cursor != '\0'; cursor++)
        sha256_update(&state, cursor, 1);

    char byte_at_a_time[SHA256_HEX_SIZE];
    sha256_finish(&state, byte_at_a_time);
    EXPECT_STREQ(whole, byte_at_a_time);
}

/* Exactly one block, and one byte either side of it: the padding takes a
   different branch when the tail leaves no room for the length. */
MOLTEST(sha256_handles_the_block_boundary) {
    char message[130];
    memset(message, 'x', sizeof message);

    for (size_t length = 55; length <= 65; length++) {
        sha256_state state;
        sha256_init(&state);
        sha256_update(&state, message, length);

        char streamed[SHA256_HEX_SIZE];
        sha256_finish(&state, streamed);

        /* Split anywhere and the answer must not move. */
        sha256_state split;
        sha256_init(&split);
        sha256_update(&split, message, length / 2);
        sha256_update(&split, message + length / 2, length - length / 2);

        char in_two[SHA256_HEX_SIZE];
        sha256_finish(&split, in_two);
        EXPECT_STREQ(streamed, in_two);
    }
}

/* What the watcher saw, so the test can say more than "it was called". */
typedef struct {
    long long last_done;
    long long total;
    size_t calls;
    bool monotonic;
} watched;

static void watch(long long done, long long total, void *context) {
    watched *seen = context;
    if (done < seen->last_done)
        seen->monotonic = false;
    seen->last_done = done;
    seen->total = total;
    seen->calls++;
}

static bool write_file(const char *path, const void *data, size_t length) {
    FILE *file = fopen(path, "wb");
    if (file == NULL)
        return false;
    const bool ok = length == 0 || fwrite(data, 1, length, file) == length;
    return fclose(file) == 0 && ok;
}

MOLTEST(sha256_hashes_a_file) {
    char dir[512];
    ASSERT_TRUE(moltest_temp_dir("molto_sha256", dir, sizeof dir));

    char path[600];
    snprintf(path, sizeof path, "%s/archive", dir);
    ASSERT_TRUE(write_file(path, "abc", 3));

    char hex[SHA256_HEX_SIZE];
    ASSERT_TRUE(sha256_file(path, hex));
    EXPECT_STREQ(ABC_DIGEST, hex);

    (void)remove(path);
}

MOLTEST(sha256_hashes_an_empty_file) {
    char dir[512];
    ASSERT_TRUE(moltest_temp_dir("molto_sha256", dir, sizeof dir));

    char path[600];
    snprintf(path, sizeof path, "%s/archive", dir);
    ASSERT_TRUE(write_file(path, "", 0));

    char hex[SHA256_HEX_SIZE];
    ASSERT_TRUE(sha256_file(path, hex));
    EXPECT_STREQ(EMPTY_DIGEST, hex);

    (void)remove(path);
}

/* Larger than SHA256_CHUNK_SIZE, so the read loop goes round more than once
   and the watcher is called with a total it can divide by. */
MOLTEST(sha256_reports_progress_over_a_file_it_reads_in_chunks) {
    const size_t length = SHA256_CHUNK_SIZE * 2 + 7;
    char *data = malloc(length);
    ASSERT_NOT_NULL(data);
    memset(data, 'a', length);

    char dir[512];
    ASSERT_TRUE(moltest_temp_dir("molto_sha256", dir, sizeof dir));

    char path[600];
    snprintf(path, sizeof path, "%s/archive", dir);
    ASSERT_TRUE(write_file(path, data, length));

    char expected[SHA256_HEX_SIZE];
    digest_of(data, length, expected);
    free(data);

    watched seen = {.monotonic = true};

    char hex[SHA256_HEX_SIZE];
    ASSERT_TRUE(sha256_file_watched(path, hex, watch, &seen));
    EXPECT_STREQ(expected, hex);

    /* More than one call, never going backwards, and finishing at the size of
       the file: a fraction that does any of those is worse than none. */
    EXPECT_GT(seen.calls, (size_t)1);
    EXPECT_TRUE(seen.monotonic);
    EXPECT_EQ((long long)length, seen.last_done);
    EXPECT_EQ((long long)length, seen.total);

    (void)remove(path);
}

MOLTEST(sha256_says_no_when_the_file_is_not_there) {
    char hex[SHA256_HEX_SIZE];
    EXPECT_FALSE(sha256_file("no/such/archive.tar.gz", hex));
}

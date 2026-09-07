#include <molto/util/sha256.h>

#include <stdio.h>
#include <string.h>

/* Size of the block the compression function works on. */
#define BLOCK_SIZE 64

/* Where the length is written inside the final block, and the largest message
   tail that still leaves room for it. */
#define LENGTH_OFFSET 56

/* The first 32 bits of the fractional parts of the cube roots of the first 64
   primes (FIPS 180-4, section 4.2.2). */
static const uint32_t round_constants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotate_right(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32 - bits));
}

static uint32_t choose(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }

static uint32_t majority(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }

static uint32_t big_sigma0(uint32_t x) {
    return rotate_right(x, 2) ^ rotate_right(x, 13) ^ rotate_right(x, 22);
}

static uint32_t big_sigma1(uint32_t x) {
    return rotate_right(x, 6) ^ rotate_right(x, 11) ^ rotate_right(x, 25);
}

static uint32_t small_sigma0(uint32_t x) {
    return rotate_right(x, 7) ^ rotate_right(x, 18) ^ (x >> 3);
}

static uint32_t small_sigma1(uint32_t x) {
    return rotate_right(x, 17) ^ rotate_right(x, 19) ^ (x >> 10);
}

/* Mix one 64-byte block into the running state. */
static void compress(uint32_t state[8], const unsigned char block[BLOCK_SIZE]) {
    uint32_t schedule[64];
    for(size_t i = 0; i < 16; i++)
        schedule[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 |
                      (uint32_t)block[i * 4 + 2] << 8 | (uint32_t)block[i * 4 + 3];
    for(size_t i = 16; i < 64; i++)
        schedule[i] = small_sigma1(schedule[i - 2]) + schedule[i - 7] +
                      small_sigma0(schedule[i - 15]) + schedule[i - 16];

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for(size_t i = 0; i < 64; i++) {
        uint32_t t1 = h + big_sigma1(e) + choose(e, f, g) + round_constants[i] + schedule[i];
        uint32_t t2 = big_sigma0(a) + majority(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void sha256_init(sha256_state *state) {
    /* First 32 bits of the fractional parts of the square roots of the first
       eight primes. */
    state->state[0] = 0x6a09e667;
    state->state[1] = 0xbb67ae85;
    state->state[2] = 0x3c6ef372;
    state->state[3] = 0xa54ff53a;
    state->state[4] = 0x510e527f;
    state->state[5] = 0x9b05688c;
    state->state[6] = 0x1f83d9ab;
    state->state[7] = 0x5be0cd19;
    state->length = 0;
    state->buffered = 0;
}

void sha256_update(sha256_state *state, const void *data, size_t length) {
    const unsigned char *cursor = data;
    state->length += length;

    /* Top up a partly filled block first, then take whole blocks straight from
       the caller's buffer without copying them. */
    if(state->buffered > 0) {
        size_t room = BLOCK_SIZE - state->buffered;
        size_t take = length < room ? length : room;
        memcpy(state->block + state->buffered, cursor, take);
        state->buffered += take;
        cursor += take;
        length -= take;
        if(state->buffered == BLOCK_SIZE) {
            compress(state->state, state->block);
            state->buffered = 0;
        }
    }

    while(length >= BLOCK_SIZE) {
        compress(state->state, cursor);
        cursor += BLOCK_SIZE;
        length -= BLOCK_SIZE;
    }

    if(length > 0) {
        memcpy(state->block, cursor, length);
        state->buffered = length;
    }
}

/* Append the 0x80 marker, pad with zeroes, and close with the bit length. */
static void pad(sha256_state *state) {
    uint64_t bits = state->length * 8;

    state->block[state->buffered++] = 0x80;
    if(state->buffered > LENGTH_OFFSET) {
        memset(state->block + state->buffered, 0, BLOCK_SIZE - state->buffered);
        compress(state->state, state->block);
        state->buffered = 0;
    }
    memset(state->block + state->buffered, 0, LENGTH_OFFSET - state->buffered);

    for(size_t i = 0; i < 8; i++)
        state->block[LENGTH_OFFSET + i] = (unsigned char)(bits >> (56 - i * 8));
    compress(state->state, state->block);
}

void sha256_finish(sha256_state *state, char *hex_out) {
    pad(state);

    static const char digits[] = "0123456789abcdef";
    for(size_t i = 0; i < 8; i++) {
        for(size_t byte = 0; byte < 4; byte++) {
            unsigned char value = (unsigned char)(state->state[i] >> (24 - byte * 8));
            hex_out[i * 8 + byte * 2] = digits[value >> 4];
            hex_out[i * 8 + byte * 2 + 1] = digits[value & 0x0f];
        }
    }
    hex_out[SHA256_HEX_SIZE - 1] = '\0';
}

bool sha256_file(const char *path, char *hex_out) {
    return sha256_file_watched(path, hex_out, NULL, NULL);
}

/* The file's size, so progress can be reported as a fraction.
 *
 * Zero when it cannot be told, which the watcher reads as "no denominator".
 * `ftell` is a `long`, which is 32 bits on the mingw side, so an artifact past
 * 2 GB reports no total — it still hashes, because hashing streams and never
 * needs the size. A missing denominator costs a percentage; using the wrong
 * one would cost the truth. */
static long long size_of(FILE *file) {
    if(fseek(file, 0, SEEK_END) != 0)
        return 0;
    long end = ftell(file);
    if(end < 0 || fseek(file, 0, SEEK_SET) != 0)
        return 0;
    return (long long)end;
}

bool sha256_file_watched(const char *path, char *hex_out, sha256_watcher watcher, void *context) {
    FILE *file = fopen(path, "rb");
    if(file == NULL)
        return false;

    long long total = watcher != NULL ? size_of(file) : 0;
    long long done = 0;

    sha256_state state;
    sha256_init(&state);

    static unsigned char chunk[SHA256_CHUNK_SIZE];
    size_t got;
    while((got = fread(chunk, 1, sizeof chunk, file)) > 0) {
        sha256_update(&state, chunk, got);
        done += (long long)got;
        if(watcher != NULL)
            watcher(done, total, context);
    }

    const bool ok = ferror(file) == 0;
    (void)fclose(file);
    if(ok)
        sha256_finish(&state, hex_out);
    return ok;
}

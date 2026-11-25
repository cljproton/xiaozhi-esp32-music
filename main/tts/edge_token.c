// Translation of edge-tts v2 Sec-MS-GEC token algorithm (rany2/edge-tts).
// Undocumented Microsoft protocol; may change without notice.
#include "edge_token_consts.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#define MASK 0xFFFFFFFFU

static uint32_t rol16(uint32_t v) { return ((v << 16) | (v >> 16)) & MASK; }
static uint32_t sigma0(uint32_t v) {
    return ((v >> 7) ^ (v << 25) ^ (v >> 18) ^ (v << 14) ^ (v >> 3)) & MASK;
}
static uint32_t sigma1(uint32_t v) {
    return ((v >> 17) ^ (v << 15) ^ (v >> 19) ^ (v << 13) ^ (v >> 10)) & MASK;
}
static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void expand_schedule(uint32_t* words, int first) {
    for (int i = first; i < 64; ++i) {
        uint64_t x = words[i - 2], y = words[i - 15];
        uint64_t s1 = (x >> 17) ^ (x << 15) ^ (x >> 19) ^ (x << 13) ^ (x >> 10);
        uint64_t s0 = (y >> 7) ^ (y << 25) ^ (y >> 18) ^ (y << 14) ^ (y >> 3);
        words[i] = (uint32_t)((s1 + words[i - 7] + s0 + words[i - 16]) & MASK);
    }
}

// SHA-256 rounds starting at `first`. K selects BLOCK constants when first==14.
#define ROTR64(v, n) ((uint32_t)((v) >> (n)))
static void sha_rounds(uint32_t state[8], uint32_t* words, int first) {
    uint32_t a=state[0],b=state[1],c=state[2],d=state[3];
    uint32_t e=state[4],f=state[5],g=state[6],h=state[7];
    const uint32_t* K = (first == 14) ? EDGE_SHA_BLOCK_K : EDGE_SHA_K;
    for (int i=first;i<64;++i){
        uint64_t re=(uint64_t)e*0x100000001ULL, ra=(uint64_t)a*0x100000001ULL;
        uint32_t S1=(uint32_t)(re>>6)^(uint32_t)(re>>11)^(uint32_t)(re>>25);
        uint32_t S0=(uint32_t)(ra>>2)^(uint32_t)(ra>>13)^(uint32_t)(ra>>22);
        uint32_t t1=(h+S1+(g^(e&(f^g)))+K[i]+words[i])&MASK;
        uint32_t t2=(S0+((a&b)|(c&(a|b))))&MASK;
        h=g;g=f;f=e;e=(d+t1)&MASK;d=c;c=b;b=a;a=(t1+t2)&MASK;
    }
    state[0]=a;state[1]=b;state[2]=c;state[3]=d;state[4]=e;state[5]=f;state[6]=g;state[7]=h;
}
static void sha_init_state(const uint8_t* data, uint32_t state[8]) {
    uint32_t words[64] = {0};
    words[14] = ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];  // first 3
    words[15] = be32(data + 3); // bytes 3..6
    for (int i = 16; i < 30; ++i) {
        uint32_t v = EDGE_SHA_SCHED[i - 16];
        if (i - 2 >= 14) {
            uint32_t s1 = sigma1(words[i - 2]);
            if (i == 16) s1 ^= EDGE_SHA_SIG1_ADJ;
            v += s1;
        }
        if (i - 7 >= 14) v += words[i - 7];
        if (i - 15 >= 14) {
            uint32_t s0 = sigma0(words[i - 15]);
            if (i == 29) s0 ^= EDGE_SHA_SIG0_ADJ;
            v += s0;
        }
        words[i] = v & MASK;
    }
    words[30] = (sigma1(words[28]) + words[23] + sigma0(words[15]) + words[14] +
                 EDGE_SHA_ADJ - EDGE_SHA_K[14]) & MASK;
    expand_schedule(words, 31);
    uint32_t work[8];
    memcpy(work, EDGE_SHA_STATE, sizeof(work));
    sha_rounds(work, words, 14);
    for (int i = 0; i < 8; ++i) state[i] = (EDGE_SHA_INIT[i] + work[i]) & MASK;
}

static void sha256_digest(const uint8_t* data, size_t len, uint8_t out[32]) {
    uint32_t state[8];
    sha_init_state(data, state);
    size_t stream_len = 57 + len;
    size_t zeros = (55 - stream_len) % 64;
    size_t padded_len = (len - 7) + 1 + zeros + 8;
    uint8_t padded[256] = {0};
    if (padded_len > sizeof(padded)) padded_len = sizeof(padded);
    memcpy(padded, data + 7, len - 7);
    padded[len - 7] = 0x80;
    uint64_t bits = (uint64_t)stream_len * 8;
    for (int i = 0; i < 8; ++i) padded[padded_len - 8 + i] = (uint8_t)(bits >> (56 - i * 8));
    uint32_t words[64];
    for (size_t off = 0; off < padded_len; off += 64) {
        for (int i = 0; i < 16; ++i) words[i] = be32(padded + off + i * 4);
        expand_schedule(words, 16);
        uint32_t working[8];
        memcpy(working, state, sizeof(working));
        sha_rounds(working, words, 0);
        for (int j = 0; j < 8; ++j) state[j] = (state[j] + working[j]) & MASK;
    }
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = (uint8_t)(state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)state[i];
    }
}

static void cs64_digest(const uint8_t* data, size_t len, const uint8_t digest[32],
                        uint32_t* out0, uint32_t* out1) {
    uint32_t ka = le32(digest) | 1;
    uint32_t kb = le32(digest + 4) | 1;
    uint32_t fa = (ka * EDGE_CS64_FACTOR) & MASK;
    uint32_t fb = (kb * EDGE_CS64_FACTOR) & MASK;
    uint32_t terms[64] = {0};
    uint32_t offsets[64] = {0};
    int nt = 0;
    for (int i = 0; i < 14; ++i) {
        // Python: coefficient=c, offset=o; term=c*factors[i%2], offset=o*factors[i%2]
        terms[nt] = (EDGE_CS64_A[i] * ((i % 2 == 0) ? fa : fb)) & MASK;
        offsets[nt] = EDGE_CS64_B[i];  // offsets are NOT multiplied by factors
        ++nt;
    }
    // Python: _words_le4_even(b"\x00" + data)
    uint8_t tmp[128] = {0};
    tmp[0] = 0;
    memcpy(tmp + 1, data, len);
    size_t dlen = len + 1;
    size_t padded = ((dlen + 3) / 4) * 4;
    if (padded % 8 != 0) padded += 4;
    data = tmp; len = dlen;
    int word_index = 0;
    for (size_t p = 0; p < padded; p += 4) {
        uint32_t word = le32(tmp + p);
        uint32_t term = (word * ((word_index % 2 == 0) ? ka : kb)) & MASK;
        uint32_t off = (rol16(word) * ((word_index % 2 == 0) ? 0xEF0569FB : 0xC31713DB)) & MASK;
        if (word_index == 0) { term = (term + EDGE_CS64_ADJ0 * fa) & MASK; off = (off + EDGE_CS64_ADJ1) & MASK; }
        terms[nt] = term; offsets[nt] = off; ++nt; ++word_index;
    }
    uint32_t swap_acc = 0, swap_last = 0, state = 0, acc = 0;
    for (int i = 0; i < nt; i += 2) {
        uint32_t x = terms[i], y = terms[i + 1];
        uint32_t t = rol16((offsets[i] + x) & MASK);
        uint32_t q = (t * 0x76F50F34) & MASK;
        swap_acc = (swap_acc + ((rol16(q) * 0xC3EFEA97 + q) & MASK)) & MASK;
        t = rol16((offsets[i + 1] + y) & MASK);
        q = (t * 0x1881CE3C) & MASK;
        swap_last = (rol16(q) * 0x35BD1EC9 + q) & MASK;
        swap_acc = (swap_acc + swap_last) & MASK;
        state = (state * ka + x) & MASK;
        state = (rol16(state) * 0xCF98B111) & MASK;
        state = (rol16(state) * 0x87085B9F) & MASK;
        state = (rol16(state) * 0x12CEB96D) & MASK;
        state = (rol16(state) * 0x257E1D83) & MASK;
        acc = (acc + state) & MASK;
        t = (state * kb + y) & MASK;
        t = (rol16(t) * 0xA27416F5) & MASK;
        t = (rol16(t) * 0xD38396FF) & MASK;
        state = (rol16((rol16(t) * 0x7C932B89) & MASK) * 0xBFA49F69) & MASK;
        acc = (acc + state) & MASK;
    }
    *out0 = swap_last ^ state;
    *out1 = swap_acc ^ acc;
}

void edge_token_v2(uint64_t unix_time, char* out, size_t out_len) {
    uint64_t ticks = unix_time + 11644473600ULL;
    ticks -= ticks % 300;
    uint64_t ns = ticks * 10000000ULL;
    char data[64];
    snprintf(data, sizeof(data), "%llu%s", (unsigned long long)ns, EDGE_TRUSTED_TOKEN);
    size_t len = strlen(data);
    uint8_t digest[32];
    sha256_digest((const uint8_t*)data, len, digest);
    uint32_t c0, c1;
    cs64_digest((const uint8_t*)data, len, digest, &c0, &c1);
    static const char* H = "0123456789ABCDEF";
    // Python: struct.pack("<II", v0, v1).hex().upper() -> v0 little-endian
    size_t pos = 0;
    uint32_t words[2] = {c0, c1};
    for (int w = 0; w < 2; ++w) {
        for (int i = 0; i < 4 && pos + 1 < out_len; ++i) {
            uint8_t byte = (uint8_t)(words[w] >> (i * 8));  // little-endian
            out[pos++] = H[byte >> 4];
            out[pos++] = H[byte & 0xF];
        }
    }
    if (pos < out_len) out[pos++] = '-';
    for (int i = 0; i < 32 && pos + 1 < out_len; ++i) {
        out[pos++] = H[digest[i] >> 4];
        out[pos++] = H[digest[i] & 0xF];
    }
    out[pos] = '\0';
}

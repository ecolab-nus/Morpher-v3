// simplified_sha.c
// Keep plain unsigned int / unsigned char as requested, but be explicit with casts.

#define SIZE 64      // 64 words/constants/rounds
#define HASH_SIZE 8

unsigned char input[SIZE];      // 512-bit block bytes (64 bytes)
unsigned int hash[HASH_SIZE];   // 8 x 32-bit hash words
unsigned int w[64];             // message schedule (64 words)
unsigned int k[64] = {
  0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
  0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
  0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
  0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
  0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
  0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
  0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
  0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
  0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
  0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
  0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
  0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
  0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
  0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
  0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
  0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

// Ensure shifts are done on unsigned ints and parenthesize
#define ROTR(x,n) ( (unsigned int)(( (unsigned int)(x) >> (n) ) | ( (unsigned int)(x) << (32u - (n)) )) )

#define CH(x,y,z)  ( ((unsigned int)(x) & (unsigned int)(y)) ^ (~(unsigned int)(x) & (unsigned int)(z)) )
#define MAJ(x,y,z) ( ((unsigned int)(x) & (unsigned int)(y)) ^ ((unsigned int)(x) & (unsigned int)(z)) ^ ((unsigned int)(y) & (unsigned int)(z)) )

#define BIG_SIGMA0(x) ( ROTR((x), 2u)  ^ ROTR((x), 13u) ^ ROTR((x), 22u) )
#define BIG_SIGMA1(x) ( ROTR((x), 6u)  ^ ROTR((x), 11u) ^ ROTR((x), 25u) )
#define SMALL_SIGMA0(x) ( ROTR((x), 7u)  ^ ROTR((x), 18u) ^ ( (unsigned int)(x) >> 3u ) )
#define SMALL_SIGMA1(x) ( ROTR((x), 17u) ^ ROTR((x), 19u) ^ ( (unsigned int)(x) >> 10u ) )

__attribute__((noinline))
void sha256() {
    unsigned int a, b, c, d, e, f, g, h;
    unsigned int t1, t2;

    a = hash[0]; b = hash[1]; c = hash[2]; d = hash[3];
    e = hash[4]; f = hash[5]; g = hash[6]; h = hash[7];

    for (int i = 0; i < 64; i++) {
        #ifdef CGRA_COMPILER
            please_map_me();
        #endif

        t1 = h + BIG_SIGMA1(e) + CH(e,f,g) + k[i] + w[i];
        t2 = BIG_SIGMA0(a) + MAJ(a,b,c);

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    hash[0] += a; hash[1] += b; hash[2] += c; hash[3] += d;
    hash[4] += e; hash[5] += f; hash[6] += g; hash[7] += h;
}

int main() {
    int i;

    // Dummy 512-bit message block (padded message)
    for (i = 0; i < 64; i++) {
        input[i] = (i == 0) ? 0x80u : 0x00u;
    }
    input[63] = 0x08u; // message length low byte

    // Init hash values (unsigned ints)
    hash[0] = 0x6a09e667u; hash[1] = 0xbb67ae85u;
    hash[2] = 0x3c6ef372u; hash[3] = 0xa54ff53au;
    hash[4] = 0x510e527fu; hash[5] = 0x9b05688cu;
    hash[6] = 0x1f83d9abu; hash[7] = 0x5be0cd19u;

    // Prepare w[0..15] — cast bytes to unsigned int BEFORE shifting
    for (i = 0; i < 16; i++) {
        unsigned int b0 = (unsigned int)input[i*4 + 0];
        unsigned int b1 = (unsigned int)input[i*4 + 1];
        unsigned int b2 = (unsigned int)input[i*4 + 2];
        unsigned int b3 = (unsigned int)input[i*4 + 3];
        w[i] = (b0 << 24u) | (b1 << 16u) | (b2 << 8u) | (b3);
    }

    // Extend message schedule safely (operating on unsigned int)
    for (i = 16; i < 64; i++) {
        unsigned int s1 = SMALL_SIGMA1(w[i - 2]);
        unsigned int s0 = SMALL_SIGMA0(w[i - 15]);
        w[i] = s1 + w[i - 7] + s0 + w[i - 16];
    }

    sha256();

    return 0;
}

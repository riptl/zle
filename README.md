**ZLE** (zero-run length encoding) is a compression algorithm for sparse
binary data.

ZLE only compresses zero bytes.  Specifically, occurrences of three
consecutive zero bytes or more.

Some nice properties:
- ZLE achieves 10 GB/s compression speed (AMD Zen5), even for small inputs
- ZLE skips over uncompressible bytes very quickly
- `libzle` expands a worst case input by at most n/386+6 bytes

That said, ZLE cannot compress text or images, and has a worse
compression ratio for binary data.

## Performance

Some inputs on which ZLE performs well on:

**Compression speed and ratio**

| File                |   | zle MB/s   | lz4 MB/s  | zstd MB/s |   | zle %    | lz4 % | zstd % |
|---------------------|---|-----------:|----------:|----------:|---|---------:|------:|-------:|
| zero-512k           |   | **233640** |   33164   |     20521 |   |    0.0   |   0.4 |    0.0 |
| **solana/acct_m**   |   |  **77282** |   30000   |      9160 |   |   30.2   |  30.5 |   30.1 |
| random-512k         |   |  **54574** |   42256   |     16053 |   |  100.0   | 100.4 |  100.0 |
| silesia/mr          |   |  **28492** |     982   |       551 |   |   72.4   |  56.1 |   38.3 |
| calgary/obj1        |   |  **28109** |    2722   |       780 |   |   85.2   |  64.9 |   51.8 |
| calgary/pic         |   |  **16229** |    2129   |      1214 |   |   18.9   |  17.8 |   10.1 |
| **solana/acct**     |   |  **10606** |    2439   |       200 |   |   33.6   |  35.6 |   33.5 |
| cantrbry/sum        |   |  **10214** |    1992   |       850 |   |   86.4   |  52.7 |   35.6 |
| silesia/mozilla     |   |   **8956** |     914   |       604 |   |   88.8   |  54.6 |   39.0 |

**Decompression speed**

| File                | zle MB/s   | lz4 MB/s  | zstd MB/s |
|---------------------|-----------:|----------:|----------:|
| zero-512k           | **256753** |   28922   |     79242 |
| **solana/acct_m**   |  **94794** |   30968   |     77350 |
| random-512k         |  **83746** |   76812   |     80954 |
| silesia/mr          |  **52252** |    5094   |      1998 |
| calgary/obj1        |  **43274** |    9053   |      1922 |
| calgary/pic         |  **22730** |   13538   |      6000 |
| **solana/acct**     |  **12650** |    6205   |      3522 |
| cantrbry/sum        |  **11320** |    5206   |      1973 |
| silesia/mozilla     |  **13092** |    4346   |      1540 |

The Solana inputs come from a mainnet incremental snapshot (slot
447682120).  `solana/acct` is the data region of every non-empty account
(1,558,253 accounts, 826 MB), each compressed independently (`lzbench -j`).
`solana/acct_m` is a single 8.5 MB account storage file holding many
accounts with their headers.  The other inputs are single files.

The benchmark program is lzbench 2.3.1 running on AMD EPYC 9555P (Zen5)
with 2 MiB pages.

The contenders are:
- `zle 2.0.0`, Clang 19
- `lz4 1.10.0 --fast -3` GCC 15
- `zstd 1.5.7 -1`, GCC 15

(`lz4 -1` is only marginally faster, `lz4` and `zstd` slightly disadvantaged by `GCC`.)

## Usage

This repository provides the `zle` command and the `libzle` C library.

```
make
sudo make install
curl -sSfO https://r2.wii.dev/zle/accounts.tar
zle accounts.tar
zle -dc accounts.tar.zle | tar -t
```

## Motivation

ZLE was originally developed to compress Solana account data, where it
outperforms Zstandard and LZ4 in speed and compression ratio.

About 30% of Solana accounts have a data region that looks as follows.

```
0000:   c6 fa 7a f3  be db ad 3a  3d 65 f3 6a  ab c9 74 31   ..z....:=e.j..t1
0010:   b1 bb e4 c2  d2 f6 e0 e4  7c a6 02 03  45 2f 5d 61   ........|...E/]a
0020:   09 46 b3 2a  b9 ce 4d af  19 9b ce 6b  76 85 be bf   .F.*..M....kv...
0030:   30 96 ad 1f  24 83 8b 83  a4 82 0f 85  95 52 e5 45   0...$........R.E
0040:   93 34 bf 03  00 00 00 00  00 00 00 00  00 00 00 00   .4..............
0050:   00 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00   ................
0060:   00 00 00 00  00 00 00 00  00 00 00 00  01 00 00 00   ................
0070:   00 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00   ................
0080:   00 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00   ................
0090:   00 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00   ................
00a0:   00 00 00 00  00                                      .....
```

Mostly zeroes.  In fact, 72% of Solana mainnet's ~310 GB of account data
is zeroes.  The rest is dominated by hashes and cryptographic curve
points, which cannot be compressed.

Naturally, the solution is to only compress zeroes.
And so, the ZLE format was born.

After ZLE compression, the above blob is so information dense that a
general-purpose compressor cannot squeeze it further.

```
0000:  [44]c6 fa 7a  f3 be db ad  3a 3d 65 f3  6a ab c9 74
0010:   31 b1 bb e4  c2 d2 f6 e0  e4 7c a6 02  03 45 2f 5d
0020:   61 09 46 b3  2a b9 ce 4d  af 19 9b ce  6b 76 85 be
0030:   bf 30 96 ad  1f 24 83 8b  83 a4 82 0f  85 95 52 e5
0040:   45 93 34 bf  03[28][01]01 [38]
```

ZLE codes data as a sequence of tokens.  A token is a literal count L,
L literal bytes, and a zero count Z.  Counts below 255 take one byte.

```
  raw     ff 00 00 00    13 37 00 00 00 00
  zle [01]ff[03]     [02]13 37[04]
       ^    ^         ^        ^
       |    |         |        \ Z=4 zero    bytes
       |    |         \ L=2 literal bytes
       |    \ Z=3 zero    bytes
       \ L=1 literal byte
```

Zero runs shorter than 3 bytes stay in the literals, unless they end
the input.  The last token has no zero count if the input ends in a
literal.

Full grammar:

```
frame        {token} [last]
token        L literals Z
last         L literals

L, Z         value (u8)                   if value<255
             0xff leb128( value-255 )     otherwise
literals     L bytes verbatim
```

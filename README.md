**ZLE** (zero-run length encoding) is a compression algorithm for sparse
binary data.

ZLE only compresses zero bytes.  Specifically, occurrences of three
consecutive zero bytes or more.

Some nice properties:
- ZLE achieves 10 GB/s compression speed (AMD Zen5), even for small inputs
- ZLE skips over uncompressible bytes very quickly
- `libzle` expands a worst case input by at most 8 bytes

That said, ZLE cannot compress text or images, and has a worse
compression ratio for binary data.

## Performance

Some inputs on which ZLE performs well on:

**Compression speed and ratio**

| File                |   | zle MB/s   | lz4 MB/s  | zstd MB/s |   | zle %    | lz4 % | zstd % |
|---------------------|---|-----------:|----------:|----------:|---|---------:|------:|-------:|
| zero-512k           |   | **221967** |   32926   |     21394 |   |    0.0   |   0.4 |    0.0 |
| random-512k         |   |  **62924** |   42216   |     17091 |   |  100.0   | 100.4 |  100.0 |
| calgary/obj1        |   |  **16656** |    2975   |       791 |   |   84.9   |  64.9 |   51.8 |
| silesia/mr          |   |  **16168** |    1075   |       605 |   |   72.4   |  56.1 |   38.3 |
| calgary/pic         |   |  **11497** |    2273   |      1360 |   |   18.0   |  17.8 |   10.1 |
| **solana/acct**     |   |   **8656** |    2174   |       181 |   | **30.1** |  33.5 |   33.1 |
| **solana/acct_m**   |   |   **7550** |    2002   |      1382 |   |   30.6   |  27.9 |   23.7 |
| silesia/mozilla     |   |   **7278** |    1006   |       651 |   |   87.5   |  54.6 |   39.0 |
| cantrbry/sum        |   |   **6215** |    2111   |       886 |   |   82.4   |  52.7 |   35.6 |

**Decompression speed**

| File                | zle MB/s   | lz4 MB/s  | zstd MB/s |
|---------------------|-----------:|----------:|----------:|
| zero-512k           | **261882** |   29762   |     78088 |
| random-512k         | **106324** |   93958   |     81335 |
| calgary/obj1        |  **35022** |    9578   |      1881 |
| silesia/mr          |  **58589** |    5448   |      1948 |
| calgary/pic         |    19293   | **19352** |      5573 |
| **solana/acct**     |   **8981** |    3448   |      2688 |
| **solana/acct_m**   |  **12155** |    8746   |      4828 |
| silesia/mozilla     |  **12914** |    4685   |      1561 |
| cantrbry/sum        |  **10947** |    5995   |      1892 |

Each test input is a single file except for `solana/acct_m` which is
the input that inspired this algorithm (see [Motivation](#motivation)).

The benchmark program is lzbench running on AMD EPYC 9555P (Zen5) with 2 MiB pages.

The contenders are:
- `zle 1.0.0`, Clang 21
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
0000:  [ff 35]c6 fa  7a f3 be db  ad 3a 3d 65  f3 6a ab c9
0010:   74 31 b1 bb  e4 c2 d2 f6  e0 e4 7c a6  02 03 45 2f
0020:   5d 61 09 46  b3 2a b9 ce  4d af 19 9b  ce 6b 76 85
0030:   be bf 30 96  ad 1f 24 83  8b 83 a4 82  0f 85 95 52
0040:   e5 45 93 34  bf 03[19 1f] 01[29]
```

ZLE uses a wire coding inspired by JPEG's RLE and LZ4 (4 bits literal,
4 bits repeat).

```
  raw     ff 00 00 00    13 37 00 00 00 00
  zle [13]ff         [24]13 37
       ^              ^
       |              |
       |              | L=2 literal bytes
       |              \ Z=4 zero    bytes
       |
       | L=1 literal byte
       \ Z=3 zero    bytes
```

Full grammar:

```
frame        {block}
block        header [lit_ext] literals [zero_ext]

header       (min(L,15)<<4) | min(Z,15)      (u8)
lit_ext      leb128( L-15 )  only if L>=15
literals     L bytes verbatim
zero_ext     leb128( Z-15 )  only if Z>=15
```

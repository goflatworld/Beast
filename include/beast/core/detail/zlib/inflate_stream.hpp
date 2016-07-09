//
// Copyright (c) 2013-2016 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// This is a derivative work based on Zlib, copyright below:
/*
    Copyright (C) 1995-2013 Jean-loup Gailly and Mark Adler

    This software is provided 'as-is', without any express or implied
    warranty.  In no event will the authors be held liable for any damages
    arising from the use of this software.

    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
       claim that you wrote the original software. If you use this software
       in a product, an acknowledgment in the product documentation would be
       appreciated but is not required.
    2. Altered source versions must be plainly marked as such, and must not be
       misrepresented as being the original software.
    3. This notice may not be removed or altered from any source distribution.

    Jean-loup Gailly        Mark Adler
    jloup@gzip.org          madler@alumni.caltech.edu

    The data format used by the zlib library is described by RFCs (Request for
    Comments) 1950 to 1952 in the files http://tools.ietf.org/html/rfc1950
    (zlib format), rfc1951 (deflate format) and rfc1952 (gzip format).
*/

#ifndef BEAST_CORE_DETAIL_INFLATE_STREAM_HPP
#define BEAST_CORE_DETAIL_INFLATE_STREAM_HPP

#include <beast/core/detail/zlib/detail/inflate_tables.hpp>

#include "zconf.hpp"
#include "zlib.hpp"
#include <cstdint>
#include <cstdlib>

namespace beast {

/* Possible inflate modes between inflate() calls */

enum inflate_mode
{
    HEAD,       /* i: waiting for magic header */
    FLAGS,      /* i: waiting for method and flags (gzip) */
    TIME,       /* i: waiting for modification time (gzip) */
    OS,         /* i: waiting for extra flags and operating system (gzip) */
    EXLEN,      /* i: waiting for extra length (gzip) */
    EXTRA,      /* i: waiting for extra bytes (gzip) */
    NAME,       /* i: waiting for end of file name (gzip) */
    COMMENT,    /* i: waiting for end of comment (gzip) */
    HCRC,       /* i: waiting for header crc (gzip) */
    TYPE,       /* i: waiting for type bits, including last-flag bit */
    TYPEDO,     /* i: same, but skip check to exit inflate on new block */
    STORED,     /* i: waiting for stored size (length and complement) */
    COPY_,      /* i/o: same as COPY below, but only first time in */
    COPY,       /* i/o: waiting for input or output to copy stored block */
    TABLE,      /* i: waiting for dynamic block table lengths */
    LENLENS,    /* i: waiting for code length code lengths */
    CODELENS,   /* i: waiting for length/lit and distance code lengths */
        LEN_,   /* i: same as LEN below, but only first time in */
        LEN,    /* i: waiting for length/lit/eob code */
        LENEXT, /* i: waiting for length extra bits */
        DIST,   /* i: waiting for distance code */
        DISTEXT,/* i: waiting for distance extra bits */
        MATCH,  /* o: waiting for output space to copy string */
        LIT,    /* o: waiting for output space to write literal */
    CHECK,      /* i: waiting for 32-bit check value */
    LENGTH,     /* i: waiting for 32-bit length (gzip) */
    DONE,       /* finished check, done -- remain here until reset */
    BAD,        /* got a data error -- remain here until reset */
    MEM,        /* got an inflate() memory error -- remain here until reset */
    SYNC        /* looking for synchronization bytes to restart inflate() */
};

/*
    State transitions between above modes -

    (most modes can go to BAD or MEM on error -- not shown for clarity)

    Process header:
        HEAD -> (gzip) or (zlib) or (raw)
        (gzip) -> FLAGS -> TIME -> OS -> EXLEN -> EXTRA -> NAME -> COMMENT ->
                  HCRC -> TYPE
        (zlib) -> DICTID or TYPE
        DICTID -> DICT -> TYPE
        (raw) -> TYPEDO
    Read deflate blocks:
            TYPE -> TYPEDO -> STORED or TABLE or LEN_ or CHECK
            STORED -> COPY_ -> COPY -> TYPE
            TABLE -> LENLENS -> CODELENS -> LEN_
            LEN_ -> LEN
    Read deflate codes in fixed or dynamic block:
                LEN -> LENEXT or LIT or TYPE
                LENEXT -> DIST -> DISTEXT -> MATCH -> LEN
                LIT -> LEN
    Process trailer:
        CHECK -> LENGTH -> DONE
 */

/* state maintained between inflate() calls.  Approximately 10K bytes. */
class inflate_stream : public z_stream
{
public:
    inflate_mode mode;          /* current inflate mode */
    int last;                   /* true if processing last block */
    int flags;                  /* gzip header method and flags (0 if zlib) */
    unsigned dmax;              /* zlib header max distance (INFLATE_STRICT) */
    unsigned long total;        /* protected copy of output count */
        /* sliding window */
    unsigned wbits;             /* log base 2 of requested window size */
    unsigned wsize;             /* window size or zero if not using window */
    unsigned whave;             /* valid bytes in the window */
    unsigned wnext;             /* window write index */
    unsigned char *window;  /* allocated sliding window, if needed */
        /* bit accumulator */
    unsigned long hold;         /* input bit accumulator */
    unsigned bits;              /* number of bits in "in" */
        /* for string and stored block copying */
    unsigned length;            /* literal or length of data to copy */
    unsigned offset;            /* distance back to copy string from */
        /* for table and code decoding */
    unsigned extra;             /* extra bits needed */
        /* fixed and dynamic code tables */
    code const *lencode;    /* starting table for length/literal codes */
    code const *distcode;   /* starting table for distance codes */
    unsigned lenbits;           /* index bits for lencode */
    unsigned distbits;          /* index bits for distcode */
        /* dynamic table building */
    unsigned ncode;             /* number of code length code lengths */
    unsigned nlen;              /* number of length code lengths */
    unsigned ndist;             /* number of distance code lengths */
    unsigned have;              /* number of code lengths in lens[] */
    code *next;             /* next available space in codes[] */
    unsigned short lens[320];   /* temporary storage for code lengths */
    unsigned short work[288];   /* work area for code table building */
    code codes[ENOUGH];         /* space for code tables */
    int sane;                   /* if false, allow invalid distance too far */
    int back;                   /* bits back of last unprocessed length/lit */
    unsigned was;               /* initial length of match */
};

extern void inflate_fast (inflate_stream* strm, unsigned start);

extern int inflate (inflate_stream* strm, int flush);
extern int inflateEnd (inflate_stream* strm);
extern int inflateSetDictionary (inflate_stream* strm, const Byte *dictionary, uInt  dictLength);
extern int inflateGetDictionary (inflate_stream* strm, Byte *dictionary, uInt  *dictLength);
extern int inflateReset (inflate_stream* strm);
extern int inflateReset2 (inflate_stream* strm, int windowBits);
extern int inflateInit_ (inflate_stream* strm,
                                     const char *version, int stream_size);
extern int inflateInit2_ (inflate_stream* strm, int  windowBits,
                                      const char *version, int stream_size);
#define inflateInit(strm) \
        inflateInit_((strm), ZLIB_VERSION, (int)sizeof(z_stream))
#define inflateInit2(strm, windowBits) \
        inflateInit2_((strm), (windowBits), ZLIB_VERSION, \
                      (int)sizeof(z_stream))

} // beast

#endif
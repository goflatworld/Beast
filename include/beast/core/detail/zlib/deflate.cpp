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

#include <beast/core/detail/zlib/deflate_stream.hpp>
#include <cstring>
#include <memory>

/*
 *  ALGORITHM
 *
 *      The "deflation" process depends on being able to identify portions
 *      of the input text which are identical to earlier input (within a
 *      sliding window trailing behind the input currently being processed).
 *
 *      The most straightforward technique turns out to be the fastest for
 *      most input files: try all possible matches and select the longest.
 *      The key feature of this algorithm is that insertions into the string
 *      dictionary are very simple and thus fast, and deletions are avoided
 *      completely. Insertions are performed at each input character, whereas
 *      string matches are performed only when the previous match ends. So it
 *      is preferable to spend more time in matches to allow very fast string
 *      insertions and avoid deletions. The matching algorithm for small
 *      strings is inspired from that of Rabin & Karp. A brute force approach
 *      is used to find longer strings when a small match has been found.
 *      A similar algorithm is used in comic (by Jan-Mark Wams) and freeze
 *      (by Leonid Broukhis).
 *         A previous version of this file used a more sophisticated algorithm
 *      (by Fiala and Greene) which is guaranteed to run in linear amortized
 *      time, but has a larger average cost, uses more memory and is patented.
 *      However the F&G algorithm may be faster for some highly redundant
 *      files if the parameter max_chain_length (described below) is too large.
 *
 *  ACKNOWLEDGEMENTS
 *
 *      The idea of lazy evaluation of matches is due to Jan-Mark Wams, and
 *      I found it in 'freeze' written by Leonid Broukhis.
 *      Thanks to many people for bug reports and testing.
 *
 *  REFERENCES
 *
 *      Deutsch, L.P.,"DEFLATE Compressed Data Format Specification".
 *      Available in http://tools.ietf.org/html/rfc1951
 *
 *      A description of the Rabin and Karp algorithm is given in the book
 *         "Algorithms" by R. Sedgewick, Addison-Wesley, p252.
 *
 *      Fiala,E.R., and Greene,D.H.
 *         Data Compression with Finite Windows, Comm.ACM, 32,4 (1989) 490-595
 *
 */

namespace beast {

deflate_stream::deflate_stream()
{
    // default level 6
    //deflateInit2(this, 6, Z_DEFLATED, 15, DEF_MEM_LEVEL, Z_DEFAULT_STRATEGY);
}

deflate_stream::~deflate_stream()
{
    deflateEnd(this);
}

//-------------------------------------------------------------------------------

#ifndef DEBUG
# define _tr_tally_lit(s, c, flush) \
  { std::uint8_t cc = (c); \
    s->d_buf_[s->last_lit_] = 0; \
    s->l_buf_[s->last_lit_++] = cc; \
    s->dyn_ltree_[cc].fc++; \
    flush = (s->last_lit_ == s->lit_bufsize_-1); \
   }
# define _tr_tally_dist(s, distance, length, flush) \
  { std::uint8_t len = (length); \
    std::uint16_t dist = (distance); \
    s->d_buf_[s->last_lit_] = dist; \
    s->l_buf_[s->last_lit_++] = len; \
    dist--; \
    s->dyn_ltree_[_length_code[len]+LITERALS+1].fc++; \
    s->dyn_dtree_[d_code(dist)].fc++; \
    flush = (s->last_lit_ == s->lit_bufsize_-1); \
  }
#else
# define _tr_tally_lit(s, c, flush) flush = _tr_tally(s, 0, c)
# define _tr_tally_dist(s, distance, length, flush) \
              flush = _tr_tally(s, distance, length)
#endif

/* ===========================================================================
 *  Function prototypes.
 */

using compress_func = block_state(*)(deflate_stream*, int flush);

#ifdef DEBUG
local  void check_match (deflate_stream *s, IPos start, IPos match,
                            int length);
#endif

/* ===========================================================================
 * Local data
 */

#define NIL 0
/* Tail of hash chains */

#ifndef TOO_FAR
#  define TOO_FAR 4096
#endif
/* Matches of length 3 are discarded if their distance exceeds TOO_FAR */

/* Values for max_lazy_match, good_match and max_chain_length, depending on
 * the desired pack level (0..9). The values given below have been tuned to
 * exclude worst case performance for pathological files. Better values may be
 * found for specific files.
 */
typedef struct config_s {
   std::uint16_t good_length; /* reduce lazy search above this match length */
   std::uint16_t max_lazy;    /* do not perform lazy search above this match length */
   std::uint16_t nice_length; /* quit search above this match length */
   std::uint16_t max_chain;
   compress_func func;
} config;

local const config configuration_table[10] = {
/*      good lazy nice chain */
/* 0 */ {0,    0,  0,    0, &deflate_stream::deflate_stored},  /* store only */
/* 1 */ {4,    4,  8,    4, &deflate_stream::deflate_fast}, /* max speed, no lazy matches */
/* 2 */ {4,    5, 16,    8, &deflate_stream::deflate_fast},
/* 3 */ {4,    6, 32,   32, &deflate_stream::deflate_fast},

/* 4 */ {4,    4, 16,   16, &deflate_stream::deflate_slow},  /* lazy matches */
/* 5 */ {8,   16, 32,   32, &deflate_stream::deflate_slow},
/* 6 */ {8,   16, 128, 128, &deflate_stream::deflate_slow},
/* 7 */ {8,   32, 128, 256, &deflate_stream::deflate_slow},
/* 8 */ {32, 128, 258, 1024, &deflate_stream::deflate_slow},
/* 9 */ {32, 258, 258, 4096, &deflate_stream::deflate_slow}}; /* max compression */

/* Note: the deflate() code requires max_lazy >= MIN_MATCH and max_chain >= 4
 * For deflate_fast() (levels <= 3) good is ignored and lazy has a different
 * meaning.
 */

#define EQUAL 0
/* result of memcmp for equal strings */

#ifndef NO_DUMMY_DECL
struct static_tree_desc {int dummy;}; /* for buggy compilers */
#endif

/* rank Z_BLOCK between Z_NO_FLUSH and Z_PARTIAL_FLUSH */
#define RANK(f) (((f) << 1) - ((f) > 4 ? 9 : 0))

/* ===========================================================================
 * Update a hash value with the given input byte
 * IN  assertion: all calls to to UPDATE_HASH are made with consecutive
 *    input characters, so that a running hash key can be computed from the
 *    previous key instead of complete recalculation each time.
 */
#define UPDATE_HASH(s,h,c) (h = (((h)<<s->hash_shift_) ^ (c)) & s->hash_mask_)


/* ===========================================================================
 * Insert string str in the dictionary and set match_head to the previous head
 * of the hash chain (the most recent string with same hash key). Return
 * the previous length of the hash chain.
 * If this file is compiled with -DFASTEST, the compression level is forced
 * to 1, and no hash chains are maintained.
 * IN  assertion: all calls to to INSERT_STRING are made with consecutive
 *    input characters and the first MIN_MATCH bytes of str are valid
 *    (except for the last MIN_MATCH-1 bytes of the input file).
 */
#define INSERT_STRING(s, str, match_head) \
   (UPDATE_HASH(s, s->ins_h_, s->window_[(str) + (MIN_MATCH-1)]), \
    match_head = s->prev_[(str) & s->w_mask_] = s->head_[s->ins_h_], \
    s->head_[s->ins_h_] = (std::uint16_t)(str))

/* ===========================================================================
 * Initialize the hash table (avoiding 64K overflow for 16 bit systems).
 * prev[] will be initialized on the fly.
 */
#define CLEAR_HASH(s) \
    s->head_[s->hash_size_-1] = NIL; \
    std::memset((Byte *)s->head_, 0, (unsigned)(s->hash_size_-1)*sizeof(*s->head_));

/* ========================================================================= */

int
deflate_stream::deflateInit(deflate_stream* strm, int level)
{
    return deflateInit2(strm, level, Z_DEFLATED, 15, DEF_MEM_LEVEL,
                         Z_DEFAULT_STRATEGY);
    /* To do: ignore strm->next_in if we use it as window */
}

void deflate_stream::fill_window(deflate_stream *s)
{
    unsigned n, m;
    std::uint16_t *p;
    unsigned more;    /* Amount of free space at the end of the window. */
    uInt wsize = s->w_size_;

    Assert(s->lookahead_ < MIN_LOOKAHEAD, "already enough lookahead");

    do {
        more = (unsigned)(s->window_size_ -(std::uint32_t)s->lookahead_ -(std::uint32_t)s->strstart_);

        /* Deal with !@#$% 64K limit: */
        if (sizeof(int) <= 2) {
            if (more == 0 && s->strstart_ == 0 && s->lookahead_ == 0) {
                more = wsize;

            } else if (more == (unsigned)(-1)) {
                /* Very unlikely, but possible on 16 bit machine if
                 * strstart == 0 && lookahead == 1 (input done a byte at time)
                 */
                more--;
            }
        }

        /* If the window is almost full and there is insufficient lookahead,
         * move the upper half to the lower one to make room in the upper half.
         */
        if (s->strstart_ >= wsize+MAX_DIST(s)) {

            std::memcpy(s->window_, s->window_+wsize, (unsigned)wsize);
            s->match_start_ -= wsize;
            s->strstart_    -= wsize; /* we now have strstart >= MAX_DIST */
            s->block_start_ -= (long) wsize;

            /* Slide the hash table (could be avoided with 32 bit values
               at the expense of memory usage). We slide even when level == 0
               to keep the hash table consistent if we switch back to level > 0
               later. (Using level 0 permanently is not an optimal usage of
               zlib, so we don't care about this pathological case.)
             */
            n = s->hash_size_;
            p = &s->head_[n];
            do {
                m = *--p;
                *p = (std::uint16_t)(m >= wsize ? m-wsize : NIL);
            } while (--n);

            n = wsize;
            p = &s->prev_[n];
            do {
                m = *--p;
                *p = (std::uint16_t)(m >= wsize ? m-wsize : NIL);
                /* If n is not on any hash chain, prev[n] is garbage but
                 * its value will never be used.
                 */
            } while (--n);
            more += wsize;
        }
        if (s->avail_in == 0) break;

        /* If there was no sliding:
         *    strstart <= WSIZE+MAX_DIST-1 && lookahead <= MIN_LOOKAHEAD - 1 &&
         *    more == window_size - lookahead - strstart
         * => more >= window_size - (MIN_LOOKAHEAD-1 + WSIZE + MAX_DIST-1)
         * => more >= window_size - 2*WSIZE + 2
         * In the BIG_MEM or MMAP case (not yet supported),
         *   window_size == input_size + MIN_LOOKAHEAD  &&
         *   strstart + s->lookahead_ <= input_size => more >= MIN_LOOKAHEAD.
         * Otherwise, window_size == 2*WSIZE so more >= 2.
         * If there was sliding, more >= WSIZE. So in all cases, more >= 2.
         */
        Assert(more >= 2, "more < 2");

        n = read_buf(s, s->window_ + s->strstart_ + s->lookahead_, more);
        s->lookahead_ += n;

        /* Initialize the hash value now that we have some input: */
        if (s->lookahead_ + s->insert_ >= MIN_MATCH) {
            uInt str = s->strstart_ - s->insert_;
            s->ins_h_ = s->window_[str];
            UPDATE_HASH(s, s->ins_h_, s->window_[str + 1]);
            while (s->insert_) {
                UPDATE_HASH(s, s->ins_h_, s->window_[str + MIN_MATCH-1]);
                s->prev_[str & s->w_mask_] = s->head_[s->ins_h_];
                s->head_[s->ins_h_] = (std::uint16_t)str;
                str++;
                s->insert_--;
                if (s->lookahead_ + s->insert_ < MIN_MATCH)
                    break;
            }
        }
        /* If the whole input has less than MIN_MATCH bytes, ins_h is garbage,
         * but this is not important since only literal bytes will be emitted.
         */

    } while (s->lookahead_ < MIN_LOOKAHEAD && s->avail_in != 0);

    /* If the WIN_INIT bytes after the end of the current data have never been
     * written, then zero those bytes in order to avoid memory check reports of
     * the use of uninitialized (or uninitialised as Julian writes) bytes by
     * the longest match routines.  Update the high water mark for the next
     * time through here.  WIN_INIT is set to MAX_MATCH since the longest match
     * routines allow scanning to strstart + MAX_MATCH, ignoring lookahead.
     */
    if (s->high_water_ < s->window_size_) {
        std::uint32_t curr = s->strstart_ + (std::uint32_t)(s->lookahead_);
        std::uint32_t init;

        if (s->high_water_ < curr) {
            /* Previous high water mark below current data -- zero WIN_INIT
             * bytes or up to end of window, whichever is less.
             */
            init = s->window_size_ - curr;
            if (init > WIN_INIT)
                init = WIN_INIT;
            std::memset(s->window_ + curr, 0, (unsigned)init);
            s->high_water_ = curr + init;
        }
        else if (s->high_water_ < (std::uint32_t)curr + WIN_INIT) {
            /* High water mark at or above current data, but below current data
             * plus WIN_INIT -- zero out to current data plus WIN_INIT, or up
             * to end of window, whichever is less.
             */
            init = (std::uint32_t)curr + WIN_INIT - s->high_water_;
            if (init > s->window_size_ - s->high_water_)
                init = s->window_size_ - s->high_water_;
            std::memset(s->window_ + s->high_water_, 0, (unsigned)init);
            s->high_water_ += init;
        }
    }

    Assert((std::uint32_t)s->strstart_ <= s->window_size_ - MIN_LOOKAHEAD,
           "not enough room for search");
}

/* ========================================================================= */

int
deflate_stream::deflateInit2(
    deflate_stream* strm,
    int  level,
    int  method,
    int  windowBits,
    int  memLevel,
    int  strategy)
{
    std::uint16_t *overlay;
    /* We overlay pending_buf and d_buf+l_buf. This works since the average
     * output size for (length,distance) codes is <= 24 bits.
     */

    if (strm == Z_NULL)
        return Z_STREAM_ERROR;

    strm->msg = Z_NULL;

    if (level == Z_DEFAULT_COMPRESSION) level = 6;

    if (windowBits < 0) { /* suppress zlib wrapper */
        windowBits = -windowBits;
    }
    if (memLevel < 1 || memLevel > MAX_MEM_LEVEL || method != Z_DEFLATED ||
        windowBits < 8 || windowBits > 15 || level < 0 || level > 9 ||
        strategy < 0 || strategy > Z_FIXED) {
        return Z_STREAM_ERROR;
    }
    if (windowBits == 8) windowBits = 9;  /* until 256-byte window bug fixed */
    auto s = strm;

    s->w_bits_ = windowBits;
    s->w_size_ = 1 << s->w_bits_;
    s->w_mask_ = s->w_size_ - 1;

    s->hash_bits_ = memLevel + 7;
    s->hash_size_ = 1 << s->hash_bits_;
    s->hash_mask_ = s->hash_size_ - 1;
    s->hash_shift_ =  ((s->hash_bits_+MIN_MATCH-1)/MIN_MATCH);

    s->window_ = (Byte *) std::malloc(s->w_size_ * 2*sizeof(Byte));
    s->prev_   = (std::uint16_t *)  std::malloc(s->w_size_ * sizeof(std::uint16_t));
    s->head_   = (std::uint16_t *)  std::malloc(s->hash_size_ * sizeof(std::uint16_t));

    s->high_water_ = 0;      /* nothing written to s->window_ yet */

    s->lit_bufsize_ = 1 << (memLevel + 6); /* 16K elements by default */

    overlay = (std::uint16_t *) std::malloc(s->lit_bufsize_ * (sizeof(std::uint16_t)+2));
    s->pending_buf_ = (std::uint8_t *) overlay;
    s->pending_buf_size_ = (std::uint32_t)s->lit_bufsize_ * (sizeof(std::uint16_t)+2L);

    if (s->window_ == Z_NULL || s->prev_ == Z_NULL || s->head_ == Z_NULL ||
        s->pending_buf_ == Z_NULL) {
        s->status_ = FINISH_STATE;
        strm->msg = z_errmsg[Z_NEED_DICT-Z_MEM_ERROR];
        deflateEnd (strm);
        return Z_MEM_ERROR;
    }
    s->d_buf_ = overlay + s->lit_bufsize_/sizeof(std::uint16_t);
    s->l_buf_ = s->pending_buf_ + (1+sizeof(std::uint16_t))*s->lit_bufsize_;

    s->level_ = level;
    s->strategy_ = strategy;

    return deflateReset(strm);
}

/* ========================================================================= */

int
deflate_stream::deflateSetDictionary (
    const Byte *dictionary,
    uInt  dictLength)
{
auto strm = this;
    uInt str, n;
    unsigned avail;
    const unsigned char *next;

    auto s = strm;

    if (s->lookahead_)
        return Z_STREAM_ERROR;

    /* if dictionary would fill window, just replace the history */
    if (dictLength >= s->w_size_) {
        CLEAR_HASH(s);
        s->strstart_ = 0;
        s->block_start_ = 0L;
        s->insert_ = 0;
        dictionary += dictLength - s->w_size_;  /* use the tail */
        dictLength = s->w_size_;
    }

    /* insert dictionary into window and hash */
    avail = strm->avail_in;
    next = strm->next_in;
    strm->avail_in = dictLength;
    strm->next_in = (const Byte *)dictionary;
    fill_window(s);
    while (s->lookahead_ >= MIN_MATCH) {
        str = s->strstart_;
        n = s->lookahead_ - (MIN_MATCH-1);
        do {
            UPDATE_HASH(s, s->ins_h_, s->window_[str + MIN_MATCH-1]);
            s->prev_[str & s->w_mask_] = s->head_[s->ins_h_];
            s->head_[s->ins_h_] = (std::uint16_t)str;
            str++;
        } while (--n);
        s->strstart_ = str;
        s->lookahead_ = MIN_MATCH-1;
        fill_window(s);
    }
    s->strstart_ += s->lookahead_;
    s->block_start_ = (long)s->strstart_;
    s->insert_ = s->lookahead_;
    s->lookahead_ = 0;
    s->match_length_ = s->prev_length_ = MIN_MATCH-1;
    s->match_available_ = 0;
    strm->next_in = next;
    strm->avail_in = avail;
    return Z_OK;
}

/* ========================================================================= */

int
deflate_stream::deflateResetKeep(deflate_stream* strm)
{
    strm->total_in = strm->total_out = 0;
    strm->msg = Z_NULL;
    strm->data_type = Z_UNKNOWN;

    auto s = strm;
    s->pending_ = 0;
    s->pending_out_ = s->pending_buf_;

    s->status_ = BUSY_STATE;
    s->last_flush_ = Z_NO_FLUSH;

    _tr_init(s);

    return Z_OK;
}

/* ========================================================================= */

int
deflate_stream::deflateReset(deflate_stream* strm)
{
    int ret;

    ret = deflateResetKeep(strm);
    if (ret == Z_OK)
        lm_init(strm);
    return ret;
}

/* ========================================================================= */

int
deflate_stream::deflatePending (
    deflate_stream* strm,
    unsigned *pending,
    int *bits)
{
    if (pending != Z_NULL)
        *pending = strm->pending_;
    if (bits != Z_NULL)
        *bits = strm->bi_valid_;
    return Z_OK;
}

/* ========================================================================= */

int
deflate_stream::deflatePrime(deflate_stream* strm, int bits, int value)
{
    int put;

    auto s = strm;
    if ((Byte *)(s->d_buf_) < s->pending_out_ + ((Buf_size + 7) >> 3))
        return Z_BUF_ERROR;
    do {
        put = Buf_size - s->bi_valid_;
        if (put > bits)
            put = bits;
        s->bi_buf_ |= (std::uint16_t)((value & ((1 << put) - 1)) << s->bi_valid_);
        s->bi_valid_ += put;
        _tr_flush_bits(s);
        value >>= put;
        bits -= put;
    } while (bits);
    return Z_OK;
}

/* ========================================================================= */

int
deflate_stream::deflateParams(deflate_stream* strm, int level, int strategy)
{
    compress_func func;
    int err = Z_OK;

    if (strm == Z_NULL || strm == Z_NULL) return Z_STREAM_ERROR;
    auto s = strm;

    if (level == Z_DEFAULT_COMPRESSION) level = 6;
    if (level < 0 || level > 9 || strategy < 0 || strategy > Z_FIXED) {
        return Z_STREAM_ERROR;
    }
    func = configuration_table[s->level_].func;

    if ((strategy != s->strategy_ || func != configuration_table[level].func) &&
        strm->total_in != 0) {
        /* Flush the last buffer: */
        err = strm->deflate(Z_BLOCK);
        if (err == Z_BUF_ERROR && s->pending_ == 0)
            err = Z_OK;
    }
    if (s->level_ != level) {
        s->level_ = level;
        s->max_lazy_match_   = configuration_table[level].max_lazy;
        s->good_match_       = configuration_table[level].good_length;
        s->nice_match_       = configuration_table[level].nice_length;
        s->max_chain_length_ = configuration_table[level].max_chain;
    }
    s->strategy_ = strategy;
    return err;
}

/* ========================================================================= */

int
deflate_stream::deflateTune(
    deflate_stream* strm,
    int good_length,
    int max_lazy,
    int nice_length,
    int max_chain)
{
    auto s = strm;
    s->good_match_ = good_length;
    s->max_lazy_match_ = max_lazy;
    s->nice_match_ = nice_length;
    s->max_chain_length_ = max_chain;
    return Z_OK;
}

/* =========================================================================
 * For the default windowBits of 15 and memLevel of 8, this function returns
 * a close to exact, as well as small, upper bound on the compressed size.
 * They are coded as constants here for a reason--if the #define's are
 * changed, then this function needs to be changed as well.  The return
 * value for 15 and 8 only works for those exact settings.
 *
 * For any setting other than those defaults for windowBits and memLevel,
 * the value returned is a conservative worst case for the maximum expansion
 * resulting from using fixed blocks instead of stored blocks, which deflate
 * can emit on compressed data for some combinations of the parameters.
 *
 * This function could be more sophisticated to provide closer upper bounds for
 * every combination of windowBits and memLevel.  But even the conservative
 * upper bound of about 14% expansion does not seem onerous for output buffer
 * allocation.
 */
uLong
deflate_stream::deflateBound(
    deflate_stream* strm,
    uLong sourceLen)
{
    uLong complen, wraplen;

    /* conservative upper bound for compressed data */
    complen = sourceLen +
              ((sourceLen + 7) >> 3) + ((sourceLen + 63) >> 6) + 5;

    /* if can't get parameters, return conservative bound plus zlib wrapper */
    if (strm == Z_NULL || strm == Z_NULL)
        return complen + 6;

    /* compute wrapper length */
    auto s = strm;
    wraplen = 0;

    /* if not default parameters, return conservative bound */
    if (s->w_bits_ != 15 || s->hash_bits_ != 8 + 7)
        return complen + wraplen;

    /* default settings: return tight bound for that case */
    return sourceLen + (sourceLen >> 12) + (sourceLen >> 14) +
           (sourceLen >> 25) + 13 - 6 + wraplen;
}

/* =========================================================================
 * Flush as much pending output as possible. All deflate() output goes
 * through this function so some applications may wish to modify it
 * to avoid allocating a large strm->next_out buffer and copying into it.
 * (See also read_buf()).
 */
void
deflate_stream::flush_pending(deflate_stream* strm)
{
    unsigned len;
    auto s = strm;

    _tr_flush_bits(s);
    len = s->pending_;
    if (len > strm->avail_out) len = strm->avail_out;
    if (len == 0) return;

    std::memcpy(strm->next_out, s->pending_out_, len);
    strm->next_out  += len;
    s->pending_out_  += len;
    strm->total_out += len;
    strm->avail_out  -= len;
    s->pending_ -= len;
    if (s->pending_ == 0) {
        s->pending_out_ = s->pending_buf_;
    }
}

/* ========================================================================= */

int
deflate_stream::deflate(int flush)
{
auto strm = this;
    int old_flush; /* value of flush param for previous deflate call */

    if (strm == Z_NULL || strm == Z_NULL ||
        flush > Z_BLOCK || flush < 0) {
        return Z_STREAM_ERROR;
    }
    auto s = strm;

    if (strm->next_out == Z_NULL ||
        (strm->next_in == Z_NULL && strm->avail_in != 0) ||
        (s->status_ == FINISH_STATE && flush != Z_FINISH)) {
        ERR_RETURN(strm, Z_STREAM_ERROR);
    }
    if (strm->avail_out == 0) ERR_RETURN(strm, Z_BUF_ERROR);

    old_flush = s->last_flush_;
    s->last_flush_ = flush;

    /* Flush as much pending output as possible */
    if (s->pending_ != 0) {
        flush_pending(strm);
        if (strm->avail_out == 0) {
            /* Since avail_out is 0, deflate will be called again with
             * more output space, but possibly with both pending and
             * avail_in equal to zero. There won't be anything to do,
             * but this is not an error situation so make sure we
             * return OK instead of BUF_ERROR at next call of deflate:
             */
            s->last_flush_ = -1;
            return Z_OK;
        }

    /* Make sure there is something to do and avoid duplicate consecutive
     * flushes. For repeated and useless calls with Z_FINISH, we keep
     * returning Z_STREAM_END instead of Z_BUF_ERROR.
     */
    } else if (strm->avail_in == 0 && RANK(flush) <= RANK(old_flush) &&
               flush != Z_FINISH) {
        ERR_RETURN(strm, Z_BUF_ERROR);
    }

    /* User must not provide more input after the first FINISH: */
    if (s->status_ == FINISH_STATE && strm->avail_in != 0) {
        ERR_RETURN(strm, Z_BUF_ERROR);
    }

    /* Start a new block or continue the current one.
     */
    if (strm->avail_in != 0 || s->lookahead_ != 0 ||
        (flush != Z_NO_FLUSH && s->status_ != FINISH_STATE)) {
        block_state bstate;

        bstate = s->strategy_ == Z_HUFFMAN_ONLY ? deflate_huff(s, flush) :
                    (s->strategy_ == Z_RLE ? deflate_rle(s, flush) :
                        (*(configuration_table[s->level_].func))(s, flush));

        if (bstate == finish_started || bstate == finish_done) {
            s->status_ = FINISH_STATE;
        }
        if (bstate == need_more || bstate == finish_started) {
            if (strm->avail_out == 0) {
                s->last_flush_ = -1; /* avoid BUF_ERROR next call, see above */
            }
            return Z_OK;
            /* If flush != Z_NO_FLUSH && avail_out == 0, the next call
             * of deflate should use the same flush parameter to make sure
             * that the flush is complete. So we don't have to output an
             * empty block here, this will be done at next call. This also
             * ensures that for a very small output buffer, we emit at most
             * one empty block.
             */
        }
        if (bstate == block_done) {
            if (flush == Z_PARTIAL_FLUSH) {
                _tr_align(s);
            } else if (flush != Z_BLOCK) { /* FULL_FLUSH or SYNC_FLUSH */
                _tr_stored_block(s, (char*)0, 0L, 0);
                /* For a full flush, this empty block will be recognized
                 * as a special marker by inflate_sync().
                 */
                if (flush == Z_FULL_FLUSH) {
                    CLEAR_HASH(s);             /* forget history */
                    if (s->lookahead_ == 0) {
                        s->strstart_ = 0;
                        s->block_start_ = 0L;
                        s->insert_ = 0;
                    }
                }
            }
            flush_pending(strm);
            if (strm->avail_out == 0) {
              s->last_flush_ = -1; /* avoid BUF_ERROR at next call, see above */
              return Z_OK;
            }
        }
    }
    Assert(strm->avail_out > 0, "bug2");

    if (flush != Z_FINISH) return Z_OK;
    return Z_STREAM_END;
}

/* ========================================================================= */

int
deflate_stream::deflateEnd(deflate_stream* strm)
{
    int status;

    if (strm == Z_NULL || strm == Z_NULL) return Z_STREAM_ERROR;

    status = strm->status_;
    if (status != EXTRA_STATE &&
        status != NAME_STATE &&
        status != COMMENT_STATE &&
        status != HCRC_STATE &&
        status != BUSY_STATE &&
        status != FINISH_STATE) {
      return Z_STREAM_ERROR;
    }

    /* Deallocate in reverse order of allocations: */
    std::free(strm->pending_buf_);
    std::free(strm->head_);
    std::free(strm->prev_);
    std::free(strm->window_);
    strm = Z_NULL;

    return status == BUSY_STATE ? Z_DATA_ERROR : Z_OK;
}

/* ===========================================================================
 * Read a new buffer from the current input stream, update the adler32
 * and total number of bytes read.  All deflate() input goes through
 * this function so some applications may wish to modify it to avoid
 * allocating a large strm->next_in buffer and copying from it.
 * (See also flush_pending()).
 */
int
deflate_stream::read_buf(deflate_stream* strm, Byte *buf, unsigned size)
{
    unsigned len = strm->avail_in;

    if (len > size) len = size;
    if (len == 0) return 0;

    strm->avail_in  -= len;

    std::memcpy(buf, strm->next_in, len);
    strm->next_in  += len;
    strm->total_in += len;

    return (int)len;
}

/* ===========================================================================
 * Initialize the "longest match" routines for a new zlib stream
 */
void
deflate_stream::lm_init(deflate_stream *s)
{
    s->window_size_ = (std::uint32_t)2L*s->w_size_;

    CLEAR_HASH(s);

    /* Set the default configuration parameters:
     */
    s->max_lazy_match_   = configuration_table[s->level_].max_lazy;
    s->good_match_       = configuration_table[s->level_].good_length;
    s->nice_match_       = configuration_table[s->level_].nice_length;
    s->max_chain_length_ = configuration_table[s->level_].max_chain;

    s->strstart_ = 0;
    s->block_start_ = 0L;
    s->lookahead_ = 0;
    s->insert_ = 0;
    s->match_length_ = s->prev_length_ = MIN_MATCH-1;
    s->match_available_ = 0;
    s->ins_h_ = 0;
}

/* ===========================================================================
 * Set match_start to the longest match starting at the given string and
 * return its length. Matches shorter or equal to prev_length are discarded,
 * in which case the result is equal to prev_length and match_start is
 * garbage.
 * IN assertions: cur_match is the head of the hash chain for the current
 *   string (strstart) and its distance is <= MAX_DIST, and prev_length >= 1
 * OUT assertion: the match length is not greater than s->lookahead_.
 */
/* For 80x86 and 680x0, an optimized version will be provided in match.asm or
 * match.S. The code will be functionally equivalent.
 */
uInt
deflate_stream::longest_match(deflate_stream *s, IPos cur_match)
{
    unsigned chain_length = s->max_chain_length_;/* max hash chain length */
    Byte *scan = s->window_ + s->strstart_; /* current string */
    Byte *match;                       /* matched string */
    int len;                           /* length of current match */
    int best_len = s->prev_length_;              /* best match length so far */
    int nice_match = s->nice_match_;             /* stop if match long enough */
    IPos limit = s->strstart_ > (IPos)MAX_DIST(s) ?
        s->strstart_ - (IPos)MAX_DIST(s) : NIL;
    /* Stop when cur_match becomes <= limit. To simplify the code,
     * we prevent matches with the string of window index 0.
     */
    std::uint16_t *prev = s->prev_;
    uInt wmask = s->w_mask_;

    Byte *strend = s->window_ + s->strstart_ + MAX_MATCH;
    Byte scan_end1  = scan[best_len-1];
    Byte scan_end   = scan[best_len];

    /* The code is optimized for HASH_BITS >= 8 and MAX_MATCH-2 multiple of 16.
     * It is easy to get rid of this optimization if necessary.
     */
    Assert(s->hash_bits_ >= 8 && MAX_MATCH == 258, "fc too clever");

    /* Do not waste too much time if we already have a good match: */
    if (s->prev_length_ >= s->good_match_) {
        chain_length >>= 2;
    }
    /* Do not look for matches beyond the end of the input. This is necessary
     * to make deflate deterministic.
     */
    if ((uInt)nice_match > s->lookahead_) nice_match = s->lookahead_;

    Assert((std::uint32_t)s->strstart_ <= s->window_size_-MIN_LOOKAHEAD, "need lookahead");

    do {
        Assert(cur_match < s->strstart_, "no future");
        match = s->window_ + cur_match;

        /* Skip to next match if the match length cannot increase
         * or if the match length is less than 2.  Note that the checks below
         * for insufficient lookahead only occur occasionally for performance
         * reasons.  Therefore uninitialized memory will be accessed, and
         * conditional jumps will be made that depend on those values.
         * However the length of the match is limited to the lookahead, so
         * the output of deflate is not affected by the uninitialized values.
         */
        if (match[best_len]   != scan_end  ||
            match[best_len-1] != scan_end1 ||
            *match            != *scan     ||
            *++match          != scan[1])      continue;

        /* The check at best_len-1 can be removed because it will be made
         * again later. (This heuristic is not always a win.)
         * It is not necessary to compare scan[2] and match[2] since they
         * are always equal when the other bytes match, given that
         * the hash keys are equal and that HASH_BITS >= 8.
         */
        scan += 2, match++;
        Assert(*scan == *match, "match[2]?");

        /* We check for insufficient lookahead only every 8th comparison;
         * the 256th check will be made at strstart+258.
         */
        do {
        } while (*++scan == *++match && *++scan == *++match &&
                 *++scan == *++match && *++scan == *++match &&
                 *++scan == *++match && *++scan == *++match &&
                 *++scan == *++match && *++scan == *++match &&
                 scan < strend);

        Assert(scan <= s->window_+(unsigned)(s->window_size_-1), "wild scan");

        len = MAX_MATCH - (int)(strend - scan);
        scan = strend - MAX_MATCH;

        if (len > best_len) {
            s->match_start_ = cur_match;
            best_len = len;
            if (len >= nice_match) break;
            scan_end1  = scan[best_len-1];
            scan_end   = scan[best_len];
        }
    } while ((cur_match = prev[cur_match & wmask]) > limit
             && --chain_length != 0);

    if ((uInt)best_len <= s->lookahead_) return (uInt)best_len;
    return s->lookahead_;
}

#ifdef DEBUG
/* ===========================================================================
 * Check that the match at match_start is indeed a match.
 */
local void check_match(
    deflate_stream *s,
    IPos start, match,
    int length)
{
    /* check that the match is indeed a match */
    if (std::memcmp(s->window_ + match,
                s->window_ + start, length) != EQUAL) {
        fprintf(stderr, " start %u, match %u, length %d\n",
                start, match, length);
        do {
            fprintf(stderr, "%c%c", s->window_[match++], s->window_[start++]);
        } while (--length != 0);
        z_error("invalid match");
    }
    if (z_verbose > 1) {
        fprintf(stderr,"\\[%d,%d]", start-match, length);
        do { putc(s->window_[start++], stderr); } while (--length != 0);
    }
}
#else
#  define check_match(s, start, match, length)
#endif /* DEBUG */

/* ===========================================================================
 * Flush the current block, with given end-of-file flag.
 * IN assertion: strstart is set to the end of the current match.
 */
#define FLUSH_BLOCK_ONLY(s, last) { \
   _tr_flush_block(s, (s->block_start_ >= 0L ? \
                   (char *)&s->window_[(unsigned)s->block_start_] : \
                   (char *)Z_NULL), \
                (std::uint32_t)((long)s->strstart_ - s->block_start_), \
                (last)); \
   s->block_start_ = s->strstart_; \
   flush_pending(s); \
   Tracev((stderr,"[FLUSH]")); \
}

/* Same but force premature exit if necessary. */
#define FLUSH_BLOCK(s, last) { \
   FLUSH_BLOCK_ONLY(s, last); \
   if (s->avail_out == 0) return (last) ? finish_started : need_more; \
}

/* ===========================================================================
 * Copy without compression as much as possible from the input stream, return
 * the current block state.
 * This function does not insert new strings in the dictionary since
 * uncompressible data is probably not useful. This function is used
 * only for the level=0 compression option.
 * NOTE: this function should be optimized to avoid extra copying from
 * window to pending_buf.
 */
block_state deflate_stream::deflate_stored(
    deflate_stream *s,
    int flush)
{
    /* Stored blocks are limited to 0xffff bytes, pending_buf is limited
     * to pending_buf_size, and each stored block has a 5 byte header:
     */
    std::uint32_t max_block_size = 0xffff;
    std::uint32_t max_start;

    if (max_block_size > s->pending_buf_size_ - 5) {
        max_block_size = s->pending_buf_size_ - 5;
    }

    /* Copy as much as possible from input to output: */
    for (;;) {
        /* Fill the window as much as possible: */
        if (s->lookahead_ <= 1) {

            Assert(s->strstart_ < s->w_size_+MAX_DIST(s) ||
                   s->block_start_ >= (long)s->w_size_, "slide too late");

            fill_window(s);
            if (s->lookahead_ == 0 && flush == Z_NO_FLUSH) return need_more;

            if (s->lookahead_ == 0) break; /* flush the current block */
        }
        Assert(s->block_start_ >= 0L, "block gone");

        s->strstart_ += s->lookahead_;
        s->lookahead_ = 0;

        /* Emit a stored block if pending_buf will be full: */
        max_start = s->block_start_ + max_block_size;
        if (s->strstart_ == 0 || (std::uint32_t)s->strstart_ >= max_start) {
            /* strstart == 0 is possible when wraparound on 16-bit machine */
            s->lookahead_ = (uInt)(s->strstart_ - max_start);
            s->strstart_ = (uInt)max_start;
            FLUSH_BLOCK(s, 0);
        }
        /* Flush if we may have to slide, otherwise block_start may become
         * negative and the data will be gone:
         */
        if (s->strstart_ - (uInt)s->block_start_ >= MAX_DIST(s)) {
            FLUSH_BLOCK(s, 0);
        }
    }
    s->insert_ = 0;
    if (flush == Z_FINISH) {
        FLUSH_BLOCK(s, 1);
        return finish_done;
    }
    if ((long)s->strstart_ > s->block_start_)
        FLUSH_BLOCK(s, 0);
    return block_done;
}

/* ===========================================================================
 * Compress as much as possible from the input stream, return the current
 * block state.
 * This function does not perform lazy evaluation of matches and inserts
 * new strings in the dictionary only for unmatched strings or for short
 * matches. It is used only for the fast compression options.
 */
block_state
deflate_stream::deflate_fast(deflate_stream *s, int flush)
{
    IPos hash_head;       /* head of the hash chain */
    int bflush;           /* set if current block must be flushed */

    for (;;) {
        /* Make sure that we always have enough lookahead, except
         * at the end of the input file. We need MAX_MATCH bytes
         * for the next match, plus MIN_MATCH bytes to insert the
         * string following the next match.
         */
        if (s->lookahead_ < MIN_LOOKAHEAD) {
            fill_window(s);
            if (s->lookahead_ < MIN_LOOKAHEAD && flush == Z_NO_FLUSH) {
                return need_more;
            }
            if (s->lookahead_ == 0) break; /* flush the current block */
        }

        /* Insert the string window[strstart .. strstart+2] in the
         * dictionary, and set hash_head to the head of the hash chain:
         */
        hash_head = NIL;
        if (s->lookahead_ >= MIN_MATCH) {
            INSERT_STRING(s, s->strstart_, hash_head);
        }

        /* Find the longest match, discarding those <= prev_length.
         * At this point we have always match_length < MIN_MATCH
         */
        if (hash_head != NIL && s->strstart_ - hash_head <= MAX_DIST(s)) {
            /* To simplify the code, we prevent matches with the string
             * of window index 0 (in particular we have to avoid a match
             * of the string with itself at the start of the input file).
             */
            s->match_length_ = longest_match (s, hash_head);
            /* longest_match() sets match_start */
        }
        if (s->match_length_ >= MIN_MATCH) {
            check_match(s, s->strstart_, s->match_start_, s->match_length_);

            _tr_tally_dist(s, s->strstart_ - s->match_start_,
                           s->match_length_ - MIN_MATCH, bflush);

            s->lookahead_ -= s->match_length_;

            /* Insert new strings in the hash table only if the match length
             * is not too large. This saves time but degrades compression.
             */
            if (s->match_length_ <= s->max_lazy_match_ &&
                s->lookahead_ >= MIN_MATCH) {
                s->match_length_--; /* string at strstart already in table */
                do {
                    s->strstart_++;
                    INSERT_STRING(s, s->strstart_, hash_head);
                    /* strstart never exceeds WSIZE-MAX_MATCH, so there are
                     * always MIN_MATCH bytes ahead.
                     */
                } while (--s->match_length_ != 0);
                s->strstart_++;
            } else
            {
                s->strstart_ += s->match_length_;
                s->match_length_ = 0;
                s->ins_h_ = s->window_[s->strstart_];
                UPDATE_HASH(s, s->ins_h_, s->window_[s->strstart_+1]);
                /* If lookahead < MIN_MATCH, ins_h is garbage, but it does not
                 * matter since it will be recomputed at next deflate call.
                 */
            }
        } else {
            /* No match, output a literal byte */
            Tracevv((stderr,"%c", s->window_[s->strstart_]));
            _tr_tally_lit (s, s->window_[s->strstart_], bflush);
            s->lookahead_--;
            s->strstart_++;
        }
        if (bflush) FLUSH_BLOCK(s, 0);
    }
    s->insert_ = s->strstart_ < MIN_MATCH-1 ? s->strstart_ : MIN_MATCH-1;
    if (flush == Z_FINISH) {
        FLUSH_BLOCK(s, 1);
        return finish_done;
    }
    if (s->last_lit_)
        FLUSH_BLOCK(s, 0);
    return block_done;
}

/* ===========================================================================
 * Same as above, but achieves better compression. We use a lazy
 * evaluation for matches: a match is finally adopted only if there is
 * no better match at the next window position.
 */
block_state
deflate_stream::deflate_slow(deflate_stream *s, int flush)
{
    IPos hash_head;          /* head of hash chain */
    int bflush;              /* set if current block must be flushed */

    /* Process the input block. */
    for (;;) {
        /* Make sure that we always have enough lookahead, except
         * at the end of the input file. We need MAX_MATCH bytes
         * for the next match, plus MIN_MATCH bytes to insert the
         * string following the next match.
         */
        if (s->lookahead_ < MIN_LOOKAHEAD) {
            fill_window(s);
            if (s->lookahead_ < MIN_LOOKAHEAD && flush == Z_NO_FLUSH) {
                return need_more;
            }
            if (s->lookahead_ == 0) break; /* flush the current block */
        }

        /* Insert the string window[strstart .. strstart+2] in the
         * dictionary, and set hash_head to the head of the hash chain:
         */
        hash_head = NIL;
        if (s->lookahead_ >= MIN_MATCH) {
            INSERT_STRING(s, s->strstart_, hash_head);
        }

        /* Find the longest match, discarding those <= prev_length.
         */
        s->prev_length_ = s->match_length_, s->prev_match_ = s->match_start_;
        s->match_length_ = MIN_MATCH-1;

        if (hash_head != NIL && s->prev_length_ < s->max_lazy_match_ &&
            s->strstart_ - hash_head <= MAX_DIST(s)) {
            /* To simplify the code, we prevent matches with the string
             * of window index 0 (in particular we have to avoid a match
             * of the string with itself at the start of the input file).
             */
            s->match_length_ = longest_match (s, hash_head);
            /* longest_match() sets match_start */

            if (s->match_length_ <= 5 && (s->strategy_ == Z_FILTERED
#if TOO_FAR <= 32767
                || (s->match_length_ == MIN_MATCH &&
                    s->strstart_ - s->match_start_ > TOO_FAR)
#endif
                )) {

                /* If prev_match is also MIN_MATCH, match_start is garbage
                 * but we will ignore the current match anyway.
                 */
                s->match_length_ = MIN_MATCH-1;
            }
        }
        /* If there was a match at the previous step and the current
         * match is not better, output the previous match:
         */
        if (s->prev_length_ >= MIN_MATCH && s->match_length_ <= s->prev_length_) {
            uInt max_insert = s->strstart_ + s->lookahead_ - MIN_MATCH;
            /* Do not insert strings in hash table beyond this. */

            check_match(s, s->strstart_-1, s->prev_match_, s->prev_length_);

            _tr_tally_dist(s, s->strstart_ -1 - s->prev_match_,
                           s->prev_length_ - MIN_MATCH, bflush);

            /* Insert in hash table all strings up to the end of the match.
             * strstart-1 and strstart are already inserted. If there is not
             * enough lookahead, the last two strings are not inserted in
             * the hash table.
             */
            s->lookahead_ -= s->prev_length_-1;
            s->prev_length_ -= 2;
            do {
                if (++s->strstart_ <= max_insert) {
                    INSERT_STRING(s, s->strstart_, hash_head);
                }
            } while (--s->prev_length_ != 0);
            s->match_available_ = 0;
            s->match_length_ = MIN_MATCH-1;
            s->strstart_++;

            if (bflush) FLUSH_BLOCK(s, 0);

        } else if (s->match_available_) {
            /* If there was no match at the previous position, output a
             * single literal. If there was a match but the current match
             * is longer, truncate the previous match to a single literal.
             */
            Tracevv((stderr,"%c", s->window_[s->strstart_-1]));
            _tr_tally_lit(s, s->window_[s->strstart_-1], bflush);
            if (bflush) {
                FLUSH_BLOCK_ONLY(s, 0);
            }
            s->strstart_++;
            s->lookahead_--;
            if (s->avail_out == 0) return need_more;
        } else {
            /* There is no previous match to compare with, wait for
             * the next step to decide.
             */
            s->match_available_ = 1;
            s->strstart_++;
            s->lookahead_--;
        }
    }
    Assert (flush != Z_NO_FLUSH, "no flush?");
    if (s->match_available_) {
        Tracevv((stderr,"%c", s->window_[s->strstart_-1]));
        _tr_tally_lit(s, s->window_[s->strstart_-1], bflush);
        s->match_available_ = 0;
    }
    s->insert_ = s->strstart_ < MIN_MATCH-1 ? s->strstart_ : MIN_MATCH-1;
    if (flush == Z_FINISH) {
        FLUSH_BLOCK(s, 1);
        return finish_done;
    }
    if (s->last_lit_)
        FLUSH_BLOCK(s, 0);
    return block_done;
}

/* ===========================================================================
 * For Z_RLE, simply look for runs of bytes, generate matches only of distance
 * one.  Do not maintain a hash table.  (It will be regenerated if this run of
 * deflate switches away from Z_RLE.)
 */
block_state
deflate_stream::deflate_rle(deflate_stream *s, int flush)
{
    int bflush;             /* set if current block must be flushed */
    uInt prev;              /* byte at distance one to match */
    Byte *scan, *strend;   /* scan goes up to strend for length of run */

    for (;;) {
        /* Make sure that we always have enough lookahead, except
         * at the end of the input file. We need MAX_MATCH bytes
         * for the longest run, plus one for the unrolled loop.
         */
        if (s->lookahead_ <= MAX_MATCH) {
            fill_window(s);
            if (s->lookahead_ <= MAX_MATCH && flush == Z_NO_FLUSH) {
                return need_more;
            }
            if (s->lookahead_ == 0) break; /* flush the current block */
        }

        /* See how many times the previous byte repeats */
        s->match_length_ = 0;
        if (s->lookahead_ >= MIN_MATCH && s->strstart_ > 0) {
            scan = s->window_ + s->strstart_ - 1;
            prev = *scan;
            if (prev == *++scan && prev == *++scan && prev == *++scan) {
                strend = s->window_ + s->strstart_ + MAX_MATCH;
                do {
                } while (prev == *++scan && prev == *++scan &&
                         prev == *++scan && prev == *++scan &&
                         prev == *++scan && prev == *++scan &&
                         prev == *++scan && prev == *++scan &&
                         scan < strend);
                s->match_length_ = MAX_MATCH - (int)(strend - scan);
                if (s->match_length_ > s->lookahead_)
                    s->match_length_ = s->lookahead_;
            }
            Assert(scan <= s->window_+(uInt)(s->window_size_-1), "wild scan");
        }

        /* Emit match if have run of MIN_MATCH or longer, else emit literal */
        if (s->match_length_ >= MIN_MATCH) {
            check_match(s, s->strstart_, s->strstart_ - 1, s->match_length_);

            _tr_tally_dist(s, 1, s->match_length_ - MIN_MATCH, bflush);

            s->lookahead_ -= s->match_length_;
            s->strstart_ += s->match_length_;
            s->match_length_ = 0;
        } else {
            /* No match, output a literal byte */
            Tracevv((stderr,"%c", s->window_[s->strstart_]));
            _tr_tally_lit (s, s->window_[s->strstart_], bflush);
            s->lookahead_--;
            s->strstart_++;
        }
        if (bflush) FLUSH_BLOCK(s, 0);
    }
    s->insert_ = 0;
    if (flush == Z_FINISH) {
        FLUSH_BLOCK(s, 1);
        return finish_done;
    }
    if (s->last_lit_)
        FLUSH_BLOCK(s, 0);
    return block_done;
}

/* ===========================================================================
 * For Z_HUFFMAN_ONLY, do not look for matches.  Do not maintain a hash table.
 * (It will be regenerated if this run of deflate switches away from Huffman.)
 */
block_state
deflate_stream::deflate_huff(deflate_stream *s, int flush)
{
    int bflush;             /* set if current block must be flushed */

    for (;;) {
        /* Make sure that we have a literal to write. */
        if (s->lookahead_ == 0) {
            fill_window(s);
            if (s->lookahead_ == 0) {
                if (flush == Z_NO_FLUSH)
                    return need_more;
                break;      /* flush the current block */
            }
        }

        /* Output a literal byte */
        s->match_length_ = 0;
        Tracevv((stderr,"%c", s->window_[s->strstart_]));
        _tr_tally_lit (s, s->window_[s->strstart_], bflush);
        s->lookahead_--;
        s->strstart_++;
        if (bflush) FLUSH_BLOCK(s, 0);
    }
    s->insert_ = 0;
    if (flush == Z_FINISH) {
        FLUSH_BLOCK(s, 1);
        return finish_done;
    }
    if (s->last_lit_)
        FLUSH_BLOCK(s, 0);
    return block_done;
}

} // beast
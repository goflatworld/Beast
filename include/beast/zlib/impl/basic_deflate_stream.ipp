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

#ifndef BEAST_ZLIB_IMPL_BASIC_DEFLATE_STREAM_IPP
#define BEAST_ZLIB_IMPL_BASIC_DEFLATE_STREAM_IPP

namespace beast {
namespace zlib {

template<class Allocator>
basic_deflate_stream<Allocator>::
basic_deflate_stream()
{
    // default level 6
    //reset(this, 6, 15, DEF_MEM_LEVEL, Strategy::normal);
}

template<class Allocator>
void
basic_deflate_stream<Allocator>::
reset(
    int  level,
    int  windowBits,
    int  memLevel,
    Strategy  strategy)
{
    if(level == Z_DEFAULT_COMPRESSION)
        level = 6;

    // VFALCO What do we do about this?
    // until 256-byte window bug fixed
    if(windowBits == 8)
        windowBits = 9;

    if(level < 0 || level > 9)
        throw std::invalid_argument{"invalid level"};

    if(windowBits < 8 || windowBits > 15)
        throw std::invalid_argument{"invalid windowBits"};

    if(memLevel < 1 || memLevel > MAX_MEM_LEVEL)
        throw std::invalid_argument{"invalid memLevel"};

    /* We overlay pending_buf and d_buf+l_buf. This works since the average
     * output size for (length,distance) codes is <= 24 bits.
     */
    std::uint16_t* overlay;

    w_bits_ = windowBits;
    w_size_ = 1 << w_bits_;
    w_mask_ = w_size_ - 1;

    hash_bits_ = memLevel + 7;
    hash_size_ = 1 << hash_bits_;
    hash_mask_ = hash_size_ - 1;
    hash_shift_ =  ((hash_bits_+limits::minMatch-1)/limits::minMatch);

    // 16K elements by default
    lit_bufsize_ = 1 << (memLevel + 6);

    {
        auto const nwindow  = w_size_ * 2*sizeof(Byte);
        auto const nprev    = w_size_ * sizeof(std::uint16_t);
        auto const nhead    = hash_size_ * sizeof(std::uint16_t);
        auto const noverlay = lit_bufsize_ * (sizeof(std::uint16_t)+2);
        
        buf_.reset(new std::uint8_t[nwindow + nprev + nhead + noverlay]);

        window_ = reinterpret_cast<Byte*>(buf_.get());
        prev_   = reinterpret_cast<std::uint16_t*>(buf_.get() + nwindow);
        head_   = reinterpret_cast<std::uint16_t*>(buf_.get() + nwindow + nprev);
        overlay = reinterpret_cast<std::uint16_t*>(buf_.get() + nwindow + nprev + nhead);
    }

    high_water_ = 0;      /* nothing written to window_ yet */

    pending_buf_ = (std::uint8_t *) overlay;
    pending_buf_size_ = (std::uint32_t)lit_bufsize_ * (sizeof(std::uint16_t)+2L);

    d_buf_ = overlay + lit_bufsize_/sizeof(std::uint16_t);
    l_buf_ = pending_buf_ + (1+sizeof(std::uint16_t))*lit_bufsize_;

    level_ = level;
    strategy_ = strategy;

    deflateReset();
}

/* ========================================================================= */

template<class Allocator>
void
basic_deflate_stream<Allocator>::
deflateResetKeep()
{
    // VFALCO TODO
    //total_in = 0;
    //total_out = 0;
    //msg = 0;
    //data_type = Z_UNKNOWN;

    pending_ = 0;
    pending_out_ = pending_buf_;

    status_ = BUSY_STATE;
    last_flush_ = Flush::none;

    tr_init();
}

/* ========================================================================= */

template<class Allocator>
void
basic_deflate_stream<Allocator>::
deflateReset()
{
    deflateResetKeep();
    lm_init();
}

/* ========================================================================= */

template<class Allocator>
void
basic_deflate_stream<Allocator>::
params(z_params& zs, int level, Strategy strategy, error_code& ec)
{
    compress_func func;

    if(level == Z_DEFAULT_COMPRESSION)
        level = 6;
    if(level < 0 || level > 9)
    {
        ec = error::stream_error;
        return;
    }
    func = get_config(level_).func;

    if((strategy != strategy_ || func != get_config(level).func) &&
        zs.total_in != 0)
    {
        // Flush the last buffer:
        write(zs, Flush::block, ec);
        if(ec == error::need_buffers && pending_ == 0)
            ec = {};
    }
    if(level_ != level)
    {
        level_ = level;
        max_lazy_match_   = get_config(level).max_lazy;
        good_match_       = get_config(level).good_length;
        nice_match_       = get_config(level).nice_length;
        max_chain_length_ = get_config(level).max_chain;
    }
    strategy_ = strategy;
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

inline
std::size_t
deflate_upper_bound(std::size_t bytes)
{
    return bytes +
        ((bytes + 7) >> 3) +
        ((bytes + 63) >> 6) + 5 +
        6;
}

template<class Allocator>
std::size_t
basic_deflate_stream<Allocator>::
upper_bound(std::size_t sourceLen) const
{
    std::size_t complen;
    std::size_t wraplen;

    /* conservative upper bound for compressed data */
    complen = sourceLen +
              ((sourceLen + 7) >> 3) + ((sourceLen + 63) >> 6) + 5;

    /* compute wrapper length */
    wraplen = 0;

    /* if not default parameters, return conservative bound */
    if(w_bits_ != 15 || hash_bits_ != 8 + 7)
        return complen + wraplen;

    /* default settings: return tight bound for that case */
    return sourceLen + (sourceLen >> 12) + (sourceLen >> 14) +
           (sourceLen >> 25) + 13 - 6 + wraplen;
}

} // zlib
} // beast

#endif

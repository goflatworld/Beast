//
// Copyright (c) 2013-2016 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BEAST_DETAIL_TYPE_TRAITS_HPP
#define BEAST_DETAIL_TYPE_TRAITS_HPP

#include <stdexcept>
#include <string>

namespace beast {
namespace detail {

template<class... Ts>
struct make_void
{
    using type = void;
};

template<class... Ts>
using void_t = typename make_void<Ts...>::type;

template<class... Ts>
inline
void
ignore_unused(Ts const& ...)
{
}

template<class... Ts>
inline
void
ignore_unused()
{}

template<class Exception>
Exception
make_exception(char const* reason, char const* file, int line)
{
	char const* n = file;
	for(auto p = file; *p; ++p)
		if(*p == '\\' || *p == '/')
			n = p + 1;
	return Exception{std::string(reason) + " (" +
		n + ":" + std::to_string(line) + ")"};
}

} // detail
} // beast

#endif

#pragma once

#include <cstdlib>
#include <iostream>

#ifdef assert
#undef assert
#endif

// Keep assertions active in optimized test binaries. Several tests use
// assertion expressions with setup side effects such as applyMove(...),
// which must still execute under NDEBUG.
#define assert(expr)                                                                                                  \
    do {                                                                                                              \
        if (!(expr)) {                                                                                                \
            std::cerr << __FILE__ << ':' << __LINE__ << ": Assertion `" #expr "` failed.\n";                        \
            std::abort();                                                                                             \
        }                                                                                                             \
    } while (false)

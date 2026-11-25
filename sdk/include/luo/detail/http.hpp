#pragma once

#include <string_view>

#ifdef LUO_PINNED_HTTP_HEADER
#include LUO_PINNED_HTTP_HEADER
#else
#include <httplib.h>
#endif

static_assert(std::string_view(CPPHTTPLIB_VERSION) == "0.67.0", "Build with the pinned cpp-httplib version");

#pragma once

#include <stdexcept>
#include <string>

namespace render_graph::unit_test
{
    [[noreturn]] inline void throw_check_failure(const char* expression, const char* file, int line)
    {
        throw std::runtime_error(std::string(file) + ":" + std::to_string(line) + ": check failed: " + expression);
    }
}

#define RG_CHECK(...)                                                                                                     \
    do                                                                                                                    \
    {                                                                                                                     \
        if (!static_cast<bool>((__VA_ARGS__)))                                                                            \
        {                                                                                                                 \
            ::render_graph::unit_test::throw_check_failure(#__VA_ARGS__, __FILE__, __LINE__);                            \
        }                                                                                                                 \
    } while (false)

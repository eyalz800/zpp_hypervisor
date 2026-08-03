#include <cstddef>
#include <cstdint>

extern "C" {
void * memcpy(void * dest, const void * src, std::size_t count)
{
    for (std::size_t i{}; i < count; ++i) {
        *(static_cast<unsigned char *>(dest) + i) =
            *(static_cast<const unsigned char *>(src) + i);
    }

    return dest;
}

void * memmove(void * dest, const void * src, std::size_t count)
{
    auto * to = static_cast<unsigned char *>(dest);
    auto * from = static_cast<const unsigned char *>(src);

    // When the ranges overlap with the destination above the source, a
    // forward copy would clobber source bytes before reading them, so
    // copy backwards instead.
    if (to > from && to < from + count) {
        for (auto i = count; i--;) {
            to[i] = from[i];
        }

        return dest;
    }

    for (std::size_t i{}; i < count; ++i) {
        to[i] = from[i];
    }

    return dest;
}

void * memset(void * dest, int value, std::size_t count)
{
    for (std::size_t i{}; i < count; ++i) {
        *(static_cast<unsigned char *>(dest) + i) =
            static_cast<unsigned char>(value);
    }

    return dest;
}

int memcmp(const void * dest, const void * src, std::size_t count)
{
    for (std::size_t i{}; i < count; ++i) {
        auto diff = *(static_cast<const unsigned char *>(dest) + i) -
                    *(static_cast<const unsigned char *>(src) + i);
        if (diff) {
            return diff;
        }
    }

    return 0;
}

std::size_t strlen(const char * string)
{
    for (std::size_t i{};; ++i) {
        if (!string[i]) {
            return i;
        }
    }
}

void __cxa_pure_virtual()
{
}
}

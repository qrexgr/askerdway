#pragma once
#include <cstddef>

namespace askerdway::comfort {
// Inspected x64 protobuf array-mode ABI, guarded by the client PE profile in the caller.
// No stream or aliasing: the bounded buffer accommodates the whole serialized message.
struct array_output {
    std::byte* end = nullptr;
    std::byte* buffer_end = nullptr;
    std::byte buffer[32]{};
    void* stream = nullptr;
    bool error = false, aliasing = false, deterministic = true, skip_check = false;
};
using size_function = std::size_t(*)(const void*);
using serialize_function = std::byte*(*)(const void*, std::byte*, array_output*);
inline bool serialize_checked(const void* message, size_function size_fn, serialize_function serialize_fn,
    std::byte* buffer, std::size_t& size) noexcept {
#if defined(_MSC_VER)
    __try {
#endif
        size = size_fn(message);
        if (size > 16384) return false;
        array_output output{}; output.end = buffer + 16416;
        const auto end = serialize_fn(message, buffer, &output);
        return !output.error && end == buffer + size;
#if defined(_MSC_VER)
    } __except (1) { return false; }
#endif
}
} // namespace askerdway::comfort

#include "huffblock/archive.hpp"

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::istringstream input(std::string(reinterpret_cast<const char*>(data), size));
    std::ostringstream output;
    try {
        huffblock::decompress_stream(input, output, {.threads = 1, .max_in_flight = 1,
            .buffer_limit = 1024U * 1024U, .max_output_size = 65536});
    } catch (const std::runtime_error&) {
    } catch (const std::invalid_argument&) {
    }
    return 0;
}

#pragma once

#include <filesystem>
#include <fstream>

namespace huffblock::detail {

// Owns only its exclusively created temporary directory and its payload file.
// Publication is a no-replace operation even if the destination appears later.
class TemporaryOutput {
public:
    explicit TemporaryOutput(const std::filesystem::path& destination);
    TemporaryOutput(const TemporaryOutput&) = delete;
    TemporaryOutput& operator=(const TemporaryOutput&) = delete;
    ~TemporaryOutput();

    std::ostream& stream() { return stream_; }
    void finish();

private:
    void cleanup() noexcept;
    std::filesystem::path destination_;
    std::filesystem::path directory_;
    std::filesystem::path payload_;
    std::ofstream stream_;
};

} // namespace huffblock::detail

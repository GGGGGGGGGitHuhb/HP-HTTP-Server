#pragma once
#include "base/unique_fd.h"
#include <cstddef>
#include <sys/types.h>

namespace hp::base {
class FileRegion final {
public:
    FileRegion(UniqueFd file, off_t offset, std::size_t length);
    FileRegion(const FileRegion&) = delete;
    FileRegion& operator=(const FileRegion&) = delete;
    FileRegion(FileRegion&& other) noexcept;
    FileRegion& operator=(FileRegion&& other) noexcept;
    [[nodiscard]] int fd() const noexcept { return file_.get(); }
    [[nodiscard]] off_t offset() const noexcept { return offset_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return remaining_; }
    void advance(std::size_t bytes);
private:
    UniqueFd file_;
    off_t offset_;
    std::size_t remaining_;
};
} // namespace hp::base

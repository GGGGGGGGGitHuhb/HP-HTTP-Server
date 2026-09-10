#include "base/file_region.h"
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace hp::base {
FileRegion::FileRegion(UniqueFd file, off_t offset, std::size_t length)
    : file_(std::move(file)), offset_(offset), remaining_(length) {
    if (file_.get() < 0 || offset < 0 ||
        length > static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max() -
                                             offset))
        throw std::invalid_argument("invalid file region");
    const int flags = ::fcntl(file_.get(), F_GETFD);
    if (flags < 0 || (!(flags & FD_CLOEXEC) &&
                      ::fcntl(file_.get(), F_SETFD, flags | FD_CLOEXEC) < 0))
        throw std::system_error(errno, std::generic_category(),
                                "file region CLOEXEC");
}

FileRegion::FileRegion(FileRegion &&other) noexcept
    : file_(std::move(other.file_)),
      offset_(std::exchange(other.offset_, 0)),
      remaining_(std::exchange(other.remaining_, 0)) {
}

FileRegion &FileRegion::operator=(FileRegion &&other) noexcept {
    if (this != &other) {
        file_ = std::move(other.file_);
        offset_ = std::exchange(other.offset_, 0);
        remaining_ = std::exchange(other.remaining_, 0);
    }
    return *this;
}

void FileRegion::advance(std::size_t bytes) {
    if (bytes > remaining_)
        throw std::out_of_range("file progress exceeds region");
    offset_ += static_cast<off_t>(bytes);
    remaining_ -= bytes;
    if (!remaining_)
        file_.reset();
}
}  // namespace hp::base

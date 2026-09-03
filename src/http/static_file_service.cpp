#include "http/static_file_service.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <utility>

#include "http/http_response.h"

namespace hp::http {
namespace {

class UniqueFd final {
   public:
    explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
    ~UniqueFd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) {
                ::close(fd_);
            }
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    [[nodiscard]] int get() const noexcept { return fd_; }

   private:
    int fd_;
};

std::span<const std::byte> byte_span(const std::vector<std::byte>& bytes) {
    return {bytes.data(), bytes.size()};
}

std::vector<std::byte> error_response(Status status) {
    return make_error_response(status);
}

bool is_symlink_at(int parent_fd, const std::string& component) {
    struct stat metadata {};
    return ::fstatat(parent_fd, component.c_str(), &metadata,
                     AT_SYMLINK_NOFOLLOW) == 0 &&
           S_ISLNK(metadata.st_mode);
}

Status classify_open_error(int parent_fd, const std::string& component,
                           int error_number) {
    if (error_number == ELOOP || is_symlink_at(parent_fd, component)) {
        return Status::forbidden;
    }
    if (error_number == EACCES || error_number == EPERM) {
        return Status::forbidden;
    }
    if (error_number == ENOENT || error_number == ENOTDIR) {
        return Status::not_found;
    }
    return Status::internal_server_error;
}

struct PathResult {
    Status status{Status::ok};
    std::string path;
    std::vector<std::string> components;
};

PathResult validate_path(std::string_view target) {
    if (target.find('%') != std::string_view::npos) {
        return {Status::bad_request, {}, {}};
    }
    const std::size_t query = target.find('?');
    const std::string_view path = target.substr(0, query);
    if (path.find('\\') != std::string_view::npos) {
        return {Status::forbidden, {}, {}};
    }
    if (path == "/") {
        return {Status::ok, "index.html", {"index.html"}};
    }
    if (path.size() < 2 || path.front() != '/' || path.back() == '/') {
        return {Status::forbidden, {}, {}};
    }

    PathResult result;
    result.path = std::string(path.substr(1));
    std::size_t start = 1;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::size_t end =
            slash == std::string_view::npos ? path.size() : slash;
        const std::string_view component = path.substr(start, end - start);
        if (component.empty() || component == "." || component == "..") {
            return {Status::forbidden, {}, {}};
        }
        result.components.emplace_back(component);
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return result;
}

std::vector<std::byte> read_file_response(int file_fd,
                                          std::string_view relative_path,
                                          const struct stat& metadata) {
    if (!S_ISREG(metadata.st_mode)) {
        return error_response(Status::not_found);
    }
    if (metadata.st_size < 0) {
        return error_response(Status::internal_server_error);
    }
    const auto size = static_cast<std::uintmax_t>(metadata.st_size);
    if (size > max_file_bytes) {
        return error_response(Status::forbidden);
    }

    std::vector<std::byte> body(static_cast<std::size_t>(size));
    std::size_t offset = 0;
    while (offset < body.size()) {
        const ssize_t count =
            ::read(file_fd, body.data() + offset, body.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        return error_response(Status::internal_server_error);
    }
    return make_response(Status::ok, byte_span(body),
                         content_type_for_path(relative_path));
}

}  // namespace

StaticFileService::StaticFileService(const std::string& root_path) {
    root_fd_ = ::open(root_path.c_str(),
                      O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root_fd_ == -1) {
        throw std::system_error(errno, std::generic_category(),
                                "static root is unavailable");
    }
    struct stat metadata {};
    if (::fstat(root_fd_, &metadata) == -1 || !S_ISDIR(metadata.st_mode)) {
        const int error_number = errno == 0 ? ENOTDIR : errno;
        ::close(root_fd_);
        root_fd_ = -1;
        throw std::system_error(error_number, std::generic_category(),
                                "static root is not a directory");
    }
}

StaticFileService::~StaticFileService() {
    if (root_fd_ >= 0) {
        ::close(root_fd_);
    }
}

std::vector<std::byte> StaticFileService::handle(
    const HttpRequest& request) const {
    try {
        const PathResult validated = validate_path(request.target);
        if (validated.status != Status::ok) {
            return error_response(validated.status);
        }

        int parent_fd = root_fd_;
        UniqueFd current_directory;
        for (std::size_t index = 0; index + 1 < validated.components.size();
             ++index) {
            const std::string& component = validated.components[index];
            const int opened =
                ::openat(parent_fd, component.c_str(),
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (opened == -1) {
                return error_response(
                    classify_open_error(parent_fd, component, errno));
            }
            current_directory = UniqueFd(opened);
            parent_fd = current_directory.get();
        }

        const std::string& filename = validated.components.back();
        UniqueFd file(::openat(parent_fd, filename.c_str(),
                               O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        if (file.get() == -1) {
            return error_response(
                classify_open_error(parent_fd, filename, errno));
        }
        struct stat metadata {};
        if (::fstat(file.get(), &metadata) == -1) {
            return error_response(Status::internal_server_error);
        }
        return read_file_response(file.get(), validated.path, metadata);
    } catch (...) {
        return error_response(Status::internal_server_error);
    }
}

}  // namespace hp::http

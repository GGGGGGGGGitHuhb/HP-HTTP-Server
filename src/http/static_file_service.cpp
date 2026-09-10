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

using base::UniqueFd;

ResponseResult error_response(Status status, ConnectionPolicy policy) {
  if (status == Status::bad_request || status == Status::method_not_allowed)
    policy = ConnectionPolicy::close;
  return {make_error_response(status, policy), policy};
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

ResponseResult prepare_file_response(UniqueFd file,
                                     std::string_view relative_path,
                                     const struct stat& metadata,
                                     ConnectionPolicy policy) {
  if (!S_ISREG(metadata.st_mode)) {
    return error_response(Status::not_found, policy);
  }
  if (metadata.st_size < 0) {
    return error_response(Status::internal_server_error, policy);
  }
  const auto size = static_cast<std::uintmax_t>(metadata.st_size);
  if (size > max_file_bytes) {
    return error_response(Status::forbidden, policy);
  }

  const auto length = static_cast<std::size_t>(size);
  return {
      make_response_header(Status::ok, length,
                           content_type_for_path(relative_path), false, policy),
      policy, base::FileRegion(std::move(file), 0, length)};
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
    const HttpRequest& request, ConnectionPolicy policy) const {
  return handle_response(request, policy).bytes;
}

ResponseResult StaticFileService::handle_response(
    const HttpRequest& request, ConnectionPolicy policy) const {
  auto result = prepare_response(request, policy);
  if (!result.file) return result;
  try {
    const auto header = result.bytes.size();
    result.bytes.resize(header + result.file->remaining());
    std::size_t offset = header;
    while (offset < result.bytes.size()) {
      const auto count = ::read(result.file->fd(), result.bytes.data() + offset,
                                result.bytes.size() - offset);
      if (count > 0) {
        offset += static_cast<std::size_t>(count);
        continue;
      }
      if (count < 0 && errno == EINTR) continue;
      return error_response(Status::internal_server_error, policy);
    }
    result.file.reset();
    return result;
  } catch (...) {
    return error_response(Status::internal_server_error, policy);
  }
}

ResponseResult StaticFileService::prepare_response(
    const HttpRequest& request, ConnectionPolicy policy) const {
  try {
    const PathResult validated = validate_path(request.target);
    if (validated.status != Status::ok) {
      return error_response(validated.status, policy);
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
        return error_response(classify_open_error(parent_fd, component, errno),
                              policy);
      }
      current_directory = UniqueFd(opened);
      parent_fd = current_directory.get();
    }

    const std::string& filename = validated.components.back();
    UniqueFd file(::openat(parent_fd, filename.c_str(),
                           O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (file.get() == -1) {
      return error_response(classify_open_error(parent_fd, filename, errno),
                            policy);
    }

    struct stat metadata {};

    if (::fstat(file.get(), &metadata) == -1) {
      return error_response(Status::internal_server_error, policy);
    }
    return prepare_file_response(std::move(file), validated.path, metadata,
                                 policy);
  } catch (...) {
    return error_response(Status::internal_server_error, policy);
  }
}

}  // namespace hp::http

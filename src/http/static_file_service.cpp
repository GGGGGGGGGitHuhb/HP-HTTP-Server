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

ResponseResult ErrorResponse(Status status, ConnectionPolicy policy) {
  if (status == Status::kBadRequest || status == Status::kMethodNotAllowed)
    policy = ConnectionPolicy::kClose;
  return {MakeErrorResponse(status, policy), policy, std::nullopt};
}

bool IsSymlinkAt(int parent_fd, const std::string& component) {
  struct stat metadata {};

  return ::fstatat(parent_fd,
                   component.c_str(),
                   &metadata,
                   AT_SYMLINK_NOFOLLOW) == 0 &&
         S_ISLNK(metadata.st_mode);
}

Status ClassifyOpenError(int parent_fd,
                         const std::string& component,
                         int error_number) {
  if (error_number == ELOOP || IsSymlinkAt(parent_fd, component)) {
    return Status::kForbidden;
  }
  if (error_number == EACCES || error_number == EPERM) {
    return Status::kForbidden;
  }
  if (error_number == ENOENT || error_number == ENOTDIR) {
    return Status::kNotFound;
  }
  return Status::kInternalServerError;
}

struct PathResult {
  Status status{Status::kOk};
  std::string path;
  std::vector<std::string> components;
};

PathResult ValidatePath(std::string_view target) {
  if (target.find('%') != std::string_view::npos) {
    return {Status::kBadRequest, {}, {}};
  }
  const std::size_t query = target.find('?');
  const std::string_view path = target.substr(0, query);
  if (path.find('\\') != std::string_view::npos) {
    return {Status::kForbidden, {}, {}};
  }
  if (path == "/") {
    return {Status::kOk, "index.html", {"index.html"}};
  }
  if (path.size() < 2 || path.front() != '/' || path.back() == '/') {
    return {Status::kForbidden, {}, {}};
  }

  PathResult result{Status::kOk, {}, {}};
  result.path = std::string(path.substr(1));
  std::size_t start = 1;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::size_t end =
        slash == std::string_view::npos ? path.size() : slash;
    const std::string_view component = path.substr(start, end - start);
    if (component.empty() || component == "." || component == "..") {
      return {Status::kForbidden, {}, {}};
    }
    result.components.emplace_back(component);
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
  return result;
}

ResponseResult PrepareFileResponse(UniqueFd file,
                                   std::string_view relative_path,
                                   const struct stat& metadata,
                                   ConnectionPolicy policy) {
  if (!S_ISREG(metadata.st_mode)) {
    return ErrorResponse(Status::kNotFound, policy);
  }
  if (metadata.st_size < 0) {
    return ErrorResponse(Status::kInternalServerError, policy);
  }
  const auto size = static_cast<std::uintmax_t>(metadata.st_size);
  if (size > kMaxFileBytes) {
    return ErrorResponse(Status::kForbidden, policy);
  }

  const auto length = static_cast<std::size_t>(size);
  return {MakeResponseHeader(Status::kOk,
                             length,
                             ContentTypeForPath(relative_path),
                             false,
                             policy),
          policy,
          base::FileRegion(std::move(file), 0, length)};
}

}  // namespace

StaticFileService::StaticFileService(const std::string& root_path) {
  root_fd_ = ::open(root_path.c_str(),
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (root_fd_ == -1) {
    throw std::system_error(errno,
                            std::generic_category(),
                            "static root is unavailable");
  }

  struct stat metadata {};

  if (::fstat(root_fd_, &metadata) == -1 || !S_ISDIR(metadata.st_mode)) {
    const int error_number = errno == 0 ? ENOTDIR : errno;
    ::close(root_fd_);
    root_fd_ = -1;
    throw std::system_error(error_number,
                            std::generic_category(),
                            "static root is not a directory");
  }
}

StaticFileService::~StaticFileService() {
  if (root_fd_ >= 0) {
    ::close(root_fd_);
  }
}

std::vector<std::byte> StaticFileService::Handle(
    const HttpRequest& request,
    ConnectionPolicy policy) const {
  return HandleResponse(request, policy).bytes;
}

ResponseResult StaticFileService::HandleResponse(
    const HttpRequest& request,
    ConnectionPolicy policy) const {
  auto result = PrepareResponse(request, policy);
  if (!result.file) return result;
  try {
    const auto header = result.bytes.size();
    result.bytes.resize(header + result.file->remaining());
    std::size_t offset = header;
    while (offset < result.bytes.size()) {
      const auto count = ::read(result.file->fd(),
                                result.bytes.data() + offset,
                                result.bytes.size() - offset);
      if (count > 0) {
        offset += static_cast<std::size_t>(count);
        continue;
      }
      if (count < 0 && errno == EINTR) continue;
      return ErrorResponse(Status::kInternalServerError, policy);
    }
    result.file.reset();
    return result;
  } catch (...) {
    return ErrorResponse(Status::kInternalServerError, policy);
  }
}

ResponseResult StaticFileService::PrepareResponse(
    const HttpRequest& request,
    ConnectionPolicy policy) const {
  try {
    const PathResult validated = ValidatePath(request.target);
    if (validated.status != Status::kOk) {
      return ErrorResponse(validated.status, policy);
    }

    int parent_fd = root_fd_;
    UniqueFd current_directory;
    for (std::size_t index = 0; index + 1 < validated.components.size();
         ++index) {
      const std::string& component = validated.components[index];
      const int opened =
          ::openat(parent_fd,
                   component.c_str(),
                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (opened == -1) {
        return ErrorResponse(ClassifyOpenError(parent_fd, component, errno),
                             policy);
      }
      current_directory = UniqueFd(opened);
      parent_fd = current_directory.get();
    }

    const std::string& filename = validated.components.back();
    UniqueFd file(::openat(parent_fd,
                           filename.c_str(),
                           O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (file.get() == -1) {
      return ErrorResponse(ClassifyOpenError(parent_fd, filename, errno),
                           policy);
    }

    struct stat metadata {};

    if (::fstat(file.get(), &metadata) == -1) {
      return ErrorResponse(Status::kInternalServerError, policy);
    }
    return PrepareFileResponse(std::move(file),
                               validated.path,
                               metadata,
                               policy);
  } catch (...) {
    return ErrorResponse(Status::kInternalServerError, policy);
  }
}

}  // namespace hp::http

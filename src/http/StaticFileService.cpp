#include "http/StaticFileService.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <utility>

#include "http/HttpResponse.h"

namespace hp::http {
namespace {

using base::UniqueFd;

ResponseResult errorResponse(Status status, ConnectionPolicy policy) {
  if (status == Status::kBadRequest || status == Status::kMethodNotAllowed)
    policy = ConnectionPolicy::kClose;
  return {makeErrorResponse(status, policy), policy, std::nullopt};
}

bool isSymlinkAt(int parentFd, const std::string& component) {
  struct stat metadata {};

  return ::fstatat(parentFd,
                   component.c_str(),
                   &metadata,
                   AT_SYMLINK_NOFOLLOW) == 0 &&
         S_ISLNK(metadata.st_mode);
}

Status classifyOpenError(int parentFd,
                         const std::string& component,
                         int errorNumber) {
  if (errorNumber == ELOOP || isSymlinkAt(parentFd, component)) {
    return Status::kForbidden;
  }
  if (errorNumber == EACCES || errorNumber == EPERM) {
    return Status::kForbidden;
  }
  if (errorNumber == ENOENT || errorNumber == ENOTDIR) {
    return Status::kNotFound;
  }
  return Status::kInternalServerError;
}

struct PathResult {
  Status status{Status::kOk};
  std::string path;
  std::vector<std::string> components;
};

PathResult validatePath(std::string_view target) {
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

ResponseResult prepareFileResponse(UniqueFd file,
                                   std::string_view relativePath,
                                   const struct stat& metadata,
                                   ConnectionPolicy policy) {
  if (!S_ISREG(metadata.st_mode)) {
    return errorResponse(Status::kNotFound, policy);
  }
  if (metadata.st_size < 0) {
    return errorResponse(Status::kInternalServerError, policy);
  }
  const auto size = static_cast<std::uintmax_t>(metadata.st_size);
  if (size > kMaxFileBytes) {
    return errorResponse(Status::kForbidden, policy);
  }

  const auto length = static_cast<std::size_t>(size);
  return {makeResponseHeader(Status::kOk,
                             length,
                             contentTypeForPath(relativePath),
                             false,
                             policy),
          policy,
          base::FileRegion(std::move(file), 0, length)};
}

}  // namespace

StaticFileService::StaticFileService(const std::string& rootPath) {
  rootFd_ =
      ::open(rootPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (rootFd_ == -1) {
    throw std::system_error(errno,
                            std::generic_category(),
                            "static root is unavailable");
  }

  struct stat metadata {};

  if (::fstat(rootFd_, &metadata) == -1 || !S_ISDIR(metadata.st_mode)) {
    const int errorNumber = errno == 0 ? ENOTDIR : errno;
    ::close(rootFd_);
    rootFd_ = -1;
    throw std::system_error(errorNumber,
                            std::generic_category(),
                            "static root is not a directory");
  }
}

StaticFileService::~StaticFileService() {
  if (rootFd_ >= 0) {
    ::close(rootFd_);
  }
}

std::vector<std::byte> StaticFileService::buildResponseBytes(
    const HttpRequest& request,
                                                             ConnectionPolicy policy) const {
  return handleResponse(request, policy).bytes;
}

ResponseResult StaticFileService::handleResponse(
    const HttpRequest& request,
    ConnectionPolicy policy) const {
  auto result = onResponse(request, policy);
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
      return errorResponse(Status::kInternalServerError, policy);
    }
    result.file.reset();
    return result;
  } catch (...) {
    return errorResponse(Status::kInternalServerError, policy);
  }
}

ResponseResult StaticFileService::onResponse(const HttpRequest& request,
                                             ConnectionPolicy policy) const {
  try {
    const PathResult validated = validatePath(request.target);
    if (validated.status != Status::kOk) {
      return errorResponse(validated.status, policy);
    }

    int parentFd = rootFd_;
    UniqueFd currentDirectory;
    for (std::size_t index = 0; index + 1 < validated.components.size();
         ++index) {
      const std::string& component = validated.components[index];
      const int opened =
          ::openat(parentFd,
                   component.c_str(),
                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (opened == -1) {
        return errorResponse(classifyOpenError(parentFd, component, errno),
                             policy);
      }
      currentDirectory = UniqueFd(opened);
      parentFd = currentDirectory.get();
    }

    const std::string& filename = validated.components.back();
    UniqueFd file(::openat(parentFd,
                           filename.c_str(),
                           O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (file.get() == -1) {
      return errorResponse(classifyOpenError(parentFd, filename, errno),
                           policy);
    }

    struct stat metadata {};

    if (::fstat(file.get(), &metadata) == -1) {
      return errorResponse(Status::kInternalServerError, policy);
    }
    return prepareFileResponse(std::move(file),
                               validated.path,
                               metadata,
                               policy);
  } catch (...) {
    return errorResponse(Status::kInternalServerError, policy);
  }
}

}  // namespace hp::http

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

ResponseResult errorResponse(Status status, ConnectionPolicy policy, bool omitBody) {
  if (status == Status::kBadRequest || status == Status::kMethodNotAllowed)
    policy = ConnectionPolicy::kClose;
  return {makeErrorResponse(status, policy, omitBody),
          policy,
          std::nullopt,
          status,
          omitBody ? 0 : errorContentBytes(status)};
}

bool isSymlinkAt(int parentFd, const std::string& component) {
  struct stat metadata {};

  return ::fstatat(parentFd, component.c_str(), &metadata, AT_SYMLINK_NOFOLLOW) == 0 &&
         S_ISLNK(metadata.st_mode);
}

Status classifyOpenError(int parentFd, const std::string& component, int errorNumber) {
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

ResponseResult prepareFileResponse(UniqueFd file,
                                   std::string_view relativePath,
                                   const struct stat& metadata,
                                   ConnectionPolicy policy,
                                   bool omitBody) {
  if (!S_ISREG(metadata.st_mode)) {
    return errorResponse(Status::kNotFound, policy, omitBody);
  }
  if (metadata.st_size < 0) {
    return errorResponse(Status::kInternalServerError, policy, omitBody);
  }
  const auto size = static_cast<std::uintmax_t>(metadata.st_size);
  if (size > kMaxFileBytes) {
    return errorResponse(Status::kForbidden, policy, omitBody);
  }

  const auto length = static_cast<std::size_t>(size);
  return {makeResponseHeader(Status::kOk, length, contentTypeForPath(relativePath), false, policy),
          policy,
          omitBody ? std::nullopt
                   : std::optional<base::FileRegion>(std::in_place, std::move(file), 0, length),
          Status::kOk,
          omitBody ? 0 : length};
}

}  // namespace

StaticFileService::StaticFileService(const std::string& rootPath) {
  rootFd_ = ::open(rootPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (rootFd_ == -1) {
    throw std::system_error(errno, std::generic_category(), "static root is unavailable");
  }

  struct stat metadata {};

  if (::fstat(rootFd_, &metadata) == -1 || !S_ISDIR(metadata.st_mode)) {
    const int errorNumber = errno == 0 ? ENOTDIR : errno;
    ::close(rootFd_);
    rootFd_ = -1;
    throw std::system_error(errorNumber, std::generic_category(), "static root is not a directory");
  }
}

StaticFileService::~StaticFileService() {
  if (rootFd_ >= 0) {
    ::close(rootFd_);
  }
}

std::vector<std::byte> StaticFileService::buildResponseBytes(const HttpRequest& request,
                                                             ConnectionPolicy policy) const {
  return handleResponse(request, policy).bytes;
}

ResponseResult StaticFileService::handleResponse(const HttpRequest& request,
                                                 ConnectionPolicy policy) const {
  const bool omitBody = request.method == "HEAD";
  auto result = onResponse(request, policy);
  if (!result.file) return result;
  try {
    const auto header = result.bytes.size();
    result.bytes.resize(header + result.file->remaining());
    std::size_t offset = header;
    while (offset < result.bytes.size()) {
      const auto count =
          ::read(result.file->fd(), result.bytes.data() + offset, result.bytes.size() - offset);
      if (count > 0) {
        offset += static_cast<std::size_t>(count);
        continue;
      }
      if (count < 0 && errno == EINTR) continue;
      return errorResponse(Status::kInternalServerError, policy, omitBody);
    }
    result.file.reset();
    return result;
  } catch (...) {
    return errorResponse(Status::kInternalServerError, policy, omitBody);
  }
}

ResponseResult StaticFileService::onResponse(const HttpRequest& request,
                                             ConnectionPolicy policy) const {
  const bool omitBody = request.method == "HEAD";
  try {
    const auto parsed = parseRequestTarget(request.target);
    if (!parsed.target) return errorResponse(parsed.status, ConnectionPolicy::kClose, omitBody);
    return onValidatedTarget(*parsed.target, omitBody, policy);
  } catch (...) {
    return errorResponse(Status::kInternalServerError, policy, omitBody);
  }
}

ResponseResult StaticFileService::onValidatedTarget(const RequestTarget& target,
                                                    bool omitBody,
                                                    ConnectionPolicy policy) const {
  try {
    int parentFd = rootFd_;
    UniqueFd currentDirectory;
    for (std::size_t index = 0; index + 1 < target.components().size(); ++index) {
      const std::string& component = target.components()[index];
      const int opened =
          ::openat(parentFd, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (opened == -1) {
        return errorResponse(classifyOpenError(parentFd, component, errno), policy, omitBody);
      }
      currentDirectory = UniqueFd(opened);
      parentFd = currentDirectory.get();
    }

    const std::string& filename = target.components().back();
    UniqueFd file(::openat(parentFd, filename.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (file.get() == -1) {
      return errorResponse(classifyOpenError(parentFd, filename, errno), policy, omitBody);
    }

    struct stat metadata {};

    if (::fstat(file.get(), &metadata) == -1) {
      return errorResponse(Status::kInternalServerError, policy, omitBody);
    }
    return prepareFileResponse(std::move(file),
                               target.path() == "/" ? "index.html" : target.path(),
                               metadata,
                               policy,
                               omitBody);
  } catch (...) {
    return errorResponse(Status::kInternalServerError, policy, omitBody);
  }
}

}  // namespace hp::http

#!/usr/bin/env python3
"""R018固定E导出接缝；仅构建槽调用，不读取dirty工作树。"""
import hashlib
from pathlib import Path
import shutil
import sys
sys.dont_write_bytecode = True

# 构建入口另核整份固定E archive SHA；每个锚必须唯一。
def replace_exact(text, before, after):
    if text.count(before) != 1:
        raise ValueError('fixed E anchor mismatch: ' + before[:70])
    return text.replace(before, after, 1)


def apply_server_patch(source_root, module_root):
    root, module = Path(source_root), Path(module_root)
    touched = {}
    def edit(relative, changes):
        path = root / relative
        original = path.read_text()
        text = original
        for before, after in changes:
            text = replace_exact(text, before, after)
        path.write_text(text)
        touched[relative] = {'before_sha256': hashlib.sha256(original.encode()).hexdigest(),
                             'after_sha256': hashlib.sha256(text.encode()).hexdigest()}
    shutil.copyfile(module / 'RequestBoundaryObserver.h', root / 'include/net/RequestBoundaryObserver.h')
    observer_source = (module / 'RequestBoundaryObserver.cpp').read_text()
    (root / 'src/net/RequestBoundaryObserver.cpp').write_text(replace_exact(observer_source, '#include "RequestBoundaryObserver.h"', '#include "net/RequestBoundaryObserver.h"'))
    edit('CMakeLists.txt', [('    src/net/TcpConnection.cpp', '    src/net/TcpConnection.cpp\n    src/net/RequestBoundaryObserver.cpp')])
    edit('include/net/TcpConnection.h', [
        ('#include "net/ConnectionIo.h"', '#include "net/ConnectionIo.h"\n#include "net/RequestBoundaryObserver.h"'),
        ('  void activateConnection();', '''  void attachBoundaryObserver(RequestBoundaryObserver* observer) noexcept;
  void observeBoundaryParsed(bool supported) noexcept;
  void observeBoundaryPartialEof() noexcept;

  void activateConnection();'''),
        ('  ConnectionIo io_;', '''  RequestBoundaryObserver* boundaryObserver_ = nullptr;
  RequestBoundaryObserver::Connection* boundaryConnection_ = nullptr;

  ConnectionIo io_;''')])
    edit('src/net/TcpConnection.cpp', [
        ('TcpConnection::~TcpConnection() noexcept { deactivateConnection(); }', '''TcpConnection::~TcpConnection() noexcept {
  deactivateConnection();
  if (boundaryObserver_ && boundaryConnection_)
    boundaryObserver_->observeClosed(boundaryConnection_, RequestBoundaryObserver::monotonicNs());
}

void TcpConnection::attachBoundaryObserver(RequestBoundaryObserver* observer) noexcept {
  boundaryObserver_ = observer;
  if (observer) boundaryConnection_ = observer->registerConnection(fd());
}

void TcpConnection::observeBoundaryPartialEof() noexcept {
  if (boundaryObserver_) boundaryObserver_->observePartialEof(boundaryConnection_);
}

void TcpConnection::observeBoundaryParsed(bool supported) noexcept {
  if (!boundaryObserver_) return;
  if (!supported) boundaryObserver_->markInvalid("unsupported_http_shape");
  boundaryObserver_->observeParsed(boundaryConnection_);
}'''),
        ('    if (read.bytesRead) {', '''    if (read.bytesRead) {
      if (boundaryObserver_)
        boundaryObserver_->observeRead(boundaryConnection_, RequestBoundaryObserver::monotonicNs());'''),
        ('    const auto written = io_.writeAvailable();', '''    const auto written = io_.writeAvailable();
    if (boundaryObserver_ && written.bytesWritten && !written.errorNumber && !io_.hasPendingOutput())
      boundaryObserver_->observeDrained(boundaryConnection_, RequestBoundaryObserver::monotonicNs());''')])
    edit('include/net/ConnectionRegistry.h', [
        ('  void beginConnectionsDrain(bool force = false);', '''  void setBoundaryObserver(RequestBoundaryObserver* observer) noexcept { boundaryObserver_ = observer; }

  void beginConnectionsDrain(bool force = false);'''),
        ('  bool draining_{false}, notified_{false};', '  RequestBoundaryObserver* boundaryObserver_ = nullptr;\n\n  bool draining_{false}, notified_{false};')])
    edit('src/net/ConnectionRegistry.cpp', [('  connection->setMessageCallback(std::move(messageCallback));',
        '  connection->attachBoundaryObserver(boundaryObserver_);\n  connection->setMessageCallback(std::move(messageCallback));')])
    edit('app/HttpConnectionHandler.cpp', [('  // 可能设置 kClose，后续构造 400 响应', '''  if (parsed.status == http::ParseStatus::kNeedMore && eof) connection.observeBoundaryPartialEof();
  if (parsed.status != http::ParseStatus::kNeedMore)
    connection.observeBoundaryParsed(parsed.status == http::ParseStatus::kComplete &&
                                     !parsed.request.closeRequested && connection.inputView().empty());
  // 可能设置 kClose，后续构造 400 响应''')])
    edit('include/net/TcpServer.h', [('  std::unique_ptr<ConnectionRegistry> mainConnectionRegistry_;', '''  // observer在注册表之前声明，反向析构仍覆盖全部连接。
  std::vector<std::unique_ptr<RequestBoundaryObserver>> boundaryObservers_;
  BoundaryOutputDirectory boundaryDirectory_;
  bool boundaryExported_ = false;

  std::unique_ptr<ConnectionRegistry> mainConnectionRegistry_;''')])
    edit('src/net/TcpServer.cpp', [
        ('#include "net/TcpServer.h"', '#include "net/TcpServer.h"\n#include <cstdlib>\n#include <cstdio>\n#include <fcntl.h>\n#include <unistd.h>'),
        ('  if (workerCount == 0) {', '''  const char* boundaryDirectory = std::getenv("HP_BOUNDARY_OUTPUT_DIR");
  if (boundaryDirectory) {
    if (!boundaryDirectory_.openDirectory(boundaryDirectory)) throw std::runtime_error("boundary output directory");
  }
  const auto observerCount = workerCount ? workerCount : 1;
  for (std::size_t index = 0; index < observerCount; ++index)
    boundaryObservers_.push_back(std::make_unique<RequestBoundaryObserver>(index));
  if (workerCount == 0) {'''),
        ('    mainConnectionRegistry_->setStopCallback(', '''    boundaryObservers_[0]->initializeWriter(RequestBoundaryObserver::processStarttime());
    mainConnectionRegistry_->setBoundaryObserver(boundaryObservers_[0].get());
    mainConnectionRegistry_->setStopCallback('''),
        ('  } else {\n    workerPool_.createWorkerThreads(', '''  } else {
    workerPool_.createWorkerThreads('''),
        ('  mainConnectionRegistry_.reset();\n  if (error)', '''  mainConnectionRegistry_.reset();
  if (workerCount_ == 0) boundaryObservers_[0]->stopWriter();
  if (!boundaryExported_ && boundaryDirectory_.fd() >= 0) {
    boundaryExported_ = true;
    const int statusFd = ::openat(boundaryDirectory_.fd(), "boundary-export-status.json",
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    FILE* boundaryStatus = statusFd < 0 ? nullptr : ::fdopen(statusFd, "w");
    if (!boundaryStatus && statusFd >= 0) ::close(statusFd);
    if (boundaryStatus) std::fputs("{\\"schema\\":\\"request-boundaries-export-v1\\",\\"workers\\":[", boundaryStatus);
    for (std::size_t index = 0; index < boundaryObservers_.size(); ++index) {
      const bool exported = boundaryObservers_[index]->exportRecords(boundaryDirectory_.fd());
      if (boundaryStatus) std::fprintf(boundaryStatus, "%s{\\"worker\\":%zu,\\"exported\\":%s}",
          index ? "," : "", index, exported ? "true" : "false");
    }
    bool statusValid = boundaryStatus != nullptr;
    if (boundaryStatus) {
      std::fputs("]}\\n", boundaryStatus);
      if (std::ferror(boundaryStatus)) statusValid = false;
      if (::fclose(boundaryStatus)) statusValid = false;
    }
    if (!boundaryDirectory_.closeDirectory()) statusValid = false;
    if (!statusValid && !error)
      error = std::make_exception_ptr(std::runtime_error("boundary status export write/close failed"));
  }
  if (error)'''),
        ('  workerRegistries_[workerIndex]->setStopCallback(', '''  if (!boundaryObservers_.empty()) {
    auto* observer = boundaryObservers_[workerIndex].get();
    observer->initializeWriter(RequestBoundaryObserver::processStarttime());
    workerRegistries_[workerIndex]->setBoundaryObserver(observer);
  }
  workerRegistries_[workerIndex]->setStopCallback('''),
        ('  workerRegistries_[workerIndex].reset();', '''  workerRegistries_[workerIndex].reset();
  if (!boundaryObservers_.empty()) boundaryObservers_[workerIndex]->stopWriter();''')])
    return touched

if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('expected source_root module_root')
    apply_server_patch(sys.argv[1], sys.argv[2])

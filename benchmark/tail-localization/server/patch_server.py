"""Static preparation: patch ONLY a fixed export copy, never repository product files."""
from pathlib import Path
import argparse
import shutil


def replace(source, old, new, count=1):
    if source.count(old) != count:
        raise ValueError('source seam drift: ' + old[:60])
    return source.replace(old, new)


def prepare(source, destination, tools):
    if destination.exists():
        raise ValueError('observed export already exists; preserve historical source')
    shutil.copytree(source, destination, ignore=shutil.ignore_patterns('.test-tmp'))
    observed = destination / 'include/tail_localization'
    observed.mkdir()
    for path in [tools / 'common/TailWire.h', tools / 'common/ClockEvidence.h', tools / 'ObserverBuffer.h', tools / 'server/ObservedRuntime.h']:
        shutil.copyfile(path, observed / path.name)
    shutil.copyfile(tools / 'server/ObservedRuntime.cpp', destination / 'src/base/ObservedRuntime.cpp')
    paths = {}
    def change(name, replacements):
        path = destination / name
        text = path.read_text()
        for old, new in replacements:
            text = replace(text, old, new)
        path.write_text(text)
        paths[name] = text
    change('CMakeLists.txt', [('    src/base/Logger.cpp','    src/base/ObservedRuntime.cpp\n    src/base/Logger.cpp'),
                             ('target_include_directories(hp_http_core PUBLIC include)',
                              'target_include_directories(hp_http_core PUBLIC include include/tail_localization)')])
    path = destination / 'app/main.cpp'
    text = '#include "tail_localization/ObservedRuntime.h"\n' + path.read_text()
    text = replace(text, '  ShutdownSignalMask mask;', '  tail_localization::initializeObserver();\n  ShutdownSignalMask mask;')
    text = replace(text, '    server.registerMessageFactoryCallback(', '    tail_localization::closeMarkerAfterStartup();\n    server.registerMessageFactoryCallback(')
    text = replace(text, '    server.runTcpServer();', '    server.runTcpServer();\n    tail_localization::exportObserverAfterWorkersJoined();')
    path.write_text(text)
    path = destination / 'src/base/AsyncLogger.cpp'
    text = '#include "tail_localization/ObservedRuntime.h"\n' + path.read_text()
    text = replace(text, 'void AsyncLogger::consumeRecords() {', 'void AsyncLogger::consumeRecords() {\n  tail_localization::registerLoggerThread();')
    path.write_text(text)
    path = destination / 'src/net/TcpServer.cpp'
    text = '#include "tail_localization/ObservedRuntime.h"\n' + path.read_text()
    text = replace(text, 'void TcpServer::initializeWorkerRegistry(std::size_t workerIndex, EventLoop& workerEventLoop) {',
                   'void TcpServer::initializeWorkerRegistry(std::size_t workerIndex, EventLoop& workerEventLoop) {\n  tail_localization::registerWorker(workerIndex);')
    text = replace(text, '  workerRegistries_[workerIndex].reset();', '  workerRegistries_[workerIndex].reset();\n  tail_localization::stopWorker(workerIndex);')
    path.write_text(text)
    path = destination / 'include/net/TcpConnection.h'
    text = '#include "tail_localization/ObservedRuntime.h"\n' + path.read_text()
    text = replace(text, '  [[nodiscard]] Identity identity() const noexcept { return identity_; }',
                   '  [[nodiscard]] Identity identity() const noexcept { return identity_; }\n\n  tail_localization::ConnectionContext& observationContext() noexcept { return observationContext_; }')
    text = replace(text, '  ConnectionIo io_;', '  tail_localization::ConnectionContext observationContext_;\n  ConnectionIo io_;')
    path.write_text(text)
    path = destination / 'src/net/TcpConnection.cpp'
    text = path.read_text()
    text = replace(text, '  if (identity == 0) throw std::invalid_argument("invalid connection identity");',
                   '  if (identity == 0) throw std::invalid_argument("invalid connection identity");\n  observationContext_ = tail_localization::registerConnection(fd(), identity);')
    text = replace(text, 'TcpConnection::~TcpConnection() noexcept { deactivateConnection(); }',
                   'TcpConnection::~TcpConnection() noexcept {\n  tail_localization::closeConnection(observationContext_);\n  deactivateConnection();\n}')
    text = replace(text, '    io_.queueOutput(bytes);', '    io_.queueOutput(bytes);\n    tail_localization::appendConnectionEvent(observationContext_, 10, bytes.size());')
    text = replace(text, '    io_.queueFile(header, std::move(file));', '    io_.queueFile(header, std::move(file));\n    tail_localization::appendConnectionEvent(observationContext_, 10, io_.pendingBytes());')
    text = replace(text, '    // 必须响应排空', '    if (!io_.hasPendingOutput() && written.bytesWritten)\n      tail_localization::markOutputDrained(observationContext_);\n    // 必须响应排空')
    text = replace(text, '  handlingEvent_ = true;',
                   '  handlingEvent_ = true;\n  auto* previousObservation = tail_localization::exchangeCurrentConnection(&observationContext_);\n  const auto firstSequence = tail_localization::prospectiveSequence(observationContext_);\n  tail_localization::appendConnectionEvent(observationContext_, 5, mask);')
    text = replace(text, '  handlingEvent_ = false;',
                   '  // 外层返回包含排空后的原日志和 interest 更新；不改原执行顺序。\n  const auto lastSequence = observationContext_.sequence < firstSequence ? firstSequence : observationContext_.sequence;\n  if (lastSequence >= firstSequence && lastSequence - firstSequence < 16) {\n    const auto savedSequence = observationContext_.sequence;\n    for (auto sequence = firstSequence; sequence <= lastSequence; ++sequence) {\n      observationContext_.sequence = sequence;\n      tail_localization::appendConnectionEvent(observationContext_, 16, mask);\n    }\n    observationContext_.sequence = savedSequence;\n  }\n  tail_localization::exchangeCurrentConnection(previousObservation);\n  handlingEvent_ = false;')
    path.write_text(text)
    path = destination / 'src/net/ConnectionIo.cpp'
    text = '#include "tail_localization/ObservedRuntime.h"\n' + path.read_text()
    text = replace(text, '  const auto count = ::sendfile(socket, file, offset, length);',
                   '  tail_localization::appendIoEvent(13, length);\n  const auto count = ::sendfile(socket, file, offset, length);\n  tail_localization::appendIoEvent(14, count > 0 ? count : 0, count < 0 ? errno : 0, count < 0 ? 1 : 0);')
    text = replace(text, '    const auto count = ::recv(fd(), tail.data(), tail.size(), 0);',
                   '    tail_localization::appendIoEvent(6, tail.size());\n    const auto count = ::recv(fd(), tail.data(), tail.size(), 0);\n    tail_localization::appendIoEvent(7, count > 0 ? count : 0, count < 0 ? errno : 0, count < 0 ? 1 : 0);\n    if (count > 0) tail_localization::receiveSucceeded();')
    text = replace(text, '    const ssize_t count = ::send(socket_.fd(), data, remaining, MSG_NOSIGNAL);',
                   '    tail_localization::appendIoEvent(11, remaining);\n    const ssize_t count = ::send(socket_.fd(), data, remaining, MSG_NOSIGNAL);\n    tail_localization::appendIoEvent(12, count > 0 ? count : 0, count < 0 ? errno : 0, count < 0 ? 1 : 0);')
    path.write_text(text)
    path = destination / 'app/HttpConnectionHandler.cpp'
    text = '#include "tail_localization/ObservedRuntime.h"\n' + path.read_text()
    text = replace(text, '  const bool eof = connection.peerClosed();',
                   '  const bool eof = connection.peerClosed();\n  struct SessionObservation {\n    tail_localization::ConnectionContext& context;\n    bool enabled;\n    ~SessionObservation() { if (enabled) tail_localization::appendConnectionEvent(context, 18); }\n  } observation{connection.observationContext(), !input.empty() || (eof && !connection.observationContext().drained)};\n  if (observation.enabled) tail_localization::appendConnectionEvent(observation.context, 17);')
    text = replace(text, '  // 可能设置 kClose，后续构造 400 响应',
                   '  tail_localization::appendConnectionEvent(connection.observationContext(), 8, parsed.requestBytes);\n  // 可能设置 kClose，后续构造 400 响应')
    text = replace(text, '  if (file)\n    connection.sendFile',
                   '  tail_localization::appendConnectionEvent(connection.observationContext(), 9, response.size());\n  if (file)\n    connection.sendFile')
    path.write_text(text)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--destination', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source.resolve(), args.destination.resolve(), Path(__file__).resolve().parents[1])

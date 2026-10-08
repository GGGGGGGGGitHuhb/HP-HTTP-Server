#include <cerrno>
#include <iostream>
#include <stdexcept>

#include "ObserverBuffer.h"

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void testDisabledBuffer() {
  tail_localization::ObserverBuffer buffer(32);
  buffer.appendEvent(tail_localization::EventKind::kClientWriteBegin, 1, 1, 0, 0, 0);
  require(buffer.countAfterWriterStopped() == 0, "disabled observer wrote data");
}

void testOverflowLatchesWithoutOverwrite() {
  tail_localization::ObserverBuffer buffer(32);
  buffer.setEnabledBeforeCallbacks(true);
  errno = EAGAIN;
  buffer.appendEvent(tail_localization::EventKind::kClientWriteBegin, 1, 1, 123, 0, 0);
  require(errno == EAGAIN, "observer changed producer errno");
  buffer.appendEvent(tail_localization::EventKind::kClientWriteComplete, 1, 1, 456, 0, 0);
  require(buffer.overflowWhileWriterActive(), "capacity did not latch overflow");
  require(buffer.countAfterWriterStopped() == 1, "overflow overwrote count");
  require(buffer.recordsAfterWriterStopped()[0].value == 123, "overflow overwrote first record");
  require(!buffer.invalidWhileWriterActive(), "valid clock marked invalid");
}

void testInvalidCapacity() {
  bool rejected = false;
  try {
    tail_localization::ObserverBuffer buffer(1);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "misaligned capacity was accepted");
}
}  // namespace

int main() {
  try {
    testDisabledBuffer();
    testOverflowLatchesWithoutOverwrite();
    testInvalidCapacity();
    std::cout << "observer buffer cases=3 status=valid\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

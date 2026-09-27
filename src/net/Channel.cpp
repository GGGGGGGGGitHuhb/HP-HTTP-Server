#include "net/Channel.h"

#include <cassert>
#include <stdexcept>
#include <utility>

#include "net/EventLoop.h"

namespace hp::net {
Channel::Channel(EventLoop& ownerEventLoop, int fd) : ownerEventLoop_(ownerEventLoop), fd_(fd) {
  if (fd < 0) throw std::invalid_argument("invalid Channel");
}

Channel::~Channel() noexcept {
  assert(ownerEventLoop_.isInLoopThread());
  assert(!registered());
}

void Channel::registerEventCallback(EventCallback eventCallback) {
  if (registered())
    throw std::logic_error("cannot replace active Channel target");
  setEventCallback(std::move(eventCallback));
}

void Channel::setInterest(std::uint32_t events) {
  if (events && !eventCallback_)
    throw std::logic_error("missing Channel target");
  ownerEventLoop_.updateChannel(*this, events);
}

void Channel::removeChannel() noexcept { ownerEventLoop_.removeChannel(*this); }
}  // namespace hp::net

#include "net/Channel.h"

#include <cassert>
#include <stdexcept>
#include <utility>

#include "net/EventLoop.h"

namespace hp::net {
Channel::Channel(EventLoop& loop, int fd) : loop_(loop), fd_(fd) {
  if (fd < 0) throw std::invalid_argument("invalid Channel");
}

Channel::~Channel() noexcept {
  assert(loop_.isInLoopThread());
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
  loop_.updateChannel(*this, events);
}

void Channel::remove() noexcept { loop_.removeChannel(*this); }
}  // namespace hp::net

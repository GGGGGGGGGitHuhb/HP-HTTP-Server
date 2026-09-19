#include "net/channel.h"

#include <cassert>
#include <stdexcept>
#include <utility>

#include "net/event_loop.h"

namespace hp::net {
Channel::Channel(EventLoop& loop, int fd) : loop_(loop), fd_(fd) {
  if (fd < 0) throw std::invalid_argument("invalid Channel");
}

Channel::~Channel() noexcept {
  assert(loop_.is_in_loop_thread());
  assert(!registered());
}

void Channel::set_HandleListenerEvent_callback(EventCallback event_callback) {
  if (registered())
    throw std::logic_error("cannot replace active Channel target");
  event_callback_ = std::move(event_callback);
}

void Channel::set_HandleConnectionEvent_callback(EventCallback event_callback) {
  if (registered())
    throw std::logic_error("cannot replace active Channel target");
  event_callback_ = std::move(event_callback);
}

void Channel::set_HandleWakeupEvent_callback(EventCallback event_callback) {
  if (registered())
    throw std::logic_error("cannot replace active Channel target");
  event_callback_ = std::move(event_callback);
}

void Channel::set_HandleShutdownSignal_callback(EventCallback event_callback) {
  if (registered())
    throw std::logic_error("cannot replace active Channel target");
  event_callback_ = std::move(event_callback);
}

void Channel::set_interest(std::uint32_t events) {
  if (events && !event_callback_)
    throw std::logic_error("missing Channel target");
  loop_.UpdateChannel(*this, events);
}

void Channel::Remove() noexcept { loop_.RemoveChannel(*this); }
}  // namespace hp::net

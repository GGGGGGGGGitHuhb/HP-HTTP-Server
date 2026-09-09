#include "net/channel.h"
#include "net/event_loop.h"
#include <cassert>
#include <stdexcept>
#include <utility>

namespace hp::net {
Channel::Channel(EventLoop& loop, int fd, Callback callback)
    : loop_(loop), fd_(fd), callback_(std::move(callback)) {
    if (fd < 0 || !callback_)
        throw std::invalid_argument("invalid Channel");
}

Channel::~Channel() noexcept {
    assert(!registered());
}

void Channel::set_interest(std::uint32_t events) {
    loop_.update_channel(*this, events);
}

void Channel::remove() noexcept {
    loop_.remove_channel(*this);
}
}

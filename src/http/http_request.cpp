#include "http/http_request.h"
#include <algorithm>
#include <cctype>

namespace hp::http {
namespace {
bool token_character(unsigned char c) {
    constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           punctuation.find(static_cast<char>(c)) != std::string_view::npos;
}
}

bool RequestParser::validate_request_line() {
    std::size_t first = line_size_, second = line_size_;
    for (std::size_t i = 0; i < line_size_; ++i) {
        ++scan_steps_;
        if (storage_[i] != ' ')
            continue;
        if (first == line_size_)
            first = i;
        else if (second == line_size_)
            second = i;
        else
            return false;
    }
    if (first == 0 || second == line_size_ || second <= first + 1)
        return false;
    for (std::size_t i = 0; i < first; ++i) {
        ++scan_steps_;
        if (!token_character(static_cast<unsigned char>(storage_[i])))
            return false;
    }
    if (storage_[first + 1] != '/')
        return false;
    for (std::size_t i = first + 1; i < second; ++i) {
        ++scan_steps_;
        const auto c = static_cast<unsigned char>(storage_[i]);
        if (c <= 0x20U || c == 0x7fU || c == '#')
            return false;
    }
    constexpr std::string_view version = "HTTP/1.1";
    if (line_size_ - second - 1 != version.size())
        return false;
    for (std::size_t i = 0; i < version.size(); ++i) {
        ++scan_steps_;
        if (storage_[second + 1 + i] != version[i])
            return false;
    }
    method_size_ = first;
    target_start_ = first + 1;
    target_size_ = second - first - 1;
    return true;
}

bool RequestParser::validate_header() {
    const std::string_view line(storage_.data() + line_start_, line_size_);
    if (line.front() == ' ' || line.front() == '\t')
        return false;
    std::size_t colon = 0;
    for (; colon < line.size(); ++colon) {
        ++scan_steps_;
        if (line[colon] == ':')
            break;
    }
    if (colon == 0 || colon == line.size())
        return false;
    for (std::size_t i = 0; i < colon; ++i) {
        ++scan_steps_;
        if (!token_character(static_cast<unsigned char>(line[i])))
            return false;
    }
    bool nonempty_value = false;
    for (std::size_t i = colon + 1; i < line.size(); ++i) {
        ++scan_steps_;
        const auto c = static_cast<unsigned char>(line[i]);
        if (c == 0x7fU || (c < 0x20U && c != '\t'))
            return false;
        if (c != ' ' && c != '\t')
            nonempty_value = true;
    }
    const auto equal_ascii = [&](std::string_view value, std::string_view expected) {
        if (value.size() != expected.size())
            return false;
        for (std::size_t i = 0; i < value.size(); ++i) {
            ++scan_steps_;
            const char c = value[i] >= 'A' && value[i] <= 'Z' ? value[i] + ('a' - 'A') : value[i];
            if (c != expected[i])
                return false;
        }
        return true;
    };
    const auto trim = [&](std::string_view value) {
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            ++scan_steps_;
            value.remove_prefix(1);
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
            ++scan_steps_;
            value.remove_suffix(1);
        }
        return value;
    };
    const auto name = line.substr(0, colon);
    const auto value = trim(line.substr(colon + 1));
    if (equal_ascii(name, "host")) {
        if (host_seen_ || !nonempty_value)
            return false;
        host_seen_ = true;
    } else if (equal_ascii(name, "content-length")) {
        if (content_length_seen_ || value.empty())
            return false;
        content_length_seen_ = true;
        // Only decimal zero is supported: no arithmetic and therefore no overflow.
        for (char c : value) {
            ++scan_steps_;
            if (c != '0')
                return false;
        }
    } else if (equal_ascii(name, "transfer-encoding") || equal_ascii(name, "expect")) {
        return false;
    } else if (equal_ascii(name, "connection")) {
        std::size_t start = 0;
        for (std::size_t i = 0; i <= value.size(); ++i) {
            ++scan_steps_;
            if (i != value.size() && value[i] != ',')
                continue;
            const auto token = trim(value.substr(start, i - start));
            for (unsigned char c : token) {
                ++scan_steps_;
                if (!token_character(c))
                    return false;
            }
            if (equal_ascii(token, "close"))
                close_requested_ = true;
            start = i + 1;
        }
    }
    return true;
}

void RequestParser::finish_line() {
    if (state_ == ParserState::request_line) {
        if (!validate_request_line()) {
            state_ = ParserState::error;
            return;
        }
        line_start_ = line_size_;
        state_ = ParserState::headers;
    } else if (line_size_ == 0) {
        if (!host_seen_) {
            state_ = ParserState::error;
            return;
        }
        state_ = ParserState::complete;
        const std::string_view method(storage_.data(), method_size_);
        status_ = method == "GET" ? ParseStatus::complete : ParseStatus::method_not_allowed;
    } else if (!validate_header()) {
        state_ = ParserState::error;
    }
    line_size_ = 0;
}

FeedResult RequestParser::result(std::size_t accepted) const {
    FeedResult result{status_, {}, accepted, request_bytes_};
    if (state_ == ParserState::complete) {
        result.request.close_requested = close_requested_;
        result.request.method.assign(storage_.data(), method_size_);
        result.request.target.assign(storage_.data() + target_start_, target_size_);
    }
    return result;
}

FeedResult RequestParser::feed(std::string_view bytes) {
    std::size_t accepted = 0;
    while (accepted < bytes.size() && state_ != ParserState::complete &&
           state_ != ParserState::error) {
        const char c = bytes[accepted];
        ++accepted;
        ++request_bytes_;
        ++scan_steps_;
        if (pending_cr_) {
            pending_cr_ = false;
            if (c != '\n')
                state_ = ParserState::error;
            else
                finish_line();
        } else if (c == '\r') {
            pending_cr_ = true;
        } else if (c == '\n' || c == '\0') {
            state_ = ParserState::error;
        } else if (state_ == ParserState::request_line && line_size_ == max_request_line_bytes) {
            state_ = ParserState::error;
        } else {
            storage_[line_start_ + line_size_++] = c;
            peak_buffered_ = std::max(peak_buffered_, buffered_bytes());
        }
        if (request_bytes_ == max_request_bytes && state_ != ParserState::complete)
            state_ = ParserState::error;
        if (state_ == ParserState::error)
            status_ = ParseStatus::bad_request;
    }
    return result(accepted);
}

void RequestParser::reset() noexcept {
    state_ = ParserState::request_line;
    status_ = ParseStatus::need_more;
    pending_cr_ = host_seen_ = content_length_seen_ = close_requested_ = false;
    line_start_ = line_size_ = request_bytes_ = 0;
    method_size_ = target_start_ = target_size_ = 0;
    scan_steps_ = peak_buffered_ = 0;
}

ParseResult parse_request(std::string_view bytes) {
    RequestParser parser;
    const auto parsed = parser.feed(bytes);
    return {parsed.status, parsed.request,
            parsed.status == ParseStatus::complete ||
                    parsed.status == ParseStatus::method_not_allowed
                ? parsed.request_bytes
                : 0};
}
}

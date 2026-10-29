#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace hp::http {

inline constexpr std::size_t kMaxHeaderBytes = 16U * 1024U;
inline constexpr std::size_t kMaxBodyBytes = 1024U * 1024U;
inline constexpr std::size_t kMaxRequestLineBytes = 4U * 1024U;

struct HttpRequest {
  std::string method;
  std::string target;
  bool closeRequested{false};
  std::string body;
};

// 当前请求解析完后的结果
enum class ParseStatus {
  kNeedMore,
  kComplete,
  kBadRequest,
  kPayloadTooLarge,
  kMethodNotAllowed,
};

struct ParseResult {
  ParseStatus status{ParseStatus::kNeedMore};
  HttpRequest request;
  std::size_t consumedBytes{0};
};

// 当前请求被解析到的位置状态
enum class ParserState { kRequestLine, kHeaders, kBody, kComplete, kError };

struct FeedResult {
  ParseStatus status{ParseStatus::kNeedMore};
  HttpRequest request;           // 独立值；在 Reset 或调用方释放输入后仍安全。
  std::size_t acceptedBytes{0};  // 仅本次 feed 接受的字节数。
  std::size_t requestBytes{0};   // 累计字节数，发生错误时包含导致错误的字节。
};

// 解析器
class RequestParser final {
 public:
  // 解析一条请求并返回解析结果
  [[nodiscard]] FeedResult feedRequestBytes(std::string_view newBytes);
  // 不会清空连接的输入缓冲区，因此在调用之后，请求仍然可读
  void resetRequestParser() noexcept;

  [[nodiscard]] ParserState state() const noexcept { return state_; }

  // 显式统计消息定界与完整行校验循环中的字节访问次数。
  [[nodiscard]] std::size_t scanSteps() const noexcept { return scanSteps_; }

  // 仅统计头解析存储，不含正文或整个进程内存。
  [[nodiscard]] std::size_t bufferedBytes() const noexcept { return lineStart_ + lineSize_; }

  [[nodiscard]] std::size_t peakBufferedBytes() const noexcept { return peakBuffered_; }

  [[nodiscard]] bool isHeadRequest() const noexcept { return headRequest_; }

  [[nodiscard]] bool pendingCr() const noexcept { return pendingCr_; }

 private:
  bool validateRequestLine();
  bool validateHeader();
  void finishLine();
  void finishRequest();
  std::size_t appendBody(std::string_view bytes);
  [[nodiscard]] FeedResult buildFeedResult(std::size_t accepted) const;

  // 仅在本次调用中借用输入；每个检查过的字符都会更新
  // scan_steps_.
  bool equalsAsciiCaseInsensitive(std::string_view value, std::string_view expected);
  // 去掉 `value` 两端的空格和制表符
  std::string_view trimHeaderWhitespace(std::string_view value);

  // 请求行只保留一次；后续 Header 行复用剩余空间。不得
  // 保留指向调用方内存的视图，也不得把流水线后续请求复制到此
  // 数组中。
  std::array<char, kMaxHeaderBytes> storage_{};

  ParserState state_{ParserState::kRequestLine};  // 当前请求被解析到的位置状态
  ParseStatus status_{ParseStatus::kNeedMore};    // 当前请求解析完后的结果
  bool pendingCr_{false};  // true 表示已收到 CR('\r')，正在等待 LF('\n')
  bool headRequest_{false};
  bool hostSeen_{false};
  bool contentLengthSeen_{false};
  bool closeRequested_{false};

  std::size_t lineStart_{0}, lineSize_{0},  // lineSize_ 表示当前行的内容长度，不含结束符
      requestBytes_{0};
  std::size_t methodSize_{0}, targetStart_{0}, targetSize_{0};

  std::size_t scanSteps_{0}, peakBuffered_{0};
  std::size_t headerBytes_{0};
  std::uint64_t contentLength_{0};
  std::string body_;
};

[[nodiscard]] ParseResult parseRequest(std::string_view bytes);

}  // namespace hp::http

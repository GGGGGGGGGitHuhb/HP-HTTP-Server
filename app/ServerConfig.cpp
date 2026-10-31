#include "ServerConfig.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <charconv>
#include <set>
#include <stdexcept>
#include <string_view>

#include "nlohmann/json.hpp"

namespace hp::app {
namespace {
using Json = nlohmann::json;
constexpr std::size_t kMaxConfigBytes = 65536;

std::string readConfigFile(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) throw std::invalid_argument("cannot open config file");
  struct FileOwner {
    int fd;
    ~FileOwner() { ::close(fd); }
  } owner{fd};
  struct stat info {};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
      info.st_size > static_cast<off_t>(kMaxConfigBytes))
    throw std::invalid_argument("config must be a regular file of at most 64KiB");
  std::string text;
  std::array<char, 4096> block{};
  for (;;) {
    const auto count = ::read(fd, block.data(), block.size());
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) throw std::invalid_argument("config read failed");
    if (!count) break;
    if (static_cast<std::size_t>(count) > kMaxConfigBytes - text.size())
      throw std::invalid_argument("config grew beyond 64KiB");
    text.append(block.data(), static_cast<std::size_t>(count));
  }
  return text;
}

class ConfigEvents {
 public:
  bool onParse(int depth, Json::parse_event_t event, Json& value);

 private:
  std::set<std::string> keys_;
};

bool ConfigEvents::onParse(int depth, Json::parse_event_t event, Json& value) {
  if (event == Json::parse_event_t::array_start ||
      (event == Json::parse_event_t::object_start && depth != 0))
    throw std::invalid_argument("config must be a flat object");
  if (event == Json::parse_event_t::key && !keys_.insert(value.get<std::string>()).second)
    throw std::invalid_argument("duplicate config key");
  return true;
}

Json parseConfigFile(const std::string& path) {
  const auto text = readConfigFile(path);
  if (text.starts_with("\xEF\xBB\xBF")) throw std::invalid_argument("config BOM is forbidden");
  ConfigEvents events;
  auto parsed = Json::parse(text, [&events](int depth, Json::parse_event_t event, Json& value) {
    return events.onParse(depth, event, value);
  });
  if (!parsed.is_object()) throw std::invalid_argument("config must be an object");
  return parsed;
}

std::uint64_t checkedInteger(const Json& value, std::uint64_t minimum, std::uint64_t maximum) {
  if (!value.is_number_unsigned()) throw std::invalid_argument("unsigned integer required");
  const auto number = value.get<std::uint64_t>();
  if (number < minimum || number > maximum) throw std::invalid_argument("config integer range");
  return number;
}

void applyField(ServerOptions& options, const std::string& key, const Json& value) {
  if (key == "root") {
    if (!value.is_string()) throw std::invalid_argument("root must be a string");
    const auto text = value.get<std::string>();
    if (text.empty() || text.size() > 4096 || text.find('\0') != std::string::npos)
      throw std::invalid_argument("root must be a nonempty path of at most 4096 bytes");
    (void)value.dump();  // 严格验证CLI字符串UTF-8；文件由JSON parser验证。
    options.root = text;
  } else if (key == "access_log" || key == "metrics_on_exit") {
    if (!value.is_boolean()) throw std::invalid_argument("boolean required");
    (key == "access_log" ? options.accessLog : options.metricsOnExit) = value.get<bool>();
  } else if (key == "port" || key == "admin_port") {
    const auto port = static_cast<std::uint16_t>(checkedInteger(value, 0, 65535));
    if (key == "port")
      options.port = port;
    else
      options.adminPort = port;
  } else if (key == "threads") {
    options.threads = checkedInteger(value, 0, 64);
  } else if (key == "max_connections") {
    options.resourceLimits.connections = checkedInteger(value, 1, 4096);
  } else if (key == "input_budget_bytes") {
    options.resourceLimits.inputBytes = checkedInteger(value, 1081344, 1073741824);
  } else if (key == "output_budget_bytes") {
    options.resourceLimits.outputBytes = checkedInteger(value, 1024, 1073741824);
  } else if (key == "idle_timeout_ms" || key == "keep_alive_timeout_ms" ||
             key == "request_read_timeout_ms" || key == "shutdown_timeout_ms") {
    const auto duration = std::chrono::milliseconds(
        checkedInteger(value, 0, key == "shutdown_timeout_ms" ? 60000 : 86400000));
    if (key == "idle_timeout_ms")
      options.timeouts.idle = duration;
    else if (key == "keep_alive_timeout_ms")
      options.timeouts.keepAlive = duration;
    else if (key == "request_read_timeout_ms")
      options.requestReadTimeout = duration;
    else
      options.shutdownTimeout = duration;
  } else {
    throw std::invalid_argument("unknown config field");
  }
}

Json parseCliValue(const std::string& key, std::string_view value) {
  if (key == "root") return std::string(value);
  std::uint64_t number{};
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
  if (value.empty() || error != std::errc{} || end != value.data() + value.size())
    throw std::invalid_argument("decimal CLI integer required");
  return number;
}

ServerOptions resolveOptions(int argc, char* argv[]) {
  Json cli = Json::object();
  std::set<std::string> seen;
  std::optional<std::string> configPath;
  bool disableAdmin = false;
  ServerOptions checkedCli;
  for (int index = 1; index < argc; ++index) {
    std::string key = argv[index];
    if (!key.starts_with("--") || key.find('_') != std::string::npos)
      throw std::invalid_argument("unknown option");
    key.erase(0, 2);
    for (auto& character : key)
      if (character == '-') character = '_';
    bool negative = key.starts_with("no_");
    if (negative && key != "no_admin" && key != "no_access_log" && key != "no_metrics_on_exit")
      throw std::invalid_argument("unknown negative option");
    if (negative) key.erase(0, 3);
    if (key == "admin" && negative) key = "admin_port";
    if (!seen.insert(key).second) throw std::invalid_argument("repeated CLI option");
    if (key == "access_log" || key == "metrics_on_exit")
      cli[key] = !negative;
    else if (key == "admin_port" && negative) {
      disableAdmin = true;
      continue;
    } else {
      if (negative || key == "help") throw std::invalid_argument("unknown option");
      if (++index >= argc) throw std::invalid_argument("option value is missing");
      if (key == "config") {
        configPath = argv[index];
        continue;
      }
      cli[key] = parseCliValue(key, argv[index]);
    }
    applyField(checkedCli, key, cli[key]);
  }
  Json file = configPath ? parseConfigFile(*configPath) : Json::object();
  ServerOptions result;
  for (const auto& [key, value] : file.items()) applyField(result, key, value);
  for (const auto& [key, value] : cli.items()) applyField(result, key, value);
  if (disableAdmin) result.adminPort.reset();
  if ((!file.contains("port") && !cli.contains("port")) || result.root.empty())
    throw std::invalid_argument("port and root are required");
  return result;
}
}  // namespace

ServerOptions parseServerOptions(int argc, char* argv[]) {
  try {
    return resolveOptions(argc, argv);
  } catch (const Json::exception&) {
    throw std::invalid_argument("invalid config JSON or UTF-8");
  }
}

void printUsage(std::ostream& output) {
  output
      << "Usage: hp_http_server --port <0-65535> --root <directory> [--config <file>]\n"
      << "       hp_http_server --help\n"
      << "V0.1 / S3 minimal HTTP static file server\n"
      << "--threads <0-64> defaults to 2; 0 selects a single Reactor.\n"
      << "--idle-timeout-ms <0-86400000> defaults to 30000; --keep-alive-timeout-ms defaults to "
         "15000.\n"
      << "--request-read-timeout-ms <0-86400000> defaults to 10000; --shutdown-timeout-ms "
         "<0-60000> defaults to 5000.\n"
      << "--access-log / --no-access-log; --metrics-on-exit / --no-metrics-on-exit (default off).\n"
      << "--admin-port <0-65535> enables IPv4 loopback administration; --no-admin disables it.\n"
      << "--max-connections <1-4096>; --input-budget-bytes <1081344-1073741824>; "
         "--output-budget-bytes <1024-1073741824>.\n"
      << "Defaults < config file < CLI. SIGINT/SIGTERM drains network; another signal forces "
         "close.\n"
      << "Network deadlines do not bound blocked logger or business worker joins.\n";
}
}  // namespace hp::app

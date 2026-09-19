#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "http/http_request.h"
#include "http/http_response.h"
#include "http/static_file_service.h"

namespace {

int failures = 0;
int ok_hits = 0;
int bad_request_hits = 0;
int forbidden_hits = 0;
int not_found_hits = 0;
int internal_error_hits = 0;
constexpr std::string_view kSecret = "S3-SIBLING-SECRET-DO-NOT-SERVE";

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class Fixture {
 public:
  Fixture() {
    const char* configured = std::getenv("HP_S3_TEST_TMP_ROOT");
    const std::filesystem::path base =
        configured == nullptr
            ? std::filesystem::path(".cache/olympus-v0.1-s3/tests")
            : std::filesystem::path(configured);
    std::filesystem::create_directories(base);
    std::string pattern = (base / "static-XXXXXX").string();
    std::vector<char> storage(pattern.begin(), pattern.end());
    storage.push_back('\0');
    char* created = ::mkdtemp(storage.data());
    if (created == nullptr) {
      throw std::runtime_error(std::string("mkdtemp: ") + std::strerror(errno));
    }
    workspace_ = created;
    root_ = workspace_ / "root";
    sibling_ = workspace_ / "sibling-secret.txt";
    std::filesystem::create_directories(root_ / "assets");
    WriteText(root_ / "index.html", "<h1>S3 index</h1>\n");
    WriteText(root_ / "assets" / "note.txt", "static text\n");
    WriteText(root_ / "assets" / "blob.weird", "unknown mime\n");
    WriteText(sibling_, kSecret);
    const std::vector<unsigned char> binary{0x00, 0x01, 0x7f, 0xff, 0x41};
    std::ofstream output(root_ / "assets" / "data.png", std::ios::binary);
    output.write(reinterpret_cast<const char*>(binary.data()),
                 static_cast<std::streamsize>(binary.size()));
    output.close();
    std::filesystem::create_symlink(sibling_, root_ / "escape.txt");
    std::filesystem::create_directory_symlink(workspace_, root_ / "escape-dir");
    const auto oversized = root_ / "oversized.bin";
    const int fd = ::open(oversized.c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                          0600);
    if (fd == -1 ||
        ::ftruncate(fd, static_cast<off_t>(hp::http::kMaxFileBytes + 1)) ==
            -1) {
      const int error_number = errno;
      if (fd >= 0) ::close(fd);
      throw std::runtime_error(std::string("oversized fixture: ") +
                               std::strerror(error_number));
    }
    ::close(fd);
  }

  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove_all(workspace_, ignored);
  }

  static void WriteText(const std::filesystem::path& path,
                        std::string_view content) {
    std::ofstream output(path, std::ios::binary);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output) {
      throw std::runtime_error("fixture write failed");
    }
  }

  std::filesystem::path workspace_;
  std::filesystem::path root_;
  std::filesystem::path sibling_;
};

struct Response {
  int status;
  std::string header;
  std::vector<std::byte> body;
};

Response ParseResponse(const std::vector<std::byte>& bytes) {
  const std::string_view raw(reinterpret_cast<const char*>(bytes.data()),
                             bytes.size());
  const std::size_t boundary = raw.find("\r\n\r\n");
  if (boundary == std::string_view::npos || raw.size() < 12) {
    throw std::runtime_error("invalid response");
  }
  const int status = std::stoi(std::string(raw.substr(9, 3)));
  if (status == 200) ++ok_hits;
  if (status == 400) ++bad_request_hits;
  if (status == 403) ++forbidden_hits;
  if (status == 404) ++not_found_hits;
  if (status == 500) ++internal_error_hits;
  return {status,
          std::string(raw.substr(0, boundary + 4)),
          std::vector<std::byte>(bytes.begin() + boundary + 4, bytes.end())};
}

std::string BodyText(const Response& response) {
  return {reinterpret_cast<const char*>(response.body.data()),
          response.body.size()};
}

std::size_t OpenFdCount() {
  std::size_t count = 0;
  for (const auto& ignored :
       std::filesystem::directory_iterator("/proc/self/fd")) {
    (void)ignored;
    ++count;
  }
  return count;
}

Response Request(const hp::http::StaticFileService& service,
                 std::string target) {
  return ParseResponse(
      service.Handle(hp::http::HttpRequest{"GET", std::move(target)}));
}

void TestSuccessAndMime(const hp::http::StaticFileService& service) {
  const Response index = Request(service, "/");
  Expect(index.status == 200, "root must map to index.html");
  Expect(BodyText(index) == "<h1>S3 index</h1>\n", "index bytes must be exact");
  Expect(index.header.find("Content-Type: text/html; charset=utf-8\r\n") !=
             std::string::npos,
         "index MIME must be HTML");

  const Response query = Request(service, "/assets/note.txt?download=1");
  Expect(query.status == 200 && BodyText(query) == "static text\n",
         "query must be ignored for file mapping");

  const Response binary = Request(service, "/assets/data.png");
  const std::vector<std::byte> expected{std::byte{0x00},
                                        std::byte{0x01},
                                        std::byte{0x7f},
                                        std::byte{0xff},
                                        std::byte{0x41}};
  Expect(binary.status == 200 && binary.body == expected,
         "binary bytes including NUL must be exact");

  const Response unknown = Request(service, "/assets/blob.weird");
  Expect(
      unknown.status == 200 &&
          unknown.header.find("Content-Type: application/octet-stream\r\n") !=
              std::string::npos,
      "unknown extension must use octet-stream");
}

void TestRejections(const hp::http::StaticFileService& service) {
  const std::vector<std::pair<std::string, int>> cases = {
      {"/missing.txt", 404},
      {"/assets", 404},
      {"/../sibling-secret.txt", 403},
      {"/./index.html", 403},
      {"//index.html", 403},
      {"/assets//note.txt", 403},
      {"/assets/", 403},
      {"/assets\\note.txt", 403},
      {"/%2e%2e/sibling-secret.txt", 400},
      {"/escape.txt", 403},
      {"/escape-dir/sibling-secret.txt", 403},
      {"/oversized.bin", 403},
  };
  for (const auto& [target, expected_status] : cases) {
    const Response response = Request(service, target);
    Expect(response.status == expected_status,
           "path policy must return its deterministic status");
    Expect(BodyText(response).find(kSecret) == std::string::npos,
           "rejected response must not contain sibling secret");
  }

  const Response internal = Request(service, "/" + std::string(300, 'x'));
  Expect(internal.status == 500,
         "unclassified open failure must become a deterministic 500");
  Expect(BodyText(internal) == "500 Internal Server Error\n",
         "500 body must be deterministic");
}

void TestFdStabilityAndReadOnly(Fixture& fixture,
                                const hp::http::StaticFileService& service) {
  const auto note_path = fixture.root_ / "assets" / "note.txt";
  const auto before_mtime = std::filesystem::last_write_time(note_path);
  std::ifstream before_file(note_path, std::ios::binary);
  const std::string before((std::istreambuf_iterator<char>(before_file)),
                           std::istreambuf_iterator<char>());
  const std::size_t baseline = OpenFdCount();
  for (int iteration = 0; iteration < 500; ++iteration) {
    (void)Request(service,
                  iteration % 2 == 0 ? "/assets/note.txt"
                                     : "/escape-dir/sibling-secret.txt");
  }
  const std::size_t after = OpenFdCount();
  Expect(after == baseline,
         "repeated success/failure requests must keep fd count stable");

  std::ifstream after_file(note_path, std::ios::binary);
  const std::string after_bytes((std::istreambuf_iterator<char>(after_file)),
                                std::istreambuf_iterator<char>());
  Expect(after_bytes == before, "static service must not modify a served file");
  Expect(std::filesystem::last_write_time(note_path) == before_mtime,
         "static service must not change a served file mtime");
}

void TestRootValidation(Fixture& fixture) {
  bool file_rejected = false;
  try {
    hp::http::StaticFileService invalid(fixture.sibling_.string());
  } catch (const std::system_error&) {
    file_rejected = true;
  }
  Expect(file_rejected, "non-directory root must be rejected");

  const auto root_link = fixture.workspace_ / "root-link";
  std::filesystem::create_directory_symlink(fixture.root_, root_link);
  bool symlink_rejected = false;
  try {
    hp::http::StaticFileService invalid(root_link.string());
  } catch (const std::system_error&) {
    symlink_rejected = true;
  }
  Expect(symlink_rejected, "symlink root must be rejected by no-follow open");
}

}  // namespace

int main() {
  try {
    Fixture fixture;
    hp::http::StaticFileService service(fixture.root_.string());
    for (const auto& target :
         std::vector<std::string>{"/",
                                  "/missing",
                                  "/../sibling-secret.txt",
                                  "/bad%20target",
                                  "/" + std::string(300, 'x')}) {
      const auto result =
          service.HandleResponse({"GET", target},
                                 hp::http::ConnectionPolicy::kKeepAlive);
      const std::string raw(reinterpret_cast<const char*>(result.bytes.data()),
                            result.bytes.size());
      const bool close =
          std::string_view(target).find('%') != std::string_view::npos;
      Expect(result.effective_policy ==
                 (close ? hp::http::ConnectionPolicy::kClose
                        : hp::http::ConnectionPolicy::kKeepAlive),
             "service effective policy");
      Expect(
          raw.find(close ? "Connection: close\r\n"
                         : "Connection: keep-alive\r\n") != std::string::npos,
          "service bytes and metadata agree");
      Expect(service.Handle({"GET", target},
                            hp::http::ConnectionPolicy::kKeepAlive) ==
                 result.bytes,
             "legacy handle delegates without changing bytes");
    }
    TestSuccessAndMime(service);
    TestRejections(service);
    TestFdStabilityAndReadOnly(fixture, service);
    TestRootValidation(fixture);
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }

  if (failures != 0) {
    std::cerr << failures << " static file assertion(s) failed\n";
    return 1;
  }
  std::cout << "Static file tests passed; 200_hits=" << ok_hits
            << " 400_hits=" << bad_request_hits
            << " 403_hits=" << forbidden_hits << " 404_hits=" << not_found_hits
            << " 500_hits=" << internal_error_hits
            << " fd_stability_iterations=500 secret_leaks=0\n";
  return 0;
}

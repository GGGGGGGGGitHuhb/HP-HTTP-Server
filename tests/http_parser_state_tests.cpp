#include "http/http_request.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace hp::http;
int failures{};
std::size_t splits{}, byte_feeds{}, cr_splits{}, headers_hits{}, complete_hits{}, error_hits{},
    line_hits{};
std::size_t max_scans{}, max_buffer{};

void expect(bool ok, const char* why) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << why << '\n';
    }
}

void observe(const RequestParser& p, const FeedResult& r) {
    switch (p.state()) {
    case ParserState::request_line:
        ++line_hits;
        break;
    case ParserState::headers:
        ++headers_hits;
        break;
    case ParserState::complete:
        ++complete_hits;
        break;
    case ParserState::error:
        ++error_hits;
        break;
    }
    expect(p.scan_steps() <= 10 * r.request_bytes, "linear framing and validation byte visits");
    expect(p.peak_buffered_bytes() <= max_request_bytes, "bounded parser storage");
    max_scans = std::max(max_scans, p.scan_steps());
    max_buffer = std::max(max_buffer, p.peak_buffered_bytes());
}

bool same(const FeedResult& a, const FeedResult& b) {
    return a.status == b.status && a.request.method == b.request.method &&
           a.request.target == b.request.target &&
           a.request.close_requested == b.request.close_requested &&
           a.request_bytes == b.request_bytes;
}

void chunk_variants(const std::string& input, ParseStatus expected, bool all_splits = true) {
    RequestParser whole;
    const auto baseline = whole.feed(input);
    expect(baseline.status == expected, "independent support-matrix result");
    observe(whole, baseline);
    std::vector<std::size_t> points;
    if (all_splits)
        for (std::size_t i = 0; i <= input.size(); ++i)
            points.push_back(i);
    else
        for (std::size_t i : {std::size_t{0}, std::size_t{1}, std::size_t{4095}, std::size_t{4096},
                              std::size_t{4097}, input.size() - 1, input.size()})
            if (i <= input.size())
                points.push_back(i);
    for (auto split : points) {
        RequestParser p;
        auto first = p.feed(std::string_view(input).substr(0, split));
        observe(p, first);
        if (p.pending_cr())
            ++cr_splits;
        auto second = p.feed(std::string_view(input).substr(split));
        observe(p, second);
        ++splits;
        expect(same(baseline, second) &&
                   first.accepted_bytes + second.accepted_bytes == baseline.request_bytes,
               "split result and error offset independent");
        if (first.status == ParseStatus::need_more)
            expect(first.accepted_bytes == split, "NeedMore accepts full block");
    }
    RequestParser p;
    FeedResult last;
    std::size_t total{};
    for (char c : input) {
        last = p.feed({&c, 1});
        total += last.accepted_bytes;
        ++byte_feeds;
        observe(p, last);
    }
    expect(same(last, baseline) && total == baseline.request_bytes,
           "one-byte feed agrees and does not rescan prefix");
    const auto wrapper = parse_request(input);
    const bool success =
        expected == ParseStatus::complete || expected == ParseStatus::method_not_allowed;
    expect(wrapper.status == baseline.status && wrapper.request.method == baseline.request.method &&
               wrapper.request.target == baseline.request.target &&
               wrapper.consumed_bytes == (success ? baseline.request_bytes : 0),
           "one-shot wrapper retains count convention");
}

void support_and_splits() {
    const std::vector<std::pair<std::string, ParseStatus>> samples = {
        {"GET /asset?x=1 HTTP/1.1\r\nHost: a\r\nX: unknown\r\n\r\n", ParseStatus::complete},
        {"CUSTOM / HTTP/1.1\r\nhOsT: \t a \t\r\n\r\n", ParseStatus::method_not_allowed},
        {"POST / HTTP/1.1\r\nHost: a\r\nBad Header: x\r\n\r\n", ParseStatus::bad_request},
        {"GET / HTTP/1.1\r\nX: a\r\n\r\n", ParseStatus::bad_request},
        {"GET / HTTP/1.1\r\nHost: a\r\nHOST: b\r\n\r\n", ParseStatus::bad_request},
        {"GET / HTTP/1.1\r\nHost: \t \r\n\r\n", ParseStatus::bad_request},
        {"G?T / HTTP/1.1\r\nHost: a\r\n\r\n", ParseStatus::bad_request},
        {"GET http://a/ HTTP/1.1\r\nHost: a\r\n\r\n", ParseStatus::bad_request},
        {"GET /#f HTTP/1.1\r\nHost: a\r\n\r\n", ParseStatus::bad_request},
        {"GET / HTTP/1.0\r\nHost: a\r\n\r\n", ParseStatus::bad_request},
        {"GET / HTTP/1.1\r\n folded: x\r\nHost: a\r\n\r\n", ParseStatus::bad_request},
        {"GET / HTTP/1.1\nHost: a\n\n", ParseStatus::bad_request},
        {"GET / HTTP/1.1\rXHost: a\r\n\r\n", ParseStatus::bad_request},
        {std::string("GET / HTTP/1.1\r\nHost: a\0junk\r\n\r\n", 32), ParseStatus::bad_request},
        {"GET / HTTP/1.1\r\nHost: a\r\nX: \x01\r\n\r\n", ParseStatus::bad_request},
        {"GET / HTTP/1.1\r\nHost: a\r\nX: \x7f\r\n\r\n", ParseStatus::bad_request},
        {"GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 9\r\nConnection: keep-alive\r\n\r\nbody-tail",
         ParseStatus::bad_request},
        {"GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nx\r\n0\r\n\r\n",
         ParseStatus::bad_request},
        {"GET / HTTP/1.1\r\nHost: par", ParseStatus::need_more}};
    for (const auto& [input, status] : samples)
        chunk_variants(input, status);
    std::cout << "matrix: samples=" << samples.size() << " split_cases=" << splits
              << " byte_feeds=" << byte_feeds << " cr_splits=" << cr_splits << '\n';
}

void framing_matrix() {
    const std::vector<std::pair<std::string, bool>> cases = {
        {"", true},
        {"Content-Length: 0\r\n", true},
        {"cOnTeNt-LeNgTh: \t00 \t\r\n", true},
        {"Content-Length: 0\r\nContent-Length: 0\r\n", false},
        {"Content-Length: 0,0\r\n", false},
        {"Content-Length: 1\r\n", false},
        {"Content-Length: -0\r\n", false},
        {"Content-Length: +0\r\n", false},
        {"Content-Length: 0 0\r\n", false},
        {"Content-Length: \t\r\n", false},
        {"Content-Length: 184467440737095516160000000000\r\n", false},
        {"Transfer-Encoding: chunked\r\n", false},
        {"Transfer-Encoding: \r\n", false},
        {"Transfer-Encoding: unknown\r\nTransfer-Encoding: unknown\r\n", false},
        {"Transfer-Encoding: chunked\r\nContent-Length: 0\r\n", false},
        {"Content-Length: 0\r\nTransfer-Encoding: chunked\r\n", false},
        {"Expect: 100-continue\r\n", false},
        {"Expect: \r\n", false},
        {"Connection: keep-alive, ClOsE\r\nConnection: keep-alive\r\n", true},
        {"cOnNeCtIoN: close\r\nConnection: xclose\r\n", true},
        {"Connection: , , \t\r\n", true},
        {"Connection: xclose, unknown\r\n", true},
        {"Connection: bad token\r\n", false},
        {"Connection: @\r\n", false},
        {"Connection: content-length\r\nContent-Length: 1\r\n", false},
        {"Upgrade: websocket\r\nProxy-Connection: close\r\n", true}};
    for (const auto& [header, ok] : cases) {
        const auto input = "GET / HTTP/1.1\r\nHost: a\r\n" + header +
                           "\r\nGET /suffix HTTP/1.1\r\nHost: a\r\n\r\n";
        chunk_variants(input, ok ? ParseStatus::complete : ParseStatus::bad_request);
    }
    RequestParser p;
    auto r = p.feed("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 00\r\nConnection: ClOsE\r\n\r\n");
    expect(r.request.close_requested, "mixed-case close recognized");
    p.reset();
    r = p.feed("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\nConnection: xclose,,,,\r\n\r\n");
    expect(r.status == ParseStatus::complete && !r.request.close_requested,
           "reset clears CL and close; token match exact");
    p.reset();
    expect(p.feed("GET / HTTP/1.1\r\n\r\n").status == ParseStatus::bad_request,
           "reset clears Host");
    for (int i = 0; i < 1000; ++i) {
        p.reset();
        r = p.feed("GET / HTTP/1.1\r\nHost: a\r\n\r\n");
        expect(r.status == ParseStatus::complete, "per-request limit after repeated resets");
    }
    std::cout << "S2 framing matrix=" << cases.size()
              << " reset=1003 scan_bound_K=10 splits=" << splits << " byte_feeds=" << byte_feeds
              << "\n";
}

void boundaries() {
    for (std::size_t length : {4095U, 4096U, 4097U}) {
        std::string line = "GET /" + std::string(length - 14, 'a') + " HTTP/1.1";
        expect(line.size() == length, "actual request-line fixture length");
        RequestParser p;
        auto prefix = p.feed(line);
        expect(prefix.status ==
                   (length <= 4096 ? ParseStatus::need_more : ParseStatus::bad_request),
               "4096 prefix waits; 4097 content fails");
        if (length <= 4096) {
            auto cr = p.feed("\r");
            expect(cr.status == ParseStatus::need_more && p.pending_cr(), "CR at bound waits LF");
            auto rest = p.feed("\nHost: x\r\n\r\n");
            expect(rest.status == ParseStatus::complete, "legal boundary split completion");
        }
        chunk_variants(line + "\r\nHost: x\r\n\r\n",
                       length <= 4096 ? ParseStatus::complete : ParseStatus::bad_request, false);
    }
    for (std::size_t length : {16383U, 16384U, 16385U}) {
        std::string request = "GET / HTTP/1.1\r\nHost: a\r\nX: ";
        request.append(length - request.size() - 4, 'b');
        request += "\r\n\r\n";
        expect(request.size() == length, "actual total-request fixture length");
        chunk_variants(request, length <= 16384 ? ParseStatus::complete : ParseStatus::bad_request,
                       false);
        RequestParser p;
        auto before = p.feed(std::string_view(request).substr(0, length - 1));
        auto last = p.feed(std::string_view(request).substr(length - 1));
        expect(last.status == (length <= 16384 ? ParseStatus::complete : ParseStatus::bad_request),
               "terminal LF at total boundary");
        expect(before.accepted_bytes + last.accepted_bytes == std::min(length, max_request_bytes),
               "total boundary consumes no byte beyond cap");
    }
    std::string partial = "GET / HTTP/1.1\r\nHost: a\r\nX: ";
    partial.resize(16383, 'a');
    RequestParser p;
    expect(p.feed(partial).status == ParseStatus::need_more, "unterminated 16383 may wait");
    auto limit = p.feed("b");
    expect(limit.status == ParseStatus::bad_request && limit.request_bytes == 16384,
           "unterminated cap rejects exactly");
    std::cout << "bounds: line4095/4096/4097=pass total16383/16384/16385=pass max_scan_visits="
              << max_scans << " peak_buffered=" << max_buffer
              << " fixed_capacity=" << max_request_bytes << '\n';
}

void sticky_reset_and_ownership() {
    const std::string one = "GET /one HTTP/1.1\r\nHost: a\r\n\r\n",
                      two = "POST /two HTTP/1.1\r\nHost: b\r\n\r\n",
                      partial = "GET /three HTTP/1.1\r";
    std::string combined = one + two + partial;
    RequestParser p;
    auto first = p.feed(combined);
    auto saved = first.request;
    expect(first.accepted_bytes == one.size() && first.request_bytes == one.size(),
           "first boundary stops exactly");
    const auto suffix = combined.substr(first.accepted_bytes);
    expect(suffix == two + partial, "suffix preserved verbatim");
    auto sticky = p.feed(suffix);
    expect(sticky.accepted_bytes == 0 && sticky.request_bytes == one.size(),
           "complete terminal accepts zero");
    p.reset();
    expect(p.state() == ParserState::request_line && p.scan_steps() == 0 && p.buffered_bytes() == 0,
           "reset clears all state");
    auto second = p.feed(suffix);
    expect(second.status == ParseStatus::method_not_allowed &&
               second.accepted_bytes == two.size() && second.request.target == "/two",
           "reset parses second request independently");
    p.reset();
    auto third = p.feed(std::string_view(suffix).substr(second.accepted_bytes));
    expect(third.status == ParseStatus::need_more && third.accepted_bytes == partial.size(),
           "incomplete third preserved");
    auto error = p.feed("X");
    expect(error.status == ParseStatus::bad_request && error.accepted_bytes == 1,
           "pending CR mismatch identifies offending byte");
    expect(p.feed(one).accepted_bytes == 0, "error terminal sticky");
    p.reset();
    combined.assign(1000000, 'z');
    auto recovered = p.feed(one + std::string(1000000, '\0'));
    expect(recovered.status == ParseStatus::complete && recovered.accepted_bytes == one.size() &&
               saved.target == "/one" && first.request.method == "GET",
           "error reset and owned result survive source mutation");
    expect(p.peak_buffered_bytes() < one.size(), "huge invalid suffix never buffered");
    std::cout << "boundaries: first=" << one.size() << " second=" << two.size()
              << " remainder=" << partial.size()
              << " resets=3 sticky_complete=1 sticky_error=1 suffix_ignored=1000000\n";
}

// S3: expectations below come from the approved restricted protocol contract,
// never from a whole-feed run of the production parser.
struct NamedCase {
    std::string id, wire;
    ParseStatus status;
    std::size_t consumed;
    std::string method, target;
    bool close{};
};

std::size_t named_runs{}, seeded_runs{};

void named_schedule(const NamedCase& sample, const std::vector<std::size_t>& chunks) {
    RequestParser parser;
    FeedResult result;
    std::size_t offset{}, accepted{};
    const int before = failures;
    for (auto count : chunks) {
        result = parser.feed(std::string_view(sample.wire).substr(offset, count));
        expect(result.accepted_bytes <= count, "per-feed accepted <= submitted");
        accepted += result.accepted_bytes;
        offset += count;
        observe(parser, result);
        expect(result.request_bytes == accepted, "per-feed cumulative consumption");
        if (result.status == ParseStatus::need_more)
            expect(result.accepted_bytes == count, "need-more consumes entire submitted block");
    }
    expect(offset == sample.wire.size(), "schedule submitted all bytes");
    expect(result.status == sample.status && accepted == sample.consumed &&
               result.request_bytes == sample.consumed,
           "independent status and exact terminal/error offset");
    expect(result.request.method == sample.method && result.request.target == sample.target &&
               result.request.close_requested == sample.close,
           "independent owned request fields and close decision");
    auto terminal = parser.feed("GET /ignored HTTP/1.1\r\nHost: z\r\n\r\n");
    expect(terminal.accepted_bytes == 0 && same(result, terminal),
           "every named terminal is sticky");
    if (failures != before)
        std::cerr << "CASE " << sample.id << " chunks=" << chunks.size()
                  << " expected_offset=" << sample.consumed << " actual=" << accepted << '\n';
    ++named_runs;
}

void named_variants(const NamedCase& sample, bool short_case = true) {
    const int before = failures;
    const auto n = sample.wire.size();
    named_schedule(sample, {n});
    if (short_case) {
        for (std::size_t i = 0; i <= n; ++i)
            named_schedule(sample, {i, n - i});
    } else {
        std::vector<std::size_t> points = {0, 1, 4095, 4096, 4097, n - 1, n};
        for (std::size_t i = 0; i < n; ++i)
            if (sample.wire[i] == '\r' && (i < 32 || i + 8 >= sample.consumed)) {
                points.push_back(i);
                points.push_back(i + 1);
            }
        std::sort(points.begin(), points.end());
        points.erase(std::unique(points.begin(), points.end()), points.end());
        for (auto i : points)
            if (i <= n)
                named_schedule(sample, {i, n - i});
    }
    named_schedule(sample, std::vector<std::size_t>(n, 1));
    for (unsigned seed : {0x13579U, 0x24680U}) {
        std::vector<std::size_t> chunks;
        std::size_t left = n;
        unsigned state = seed;
        while (left) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            const auto count = std::min(left, std::size_t{1} + state % 31);
            chunks.push_back(count);
            left -= count;
        }
        named_schedule(sample, chunks);
        ++seeded_runs;
    }
    std::cout << "S3 parser case=" << sample.id
              << " expected_status=" << static_cast<int>(sample.status)
              << " consumed=" << sample.consumed << " failures=" << failures - before << '\n';
}

void named_protocol_matrix() {
    std::vector<NamedCase> cases;
    const std::string line = "GET /case HTTP/1.1\r\n", host = "Host: a\r\n",
                      tail = "GET /tail HTTP/1.1\r\nHost: z\r\n\r\n";
    auto good = [&](std::string id, std::string method, std::string headers, bool close = false) {
        auto wire = method + " /case HTTP/1.1\r\n" + host + headers + "\r\n";
        const auto size = wire.size();
        const auto status =
            method == "GET" ? ParseStatus::complete : ParseStatus::method_not_allowed;
        cases.push_back(
            {std::move(id), wire + tail, status, size, std::move(method), "/case", close});
    };
    auto bad = [&](std::string id, std::string failing_prefix, std::string remainder = "\r\n") {
        const auto size = failing_prefix.size();
        cases.push_back({std::move(id), failing_prefix + remainder + tail, ParseStatus::bad_request,
                         size, "", "", false});
    };
    good("G01-get", "GET", "");
    good("G01-token-method", "M!X", "");
    for (const auto& [id, request_line] : std::vector<std::pair<std::string, std::string>>{
             {"method", "G?T /case HTTP/1.1"},
             {"empty-method", " /case HTTP/1.1"},
             {"double-space", "GET  /case HTTP/1.1"},
             {"tab-separator", "GET\t/case HTTP/1.1"},
             {"extra-space", "GET /case HTTP/1.1 "},
             {"version", "GET /case HTTP/1.0"},
             {"absolute-target", "GET http://a/case HTTP/1.1"},
             {"fragment", "GET /case#f HTTP/1.1"}})
        bad("G01-" + id, request_line + "\r\n", host + "\r\n");
    bad("G02-missing-host", line + "\r\n");
    bad("G02-duplicate-host", line + host + "hOsT: b\r\n");
    for (const auto& [id, header] :
         std::vector<std::pair<std::string, std::string>>{{"empty-host", "Host: \t "},
                                                          {"field-token", "Bad@Name: x"},
                                                          {"precolon-space", "Host : a"},
                                                          {"fold", " X: a"},
                                                          {"control", "X: a\x01"},
                                                          {"del", "X: a\x7f"}})
        bad("G02-" + id, line + (id == "empty-host" ? "" : host) + header + "\r\n");
    bad("G02-nul", line + host + std::string("X: a\0", 5));
    bad("G02-bare-lf", line + host + "X: a\n");
    bad("G02-cr-not-lf", line + host + "X: a\rQ");
    good("G02-unknown-ows", "GET", "X-Unknown: \t a \t\r\n");
    for (const auto& [id, header] : std::vector<std::pair<std::string, std::string>>{
             {"zero", "Content-Length: 0\r\n"},
             {"zeros-ows", "cOnTeNt-LeNgTh: \t000 \t\r\n"},
             {"unknown-token", "Connection: xclose, mystery\r\n"},
             {"empty-token", "Connection: , ,\t,\r\n"},
             {"proxy-ignored", "Proxy-Connection: close\r\n"}})
        good("G03-04-" + id, "GET", header);
    good("G04-multi-close", "GET", "Connection: keep-alive\r\ncOnNeCtIoN: , ClOsE, xclose\r\n",
         true);
    good("G04-close-first", "GET", "Connection: close\r\nConnection: keep-alive\r\n", true);
    good("G04-nonget-close", "POST", "Connection: close\r\n", true);
    const std::vector<std::pair<std::string, std::string>> rejected = {
        {"duplicate-cl", "Content-Length: 0\r\nContent-Length: 0\r\n"},
        {"list", "Content-Length: 0,0\r\n"},
        {"plus", "Content-Length: +0\r\n"},
        {"minus", "Content-Length: -0\r\n"},
        {"inner-space", "Content-Length: 0 0\r\n"},
        {"nonzero", "Content-Length: 7\r\n"},
        {"empty-cl", "Content-Length: \t\r\n"},
        {"huge-nonzero", "Content-Length: 999999999999999999999999999999999\r\n"},
        {"te-cl", "tRaNsFeR-EnCoDiNg: chunked\r\n"},
        {"cl-te", "Content-Length: 0\r\nTransfer-Encoding: identity\r\n"},
        {"empty-te", "Transfer-Encoding: \r\n"},
        {"expect", "eXpEcT: 100-continue\r\n"},
        {"empty-expect", "Expect: \r\n"},
        {"connection-cl", "Connection: content-length\r\nContent-Length: 1\r\n"},
        {"connection-te", "Connection: transfer-encoding\r\nTransfer-Encoding: chunked\r\n"},
        {"bad-token", "Connection: bad token\r\n"},
        {"punctuation", "Connection: @\r\n"},
        {"header-after-close", "Bad Header: x\r\n"}};
    for (const auto& [id, header] : rejected)
        for (bool nonget : {false, true}) {
            const std::string prefix = std::string(nonget ? "POST" : "GET") +
                                       " /case HTTP/1.1\r\n" + host + "Connection: close\r\n";
            bad("G03-04-" + id + (nonget ? "-405-priority" : "-after-close"), prefix + header,
                id == "te-cl" ? "Content-Length: 0\r\n\r\n" : "\r\n");
        }
    for (const auto& sample : cases)
        named_variants(sample);
    std::cout << "S3 named_matrix=" << cases.size() << " schedules=" << named_runs
              << " seeded=" << seeded_runs << " seeds=0x13579,0x24680\n";
}

std::string cumulative_headers(std::size_t size) {
    std::string wire = "GET /case HTTP/1.1\r\nHost: a\r\n";
    while (size - wire.size() > 70)
        wire += "X: " + std::string(59, 'a') + "\r\n";
    wire += "Y: " + std::string(size - wire.size() - 7, 'b') + "\r\n\r\n";
    return wire;
}

void named_boundaries_and_reset() {
    for (std::size_t size : {4095U, 4096U, 4097U}) {
        const auto target = "/" + std::string(size - 14, 'a');
        const auto wire = "GET " + target + " HTTP/1.1\r\nHost: a\r\n\r\n";
        named_variants({"G05-independent-line-" + std::to_string(size), wire,
                        size <= 4096 ? ParseStatus::complete : ParseStatus::bad_request,
                        size <= 4096 ? wire.size() : 4097, size <= 4096 ? "GET" : "",
                        size <= 4096 ? target : "", false},
                       false);
    }
    for (std::size_t size : {16383U, 16384U, 16385U}) {
        std::string wire = "GET /case HTTP/1.1\r\nHost: a\r\nX: ";
        wire.append(size - wire.size() - 4, 'a');
        wire += "\r\n\r\n";
        named_variants({"G05-independent-single-header-" + std::to_string(size), wire,
                        size <= 16384 ? ParseStatus::complete : ParseStatus::bad_request,
                        std::min(size, max_request_bytes), size <= 16384 ? "GET" : "",
                        size <= 16384 ? "/case" : "", false},
                       false);
    }
    for (std::size_t size : {16383U, 16384U, 16385U}) {
        auto wire = cumulative_headers(size);
        expect(wire.size() == size, "many-header exact fixture size");
        named_variants({"G05-many-headers-" + std::to_string(size), wire,
                        size <= 16384 ? ParseStatus::complete : ParseStatus::bad_request,
                        std::min(size, max_request_bytes), size <= 16384 ? "GET" : "",
                        size <= 16384 ? "/case" : "", false},
                       false);
    }
    RequestParser parser;
    const auto large = cumulative_headers(16384);
    auto saved = parser.feed(large);
    parser.reset();
    auto second = parser.feed(large);
    expect(saved.status == ParseStatus::complete && second.status == ParseStatus::complete &&
               saved.request_bytes + second.request_bytes > max_request_bytes,
           "two max requests reset byte budget");
    parser.reset();
    (void)parser.feed(
        "GET /old HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\nConnection: close\r\nX: y\r");
    expect(parser.pending_cr(), "reset fixture pending header CR");
    parser.reset();
    auto fresh = parser.feed("GET /fresh HTTP/1.1\r\nHost: b\r\nContent-Length: 00\r\n\r\n");
    expect(fresh.status == ParseStatus::complete && !fresh.request.close_requested &&
               !parser.pending_cr() && saved.request.target == "/case",
           "reset clears pending CR/Host/CL/close and retains saved result");
    std::cout << "S3 limits: many_headers=3 max_requests_total=32768 reset_pending_CR=1 named_runs="
              << named_runs << " peak_buffer=" << max_buffer << " max_scan=" << max_scans << '\n';
}

}

int main() {
    support_and_splits();
    framing_matrix();
    boundaries();
    sticky_reset_and_ownership();
    named_protocol_matrix();
    named_boundaries_and_reset();
    std::cout << "states: RequestLine=" << line_hits << " Headers=" << headers_hits
              << " Complete=" << complete_hits << " Error=" << error_hits
              << " assertions_failed=" << failures << '\n';
    return failures ? 1 : 0;
}

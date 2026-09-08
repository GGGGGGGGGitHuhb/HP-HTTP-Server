#include "http/http_request.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace hp::http;
int failures{};
std::size_t splits{}, byte_feeds{}, cr_splits{}, headers_hits{}, complete_hits{}, error_hits{}, line_hits{};
std::size_t max_scans{}, max_buffer{};
void expect(bool ok,const char* why){if(!ok){++failures;std::cerr<<"FAIL: "<<why<<'\n';}}
void observe(const RequestParser& p, const FeedResult& r){
    switch(p.state()){
        case ParserState::request_line: ++line_hits;break;
        case ParserState::headers: ++headers_hits;break;
        case ParserState::complete: ++complete_hits;break;
        case ParserState::error: ++error_hits;break;
    }
    expect(p.scan_steps()<=4*r.request_bytes,"linear framing and validation byte visits");
    expect(p.peak_buffered_bytes()<=max_request_bytes,"bounded parser storage");
    max_scans=std::max(max_scans,p.scan_steps());max_buffer=std::max(max_buffer,p.peak_buffered_bytes());
}
bool same(const FeedResult& a,const FeedResult& b){
    return a.status==b.status&&a.request.method==b.request.method&&a.request.target==b.request.target&&a.request_bytes==b.request_bytes;
}
void chunk_variants(const std::string& input,ParseStatus expected,bool all_splits=true){
    RequestParser whole;const auto baseline=whole.feed(input);expect(baseline.status==expected,"independent support-matrix result");observe(whole,baseline);
    std::vector<std::size_t> points;
    if(all_splits)for(std::size_t i=0;i<=input.size();++i)points.push_back(i);
    else for(std::size_t i: {std::size_t{0},std::size_t{1},std::size_t{4095},std::size_t{4096},std::size_t{4097},input.size()-1,input.size()})if(i<=input.size())points.push_back(i);
    for(auto split:points){
        RequestParser p;auto first=p.feed(std::string_view(input).substr(0,split));observe(p,first);
        if(p.pending_cr())++cr_splits;
        auto second=p.feed(std::string_view(input).substr(split));observe(p,second);++splits;
        expect(same(baseline,second)&&first.accepted_bytes+second.accepted_bytes==baseline.request_bytes,"split result and error offset independent");
        if(first.status==ParseStatus::need_more)expect(first.accepted_bytes==split,"NeedMore accepts full block");
    }
    RequestParser p;FeedResult last;std::size_t total{};
    for(char c:input){last=p.feed({&c,1});total+=last.accepted_bytes;++byte_feeds;observe(p,last);}
    expect(same(last,baseline)&&total==baseline.request_bytes,"one-byte feed agrees and does not rescan prefix");
    const auto wrapper=parse_request(input);
    const bool success=expected==ParseStatus::complete||expected==ParseStatus::method_not_allowed;
    expect(wrapper.status==baseline.status&&wrapper.request.method==baseline.request.method&&wrapper.request.target==baseline.request.target&&wrapper.consumed_bytes==(success?baseline.request_bytes:0),"one-shot wrapper retains count convention");
}
void support_and_splits(){
    const std::vector<std::pair<std::string,ParseStatus>> samples={
        {"GET /asset?x=1 HTTP/1.1\r\nHost: a\r\nX: unknown\r\n\r\n",ParseStatus::complete},
        {"CUSTOM / HTTP/1.1\r\nhOsT: \t a \t\r\n\r\n",ParseStatus::method_not_allowed},
        {"POST / HTTP/1.1\r\nHost: a\r\nBad Header: x\r\n\r\n",ParseStatus::bad_request},
        {"GET / HTTP/1.1\r\nX: a\r\n\r\n",ParseStatus::bad_request},
        {"GET / HTTP/1.1\r\nHost: a\r\nHOST: b\r\n\r\n",ParseStatus::bad_request},
        {"GET / HTTP/1.1\r\nHost: \t \r\n\r\n",ParseStatus::bad_request},
        {"G?T / HTTP/1.1\r\nHost: a\r\n\r\n",ParseStatus::bad_request},
        {"GET http://a/ HTTP/1.1\r\nHost: a\r\n\r\n",ParseStatus::bad_request},
        {"GET /#f HTTP/1.1\r\nHost: a\r\n\r\n",ParseStatus::bad_request},
        {"GET / HTTP/1.0\r\nHost: a\r\n\r\n",ParseStatus::bad_request},
        {"GET / HTTP/1.1\r\n folded: x\r\nHost: a\r\n\r\n",ParseStatus::bad_request},
        {"GET / HTTP/1.1\nHost: a\n\n",ParseStatus::bad_request},
        {"GET / HTTP/1.1\rXHost: a\r\n\r\n",ParseStatus::bad_request},
        {std::string("GET / HTTP/1.1\r\nHost: a\0junk\r\n\r\n",32),ParseStatus::bad_request},
        {"GET / HTTP/1.1\r\nHost: a\r\nX: \x01\r\n\r\n",ParseStatus::bad_request},
        {"GET / HTTP/1.1\r\nHost: a\r\nX: \x7f\r\n\r\n",ParseStatus::bad_request},
        {"GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 9\r\nConnection: keep-alive\r\n\r\nbody-tail",ParseStatus::complete},
        {"GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nx\r\n0\r\n\r\n",ParseStatus::complete},
        {"GET / HTTP/1.1\r\nHost: par",ParseStatus::need_more}
    };
    for(const auto& [input,status]:samples)chunk_variants(input,status);
    std::cout<<"matrix: samples="<<samples.size()<<" split_cases="<<splits<<" byte_feeds="<<byte_feeds<<" cr_splits="<<cr_splits<<'\n';
}
void boundaries(){
    for(std::size_t length:{4095U,4096U,4097U}){
        std::string line="GET /"+std::string(length-14,'a')+" HTTP/1.1";
        expect(line.size()==length,"actual request-line fixture length");
        RequestParser p;auto prefix=p.feed(line);
        expect(prefix.status==(length<=4096?ParseStatus::need_more:ParseStatus::bad_request),"4096 prefix waits; 4097 content fails");
        if(length<=4096){auto cr=p.feed("\r");expect(cr.status==ParseStatus::need_more&&p.pending_cr(),"CR at bound waits LF");auto rest=p.feed("\nHost: x\r\n\r\n");expect(rest.status==ParseStatus::complete,"legal boundary split completion");}
        chunk_variants(line+"\r\nHost: x\r\n\r\n",length<=4096?ParseStatus::complete:ParseStatus::bad_request,false);
    }
    for(std::size_t length:{16383U,16384U,16385U}){
        std::string request="GET / HTTP/1.1\r\nHost: a\r\nX: ";request.append(length-request.size()-4,'b');request+="\r\n\r\n";
        expect(request.size()==length,"actual total-request fixture length");chunk_variants(request,length<=16384?ParseStatus::complete:ParseStatus::bad_request,false);
        RequestParser p;auto before=p.feed(std::string_view(request).substr(0,length-1));auto last=p.feed(std::string_view(request).substr(length-1));
        expect(last.status==(length<=16384?ParseStatus::complete:ParseStatus::bad_request),"terminal LF at total boundary");
        expect(before.accepted_bytes+last.accepted_bytes==std::min(length,max_request_bytes),"total boundary consumes no byte beyond cap");
    }
    std::string partial="GET / HTTP/1.1\r\nHost: a\r\nX: ";partial.resize(16383,'a');RequestParser p;
    expect(p.feed(partial).status==ParseStatus::need_more,"unterminated 16383 may wait");
    auto limit=p.feed("b");expect(limit.status==ParseStatus::bad_request&&limit.request_bytes==16384,"unterminated cap rejects exactly");
    std::cout<<"bounds: line4095/4096/4097=pass total16383/16384/16385=pass max_scan_visits="<<max_scans<<" peak_buffered="<<max_buffer<<" fixed_capacity="<<max_request_bytes<<'\n';
}
void sticky_reset_and_ownership(){
    const std::string one="GET /one HTTP/1.1\r\nHost: a\r\n\r\n",two="POST /two HTTP/1.1\r\nHost: b\r\n\r\n",partial="GET /three HTTP/1.1\r";
    std::string combined=one+two+partial;RequestParser p;auto first=p.feed(combined);auto saved=first.request;
    expect(first.accepted_bytes==one.size()&&first.request_bytes==one.size(),"first boundary stops exactly");
    const auto suffix=combined.substr(first.accepted_bytes);expect(suffix==two+partial,"suffix preserved verbatim");
    auto sticky=p.feed(suffix);expect(sticky.accepted_bytes==0&&sticky.request_bytes==one.size(),"complete terminal accepts zero");
    p.reset();expect(p.state()==ParserState::request_line&&p.scan_steps()==0&&p.buffered_bytes()==0,"reset clears all state");
    auto second=p.feed(suffix);expect(second.status==ParseStatus::method_not_allowed&&second.accepted_bytes==two.size()&&second.request.target=="/two","reset parses second request independently");
    p.reset();auto third=p.feed(std::string_view(suffix).substr(second.accepted_bytes));expect(third.status==ParseStatus::need_more&&third.accepted_bytes==partial.size(),"incomplete third preserved");
    auto error=p.feed("X");expect(error.status==ParseStatus::bad_request&&error.accepted_bytes==1,"pending CR mismatch identifies offending byte");
    expect(p.feed(one).accepted_bytes==0,"error terminal sticky");p.reset();combined.assign(1000000,'z');auto recovered=p.feed(one+std::string(1000000,'\0'));
    expect(recovered.status==ParseStatus::complete&&recovered.accepted_bytes==one.size()&&saved.target=="/one"&&first.request.method=="GET","error reset and owned result survive source mutation");
    expect(p.peak_buffered_bytes()<one.size(),"huge invalid suffix never buffered");
    std::cout<<"boundaries: first="<<one.size()<<" second="<<two.size()<<" remainder="<<partial.size()<<" resets=3 sticky_complete=1 sticky_error=1 suffix_ignored=1000000\n";
}
}
int main(){support_and_splits();boundaries();sticky_reset_and_ownership();std::cout<<"states: RequestLine="<<line_hits<<" Headers="<<headers_hits<<" Complete="<<complete_hits<<" Error="<<error_hits<<" assertions_failed="<<failures<<'\n';return failures?1:0;}

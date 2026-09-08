#include "http_connection_handler.h"
#include "http/http_response.h"
#include <arpa/inet.h>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace hp::net {
struct TcpConnectionTestAccess {
    static auto result(TcpConnection& c) { return c.last_result_; }
    static auto mask(TcpConnection& c) { return c.last_mask_; }
    static auto messages(TcpConnection& c) { return c.message_count_; }
    static auto buffered(TcpConnection& c) { return c.io_.input_view().size(); }
    static auto events(TcpConnection& c) { return c.event_count_; }
    static auto token(TcpConnection& c) { return c.channel_.token(); }
    static auto interest(TcpConnection& c) { return c.channel_.interest(); }
};
struct TcpServerTestAccess {
    static auto& loop(TcpServer& s) { return s.loop_; }
    static void add(TcpServer& s,Socket socket) { s.add_connection(std::move(socket)); }
    static auto& connection(TcpServer& s,int fd) { return *s.connections_.at(fd); }
    static auto size(TcpServer& s) { return s.connections_.size(); }
    static void notice(TcpServer& s,int fd,TcpConnection::Identity id) { s.connection_closed(fd,id); }
    static void drain(TcpServer& s) { s.drain_closed_connections(); }
};
struct EventLoopTestAccess {
    static void stale(EventLoop& loop,std::uint64_t token) { loop.dispatch(token,EPOLLIN); }
};
}
namespace {
using namespace hp;
using namespace hp::net;
using C=TcpConnectionTestAccess;
using S=TcpServerTestAccess;
int failures{};
void expect(bool ok,const char* why){if(!ok){++failures;std::cerr<<"FAIL: "<<why<<'\n';}}
std::span<const std::byte> bytes(std::string_view s){return {reinterpret_cast<const std::byte*>(s.data()),s.size()};}
std::size_t fds(){return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator("/proc/self/fd"),std::filesystem::directory_iterator{}));}
struct Pair {
    Socket owner,peer;
    Pair(){int fd[2];if(::socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0,fd))throw std::runtime_error("pair");owner.reset(fd[0]);peer.reset(fd[1]);}
    void send(std::string_view s){expect(::send(peer.fd(),s.data(),s.size(),MSG_NOSIGNAL)==static_cast<ssize_t>(s.size()),"send controlled input");}
};
std::vector<std::byte> collect(int fd){
    std::vector<std::byte> out;std::byte buffer[16384];ssize_t n;
    while((n=::recv(fd,buffer,sizeof(buffer),MSG_DONTWAIT))>0)out.insert(out.end(),buffer,buffer+n);
    return out;
}
std::vector<std::byte> response(std::string_view body){return http::make_response(http::Status::ok,bytes(body),"text/plain");}
const std::string request="GET /first HTTP/1.1\r\nHost: test\r\n\r\n";
void interleaved_and_eof(){
    EventLoop loop;Pair a,b;app::HttpCallbackStats sa,sb;
    auto provider=[](const http::HttpRequest& r){return response(r.target);};
    int closed{};
    TcpConnection ca(loop,std::move(a.owner),1,app::make_http_callback(provider,&sa),http::max_request_bytes,[&](int,auto){++closed;});
    TcpConnection cb(loop,std::move(b.owner),2,app::make_http_callback(provider,&sb),http::max_request_bytes,[&](int,auto){++closed;});
    ca.start();cb.start();a.send("GET /one HTTP/1.1\r\n");loop.poll_once(250);
    b.send("GET /two HTTP/1.1\r\nHost: test\r\n");loop.poll_once(250);
    expect(collect(a.peer.fd()).empty()&&collect(b.peer.fd()).empty(),"independent NeedMore no response");
    a.send("Host: test\r\n\r\n");loop.poll_once(250);b.send("\r\n");loop.poll_once(250);
    expect(collect(a.peer.fd())==response("/one") && collect(b.peer.fd())==response("/two"),"interleaved exact distinct responses");
    expect(sa.responses==1&&sb.responses==1&&sa.need_more==1&&sb.need_more==1&&closed==2,"per-connection done state");
    Pair eof;app::HttpCallbackStats se;TcpConnection ce(loop,std::move(eof.owner),3,app::make_http_callback(provider,&se),http::max_request_bytes,[](int,auto){});ce.start();
    eof.send("GET / HTTP/1.1\r\n");loop.poll_once(250);::shutdown(eof.peer.fd(),SHUT_WR);loop.poll_once(250);
    expect(collect(eof.peer.fd())==http::make_error_response(http::Status::bad_request)&&se.eof_notifications==1,"EOF without new bytes incomplete400");
    int eof_empty{};Pair empty;TcpConnection cc(loop,std::move(empty.owner),4,[&](TcpConnection& c,std::span<const std::byte> in,bool ended){if(ended&&in.empty())++eof_empty;c.close_after_flush();},0,[](int,auto){});cc.start();::shutdown(empty.peer.fd(),SHUT_WR);loop.poll_once(250);
    expect(eof_empty==1,"empty EOF notification once");
    std::cout<<"isolation: callbacks="<<sa.callbacks+sb.callbacks<<" need_more="<<sa.need_more+sb.need_more<<" unique_responses="<<closed<<" eof400="<<se.responses<<" empty_eof="<<eof_empty<<'\n';
}
void incremental_consumption(){
    EventLoop loop; Pair a,b; app::HttpCallbackStats sa,sb;
    auto provider=[](const http::HttpRequest& r){return response(r.target);};
    TcpConnection ca(loop,std::move(a.owner),90,app::make_http_callback(provider,&sa),http::max_request_bytes,[](int,auto){});
    TcpConnection cb(loop,std::move(b.owner),91,app::make_http_callback(provider,&sb),http::max_request_bytes,[](int,auto){});
    ca.start(); cb.start();
    const std::vector<std::string> pa={"GET /", "a HTTP/1.1\r\nHo", "st: x\r\n\r\n"};
    const std::vector<std::string> pb={"GET /b HTTP/1.1\r", "\nHost: y\r", "\n\r\n"};
    std::size_t sent_a=0,sent_b=0;
    for(std::size_t i=0;i<pa.size();++i){
        a.send(pa[i]);sent_a+=pa[i].size();loop.poll_once(250);
        expect(sa.parses==i+1&&sa.accepted_bytes==sent_a&&sa.submitted_bytes==sent_a&&sa.consumed_bytes==sent_a&&C::buffered(ca)==0,"each A segment fed once and released before next write");
        b.send(pb[i]);sent_b+=pb[i].size();loop.poll_once(250);
        expect(sb.parses==i+1&&sb.accepted_bytes==sent_b&&sb.submitted_bytes==sent_b&&sb.consumed_bytes==sent_b&&C::buffered(cb)==0,"each B segment fed once and released before next write");
        if(i<2)expect(sa.responses==0&&sb.responses==0,"incremental partial states independent");
    }
    expect(collect(a.peer.fd())==response("/a")&&collect(b.peer.fd())==response("/b")&&sa.need_more==2&&sb.need_more==2,"three fragments preserve independent request fields");
    Pair empty; app::HttpCallbackStats se;
    TcpConnection ce(loop,std::move(empty.owner),92,app::make_http_callback(provider,&se),http::max_request_bytes,[](int,auto){});ce.start();::shutdown(empty.peer.fd(),SHUT_WR);loop.poll_once(250);
    expect(collect(empty.peer.fd())==http::make_error_response(http::Status::bad_request)&&se.accepted_bytes==0&&se.eof_notifications==1,"empty HTTP EOF feed produces400 without consumed bytes");
    std::cout<<"incremental: feeds="<<sa.parses+sb.parses<<" need_more="<<sa.need_more+sb.need_more<<" submitted="<<sa.submitted_bytes+sb.submitted_bytes<<" accepted="<<sa.accepted_bytes+sb.accepted_bytes<<" consumed="<<sa.consumed_bytes+sb.consumed_bytes<<" network_buffer=0 independent_responses=2 empty_http_eof400=1\n";
}
void limits_and_500(){
    EventLoop loop;auto provider=[](const http::HttpRequest&){return response("ok");};
    std::string exact="GET / HTTP/1.1\r\nHost: t\r\nX: ";exact.append(http::max_request_bytes-exact.size()-4,'a');exact+="\r\n\r\n";
    for(bool over:{false,true}){
        Pair p;app::HttpCallbackStats st;TcpConnection c(loop,std::move(p.owner),5,app::make_http_callback(provider,&st),http::max_request_bytes,[](int,auto){});c.start();
        std::string input=over?std::string(http::max_request_bytes+1,'a'):exact;p.send(input);loop.poll_once(250);
        auto got=collect(p.peer.fd());expect(got==(over?http::make_error_response(http::Status::bad_request):response("ok")),"precise HTTP input bound response");
    }
    Pair p;app::HttpCallbackStats st;TcpConnection c(loop,std::move(p.owner),6,app::make_http_callback([](const http::HttpRequest&)->std::vector<std::byte>{throw std::runtime_error("provider");},&st),http::max_request_bytes,[](int,auto){});c.start();p.send(request);loop.poll_once(250);
    expect(collect(p.peer.fd())==http::make_error_response(http::Status::internal_server_error)&&st.responses==1,"adapter exception maps500");
    int exact_seen{},limit_closed{};Pair bounded;
    TcpConnection cap(loop,std::move(bounded.owner),7,[&](TcpConnection&,std::span<const std::byte> in,bool){if(in.size()==4)++exact_seen;},4,[&](int,auto){++limit_closed;});cap.start();bounded.send("12345");loop.poll_once(250);
    expect(exact_seen==1&&limit_closed==1&&C::result(cap).read_error==EMSGSIZE,"notify exact cap before controlled overflow close");
    std::cout<<"limits: exact_http=1 over_http=1 exact_cap_notification="<<exact_seen<<" cap_close="<<limit_closed<<" adapter500="<<st.responses<<'\n';
}
void drain_pipeline_and_borrow(){
    EventLoop loop;Pair p;int sndbuf=4096;::setsockopt(p.owner.fd(),SOL_SOCKET,SO_SNDBUF,&sndbuf,sizeof(sndbuf));
    app::HttpCallbackStats stats;std::vector<std::byte> body(512*1024);
    for(std::size_t i=0;i<body.size();++i)body[i]=static_cast<std::byte>(i*71U);
    const auto expected=http::make_response(http::Status::ok,body,"application/octet-stream");
    int closes{};
    TcpConnection c(loop,std::move(p.owner),8,app::make_http_callback([&](const http::HttpRequest&){return http::make_response(http::Status::ok,body,"application/octet-stream");},&stats),http::max_request_bytes,[&](int,auto){++closes;});c.start();
    p.send(request+request);loop.poll_once(250);
    const auto pending=c.pending_bytes();expect(pending>0&&C::result(c).write_would_block&&closes==0,"close_after_flush retains temporary response tail");
    const auto parses=stats.parses,sends=stats.responses,callbacks=stats.callbacks;
    p.send(request);::shutdown(p.peer.fd(),SHUT_WR);
    std::vector<std::byte> received;
    for(int i=0;i<1000&&received.size()<expected.size();++i){auto part=collect(p.peer.fd());received.insert(received.end(),part.begin(),part.end());loop.poll_once(0);}
    auto part=collect(p.peer.fd());received.insert(received.end(),part.begin(),part.end());
    expect(received==expected&&c.pending_bytes()==0&&closes==1&&!(C::interest(c)&EPOLLOUT),"owned temporary bytes drain exactly before close");
    const auto events=C::events(c);for(int i=0;i<10;++i)loop.poll_once(0);
    expect(stats.parses==parses&&stats.responses==sends&&stats.callbacks==callbacks&&C::events(c)==events,"same/later pipeline and EOF no second parse/send or busyloop");
    Pair invalid;int consumed_close{};
    TcpConnection bad(loop,std::move(invalid.owner),9,[](TcpConnection& c,std::span<const std::byte> input,bool){c.consume(input.size()+1);},0,[&](int,auto){++consumed_close;});bad.start();invalid.send("x");loop.poll_once(250);expect(consumed_close==1,"consume overflow contained");
    std::cout<<"drain: pending="<<pending<<" eagain=1 full_bytes="<<received.size()<<" final_pending="<<c.pending_bytes()<<" close="<<closes<<" second_parse="<<stats.parses-parses<<" second_send="<<stats.responses-sends<<" empty_polls=10 consume_error="<<consumed_close<<'\n';
}
void factory_message_lifetime(){
    const auto before=fds();int factory_errors{},message_errors{},callback_alive{},recovered{};
    {
        int factories{};TcpServer server(0,[&]()->TcpConnection::MessageCallback{
            if(++factories==1){++factory_errors;throw std::runtime_error("factory");}
            return [&](TcpConnection& c,std::span<const std::byte> input,bool){
                if(input[0]==std::byte{'x'}){++message_errors;throw std::runtime_error("message");}
                if(input[0]==std::byte{'c'}){c.request_close();c.request_close();if(::fcntl(c.fd(),F_GETFD)>=0)++callback_alive;return;}
                ++recovered;c.send(input);c.consume(input.size());
            };
        });
        Pair first;int ff=first.owner.fd();try{S::add(server,std::move(first.owner));}catch(const std::runtime_error&){}
        expect(S::size(server)==0&&::fcntl(ff,F_GETFD)==-1,"factory failure owns/closes socket");
        Pair second;int oldfd=second.owner.fd();S::add(server,std::move(second.owner));auto& old=S::connection(server,oldfd);auto oldtoken=C::token(old),oldid=old.identity();second.send("x");S::loop(server).poll_once(250);expect(S::size(server)==0,"message exception closes one");
        Pair fresh;if(fresh.owner.fd()!=oldfd){expect(::dup2(fresh.owner.fd(),oldfd)==oldfd,"reuse numeric fd");fresh.owner.reset(oldfd);}S::add(server,std::move(fresh.owner));
        EventLoopTestAccess::stale(S::loop(server),oldtoken);S::notice(server,oldfd,oldid);S::drain(server);
        expect(S::size(server)==1&&C::messages(S::connection(server,oldfd))==0,"old close/token no misdelete");fresh.send("ok");S::loop(server).poll_once(250);expect(collect(fresh.peer.fd())==std::vector<std::byte>(bytes("ok").begin(),bytes("ok").end()),"new callback recovers echo");
        fresh.send("c");S::loop(server).poll_once(250);expect(S::size(server)==0&&::fcntl(oldfd,F_GETFD)==-1,"callback returns before fd close");
        Pair output;TcpConnection c(S::loop(server),std::move(output.owner),77,{},0,[](int,auto){});c.start();output.peer.reset();c.send(bytes("out"));expect(c.state()==TcpConnection::State::closing,"send output error closes");
        Pair active;S::add(server,std::move(active.owner)); // destructor with live owner
        std::cout<<"identity: stale="<<S::loop(server).counters().stale<<" old_close_misdelete=0 new_callback="<<recovered<<'\n';
    }
    expect(fds()==before&&factory_errors==1&&message_errors==1&&callback_alive==1&&recovered==1,"failure lifecycle and fd restoration");
    std::cout<<"failures: factory="<<factory_errors<<" message="<<message_errors<<" callback_alive="<<callback_alive<<" recovery="<<recovered<<" output_error=1 fd="<<before<<"->"<<fds()<<'\n';
}
void reset_new_message(){
    EventLoop loop;Socket accepted;Acceptor acceptor(loop,0,[&](Socket s){accepted=std::move(s);});acceptor.start();
    Socket client(::socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0));sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(acceptor.bound_port());address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    expect(::connect(client.fd(),reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"reset TCP connect");loop.poll_once(250);acceptor.stop();
    std::vector<std::byte> received;int messages{},closed{};
    TcpConnection c(loop,std::move(accepted),78,[&](TcpConnection& conn,std::span<const std::byte> input,bool){++messages;received.insert(received.end(),input.begin(),input.end());conn.consume(input.size());},0,[&](int,auto){++closed;});c.start();
    std::vector<std::byte> queued(1053,std::byte{0x5a});expect(::send(client.fd(),queued.data(),queued.size(),MSG_NOSIGNAL)==1053,"reset queue bytes");
    std::vector<std::byte> peek(1053);ssize_t count=-1;for(int i=0;i<1000;++i){count=::recv(c.fd(),peek.data(),peek.size(),MSG_PEEK);if(count==1053)break;::usleep(1000);}expect(count==1053&&peek==queued,"reset full queued");
    Epoller waiting;waiting.add(c.fd(),0,1);linger reset{1,0};::setsockopt(client.fd(),SOL_SOCKET,SO_LINGER,&reset,sizeof(reset));client.reset();bool pending=false;
    for(int i=0;i<8&&!pending;++i) {
        for(const auto& e:waiting.wait(250)) if(e.events&EPOLLERR) pending=true;
    }
    expect(pending,"reset error ready");waiting.remove(c.fd());loop.poll_once(250);
    auto result=C::result(c);bool combined=(C::mask(c)&(EPOLLERR|EPOLLIN))==(EPOLLERR|EPOLLIN);
    expect(combined&&result.socket_error==ECONNRESET&&result.bytes_read==1053&&received==queued&&messages>0&&closed==1,"new callback receives all diagnosed reset bytes");
    std::cout<<"reset: combined="<<combined<<" so_error="<<result.socket_error<<" recv_bytes="<<result.bytes_read<<" message_bytes="<<received.size()<<" messages="<<messages<<" closes="<<closed<<'\n';
}
}
int main(){interleaved_and_eof();incremental_consumption();limits_and_500();drain_pipeline_and_borrow();factory_message_lifetime();reset_new_message();std::cout<<"HTTP callback assertions_failed="<<failures<<'\n';return failures?1:0;}

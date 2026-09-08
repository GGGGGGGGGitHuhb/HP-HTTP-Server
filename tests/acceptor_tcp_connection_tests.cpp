#include "net/tcp_server.h"
#include <arpa/inet.h>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <netinet/tcp.h>
#include <set>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace hp::net {
struct EventLoopTestAccess {
    static void stale(EventLoop& loop, std::uint64_t token) { loop.dispatch(token, EPOLLIN); }
    static void dispatch_after_kernel_detach(EventLoop& loop, int fd) {
        // Keep a real queued event, detach only kernel registration, then dispatch
        // through production Channel/TcpConnection so the next MOD fails ENOENT.
        const auto events = loop.epoller_.wait(250);
        if (events.size() != 1) throw std::runtime_error("expected one pending event");
        const auto event = events.front();
        loop.epoller_.remove(fd);
        loop.dispatch(event.data.u64, event.events);
    }
};
struct TcpConnectionTestAccess {
    static auto token(TcpConnection& c) { return c.channel_.token(); }
    static auto events(TcpConnection& c) { return c.event_count_; }
    static auto pending(TcpConnection& c) { return c.io_.pending_bytes(); }
    static auto interest(TcpConnection& c) { return c.channel_.interest(); }
    static auto result(TcpConnection& c) { return c.last_result_; }
    static auto mask(TcpConnection& c) { return c.last_mask_; }
    static void interest(TcpConnection& c, std::uint32_t mask) { c.channel_.set_interest(mask); }
};
struct AcceptorTestAccess {
    static int fd(Acceptor& a) { return a.listener_.fd(); }
    static void invalidate_listener(Acceptor& a) { a.listener_.reset(); }
};
struct TcpServerTestAccess {
    static EventLoop& loop(TcpServer& s) { return s.loop_; }
    static void add(TcpServer& s, Socket socket) { s.add_connection(std::move(socket)); }
    static TcpConnection& connection(TcpServer& s, int fd) { return *s.connections_.at(fd); }
    static auto size(TcpServer& s) { return s.connections_.size(); }
    static void close_notice(TcpServer& s, int fd, TcpConnection::Identity id) { s.connection_closed(fd,id); }
    static void drain(TcpServer& s) { s.drain_closed_connections(); }
};
}
namespace {
using namespace hp::net;
using C = TcpConnectionTestAccess;
using S = TcpServerTestAccess;
int failures{};
void expect(bool ok, const char* text) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << text << '\n'; }
}
std::size_t fd_count() {
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                                                std::filesystem::directory_iterator{}));
}
struct Pair {
    Socket observed, peer;
    Pair() {
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds))
            throw std::runtime_error("socketpair");
        observed.reset(fds[0]); peer.reset(fds[1]);
    }
    void send(std::span<const std::byte> bytes) {
        expect(::send(peer.fd(), bytes.data(), bytes.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(bytes.size()), "send controlled bytes");
    }
    void ready() { const std::byte b{0x61}; send({&b, 1}); }
};
Socket connect_to(std::uint16_t port) {
    Socket client(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (!client.valid() || ::connect(client.fd(), reinterpret_cast<sockaddr*>(&address), sizeof(address)))
        throw std::runtime_error("connect");
    return client;
}
void await_accept_queue(Acceptor& acceptor, unsigned expected) {
    // Linux reports the listener accept backlog in TCP_INFO.tcpi_unacked.
    // Read the kernel queue before the single dispatch, rather than assuming that
    // client connect() or a fixed delay means the server processed the final ACK.
    for (int attempt=0; attempt<1000; ++attempt) {
        tcp_info info{}; socklen_t length=sizeof(info);
        if (::getsockopt(AcceptorTestAccess::fd(acceptor),IPPROTO_TCP,TCP_INFO,&info,&length))
            throw std::runtime_error("listener TCP_INFO");
        if (info.tcpi_unacked==expected) return;
        ::usleep(1000);
    }
    throw std::runtime_error("listener accept queue did not reach expected count");
}
void acceptor_delivery() {
    const auto before = fd_count();
    int accepted{}, transferred{}, failures_seen{}, callbacks_after_stop{}, bind_failure{}, initial_drain{};
    {
        EventLoop loop;
        std::vector<Socket> owners, clients;
        std::set<int> seen;
        bool stopped = false, fail_delivery = false;
        Acceptor acceptor(loop, 0, [&](Socket socket) {
            ++accepted;
            if (stopped) ++callbacks_after_stop;
            expect((::fcntl(socket.fd(), F_GETFL) & O_NONBLOCK) != 0, "accepted nonblocking");
            expect((::fcntl(socket.fd(), F_GETFD) & FD_CLOEXEC) != 0, "accepted CLOEXEC");
            if (fail_delivery) { ++failures_seen; fail_delivery = false; throw std::runtime_error("delivery failure"); }
            expect(seen.insert(socket.fd()).second, "no duplicate delivery");
            owners.push_back(std::move(socket)); ++transferred;
        });
        acceptor.start(); acceptor.start();
        for (int i=0;i<8;++i) clients.push_back(connect_to(acceptor.bound_port()));
        await_accept_queue(acceptor,8);
        const auto dispatch_before = loop.counters().dispatches;
        loop.poll_once(250);
        initial_drain = accepted;
        await_accept_queue(acceptor,0);
        expect(accepted == 8 && transferred == 8 && loop.counters().dispatches == dispatch_before + 1,
               "one listener callback drains eight queued clients");
        try { Acceptor conflict(loop, acceptor.bound_port(), [](Socket) {}); }
        catch (const std::system_error&) { ++bind_failure; }
        fail_delivery = true;
        clients.push_back(connect_to(acceptor.bound_port()));
        clients.push_back(connect_to(acceptor.bound_port()));
        await_accept_queue(acceptor,2);
        const auto fd_before_failure = fd_count();
        loop.poll_once(250);
        expect(failures_seen == 1 && transferred == 9 && fd_count() == fd_before_failure + 1,
               "failed delivery closes socket and next delivery succeeds");
        acceptor.stop(); acceptor.stop(); stopped = true;
        clients.push_back(connect_to(acceptor.bound_port()));
        loop.poll_once(0);
        expect(accepted == 10 && callbacks_after_stop == 0 && bind_failure == 1, "stop and bind failure");
    }
    expect(fd_count() == before, "acceptor resources restored");
    std::cout << "acceptor: initial_drain=" << initial_drain << " accepted=" << accepted << " transferred=" << transferred
              << " delivery_failure=" << failures_seen << " bind_failure=" << bind_failure
              << " post_stop=" << callbacks_after_stop << " duplicate=0 fd=" << before << "->" << fd_count() << '\n';
}
void buffers_and_echo() {
    EventLoop loop;
    Pair pair;
    int size=4096;
    expect(::setsockopt(pair.observed.fd(), SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0, "small send buffer");
    int closes{};
    TcpConnection c(loop,std::move(pair.observed),1,{},0,[&](int,auto){++closes;});
    c.start(); c.start();
    std::vector<std::byte> payload(65536);
    for (std::size_t i=0;i<payload.size();++i) payload[i]=static_cast<std::byte>(i*131U+17U);
    pair.send(payload); loop.poll_once(250);
    const auto pending = C::pending(c);
    expect(C::result(c).write_would_block && pending > 0 && pending < payload.size() && (C::interest(c)&EPOLLOUT),
           "real partial send and EAGAIN preserve tail");
    std::vector<std::byte> received;
    std::byte buffer[8192];
    int iterations{};
    while (received.size()<payload.size() && iterations++<200) {
        ssize_t n;
        while ((n=::recv(pair.peer.fd(),buffer,sizeof(buffer),0))>0)
            received.insert(received.end(),buffer,buffer+n);
        loop.poll_once(0);
    }
    expect(received == payload && C::pending(c)==0 && !(C::interest(c)&EPOLLOUT), "exact echoed bytes and disable write");
    const auto events = C::events(c);
    for(int i=0;i<10;++i)loop.poll_once(0);
    expect(C::events(c)==events && closes==0,"no writable busy-loop after drain");
    c.request_close(); c.request_close();
    expect(closes==1,"one close notification");
    std::cout << "buffers: initial_pending=" << pending << " eagain=1 recovered_bytes=" << received.size()
              << " final_pending=" << C::pending(c) << " callbacks=" << events << " empty_polls=10 closes=" << closes << '\n';
}
void application_boundaries() {
    EventLoop loop;
    int calls{}, notices{};
    Pair pair;
    TcpConnection c(loop,std::move(pair.observed),2,[&](std::span<const std::byte> input,bool){
        ++calls;
        if(input.size()<2) return ApplicationResult::need_more();
        expect(input.size()==2,"accumulated borrowed input span");
        return ApplicationResult::respond({std::byte{0x78},std::byte{0x79}});
    },2,[&](int,auto){++notices;});
    c.start();pair.ready();loop.poll_once(250);
    expect(calls==1 && c.state()==TcpConnection::State::active,"partial input no response");
    pair.ready();loop.poll_once(250);
    std::byte bytes[8];const auto n=::recv(pair.peer.fd(),bytes,sizeof(bytes),0);
    expect(n==2 && bytes[0]==std::byte{0x78} && bytes[1]==std::byte{0x79} && calls==2 && notices==1,
           "one copied response after local application result lifetime");
    pair.ready();loop.poll_once(0);expect(calls==2,"removed connection does not call application again");
    Pair limit;
    int limit_calls{},limit_closed{};
    TcpConnection capped(loop,std::move(limit.observed),3,[&](std::span<const std::byte> input,bool){
        ++limit_calls;expect(input.size()<=2,"input limit respected");return ApplicationResult::need_more();
    },2,[&](int,auto){++limit_closed;});
    capped.start();std::byte over[3]{};limit.send(over);loop.poll_once(250);
    expect(limit_closed==1 && C::result(capped).read_error==EMSGSIZE,"oversized input closes within original cap");
    std::cout << "application: segmented_calls=" << calls << " one_response=1 limit_calls=" << limit_calls << " limit_close=" << limit_closed << '\n';
}
void close_batch_and_reuse() {
    const auto before=fd_count();
    int in_callback_fd_alive{},survivor_calls{},self_calls{};
    std::size_t stale{};
    {
        TcpConnection* a{};TcpConnection* b{};
        TcpServer server(0,[&](std::span<const std::byte> bytes,bool){
            if(bytes[0]==std::byte{0x73}){++survivor_calls;return ApplicationResult::need_more();}
            ++self_calls;
            a->request_close();a->request_close();b->request_close();
            if(::fcntl(a->fd(),F_GETFD)>=0 && ::fcntl(b->fd(),F_GETFD)>=0)++in_callback_fd_alive;
            return ApplicationResult::need_more();
        });
        Pair x,y,z;int fx=x.observed.fd(),fy=y.observed.fd(),fz=z.observed.fd();
        S::add(server,std::move(x.observed));S::add(server,std::move(y.observed));S::add(server,std::move(z.observed));
        a=&S::connection(server,fx);b=&S::connection(server,fy);
        x.ready();y.ready();std::byte marker{0x73};z.send({&marker,1});
        S::loop(server).poll_once(250);stale=S::loop(server).counters().stale;
        expect(self_calls==1 && survivor_calls==1 && stale==1 && S::size(server)==1,"same batch two closes, survivor and stale");
        expect(::fcntl(fx,F_GETFD)==-1 && ::fcntl(fy,F_GETFD)==-1 && in_callback_fd_alive==1,"close only after callback returns");
        const auto survivor_events=C::events(S::connection(server,fz));
        S::loop(server).poll_once(0);expect(C::events(S::connection(server,fz))==survivor_events,"post remove no extra event");
        // Active survivor intentionally remains for server destructor cleanup.
    }
    expect(fd_count()==before,"server destructor restores active connection resources");
    {
        TcpServer server(0);
        Pair old;const int fd=old.observed.fd();S::add(server,std::move(old.observed));
        auto& original=S::connection(server,fd);
        const auto old_token=C::token(original), old_id=original.identity();
        original.request_close();S::drain(server);
        Pair fresh;
        if(fresh.observed.fd()!=fd){expect(::dup2(fresh.observed.fd(),fd)==fd,"force same numeric fd");fresh.observed.reset(fd);}
        S::add(server,std::move(fresh.observed));
        auto& current=S::connection(server,fd);
        expect(current.identity()!=old_id && C::token(current)!=old_token,"new identity and token for reused fd");
        EventLoopTestAccess::stale(S::loop(server),old_token);
        S::close_notice(server,fd,old_id);S::drain(server);
        expect(S::size(server)==1 && ::fcntl(fd,F_GETFD)>=0 && C::events(current)==0,"old close/token cannot destroy or reach new owner");
        fresh.ready();S::loop(server).poll_once(250);
        expect(C::events(current)==1,"new real readiness reaches TcpConnection");
        std::cout << "reuse: fd=" << fd << " old_id=" << old_id << " new_id=" << current.identity()
                  << " stale=" << S::loop(server).counters().stale << " old_close_misdelete=0 old_event_misdelivery=0 new_callback=" << C::events(current) << '\n';
    }
    expect(fd_count()==before,"reuse resources restored");
    std::cout<<"lifecycle: callback_alive="<<in_callback_fd_alive<<" self="<<self_calls<<" survivor="<<survivor_calls<<" same_batch_stale="<<stale<<" fd="<<before<<"->"<<fd_count()<<'\n';
}
void failures_and_recovery() {
    const auto before=fd_count();
    int constructor_fail{},listener_add_fail{},add_fail{},mod_fail{},recovery{};
    {
        EventLoop loop;
        Pair pair;const int fd=pair.observed.fd();
        try { TcpConnection bad(loop,std::move(pair.observed),0,{},0,[](int,auto){}); }
        catch(const std::invalid_argument&){++constructor_fail;}
        expect(::fcntl(fd,F_GETFD)==-1,"failed constructor reclaims socket");
        try { Acceptor invalid(loop,0,{}); }
        catch(const std::invalid_argument&){++constructor_fail;}
        Acceptor broken_listener(loop,0,[](Socket){});
        AcceptorTestAccess::invalidate_listener(broken_listener);
        try { broken_listener.start(); }catch(const std::system_error&){++listener_add_fail;}
        expect(listener_add_fail==1,"listener registration failure propagates without fake registration");
        // /dev/null is a valid owned fd but cannot be epoll-registered (EPERM).
        Socket regular(::open("/dev/null",O_RDONLY|O_CLOEXEC));int regular_fd=regular.fd();
        TcpConnection unsupported(loop,std::move(regular),4,{},0,[](int,auto){});
        try { unsupported.start(); }catch(const std::system_error&){++add_fail;}
        expect(unsupported.state()==TcpConnection::State::unregistered && C::token(unsupported)==0 && ::fcntl(regular_fd,F_GETFD)>=0,
               "failed ADD does not fake active registration or close owner early");
    }
    {
        TcpServer server(0);
        Socket regular(::open("/dev/null",O_RDONLY|O_CLOEXEC));int regular_fd=regular.fd();
        try { S::add(server,std::move(regular)); }catch(const std::system_error&){++add_fail;}
        expect(S::size(server)==0 && ::fcntl(regular_fd,F_GETFD)==-1,"server rolls back failed ADD object and fd");
        Pair broken;const int fd=broken.observed.fd();int size=4096;
        expect(::setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&size,sizeof(size))==0,"constrain failed MOD send buffer");
        S::add(server,std::move(broken.observed));
        std::vector<std::byte> bytes(65536,std::byte{0x37});broken.send(bytes);
        EventLoopTestAccess::dispatch_after_kernel_detach(S::loop(server),fd);
        if(S::size(server)==0 && ::fcntl(fd,F_GETFD)==-1)++mod_fail;
        expect(mod_fail==1,"real MOD failure closes only failed connection after dispatch");
        Socket client=connect_to(server.bound_port());S::loop(server).poll_once(250);
        expect(::send(client.fd(),"ok",2,MSG_NOSIGNAL)==2,"send after failures");
        S::loop(server).poll_once(250);char output[2];
        pollfd readable{client.fd(),POLLIN,0};
        expect(::poll(&readable,1,1000)==1,"await actual TCP echo delivery");
        if(::recv(client.fd(),output,2,MSG_DONTWAIT)==2 && output[0]=='o' && output[1]=='k')++recovery;
        expect(recovery==1,"real accepted connection succeeds after ADD/MOD failure");
    }
    expect(fd_count()==before,"failure resources return to baseline");
    std::cout<<"failures: constructors="<<constructor_fail<<" listener_add="<<listener_add_fail<<" add="<<add_fail<<" mod="<<mod_fail<<" recovery="<<recovery<<" fd="<<before<<"->"<<fd_count()<<'\n';
}
void real_reset() {
    EventLoop loop;
    Socket accepted;
    Acceptor acceptor(loop,0,[&](Socket s){accepted=std::move(s);});acceptor.start();
    Socket client=connect_to(acceptor.bound_port());loop.poll_once(250);acceptor.stop();
    expect(accepted.valid(),"accept real reset TCP client");
    int notices{};
    TcpConnection c(loop,std::move(accepted),7,{},0,[&](int,auto){++notices;});c.start();
    std::vector<std::byte> bytes(1053,std::byte{0x7a});
    expect(::send(client.fd(),bytes.data(),bytes.size(),MSG_NOSIGNAL)==1053,"queue real reset bytes");
    std::vector<std::byte> peeked(1053);
    ssize_t n=-1;
    for(int i=0;i<8;++i){n=::recv(c.fd(),peeked.data(),peeked.size(),MSG_PEEK);if(n==1053)break;::usleep(1000);}
    expect(n==1053 && peeked==bytes,"queued bytes before TCP reset");
    // Poll for ERR on an independent observer without consuming SO_ERROR/data.
    // The target TcpConnection is still untouched until the real combined event.
    Epoller reset_ready;reset_ready.add(c.fd(),0,1);
    linger reset{1,0};expect(::setsockopt(client.fd(),SOL_SOCKET,SO_LINGER,&reset,sizeof(reset))==0,"reset linger");client.reset();
    bool error_ready=false;
    for(int i=0;i<8 && !error_ready;++i)for(const auto& e:reset_ready.wait(250))if(e.events&EPOLLERR)error_ready=true;
    expect(error_ready,"kernel TCP reset pending");reset_ready.remove(c.fd());
    loop.poll_once(250);
    const auto result=C::result(c);
    const bool combined=(C::mask(c)&(EPOLLERR|EPOLLIN))==(EPOLLERR|EPOLLIN);
    expect(combined && result.socket_error_observed && result.socket_error==ECONNRESET && result.bytes_read==bytes.size() && notices==1,
           "TcpConnection full ERR/IN -> SO_ERROR -> exact recv -> deferred close");
    std::cout<<"reset: tcp_connection_callbacks="<<C::events(c)<<" combined="<<combined<<" so_error="<<result.socket_error<<" recv_bytes="<<result.bytes_read<<" close_notice="<<notices<<'\n';
}
}
int main(){
    acceptor_delivery();buffers_and_echo();application_boundaries();close_batch_and_reuse();failures_and_recovery();real_reset();
    std::cout<<"Acceptor/TcpConnection assertions_failed="<<failures<<'\n';
    return failures?1:0;
}

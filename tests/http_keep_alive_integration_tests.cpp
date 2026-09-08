// Reuse the existing production-process/fixture harness; its regression entry
// remains a separate CTest identity. This target executes only the tests below.
#define main legacy_http_integration_entry
#include "http_server_integration_tests.cpp"
#undef main

namespace {
class Client {
public:
    explicit Client(std::uint16_t port):fd(connect_client(port)){}
    ~Client(){::close(fd);}
    Client(const Client&)=delete;
    Client& operator=(const Client&)=delete;
    int fd;
    std::string pending;
    struct Message { int status; std::string header,body; };
    Message next(){
        const auto read_more=[&]{char buffer[32768];auto n=::recv(fd,buffer,sizeof(buffer),0);if(n<=0)throw std::runtime_error("response ended or timed out before Content-Length boundary");pending.append(buffer,static_cast<std::size_t>(n));};
        while(pending.find("\r\n\r\n")==std::string::npos)read_more();
        const auto boundary=pending.find("\r\n\r\n")+4;
        const auto header=pending.substr(0,boundary);
        const std::string key="Content-Length: ";const auto pos=header.find(key);
        expect(pos!=std::string::npos&&header.find(key,pos+1)==std::string::npos,"unique Content-Length");
        const auto length=std::stoull(header.substr(pos+key.size()));
        while(pending.size()<boundary+length)read_more();
        auto body=pending.substr(boundary,length);pending.erase(0,boundary+length);
        const auto connection=header.find("Connection: ");expect(connection!=std::string::npos&&header.find("Connection: ",connection+1)==std::string::npos,"unique Connection");
        return {std::stoi(header.substr(9,3)),header,std::move(body)};
    }
    void eof(){expect(pending.empty(),"no extra pipeline response");char b;expect(::recv(fd,&b,1,0)==0,"terminal response followed by real EOF");}
};
std::string get(std::string_view target,std::string_view headers=""){return "GET "+std::string(target)+" HTTP/1.1\r\nHost: localhost\r\n"+std::string(headers)+"\r\n";}
void check(const Client::Message& m,int status,std::string_view body,bool close){expect(m.status==status&&m.body==body,"ordered exact status and body");expect(m.header.find(close?"Connection: close\r\n":"Connection: keep-alive\r\n")!=std::string::npos,"wire connection policy");}
void reuse(std::uint16_t port){
    const std::vector<std::string> targets={"/","/note.txt","/assets/unknown.blob"};
    const std::vector<std::string> bodies={"<h1>integration index</h1>\n","hello from S3\n","unknown mime\n"};
    {Client c(port);for(std::size_t i=0;i<3;++i){send_all(c.fd,get(targets[i],i==2?"Connection: close\r\n":""));check(c.next(),200,bodies[i],i==2);}c.eof();std::cout<<"sequential: client_fd="<<c.fd<<" connection=1 responses=3 reconnect=0\n";}
    {Client c(port);send_all(c.fd,get(targets[0])+get(targets[1])+get(targets[2],"Connection: close\r\n"));for(std::size_t i=0;i<3;++i)check(c.next(),200,bodies[i],i==2);c.eof();std::cout<<"pipeline: connection=1 single_write=1 responses=3 order=index,note,unknown\n";}
    {Client c(port);send_all(c.fd,get(targets[0])+get(targets[1])+"GET /assets/unknown.blob HTTP/1.1\r\nHo");check(c.next(),200,bodies[0],false);check(c.next(),200,bodies[1],false);expect(c.pending.empty(),"incomplete third has no response");send_all(c.fd,"st: x\r\nConnection: close\r\n\r\n");check(c.next(),200,bodies[2],true);c.eof();}
    {Client c(port);send_all(c.fd,get("/")+get("/note.txt")+get("/assets/unknown.blob"));::shutdown(c.fd,SHUT_WR);for(std::size_t i=0;i<3;++i)check(c.next(),200,bodies[i],false);c.eof();}
    {Client c(port);send_all(c.fd,get("/"));check(c.next(),200,bodies[0],false);::shutdown(c.fd,SHUT_WR);c.eof();}
    {Client c(port);send_all(c.fd,get("/")+"GET /next HTTP/1.1\r\n");::shutdown(c.fd,SHUT_WR);check(c.next(),200,bodies[0],false);check(c.next(),400,"400 Bad Request\n",true);c.eof();}
    for(bool prefix:{false,true}){
        Client c(port);send_all(c.fd,(prefix?get("/"):"")+get("/bad%20target")+get("/note.txt"));if(prefix)check(c.next(),200,bodies[0],false);check(c.next(),400,"400 Bad Request\n",true);c.eof();
    }
    for(const auto& target:{"/../sibling-secret.txt","/missing"}){
        Client c(port);send_all(c.fd,get(target)+get("/note.txt","Connection: close\r\n"));const auto m=c.next();expect(m.status==(std::string_view(target)=="/missing"?404:403)&&m.header.find("keep-alive")!=std::string::npos,"normal 403/404 retain reuse");check(c.next(),200,bodies[1],true);c.eof();
    }
    for(const auto& header:{"Connection: ClOsE, keep-alive\r\nConnection: keep-alive\r\n","Content-Length: 9\r\n","Transfer-Encoding: chunked\r\n","Expect: 100-continue\r\n"}){
        Client c(port);send_all(c.fd,get("/note.txt",header)+get("/"));const bool normal=std::string_view(header).starts_with("Connection");check(c.next(),normal?200:400,normal?bodies[1]:"400 Bad Request\n",true);c.eof();
    }
    std::cout<<"production: partial_third=1 FIN_pipeline=3 idle_EOF=1 partial_EOF400=1 service400_suffix=2 reusable403_404=2 terminal_matrix=4\n";
}
}
int main(int argc,char** argv){
    if(argc!=2)return 2;
    try{Fixture fixture;auto server=start_server(argv[1],fixture.root);const auto before=server.fd_count();reuse(server.port());
        // Reset a paused large response, then prove other connections remain usable.
        {Client c(server.port());send_all(c.fd,get("/large.bin")+get("/note.txt"));server.drain_output(std::chrono::milliseconds(80));linger reset{1,0};::setsockopt(c.fd,SOL_SOCKET,SO_LINGER,&reset,sizeof(reset));}
        for(int i=0;i<20;++i){Client c(server.port());send_all(c.fd,get("/note.txt","Connection: close\r\n"));check(c.next(),200,"hello from S3\n",true);c.eof();}
        server.drain_output(std::chrono::milliseconds(100));const auto after=server.fd_count();expect(before==after,"production fd baseline restored after reset and 20 survivors");expect(server.output().find("write reached EAGAIN")!=std::string::npos,"reset targeted genuinely blocked large output");
        std::cout<<"reset: blocked_EAGAIN=1 survivors=20 fd_before="<<before<<" fd_after="<<after<<" failures="<<failures<<'\n';
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}return failures?1:0;
}

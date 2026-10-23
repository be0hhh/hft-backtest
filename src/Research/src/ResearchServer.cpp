#include "ResearchStorage.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace hft_backtest::research {
namespace {
using Clock=std::chrono::steady_clock;
std::vector<std::uint8_t> frame(std::vector<std::uint8_t> payload){std::vector<std::uint8_t> out{'B','T','R','S'};for(unsigned i=0;i<4;++i)out.push_back(static_cast<std::uint8_t>(payload.size()>>(8*i)));out.insert(out.end(),payload.begin(),payload.end());return out;}
std::uint32_t frameSize(const std::uint8_t* p){if(p[0]!='B'||p[1]!='T'||p[2]!='R'||p[3]!='S')throw std::runtime_error("research frame magic mismatch");std::uint32_t n=0;for(unsigned i=0;i<4;++i)n|=std::uint32_t(p[4+i])<<(i*8);return n;}
bool address(const std::filesystem::path& path,sockaddr_un& out,std::string& error){auto s=path.string();if(s.empty()||s.find('\0')!=std::string::npos||s.size()>=sizeof(out.sun_path)){error="research Unix socket path exceeds bound";return false;}out={};out.sun_family=AF_UNIX;std::copy(s.begin(),s.end(),out.sun_path);out.sun_path[s.size()]=0;return true;}
bool sameOwner(int fd){ucred peer{};socklen_t length=sizeof(peer);return ::getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&length)==0&&peer.uid==::geteuid();}

}
struct UnixResearchServer::Impl {
 struct Client {int fd{-1};std::vector<std::uint8_t> input,output;std::size_t sent{};Clock::time_point deadline;};
 ServiceConfig config;ResearchService& service;int fd{-1};int lease{-1};std::vector<Client> clients;bool ownsSocket{};dev_t device{};ino_t inode{};
 Impl(ServiceConfig c,ResearchService& s):config(std::move(c)),service(s){}
 ~Impl(){if(lease>=0)::close(lease);for(auto& c:clients)::close(c.fd);if(fd>=0)::close(fd);if(ownsSocket){struct stat st{};if(::lstat(config.socketPath.c_str(),&st)==0&&st.st_dev==device&&st.st_ino==inode)::unlink(config.socketPath.c_str());}}
};
UnixResearchServer::UnixResearchServer(ServiceConfig c,ResearchService&s):impl_(std::make_unique<Impl>(std::move(c),s)){}
UnixResearchServer::~UnixResearchServer()=default;
bool UnixResearchServer::open(std::string& error){auto& s=*impl_;if(s.fd>=0){error="research server already open";return false;}sockaddr_un local{};if(!address(s.config.socketPath,local,error))return false;
 const auto parent=s.config.socketPath.parent_path();struct stat st{};if(::lstat(parent.c_str(),&st)!=0||!S_ISDIR(st.st_mode)||st.st_uid!=::geteuid()||(st.st_mode&0077)!=0){error="research socket parent must be an owner-only real directory";return false;}
 const auto leasePath=s.config.socketPath.string()+".lease";s.lease=::open(leasePath.c_str(),O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);if(s.lease<0||::fstat(s.lease,&st)!=0||!S_ISREG(st.st_mode)||st.st_uid!=::geteuid()||st.st_nlink!=1||(st.st_mode&0077)!=0||::flock(s.lease,LOCK_EX|LOCK_NB)!=0){error="research socket owner lease unavailable";return false;}
 if(::lstat(s.config.socketPath.c_str(),&st)==0){
  if(!S_ISSOCK(st.st_mode)||st.st_uid!=::geteuid()){error="existing research endpoint ownership/type mismatch";return false;}
  const auto oldDevice=st.st_dev;const auto oldInode=st.st_ino;int probe=::socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(probe<0){error="cannot inspect existing research endpoint";return false;}const auto connected=::connect(probe,reinterpret_cast<sockaddr*>(&local),sizeof(local));const auto outcome=errno;::close(probe);if(connected==0||outcome!=ECONNREFUSED){error="existing research endpoint remains active or uncertain";return false;}
  if(::lstat(s.config.socketPath.c_str(),&st)!=0||st.st_dev!=oldDevice||st.st_ino!=oldInode||::unlink(s.config.socketPath.c_str())!=0){error="stale research endpoint changed during recovery";return false;}
 }else if(errno!=ENOENT){error="cannot inspect research endpoint";return false;}
 s.fd=::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(s.fd<0){error="research socket failed";return false;}
 if(::bind(s.fd,reinterpret_cast<sockaddr*>(&local),sizeof(local))!=0){error=std::strerror(errno);return false;}
 s.ownsSocket=true;if(::chmod(s.config.socketPath.c_str(),0600)!=0||::lstat(s.config.socketPath.c_str(),&st)!=0){error="research socket ownership failed";return false;}s.device=st.st_dev;s.inode=st.st_ino;
 if(::listen(s.fd,16)!=0){error="research listen failed";return false;}return true;
}
bool UnixResearchServer::poll(int timeoutMs,std::string& error){auto& s=*impl_;if(s.fd<0){error="research server not open";return false;}
 std::vector<pollfd> fds{{s.fd,POLLIN,0}};for(auto& c:s.clients)fds.push_back({c.fd,static_cast<short>(c.output.empty()?POLLIN:POLLOUT),0});
 int ready;do{ready=::poll(fds.data(),fds.size(),std::clamp(timeoutMs,0,1000));}while(ready<0&&errno==EINTR);if(ready<0){error="research poll failed";return false;}
 // Process only connections present in this poll snapshot before accepting more.
 for(std::size_t i=s.clients.size();i>0;--i){auto& c=s.clients[i-1];auto flags=fds[i].revents;bool close=Clock::now()>c.deadline||(flags&(POLLERR|POLLNVAL));
  try{if(!close&&c.output.empty()&&(flags&POLLIN)){std::array<std::uint8_t,8192> buffer{};auto n=::recv(c.fd,buffer.data(),buffer.size(),0);if(n==0)close=true;else if(n<0){if(errno!=EINTR&&errno!=EAGAIN)close=true;}else{if(c.input.size()+static_cast<std::size_t>(n)>s.config.maxRequestBytes+8)throw std::runtime_error("research request byte limit");c.input.insert(c.input.end(),buffer.begin(),buffer.begin()+n);if(c.input.size()>=8){auto count=frameSize(c.input.data());if(count>s.config.maxRequestBytes)throw std::runtime_error("research request byte limit");if(c.input.size()>count+8)throw std::runtime_error("one research request per connection");if(c.input.size()==count+8){std::vector<std::uint8_t> payload(c.input.begin()+8,c.input.end());c.output=frame(detail::encode(s.service.dispatch(detail::decode<Request>(std::move(payload)))));c.input.clear();}}}}
   if(!close&&!c.output.empty()&&(flags&POLLOUT)){auto n=::send(c.fd,c.output.data()+c.sent,c.output.size()-c.sent,MSG_NOSIGNAL);if(n>0){c.sent+=static_cast<std::size_t>(n);close=c.sent==c.output.size();}else if(n<0&&errno!=EINTR&&errno!=EAGAIN)close=true;}
   if(flags&POLLHUP)close=true;
  }catch(const std::exception&e){Response r;r.error=e.what();try{c.output=frame(detail::encode(r));c.sent=0;c.input.clear();}catch(...){close=true;}}
  if(close){::close(c.fd);s.clients.erase(s.clients.begin()+static_cast<std::ptrdiff_t>(i-1));}
 }
 if(fds[0].revents&POLLIN){int client=::accept4(s.fd,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);if(client>=0){if(s.clients.size()>=16||!sameOwner(client))::close(client);else s.clients.push_back({client,{}, {},0,Clock::now()+std::chrono::seconds(5)});}}
 return true;
}
} // namespace hft_backtest::research

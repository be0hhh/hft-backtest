#include "ResearchStorage.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
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
bool transfer(int fd,std::uint8_t* bytes,std::size_t size,bool send,std::string& error){std::size_t done=0;while(done<size){auto n=send?::send(fd,bytes+done,size-done,MSG_NOSIGNAL) : ::recv(fd,bytes+done,size-done,0);if(n<0&&errno==EINTR)continue;if(n<=0){error="research local transfer failed or timed out";return false;}done+=static_cast<std::size_t>(n);}return true;}
}
ResearchClient::ResearchClient(std::filesystem::path path):socketPath_(std::move(path)){}
Response ResearchClient::execute(const Request& request)const {
 Response response;int fd=-1;
 try{sockaddr_un local{};if(!address(socketPath_,local,response.error))return response;
  auto bytes=frame(detail::encode(request));fd=::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0){response.error="cannot open research Unix client";return response;}
  timeval timeout{5,0};::setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));::setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
  if(::connect(fd,reinterpret_cast<sockaddr*>(&local),sizeof(local))!=0||!sameOwner(fd)){response.error="research daemon unavailable or peer ownership mismatch";::close(fd);return response;}
  if(!transfer(fd,bytes.data(),bytes.size(),true,response.error)){::close(fd);return response;}
  std::array<std::uint8_t,8> header{};if(!transfer(fd,header.data(),header.size(),false,response.error)){::close(fd);return response;}
  auto size=frameSize(header.data());if(size>detail::kMaxWireBytes)throw std::runtime_error("research response byte limit");bytes.resize(size);if(!transfer(fd,bytes.data(),bytes.size(),false,response.error)){::close(fd);return response;}
  response=detail::decode<Response>(std::move(bytes));::close(fd);return response;
 }catch(const std::exception&e){if(fd>=0)::close(fd);response.error=e.what();return response;}
}
} // namespace hft_backtest::research

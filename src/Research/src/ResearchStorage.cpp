#include "ResearchStorage.hpp"
#include <cerrno>
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>
namespace hft_backtest::research::detail {
bool readFile(const std::filesystem::path& path,std::vector<std::uint8_t>& out,std::string& error) {
 std::error_code ec;auto size=std::filesystem::file_size(path,ec);if(ec||size>kMaxWireBytes){error="missing or oversized research file: "+path.string();return false;}
 std::ifstream file(path,std::ios::binary);out.resize(size);if(!file.read(reinterpret_cast<char*>(out.data()),static_cast<std::streamsize>(size))){error="research file read failed";return false;}return true;
}
bool durableWrite(const std::filesystem::path& path,std::span<const std::uint8_t> data,bool replace,std::string& error) {
 static std::atomic<std::uint64_t> sequence{};
 const auto temporary=path.string()+".pending-"+std::to_string(::getpid())+"-"+std::to_string(sequence.fetch_add(1));
 int fd=::open(temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
 if(fd<0){error="research pending file: "+std::string(std::strerror(errno));return false;}
 bool ok=true;std::size_t done=0;
 while(done<data.size()){const auto n=::write(fd,data.data()+done,data.size()-done);if(n<0&&errno==EINTR)continue;if(n<=0){ok=false;break;}done+=static_cast<std::size_t>(n);}
 if(ok)ok=::fsync(fd)==0;::close(fd);
 if(ok){if(replace)ok=::rename(temporary.c_str(),path.c_str())==0;else{ok=::link(temporary.c_str(),path.c_str())==0;if(ok)::unlink(temporary.c_str());}}
 if(ok){int dir=::open(path.parent_path().c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);ok=dir>=0&&::fsync(dir)==0;if(dir>=0)::close(dir);}
 if(!ok){error="research durable write failed: "+std::string(std::strerror(errno));::unlink(temporary.c_str());}return ok;
}
} // namespace detail


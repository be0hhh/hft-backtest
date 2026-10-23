#pragma once
#include "hft_backtest/Research.hpp"
#include <span>
#include <stdexcept>

namespace hft_backtest::research::detail {
inline constexpr std::size_t kMaxWireBytes = 4U << 20;
struct Codec {
  std::vector<std::uint8_t> bytes;
  std::size_t offset{};
  bool reading{};
  explicit Codec(bool read = false) : reading(read) {}
  void number(std::uint64_t& value) {
    if (reading) {
      if (offset > bytes.size() || bytes.size()-offset < 8) throw std::runtime_error("truncated research integer");
      value=0; for(unsigned i=0;i<8;++i) value|=std::uint64_t(bytes[offset++])<<(8*i);
    } else {
      if(bytes.size()+8>kMaxWireBytes) throw std::runtime_error("research message byte limit");
      for(unsigned i=0;i<8;++i) bytes.push_back(static_cast<std::uint8_t>(value>>(8*i)));
    }
  }
  template<class T> void integer(T& v) {
    std::uint64_t n=static_cast<std::uint64_t>(v); number(n);
    if(reading) { v=static_cast<T>(n); if(static_cast<std::uint64_t>(v)!=n) throw std::runtime_error("research integer range"); }
  }
  void text(std::string& s, std::size_t maximum=1U<<20) {
    std::uint64_t n=s.size(); number(n);
    if(n>maximum) throw std::runtime_error("research string limit");
    if(reading) {
      if(n>bytes.size()-offset) throw std::runtime_error("truncated research string");
      s.assign(reinterpret_cast<const char*>(bytes.data()+offset),n); offset+=n;
    } else { if(bytes.size()+n>kMaxWireBytes) throw std::runtime_error("research message byte limit"); bytes.insert(bytes.end(),s.begin(),s.end()); }
  }
  template<class T, class F> void list(std::vector<T>& v, std::size_t maximum,F element) {
    std::uint64_t n=v.size(); number(n); if(n>maximum) throw std::runtime_error("research list limit");
    if(reading) v.resize(n); for(auto& e:v) element(e);
  }
  void done() const { if(reading && offset!=bytes.size()) throw std::runtime_error("research trailing bytes"); }
};
inline std::uint64_t fingerprint(std::span<const std::uint8_t> bytes) {std::uint64_t value=14695981039346656037ULL;for(auto byte:bytes){value^=byte;value*=1099511628211ULL;}return value;}
inline std::uint64_t variantIdentityFromFingerprint(std::uint64_t value,std::size_t ordinal) {const auto position=static_cast<std::uint64_t>(ordinal);for(unsigned i=0;i<8;++i){value^=(position>>(i*8))&255;value*=1099511628211ULL;}return value?value:1;}
bool validateStoredExperimentSpec(const ExperimentSpec&,std::string& error);
void code(Codec&, ExperimentSpec&);
void code(Codec&, ExperimentStatus&);
void code(Codec&, ResultRow&);
void code(Codec&, Request&);
void code(Codec&, Response&);
bool readFile(const std::filesystem::path&,std::vector<std::uint8_t>&,std::string&);
bool durableWrite(const std::filesystem::path&,std::span<const std::uint8_t>,bool replace,std::string&);
template<class T> std::vector<std::uint8_t> encode(T value) { Codec c; code(c,value); return std::move(c.bytes); }
template<class T> T decode(std::vector<std::uint8_t> bytes) { Codec c(true); c.bytes=std::move(bytes); T value; code(c,value); c.done(); return value; }
} // namespace hft_backtest::research::detail

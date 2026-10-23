#pragma once
#include "hft_backtest/Research.hpp"
#include "hftrec/CorpusContract/BinaryMarketCorpusFormat.hpp"
#include <memory>
#include <span>

namespace hft_backtest::research {
using DataStudyOutput = bool (*)(ResultRow,void*);
struct DataStudyParameter {
 const char* name;
 const char* defaultValue;
 std::uint8_t kind;
 bool required;
};
struct DataStudyOutputDescriptor {
 ResultKind kind;
 const char* metric;
 const char* unit;
};
struct DataStudyBinding {
 // Session pins emitted variant/inputCut to its prepared identity and current
 // observed record. Zero display timestamps inherit receive UTC; older display
 // timestamps never change when their facts become available.
 void* context{};
 bool (*observe)(void*,const hftrec::corpus::BinaryMarketRecord&,DataStudyOutput,void*,std::string&){};
 void (*destroy)(void*) noexcept{};
 std::uint64_t residentBytes{};
};
// Compiled app-owned extension point. A module declares every consumed channel,
// parameter, output and resident-state bound. Factories own private concrete
// state; no virtual dispatch or trading/order interface crosses this boundary.
struct DataStudyDescriptor {
 const char* name;
 const char* description;
 std::uint16_t inputChannels;
 std::span<const DataStudyParameter> parameters;
 std::span<const DataStudyOutputDescriptor> outputs;
 std::uint64_t (*requiredStateBytes)(const ExperimentSpec&,const ExperimentVariant&) noexcept;
 bool (*prepare)(const ExperimentSpec&,const ExperimentVariant&,std::uint64_t,
                 DataStudyBinding&,std::string&);
};
const DataStudyDescriptor* dataStudyDescriptorAt(std::size_t) noexcept;
std::size_t dataStudyDescriptorCount() noexcept;
const DataStudyDescriptor* findDataStudyDescriptor(const std::string& name) noexcept;

class DataStudySession {
 public:
  DataStudySession();
  ~DataStudySession();
  bool prepare(const ExperimentSpec&,const ExperimentVariant&,std::uint64_t variant,std::string& error);
  // Explicit native binding supports independently compiled modules and offline
  // fixtures without mutating the production registry. Metadata is consumed at
  // prepare; callbacks and owned state remain valid until destroy.
  bool prepare(const DataStudyDescriptor&,const ExperimentSpec&,const ExperimentVariant&,
               std::uint64_t variant,std::string& error);
  bool observe(const hftrec::corpus::BinaryMarketRecord&,DataStudyOutput,void*,std::string& error);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace hft_backtest::research

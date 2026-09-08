#pragma once

#include <string_view>

namespace hft_backtest {

enum class Status : int {
  Ok = 0,
  InvalidArgument,
  IoError,
  CorruptData,
  ConfigMismatch,
  Unsupported,
  Cancelled,
  Unknown,
};

inline constexpr std::string_view statusToString(Status status) noexcept {
  switch (status) {
    case Status::Ok: return "Ok";
    case Status::InvalidArgument: return "InvalidArgument";
    case Status::IoError: return "IoError";
    case Status::CorruptData: return "CorruptData";
    case Status::ConfigMismatch: return "ConfigMismatch";
    case Status::Unsupported: return "Unsupported";
    case Status::Cancelled: return "Cancelled";
    case Status::Unknown: return "Unknown";
  }
  return "Unknown";
}

inline constexpr bool isOk(Status status) noexcept { return status == Status::Ok; }

}  // namespace hft_backtest

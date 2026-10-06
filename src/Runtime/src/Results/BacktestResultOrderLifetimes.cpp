#include "trading_core/Algorithm/Math/Integer/Saturating.hpp"
#include "../Engine/BacktestEngine.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "absl/container/flat_hash_map.h"

#include "Internal/BacktestResultWriterInternal.hpp"

namespace hft_backtest::detail::result_writer {

OrderLifetimeKey lifetimeKey(std::uint32_t leg, std::uint32_t context, std::uint64_t id) noexcept {
  return {static_cast<std::uint64_t>(leg) << 32u | context, id};
}

bool isEffectiveOrderStatus(std::uint8_t status) noexcept {
  return status == kRecordStatusAcked ||
         status == kRecordStatusFilled ||
         status == kRecordStatusCancelled ||
         status == kRecordStatusPartialUnfilled;
}

bool isOpenOrderStatus(std::uint8_t status) noexcept {
  return status == kRecordStatusAcked;
}

void buildOrderLifetimeIndexes(const EngineState& engine, OrderLifetimeIndexes& out) {
  out.fillByOrderId.clear();
  out.commandsByTargetId.clear();
  out.fillByOrderId.reserve(engine.fills.size());

  for (const JsonFill& fill : engine.fills) {
    if (fill.exitTsNs <= 0 || fill.priceE8 <= 0) continue;
    auto [it, inserted] = out.fillByOrderId.emplace(lifetimeKey(fill.legIndex, fill.contextIndex, fill.orderId), &fill);
    if (!inserted && fill.exitTsNs < it->second->exitTsNs) it->second = &fill;
  }

  absl::flat_hash_map<OrderLifetimeKey, std::size_t> commandCounts;
  commandCounts.reserve(engine.orders.size() / 2u + 1u);
  for (const JsonOrder& order : engine.orders) {
    if (order.targetId == 0 || order.activeTsNs <= 0) continue;
    if (order.action != kRecordActionCancel) continue;
    if (!isEffectiveOrderStatus(order.status)) continue;
    ++commandCounts[lifetimeKey(order.legIndex, order.contextIndex, order.targetId)];
  }

  out.commandsByTargetId.reserve(commandCounts.size());
  for (const auto& row : commandCounts) {
    out.commandsByTargetId[row.first].reserve(row.second);
  }

  for (const JsonOrder& order : engine.orders) {
    if (order.targetId == 0 || order.activeTsNs <= 0) continue;
    if (order.action != kRecordActionCancel) continue;
    if (!isEffectiveOrderStatus(order.status)) continue;
    out.commandsByTargetId[lifetimeKey(order.legIndex, order.contextIndex, order.targetId)].push_back(&order);
  }

  for (auto& row : out.commandsByTargetId) {
    std::stable_sort(row.second.begin(), row.second.end(), [](const JsonOrder* lhs, const JsonOrder* rhs) noexcept {
      if (lhs->activeTsNs != rhs->activeTsNs) return lhs->activeTsNs < rhs->activeTsNs;
      return lhs->id < rhs->id;
    });
  }
}

const JsonFill* lifetimeFill(const OrderLifetimeIndexes& indexes, OrderLifetimeKey orderId) noexcept {
  const auto it = indexes.fillByOrderId.find(orderId);
  return it == indexes.fillByOrderId.end() ? nullptr : it->second;
}

std::int64_t lifetimeCommandEndTs(const OrderLifetimeIndexes& indexes,
                                  OrderLifetimeKey orderId,
                                  std::int64_t startTsNs) noexcept {
  const auto it = indexes.commandsByTargetId.find(orderId);
  if (it == indexes.commandsByTargetId.end()) return 0;
  const auto& commands = it->second;
  const auto command = std::upper_bound(commands.begin(), commands.end(), startTsNs, [](std::int64_t tsNs, const JsonOrder* row) noexcept {
    return tsNs < row->activeTsNs;
  });
  return command == commands.end() ? 0 : (*command)->activeTsNs;
}

bool hasSameTimestampLifetimeReplaceEnd(const OrderLifetimeIndexes& indexes,
                                        OrderLifetimeKey orderId,
                                        std::int64_t startTsNs) noexcept {
  const auto it = indexes.commandsByTargetId.find(orderId);
  if (it == indexes.commandsByTargetId.end()) return false;
  const auto& commands = it->second;
  const auto command = std::lower_bound(commands.begin(), commands.end(), startTsNs, [](const JsonOrder* row, std::int64_t tsNs) noexcept {
    return row->activeTsNs < tsNs;
  });
  return command != commands.end() && (*command)->activeTsNs == startTsNs;
}

std::vector<OrderLifetimeRow> materializeOrderLifetimes(const EngineState& engine) {
  std::vector<OrderLifetimeRow> out;
  out.reserve(engine.fills.size() + 16u);
  OrderLifetimeIndexes indexes{};
  buildOrderLifetimeIndexes(engine, indexes);

  for (const JsonOrder& order : engine.orders) {
    if (order.action != kRecordActionOrder) continue;
    if (order.type != kRecordTypeLimit && order.type != kRecordTypeStopMarket) continue;
    if (!isEffectiveOrderStatus(order.status)) continue;
    const std::int64_t displayPriceE8 =
        order.type == kRecordTypeStopMarket ? order.triggerPriceE8 : order.priceE8;
    if (order.activeTsNs <= 0 || displayPriceE8 <= 0 || order.qtyE8 <= 0) continue;
    const bool replacedAtStart = hasSameTimestampLifetimeReplaceEnd(indexes, lifetimeKey(order.legIndex, order.contextIndex, order.id), order.activeTsNs);
    if (replacedAtStart) continue;

    const JsonFill* fill = lifetimeFill(indexes, lifetimeKey(order.legIndex, order.contextIndex, order.id));
    std::int64_t endTs = lifetimeCommandEndTs(indexes, lifetimeKey(order.legIndex, order.contextIndex, order.id), order.activeTsNs);
    if (fill != nullptr && (endTs == 0 || fill->exitTsNs < endTs)) endTs = fill->exitTsNs;

    bool openEnded = false;
    if (endTs <= order.activeTsNs) {
      if (fill != nullptr) {
        endTs = order.activeTsNs;
      } else if (isOpenOrderStatus(order.status)) {
        endTs = engine.lastRiskSnapshotTsNs > order.activeTsNs ? engine.lastRiskSnapshotTsNs : order.activeTsNs;
        openEnded = endTs > order.activeTsNs;
      }
    }
    if (endTs <= order.activeTsNs) continue;
    out.push_back(OrderLifetimeRow{
        order.activeTsNs,
        endTs,
        displayPriceE8,
        order.qtyE8,
        order.side,
        openEnded,
        order.legIndex,
        order.type,
    });
  }

  std::stable_sort(out.begin(), out.end(), [](const OrderLifetimeRow& lhs, const OrderLifetimeRow& rhs) noexcept {
    if (lhs.tsStartNs != rhs.tsStartNs) return lhs.tsStartNs < rhs.tsStartNs;
    return lhs.priceE8 < rhs.priceE8;
  });
  return out;
}

bool writeOrderLifetimes(const std::filesystem::path& path,
                         const std::vector<OrderLifetimeRow>& lifetimes) noexcept {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return false;
  for (const OrderLifetimeRow& row : lifetimes) {
    out << '[' << row.tsStartNs
        << ',' << row.tsEndNs
        << ',' << row.priceE8
        << ',' << row.qtyE8
        << ',' << static_cast<unsigned>(row.side)
        << ',' << (row.openEnded ? 1 : 0)
        << ',' << row.legIndex
        << ',' << static_cast<unsigned>(row.orderType)
        << "]\n";
  }
  return out.good();
}

}  // namespace hft_backtest::detail::result_writer


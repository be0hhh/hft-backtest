#pragma once
#include "cxet/Primitives/Buf/Span.hpp"
#include <cstddef>
#include <span>
#include <type_traits>
#include <vector>

namespace hft_backtest::detail {
struct EngineState;
// Context route indices borrow permanent source owners; overlapping configured
// groups never copy orders, positions or accounting authority.
template <typename T> class BasicEngineView {
  template <typename> friend class BasicEngineView;
  T* contiguous_{};
  EngineState* const* indexed_{};
  std::size_t size_{};
 public:
  BasicEngineView() = default;
  BasicEngineView(T* data, std::size_t size) : contiguous_(data), size_(size) {}
  BasicEngineView(EngineState* const* indexed, std::size_t size) : indexed_(indexed), size_(size) {}
  template <typename U> requires std::is_convertible_v<U*, T*>
  BasicEngineView(std::vector<U>& values) : contiguous_(values.data()), size_(values.size()) {}
  template <typename U> requires std::is_convertible_v<const U*, T*>
  BasicEngineView(const std::vector<U>& values) : contiguous_(values.data()), size_(values.size()) {}
  template <typename U> requires std::is_convertible_v<U*, T*>
  BasicEngineView(Span<U> values) : contiguous_(values.data()), size_(values.size()) {}
  template <typename U> requires std::is_convertible_v<U*, T*>
  BasicEngineView(BasicEngineView<U> values) :
      contiguous_(values.contiguous_), indexed_(values.indexed_), size_(values.size_) {}
  std::size_t size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0u; }
  T& operator[](std::size_t i) const noexcept { return indexed_ ? *indexed_[i] : contiguous_[i]; }
  BasicEngineView first(std::size_t n) const noexcept {
    return indexed_ ? BasicEngineView(indexed_, n < size_ ? n : size_) :
        BasicEngineView(contiguous_, n < size_ ? n : size_);
  }
  struct Iterator {
    const BasicEngineView* owner;
    std::size_t index;
    T& operator*() const noexcept { return (*owner)[index]; }
    Iterator& operator++() noexcept { ++index; return *this; }
    bool operator!=(const Iterator& other) const noexcept { return index != other.index; }
  };
  Iterator begin() const noexcept { return {this, 0u}; }
  Iterator end() const noexcept { return {this, size_}; }
};
using EngineView = BasicEngineView<EngineState>;
using ConstEngineView = BasicEngineView<const EngineState>;
}  // namespace hft_backtest::detail

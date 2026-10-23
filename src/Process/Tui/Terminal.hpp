#pragma once
#include <csignal>
#include <string>
#include <termios.h>
#include <vector>

namespace hft_backtest::tui {
enum class KeyKind { None, Character, Enter, Escape, Backspace, Up, Down, Left, Right, PageUp, PageDown, Home, End, Tab, Closed };
struct Key { KeyKind kind{KeyKind::None}; char character{}; };
struct Viewport { int rows{24}, columns{80}; };
extern volatile std::sig_atomic_t interrupted;
// Independent terminal lifetime only: destroying this object never sends a
// Recorder/Research stop or terminates a producer process.
class Terminal {
 public:
  Terminal();
  ~Terminal();
  Terminal(const Terminal&) = delete;
  Terminal& operator=(const Terminal&) = delete;
  bool open(std::string& error);
  Key read(int timeoutMs);
  Viewport viewport() const;
  void draw(const std::vector<std::string>& lines, const std::string& footer,
            const std::string& prompt, std::size_t scroll);
 private:
  termios saved_{};
  bool active_{};
  using Handler = void (*)(int);
  Handler oldInt_{SIG_DFL}, oldTerm_{SIG_DFL};
};
} // namespace hft_backtest::tui

#include "Terminal.hpp"
#include "ResearchTui.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace hft_backtest::tui {
volatile std::sig_atomic_t interrupted = 0;
namespace {
void interrupt(int) { interrupted = 1; }
bool readByte(char& c, int timeout) {
  pollfd descriptor{STDIN_FILENO, POLLIN, 0};
  return ::poll(&descriptor, 1, timeout) > 0 && (descriptor.revents & POLLIN) != 0 && ::read(STDIN_FILENO, &c, 1) == 1;
}
}
Terminal::Terminal() = default;
Terminal::~Terminal() {
  if (!active_) return;
  std::fputs("\033[0m\033[?25h\033[?1049l", stdout);
  std::fflush(stdout);
  ::tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
  std::signal(SIGINT, oldInt_); std::signal(SIGTERM, oldTerm_);
}
bool Terminal::open(std::string& error) {
  if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO)) { error = "A terminal is required. Use the research/recorder CLI for scripts."; return false; }
  if (::tcgetattr(STDIN_FILENO, &saved_) != 0) { error = "Cannot read terminal settings"; return false; }
  auto raw = saved_;
  raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
  raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
  if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) { error = "Cannot enter terminal mode"; return false; }
  oldInt_ = std::signal(SIGINT, interrupt); oldTerm_ = std::signal(SIGTERM, interrupt);
  interrupted = 0; active_ = true;
  std::fputs("\033[?1049h\033[?25l", stdout); std::fflush(stdout);
  return true;
}
Key Terminal::read(int timeoutMs) {
  pollfd descriptor{STDIN_FILENO, POLLIN, 0};
  const int ready = ::poll(&descriptor, 1, timeoutMs);
  if (ready < 0) return errno == EINTR ? Key{} : Key{KeyKind::Closed};
  if (ready == 0) return {};
  if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) return {KeyKind::Closed};
  char c{};
  if (::read(STDIN_FILENO, &c, 1) != 1) return {};
  if (c == '\n' || c == '\r') return {KeyKind::Enter};
  if (c == '\t') return {KeyKind::Tab};
  if (c == '\b' || c == 127) return {KeyKind::Backspace};
  if (c == 3) { interrupted = 1; return {}; }
  if (c == 27) {
    char first{}, second{};
    if (!readByte(first, 25) || first != '[' || !readByte(second, 25)) return {KeyKind::Escape};
    if (second == 'A') return {KeyKind::Up}; if (second == 'B') return {KeyKind::Down};
    if (second == 'C') return {KeyKind::Right}; if (second == 'D') return {KeyKind::Left};
    if (second == 'H') return {KeyKind::Home}; if (second == 'F') return {KeyKind::End};
    if (second == '5' || second == '6') {
      char suffix{};
      if (readByte(suffix, 25) && suffix == '~') return {second == '5' ? KeyKind::PageUp : KeyKind::PageDown};
    }
    return {KeyKind::Escape};
  }
  if (static_cast<unsigned char>(c) >= 32 && static_cast<unsigned char>(c) != 127) return {KeyKind::Character, c};
  return {};
}
Viewport Terminal::viewport() const {
  winsize size{};
  if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0) return {};
  return {std::clamp(static_cast<int>(size.ws_row), 6, 160), std::clamp(static_cast<int>(size.ws_col), 20, 400)};
}
void Terminal::draw(const std::vector<std::string>& lines, const std::string& footer,
                    const std::string& prompt, std::size_t scroll) {
  const auto size = viewport();
  std::string frame{"\033[H\033[2J"};
  const auto append = [&](const std::string& line) { frame += safeText(line, static_cast<std::size_t>(size.columns - 1)); frame += "\r\n"; };
  if (!lines.empty()) append(lines.front());
  const auto capacity = static_cast<std::size_t>(size.rows - 4);
  const auto remaining = lines.size() > 1 ? lines.size() - 1 : 0;
  scroll = std::min(scroll, remaining > capacity ? remaining - capacity : 0);
  for (std::size_t row = 0; row < capacity; ++row) {
    const auto index = 1 + scroll + row;
    append(index < lines.size() ? lines[index] : "");
  }
  append(footer); append(prompt);
  frame += "\033[?25l";
  std::fwrite(frame.data(), 1, frame.size(), stdout); std::fflush(stdout);
}
} // namespace hft_backtest::tui

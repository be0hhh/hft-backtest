#pragma once

#if defined(_WIN32) && defined(HFT_BACKTEST_SHARED)
#if defined(HFT_BACKTEST_BUILDING_LIBRARY)
#define HFT_BACKTEST_API __declspec(dllexport)
#else
#define HFT_BACKTEST_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(HFT_BACKTEST_SHARED)
#define HFT_BACKTEST_API __attribute__((visibility("default")))
#else
#define HFT_BACKTEST_API
#endif

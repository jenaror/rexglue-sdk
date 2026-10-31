/**
 * @file        rex/ui/overlay/debug_overlay.h
 *
 * @brief       ImGui debug overlay dialog for frame timing display.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <string_view>
#include <rex/ui/imgui_dialog.h>
#include <array>
#include <cstdint>
#include <functional>

namespace rex::ui {

// DP1 2026-09-06: lets the app replace the SDK build stamp watermark shown by the
// debug overlay (F3) with its own build string. Empty = REXGLUE_BUILD_STAMP.
void SetDebugOverlayBuildStamp(std::string_view stamp);

struct FrameStats {
  double frame_time_ms = 0;
  double fps = 0;
  uint64_t frame_count = 0;
};

class DebugOverlayDialog : public ImGuiDialog {
 public:
  using FrameStatsProvider = std::function<FrameStats()>;

  explicit DebugOverlayDialog(ImGuiDrawer* imgui_drawer, FrameStatsProvider stats_provider = {});
  ~DebugOverlayDialog();

  void SetStatsProvider(FrameStatsProvider provider) { stats_provider_ = std::move(provider); }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  FrameStatsProvider stats_provider_;
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  static constexpr size_t kFrameHistorySize = 120;
  std::array<float, kFrameHistorySize> frame_time_history_{};
  size_t frame_history_idx_ = 0;
#endif
};

}  // namespace rex::ui

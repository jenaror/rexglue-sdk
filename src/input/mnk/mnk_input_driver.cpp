#include <chrono>
/**
 * @file        input/mnk/mnk_input_driver.cpp
 * @brief       Keyboard/mouse input driver implementation.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/mnk/stick_mapping.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>

REXCVAR_DEFINE_BOOL(mnk_mode, false, "Input", "Enable keyboard/mouse controller emulation");
REXCVAR_DEFINE_BOOL(mnk_mouse, false, "Input",
                    "Use the mouse for the right stick. Off means the right stick comes "
                    "from the keybind_rstick_* keys only");
REXCVAR_DEFINE_DOUBLE(mnk_sensitivity, 1.0, "Input", "Mouse sensitivity for right stick")
    .range(0.01, 10.0);
// DP1 2026-09-06 (ported from the Downpour SDK fork, mnk_input_driver.cpp
// "raw path"): mouse deltas map to right-stick deflection DIRECTLY, with a
// floor above the game's inner right-stick deadzone. The stock mapping
// (delta * sensitivity * 200) produced 200-600 for ordinary 1-3 px moves,
// which the game discards below its ~8689 deadzone - that read as "slow,
// dead mouse that only reacts to flicks" in Deadly Premonition (and Downpour).
REXCVAR_DEFINE_DOUBLE(mnk_stick_scale, 300.0, "Input",
                      "Right-stick units per mouse pixel (per poll) on top of the deadzone floor; "
                      "multiplied by mnk_sensitivity")
    .range(10.0, 5000.0);
// DPRecomp #30 (2026-09-19): while aiming, the mouse drives the LEFT stick and
// the game's aim routine reads that stick per axis, each axis with its own
// deadzone. The vector mapping below puts the floor on the length of the
// motion, so a diagonal move only clears the deadzone on one axis at a time
// and the reticle staircases. With this on, aim mode adds the floor to each
// axis that moved instead (small moves lean to 45 degrees, which is what a
// per-axis reader expects); the look camera keeps the vector mapping.
REXCVAR_DEFINE_BOOL(mnk_aim_axis_floor, true, "Input",
                    "Aiming: add the right-stick deadzone floor per axis (diagonal aim moves both "
                    "axes at once; off = vector mapping as for the look camera)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(mnk_deadzone_floor, 8689, "Input",
                     "Minimum right-stick deflection for any nonzero mouse motion (XInput "
                     "canonical inner deadzone is 8689; 0 = no floor)")
    .range(0, 32767);
REXCVAR_DEFINE_BOOL(mnk_auto_shake, false, "Input/MnK",
                    "Hold the left and right movement keys together to shake the left stick "
                    "automatically (stick-shake QTEs such as \"Get it off!\")")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(mnk_key_stick_ramp_ms, 60.0, "Input/MnK",
                      "Keyboard left stick travel time from centre to full deflection (and back) "
                      "in ms, like the PC port: tapping A/D alternately sweeps through the centre "
                      "so stick-shake QTEs count every tap. 0 = instant (1.0 behaviour)")
    .range(0.0, 300.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(mnk_tap_shake_window_ms, 250.0, "Input/MnK",
                      "Tap-to-shake: two alternating left/right movement key presses closer "
                      "than this (ms) start a full-amplitude left-stick shake (stick-shake "
                      "QTEs such as \"Get it off!\", tap A > D > A > D like the PC port). 0 = off")
    .range(0.0, 1000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(mnk_tap_shake_hold_ms, 300.0, "Input/MnK",
                      "Tap-to-shake: keep shaking this long (ms) after the last tap")
    .range(0.0, 2000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(mnk_wheel_rstick_ms, 120.0, "Input/MnK",
                      "Outside gameplay cameras (menus, the map) each mouse wheel notch also holds "
                      "the right stick up/down for this long (ms) - map zoom with the wheel. 0 = off")
    .range(0.0, 1000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(mnk_auto_shake_hz, 12.0, "Input/MnK",
                      "Auto-shake oscillation rate in full left-right cycles per second")
    .range(2.0, 30.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(mnk_invert_y, false, "Input", "Invert mouse Y for the right stick");

REXCVAR_DEFINE_STRING(keybind_a, "Semicolon,Space", "Input/Keybinds/Controller", "A button");
REXCVAR_DEFINE_STRING(keybind_b, "Quote,Backspace", "Input/Keybinds/Controller", "B button");
REXCVAR_DEFINE_STRING(keybind_x, "L", "Input/Keybinds/Controller", "X button");
REXCVAR_DEFINE_STRING(keybind_y, "P", "Input/Keybinds/Controller", "Y button");
REXCVAR_DEFINE_STRING(keybind_left_trigger, "Q,I", "Input/Keybinds/Controller", "Left trigger");
REXCVAR_DEFINE_STRING(keybind_right_trigger, "E,O", "Input/Keybinds/Controller", "Right trigger");
REXCVAR_DEFINE_STRING(keybind_left_shoulder, "1", "Input/Keybinds/Controller", "Left shoulder");
REXCVAR_DEFINE_STRING(keybind_right_shoulder, "3", "Input/Keybinds/Controller", "Right shoulder");
REXCVAR_DEFINE_STRING(keybind_lstick_up, "W", "Input/Keybinds/Controller", "Left stick up");
REXCVAR_DEFINE_STRING(keybind_lstick_down, "S", "Input/Keybinds/Controller", "Left stick down");
REXCVAR_DEFINE_STRING(keybind_lstick_left, "A", "Input/Keybinds/Controller", "Left stick left");
REXCVAR_DEFINE_STRING(keybind_lstick_right, "D", "Input/Keybinds/Controller", "Left stick right");
REXCVAR_DEFINE_STRING(keybind_lstick_press, "F", "Input/Keybinds/Controller", "Left stick press");
REXCVAR_DEFINE_STRING(keybind_rstick_up, "Up", "Input/Keybinds/Controller", "Right stick up");
REXCVAR_DEFINE_STRING(keybind_rstick_down, "Down", "Input/Keybinds/Controller", "Right stick down");
REXCVAR_DEFINE_STRING(keybind_rstick_left, "Left", "Input/Keybinds/Controller", "Right stick left");
REXCVAR_DEFINE_STRING(keybind_rstick_right, "Right", "Input/Keybinds/Controller",
                      "Right stick right");
REXCVAR_DEFINE_STRING(keybind_rstick_press, "K", "Input/Keybinds/Controller", "Right stick press");
REXCVAR_DEFINE_STRING(keybind_dpad_up, "Shift+Up", "Input/Keybinds/Controller", "D-pad up");
REXCVAR_DEFINE_STRING(keybind_dpad_down, "Shift+Down", "Input/Keybinds/Controller", "D-pad down");
REXCVAR_DEFINE_STRING(keybind_dpad_left, "Shift+Left", "Input/Keybinds/Controller", "D-pad left");
REXCVAR_DEFINE_STRING(keybind_dpad_right, "Shift+Right", "Input/Keybinds/Controller",
                      "D-pad right");
REXCVAR_DEFINE_STRING(keybind_back, "Z,Tab", "Input/Keybinds/Controller", "Back button");
REXCVAR_DEFINE_STRING(keybind_start, "X,Return", "Input/Keybinds/Controller", "Start button");
REXCVAR_DEFINE_STRING(keybind_guide, "", "Input/Keybinds/Controller", "Guide button");

namespace rex::input::mnk {

namespace {

// A single device, so its handle is a constant.
constexpr rex::input::DeviceId kMnkDevice = static_cast<rex::input::DeviceId>(0x4D4E4B00);

// Bind values are a comma-separated list of alternatives, each optionally
// carrying modifier prefixes: "Q,I" or "Shift+W". Modifier matching is exact,
// so "Up" stays silent while Shift is held and "Shift+Up" stays silent without
// it. That is what lets the D-pad share the arrow keys. A consequence is
// that binding a bare modifier name ("Shift") can never fire, since holding it
// makes the live mask non-zero while the bind wants zero.
//
// DP1 port 2026-09-05 (from dpour fork ce7d25d, mnk_input_driver.cpp): inside
// one alternative, '+' may also join several non-modifier keys into an AND
// combo, "Space+LMB" or "Shift+Space+LMB". Every listed key has to be down and
// the modifier mask still has to match exactly, so all pre-existing bind
// strings keep their meaning. "WheelUp" / "WheelDown" are ordinary key names
// here; the driver pulses them for one poll per scroll detent.
constexpr uint8_t kModShift = 1u << 0;
constexpr uint8_t kModCtrl = 1u << 1;
constexpr uint8_t kModAlt = 1u << 2;

uint8_t LiveModifiers(const bool (&key_down)[256]) {
  uint8_t mods = 0;
  if (key_down[static_cast<uint16_t>(rex::ui::VirtualKey::kShift)])
    mods |= kModShift;
  if (key_down[static_cast<uint16_t>(rex::ui::VirtualKey::kControl)])
    mods |= kModCtrl;
  if (key_down[static_cast<uint16_t>(rex::ui::VirtualKey::kMenu)])
    mods |= kModAlt;
  return mods;
}

// Modifier mask bit for a modifier token name, or 0 for anything else.
uint8_t ModifierBit(std::string_view token) {
  if (token == "Shift") {
    return kModShift;
  }
  if (token == "Ctrl" || token == "Control") {
    return kModCtrl;
  }
  if (token == "Alt") {
    return kModAlt;
  }
  return 0;
}

std::string_view TrimSpaces(std::string_view s) {
  while (!s.empty() && s.front() == ' ') {
    s.remove_prefix(1);
  }
  while (!s.empty() && s.back() == ' ') {
    s.remove_suffix(1);
  }
  return s;
}

// DP1 v1.1 (DPRecomp #11 "holding Shift freezes York"): bind evaluation.
// One comma-separated alternative = modifier names + key names joined by '+'.
// Rules:
//   * every listed key must be down and every listed modifier must be held;
//     OTHER modifiers being held no longer silence a bind ("W" keeps working
//     while Shift = X button is held, so run = Shift + WASD works);
//   * a bare modifier group ("Shift") fires while that modifier is held;
//   * shadowing keeps the D-pad on the arrow keys: a group is silent while a
//     MORE specific configured group (same keys, strictly more modifiers) is
//     satisfied, so "Left" stays quiet while "Shift+Left" fires.
struct ParsedGroup {
  uint8_t mods = 0;
  std::vector<uint16_t> keys;  // sorted
  bool valid = false;
};

ParsedGroup ParseGroup(std::string_view group) {
  ParsedGroup out;
  bool bad = false;
  while (!group.empty()) {
    size_t plus = group.find('+');
    std::string_view token = TrimSpaces(group.substr(0, plus));
    if (plus == std::string_view::npos) {
      group = std::string_view();
    } else {
      group.remove_prefix(plus + 1);
    }
    if (token.empty()) {
      continue;
    }
    uint8_t bit = ModifierBit(token);
    if (bit) {
      out.mods |= bit;
      continue;
    }
    rex::ui::VirtualKey vk = rex::ui::ParseVirtualKey(token);
    uint16_t idx = static_cast<uint16_t>(vk);
    if (vk == rex::ui::VirtualKey::kNone || idx >= 256) {
      bad = true;
      break;
    }
    out.keys.push_back(idx);
  }
  std::sort(out.keys.begin(), out.keys.end());
  out.valid = !bad && (out.mods != 0 || !out.keys.empty());
  return out;
}

bool GroupSatisfied(const ParsedGroup& g, const bool (&key_down)[256], uint8_t live_mods) {
  if (!g.valid || (live_mods & g.mods) != g.mods) {
    return false;
  }
  for (uint16_t k : g.keys) {
    if (!key_down[k]) {
      return false;
    }
  }
  return true;
}

// All configured groups of the current poll (for shadowing); filled at the
// top of GetState under state_mutex_.
std::vector<ParsedGroup> g_all_groups;

void ForEachGroup(const std::string& cvar_val, const std::function<void(std::string_view)>& fn) {
  std::string_view rest(cvar_val);
  while (!rest.empty()) {
    size_t comma = rest.find(',');
    std::string_view token = TrimSpaces(rest.substr(0, comma));
    if (comma == std::string_view::npos) {
      rest = std::string_view();
    } else {
      rest.remove_prefix(comma + 1);
    }
    if (!token.empty()) {
      fn(token);
    }
  }
}

bool GroupPressed(const bool (&key_down)[256], std::string_view group, uint8_t live_mods) {
  ParsedGroup g = ParseGroup(group);
  if (!GroupSatisfied(g, key_down, live_mods)) {
    return false;
  }
  // Shadowed by a more specific configured group?
  for (const ParsedGroup& other : g_all_groups) {
    if (other.keys == g.keys && other.mods != g.mods && (other.mods & g.mods) == g.mods &&
        GroupSatisfied(other, key_down, live_mods)) {
      return false;
    }
  }
  return true;
}

std::atomic<bool> mouse_look_active{true};

}  // namespace

void SetMouseLookActive(bool active) {
  mouse_look_active.store(active, std::memory_order_relaxed);
}

bool IsMouseLookActive() {
  return mouse_look_active.load(std::memory_order_relaxed);
}

// DP1: camera hook state + raw delta accumulator (see the header).
std::atomic<bool> mouse_camera_hook_active{false};
std::atomic<float> mouse_camera_dx{0.0f};
std::atomic<float> mouse_camera_dy{0.0f};

void SetMouseCameraHookActive(bool active) {
  mouse_camera_hook_active.store(active, std::memory_order_relaxed);
}

bool IsMouseCameraHookActive() {
  return mouse_camera_hook_active.load(std::memory_order_relaxed);
}

std::atomic<int64_t> mouse_camera_hook_frame_ns{0};

void TakeMouseCameraDelta(float& dx_out, float& dy_out) {
  dx_out = mouse_camera_dx.exchange(0.0f, std::memory_order_relaxed);
  dy_out = mouse_camera_dy.exchange(0.0f, std::memory_order_relaxed);
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  mouse_camera_hook_frame_ns.store(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count(), std::memory_order_relaxed);
}

bool IsMouseCameraHookRecent() {
  const int64_t last = mouse_camera_hook_frame_ns.load(std::memory_order_relaxed);
  if (last == 0) return false;
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return now_ns - last < 200'000'000;
}

// DP1 v1.1: "aim camera ran this frame" stamp (steady clock, ns).
std::atomic<int64_t> mouse_aim_frame_ns{0};

void NoteMouseAimFrame() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  mouse_aim_frame_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count(),
                           std::memory_order_relaxed);
}

bool IsMouseAimStickActive() {
  const int64_t last = mouse_aim_frame_ns.load(std::memory_order_relaxed);
  if (last == 0) {
    return false;
  }
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return (now_ns - last) < 150 * 1000000LL;
}

using rex::ui::VirtualKey;

MnkInputDriver::MnkInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

MnkInputDriver::~MnkInputDriver() {
  // Detach handled by OnClosing; if window outlives the driver, clean up here.
  DetachFromWindow();
}

X_STATUS MnkInputDriver::Setup() {
  return X_STATUS_SUCCESS;
}

void MnkInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (window) {
    {
      std::lock_guard lock(state_mutex_);
      attached_window_ = window;
    }
    window->AddInputListener(this, window_z_order());
    window->AddListener(this);
  }
}

void MnkInputDriver::OnClosing(rex::ui::UIEvent&) {
  DetachFromWindow();
}

void MnkInputDriver::DetachFromWindow() {
  rex::ui::Window* window = attached_window_;
  if (!window) {
    return;
  }
  window->app_context().CallInUIThreadSynchronous([this, window] {
    // Detach first so nothing new is queued, then run out what already was.
    {
      std::lock_guard lock(state_mutex_);
      attached_window_ = nullptr;
    }
    if (mouse_capture_update_queued_.load(std::memory_order_relaxed)) {
      window->app_context().ExecutePendingFunctionsFromUIThread();
    }
    ReleaseMouseCaptureFromUIThread(window);
    window->RemoveInputListener(this);
    window->RemoveListener(this);
  });
}

bool MnkInputDriver::IsEnabled() const {
  return REXCVAR_GET(mnk_mode);
}

static bool IsBindPressed(const bool (&key_down)[256], const std::string& cvar_val) {
  const uint8_t live_mods = LiveModifiers(key_down);
  std::string_view rest(cvar_val);
  while (!rest.empty()) {
    size_t comma = rest.find(',');
    std::string_view token = rest.substr(0, comma);
    if (comma == std::string_view::npos) {
      rest = std::string_view();
    } else {
      rest.remove_prefix(comma + 1);
    }
    token = TrimSpaces(token);
    // DP1 port 2026-09-05: each alternative may be an AND combo now.
    if (!token.empty() && GroupPressed(key_down, token, live_mods)) {
      return true;
    }
  }
  return false;
}

void MnkInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  // Disabled means no device at all, so it never occupies a guest user slot.
  if (!IsEnabled()) {
    static bool warned_disabled = false;
    if (!warned_disabled) {
      warned_disabled = true;
      REXLOG_INFO("MnK: mnk_mode is false - keyboard/mouse device NOT enumerated");
    }
    return;
  }
  static bool logged_enabled = false;
  if (!logged_enabled) {
    logged_enabled = true;
    REXLOG_INFO("MnK: keyboard/mouse device enumerated (mnk_mode=true, mnk_mouse={})", REXCVAR_GET(mnk_mouse));
  }
  DeviceInfo info;
  info.id = kMnkDevice;
  info.name = "Keyboard and Mouse";
  info.synthetic = true;
  out.push_back(info);
}

X_RESULT MnkInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                               X_INPUT_CAPABILITIES* out_caps) {
  if (!IsEnabled() || id != kMnkDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;
    out_caps->sub_type = 0x01;
    out_caps->flags = 0;
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    out_caps->vibration.left_motor_speed = 0xFFFF;
    out_caps->vibration.right_motor_speed = 0xFFFF;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  if (!IsEnabled() || id != kMnkDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Mouse look is opt in. Without this gate, keyboard input alone would hide
  // and lock the cursor, breaking the ImGui overlays.
  QueueMouseCaptureUpdate(IsEnabled() && REXCVAR_GET(mnk_mouse) && IsMouseLookActive() &&
                          has_focus_ && is_active());

  if (!is_active() || !has_focus_) {
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = packet_number_;
    }
    // DP1 port 2026-09-05: drop scroll gathered while the device was not the
    // active one, or it would replay as a burst of presses on reactivation.
    std::lock_guard lock(state_mutex_);
    wheel_accumulator_y_ = 0;
    return X_ERROR_SUCCESS;
  }

  std::lock_guard lock(state_mutex_);

  // DP1 port 2026-09-05 (from dpour fork ce7d25d, mnk_input_driver.cpp):
  // drain one mouse-wheel detent per poll and pulse the synthetic kWheelUp /
  // kWheelDown keys for exactly this poll, so binds referencing "WheelUp" /
  // "WheelDown" register as one button press per scroll detent.
  constexpr int32_t kWheelDetent = int32_t(rex::ui::MouseEvent::kScrollPerDetent);
  bool wheel_up_pulse = false;
  bool wheel_down_pulse = false;
  if (wheel_accumulator_y_ >= kWheelDetent) {
    wheel_accumulator_y_ -= kWheelDetent;
    wheel_up_pulse = true;
  } else if (wheel_accumulator_y_ <= -kWheelDetent) {
    wheel_accumulator_y_ += kWheelDetent;
    wheel_down_pulse = true;
  }
  key_down_[static_cast<uint16_t>(VirtualKey::kWheelUp)] = wheel_up_pulse;
  key_down_[static_cast<uint16_t>(VirtualKey::kWheelDown)] = wheel_down_pulse;

  // DP1 v1.1: snapshot every configured group for bind shadowing (see GroupPressed).
  g_all_groups.clear();
  for (const std::string* bind : {&REXCVAR_GET(keybind_a), &REXCVAR_GET(keybind_b), &REXCVAR_GET(keybind_x),
                                  &REXCVAR_GET(keybind_y), &REXCVAR_GET(keybind_left_shoulder),
                                  &REXCVAR_GET(keybind_right_shoulder), &REXCVAR_GET(keybind_lstick_press),
                                  &REXCVAR_GET(keybind_rstick_press), &REXCVAR_GET(keybind_back),
                                  &REXCVAR_GET(keybind_start), &REXCVAR_GET(keybind_guide),
                                  &REXCVAR_GET(keybind_dpad_up), &REXCVAR_GET(keybind_dpad_down),
                                  &REXCVAR_GET(keybind_dpad_left), &REXCVAR_GET(keybind_dpad_right),
                                  &REXCVAR_GET(keybind_left_trigger), &REXCVAR_GET(keybind_right_trigger),
                                  &REXCVAR_GET(keybind_lstick_left), &REXCVAR_GET(keybind_lstick_right),
                                  &REXCVAR_GET(keybind_lstick_up), &REXCVAR_GET(keybind_lstick_down),
                                  &REXCVAR_GET(keybind_rstick_left), &REXCVAR_GET(keybind_rstick_right),
                                  &REXCVAR_GET(keybind_rstick_up), &REXCVAR_GET(keybind_rstick_down)}) {
    ForEachGroup(*bind, [](std::string_view grp) { g_all_groups.push_back(ParseGroup(grp)); });
  }

  uint16_t buttons = 0;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_a)))
    buttons |= X_INPUT_GAMEPAD_A;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_b)))
    buttons |= X_INPUT_GAMEPAD_B;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_x)))
    buttons |= X_INPUT_GAMEPAD_X;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_y)))
    buttons |= X_INPUT_GAMEPAD_Y;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_left_shoulder)))
    buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_right_shoulder)))
    buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_press)))
    buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_rstick_press)))
    buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_back)))
    buttons |= X_INPUT_GAMEPAD_BACK;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_start)))
    buttons |= X_INPUT_GAMEPAD_START;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_guide)))
    buttons |= X_INPUT_GAMEPAD_GUIDE;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_up)))
    buttons |= X_INPUT_GAMEPAD_DPAD_UP;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_down)))
    buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_left)))
    buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_right)))
    buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;

  uint8_t lt = IsBindPressed(key_down_, REXCVAR_GET(keybind_left_trigger)) ? 0xFF : 0;
  uint8_t rt = IsBindPressed(key_down_, REXCVAR_GET(keybind_right_trigger)) ? 0xFF : 0;

  int32_t lx = 0;
  int32_t ly = 0;
  const bool lstick_left_down = IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_left));
  const bool lstick_right_down = IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_right));
  // DP1 v1.1.1: tap-to-shake. Rapid alternating taps of the left/right
  // movement keys (A > D > A > D, PC-port style) drive a full-amplitude
  // oscillation for mnk_tap_shake_hold_ms after the last tap. The slew-rate
  // ramp alone left short taps far from the extremes and "Get it off!" did
  // not register. Press edges are taken from key_pressed_since_poll_ so a
  // tap shorter than one poll still counts.
  bool tap_shake = false;
  {
    static std::chrono::steady_clock::time_point last_edge{}, prev_edge{};
    static int last_side = 0;
    const auto now_tp = std::chrono::steady_clock::now();
    const bool left_edge = IsBindPressed(key_pressed_since_poll_, REXCVAR_GET(keybind_lstick_left));
    const bool right_edge = IsBindPressed(key_pressed_since_poll_, REXCVAR_GET(keybind_lstick_right));
    const int side = left_edge ? -1 : (right_edge ? 1 : 0);
    if (side != 0 && side != last_side) {
      prev_edge = last_edge;
      last_edge = now_tp;
      last_side = side;
    }
    const double window_ms = REXCVAR_GET(mnk_tap_shake_window_ms);
    if (window_ms > 0.0 && prev_edge.time_since_epoch().count() != 0) {
      const double gap_ms = std::chrono::duration<double, std::milli>(last_edge - prev_edge).count();
      const double since_ms = std::chrono::duration<double, std::milli>(now_tp - last_edge).count();
      tap_shake = gap_ms <= window_ms && since_ms <= REXCVAR_GET(mnk_tap_shake_hold_ms);
    }
  }
  std::memset(key_pressed_since_poll_, 0, sizeof(key_pressed_since_poll_));
  if (tap_shake || (lstick_left_down && lstick_right_down && REXCVAR_GET(mnk_auto_shake))) {
    // DP1 v1.1: "shake the left stick" QTEs (Get it off!) count stick sweeps;
    // alternating A/D on a keyboard jumps between the extremes without a sweep
    // and half the shakes are missed. Holding both keys makes the driver
    // oscillate the stick left/right at mnk_auto_shake_hz, passing through
    // the centre like a real stick.
    const double hz = std::max(1.0, REXCVAR_GET(mnk_auto_shake_hz));
    const double t = std::chrono::duration<double>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    lx = static_cast<int32_t>(std::lround(std::sin(t * hz * 2.0 * 3.14159265358979) * INT16_MAX));
  } else {
    if (lstick_left_down) lx -= INT16_MAX;
    if (lstick_right_down) lx += INT16_MAX;
  }
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_up)))
    ly += INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_down)))
    ly -= INT16_MAX;
  // DP1 v1.1: slew-rate limit the keyboard left stick (PC-port feel). The
  // targets above are +-32767 / 0; the emitted value travels towards the
  // target at full-scale / mnk_key_stick_ramp_ms, so A > D > A > D taps sweep
  // through the centre instead of jumping between the extremes.
  {
    static double kb_lx = 0.0, kb_ly = 0.0;
    static std::chrono::steady_clock::time_point kb_last{};
    const auto now_tp = std::chrono::steady_clock::now();
    const double ramp_ms = REXCVAR_GET(mnk_key_stick_ramp_ms);
    if (ramp_ms <= 0.0 || kb_last.time_since_epoch().count() == 0) {
      kb_lx = double(lx);
      kb_ly = double(ly);
    } else {
      double dt_ms = std::chrono::duration<double, std::milli>(now_tp - kb_last).count();
      dt_ms = std::clamp(dt_ms, 0.0, 100.0);
      const double step = double(INT16_MAX) * dt_ms / ramp_ms;
      auto approach = [step](double cur, double target) {
        if (target > cur) return std::min(cur + step, target);
        if (target < cur) return std::max(cur - step, target);
        return cur;
      };
      kb_lx = tap_shake ? double(lx) : approach(kb_lx, double(lx));
      kb_ly = approach(kb_ly, double(ly));
    }
    kb_last = now_tp;
    lx = static_cast<int32_t>(std::lround(kb_lx));
    ly = static_cast<int32_t>(std::lround(kb_ly));
  }

  int32_t rx = 0;
  int32_t ry = 0;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_rstick_left)))
    rx -= INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_rstick_right)))
    rx += INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_rstick_up)))
    ry += INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_rstick_down)))
    ry -= INT16_MAX;
  // DP1 v1.1.1: mouse wheel -> right stick Y while no gameplay camera runs
  // (the map zooms with the right stick). The wheel keeps its D-pad binding.
  {
    static std::chrono::steady_clock::time_point wheel_up_until{}, wheel_down_until{};
    const double hold_ms = REXCVAR_GET(mnk_wheel_rstick_ms);
    const auto now_tp = std::chrono::steady_clock::now();
    if (wheel_up_pulse) wheel_up_until = now_tp + std::chrono::milliseconds(int64_t(hold_ms));
    if (wheel_down_pulse) wheel_down_until = now_tp + std::chrono::milliseconds(int64_t(hold_ms));
    const bool camera_live = IsMouseCameraHookActive() && IsMouseCameraHookRecent();
    if (hold_ms > 0.0 && !camera_live) {
      if (now_tp < wheel_up_until) ry += INT16_MAX;
      if (now_tp < wheel_down_until) ry -= INT16_MAX;
    }
  }

  // DP1 v1.1: while the game's aim camera runs, the mouse becomes the LEFT
  // stick (the aim camera's native input); the camera hook path is bypassed.
  // v1.1 rev.2: the right trigger (aim) held is the primary signal - the game
  // aims with the LEFT stick while RT is down, whatever camera routine runs;
  // the aim-camera hook stamp stays as a secondary signal.
  // DP1 v1.1.1: the camera hook only owns the mouse while a gameplay camera is
  // actually running; in menus and the map the mouse is the right stick again.
  const bool hook_live = IsMouseCameraHookActive() && IsMouseCameraHookRecent();
  if (IsMouseCameraHookActive() && !hook_live) {
    float drop_dx = 0.0f, drop_dy = 0.0f;
    TakeMouseCameraDelta(drop_dx, drop_dy);  // keep the stamp fresh-free: no snap when the camera resumes
    mouse_camera_hook_frame_ns.store(0, std::memory_order_relaxed);
  }
  // DP1 1.3.3 (#24): the right trigger held means "aim" whatever camera runs -
  // the clock-tower boss fight uses a locked camera none of the hooked
  // gameplay routines drive, so hook_live is false there and the mouse fell
  // back to the (ignored) right stick. Aim-camera frames still count as before.
  const bool aim_stick = (rt != 0) || (hook_live && IsMouseAimStickActive());
  if (aim_stick) {
    float drop_dx = 0.0f, drop_dy = 0.0f;
    TakeMouseCameraDelta(drop_dx, drop_dy);  // never replay aim motion as a camera snap later
  }
  if (REXCVAR_GET(mnk_mouse) && IsMouseLookActive() && (!hook_live || aim_stick)) {
    // DP1 (rev. 2, 2026-09-06): VECTOR mapping. The deadzone floor is added to
    // the length of the mouse motion vector, not per axis - a per-axis floor
    // turned every small move into a 45-degree diagonal ("crooked"). The
    // direction of the mouse motion is preserved exactly; only the magnitude
    // is remapped: stick length = floor + delta length * sensitivity * scale.
    // Motion this poll = deflection this poll; no motion = instant zero.
    const double dx = double(mouse_dx_);
    // Stick Y is up-positive, screen Y is down-positive.
    const double dy = REXCVAR_GET(mnk_invert_y) ? double(mouse_dy_) : -double(mouse_dy_);
    if (dx != 0.0 || dy != 0.0) {
      const double gain = REXCVAR_GET(mnk_sensitivity) * REXCVAR_GET(mnk_stick_scale);
      const double floor = double(std::clamp(REXCVAR_GET(mnk_deadzone_floor), 0, 32767));
      // Aim mode reads the stick per axis (#30): floor per axis there.
      const StickDeflection d =
          MapMouseToStick(dx, dy, floor, gain, aim_stick && REXCVAR_GET(mnk_aim_axis_floor));
      if (aim_stick) {
        lx += d.x;
        ly += d.y;
      } else {
        rx += d.x;
        ry += d.y;
      }
    }
  }
  // Drained unconditionally: deltas keep accumulating in OnMouseMove while the
  // mouse is off, and toggling it on would otherwise dump the whole backlog
  // into one frame as a camera snap.
  mouse_dx_ = 0.0f;
  mouse_dy_ = 0.0f;

  auto clamp16 = [](int32_t v) -> int16_t {
    return static_cast<int16_t>(std::clamp(v, (int32_t)INT16_MIN, (int32_t)INT16_MAX));
  };

  packet_number_++;

  if (out_state) {
    out_state->packet_number = packet_number_;
    out_state->gamepad.buttons = buttons;
    out_state->gamepad.left_trigger = lt;
    out_state->gamepad.right_trigger = rt;
    out_state->gamepad.thumb_lx = clamp16(lx);
    out_state->gamepad.thumb_ly = clamp16(ly);
    out_state->gamepad.thumb_rx = clamp16(rx);
    out_state->gamepad.thumb_ry = clamp16(ry);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  if (!IsEnabled() || id != kMnkDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t flags,
                                            X_INPUT_KEYSTROKE* out_keystroke) {
  if (!IsEnabled() || id != kMnkDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::lock_guard lock(state_mutex_);
  if (keystroke_queue_.empty()) {
    return X_ERROR_EMPTY;
  }
  if (out_keystroke) {
    *out_keystroke = keystroke_queue_.front();
  }
  keystroke_queue_.pop();
  return X_ERROR_SUCCESS;
}

void MnkInputDriver::EnqueueKeystroke(uint16_t vk_pad, bool down) {
  X_INPUT_KEYSTROKE ks = {};
  ks.virtual_key = vk_pad;
  ks.unicode = 0;
  ks.flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
  // InputSystem stamps the guest user this device is assigned to.
  ks.user_index = 0;
  ks.hid_code = 0;
  keystroke_queue_.push(ks);
}

void MnkInputDriver::QueueMouseCaptureUpdate(bool should_capture) {
  mouse_capture_requested_.store(should_capture, std::memory_order_relaxed);
  bool already_queued = false;
  if (!mouse_capture_update_queued_.compare_exchange_strong(already_queued, true)) {
    return;
  }
  std::lock_guard lock(state_mutex_);
  if (!attached_window_) {
    mouse_capture_update_queued_.store(false, std::memory_order_relaxed);
    return;
  }
  // Deferred, not CallInUIThread: running inline would re-enter state_mutex_.
  attached_window_->app_context().CallInUIThreadDeferred([this] {
    mouse_capture_update_queued_.store(false, std::memory_order_relaxed);
    ApplyMouseCaptureFromUIThread();
  });
}

void MnkInputDriver::ApplyMouseCaptureFromUIThread() {
  rex::ui::Window* window = attached_window_;
  if (!window) {
    return;
  }
  bool should_capture = mouse_capture_requested_.load(std::memory_order_relaxed);
  if (should_capture == mouse_captured_) {
    return;
  }
  if (!should_capture) {
    ReleaseMouseCaptureFromUIThread(window);
    return;
  }
  mouse_captured_ = true;
  precapture_cursor_visibility_ = window->GetCursorVisibility();
  window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
  window->CaptureMouse();
  relative_mouse_mode_ = window->SetRelativeMouseMode(true);
  if (!relative_mouse_mode_) {
    REXLOG_WARN("Pointer lock unavailable, mouse look falls back to recentering the cursor");
  }
  // Reset deltas to avoid a spike on capture start
  std::lock_guard lock(state_mutex_);
  mouse_dx_ = 0.0f;
  mouse_dy_ = 0.0f;
}

void MnkInputDriver::ReleaseMouseCaptureFromUIThread(rex::ui::Window* window) {
  if (!mouse_captured_) {
    return;
  }
  mouse_captured_ = false;
  window->SetCursorVisibility(precapture_cursor_visibility_);
  window->SetRelativeMouseMode(false);
  relative_mouse_mode_ = false;
  window->ReleaseMouse();
}

void MnkInputDriver::RecenterCursorFromUIThread(int32_t x, int32_t y) {
  rex::ui::Window* window = attached_window_;
  if (!window) {
    return;
  }
  int32_t width = int32_t(window->GetActualPhysicalWidth());
  int32_t height = int32_t(window->GetActualPhysicalHeight());
  if (width <= 0 || height <= 0) {
    return;
  }
  // Only once the pointer has drifted well off the middle, to keep warps rare.
  if (std::abs(x - width / 2) < width / 4 && std::abs(y - height / 2) < height / 4) {
    return;
  }
  int32_t center_x = 0;
  int32_t center_y = 0;
  if (window->WarpMouseToCenter(center_x, center_y)) {
    prev_mouse_x_ = center_x;
    prev_mouse_y_ = center_y;
  }
}

void MnkInputDriver::SetKeyState(uint16_t vk, bool down) {
  if (vk < 256) {
    if (down && !key_down_[vk]) key_pressed_since_poll_[vk] = true;
    key_down_[vk] = down;
  }
}

void MnkInputDriver::OnKeyDown(rex::ui::KeyEvent& e) {
  if (!IsEnabled() || !has_focus_) {
    static bool warned_gate = false;
    if (!warned_gate) {
      warned_gate = true;
      REXLOG_INFO("MnK: key event dropped (enabled={}, has_focus={})", IsEnabled(), has_focus_.load());
    }
    return;
  }
  std::lock_guard lock(state_mutex_);
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  REXLOG_DEBUG("MnK: key down vk=0x{:02X}", vk);
  SetKeyState(vk, true);
}

void MnkInputDriver::OnKeyUp(rex::ui::KeyEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  SetKeyState(vk, false);
}

void MnkInputDriver::OnMouseDown(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), true);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), true);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), true);
      break;
    default:
      break;
  }
}

void MnkInputDriver::OnMouseUp(rex::ui::MouseEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), false);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), false);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), false);
      break;
    default:
      break;
  }
}

void MnkInputDriver::OnMouseMove(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  int32_t x = e.x();
  int32_t y = e.y();
  {
    std::lock_guard lock(state_mutex_);
    if (relative_mouse_mode_) {
      // The pointer is locked, so the absolute position no longer moves.
      mouse_dx_ += e.dx();
      mouse_dy_ += e.dy();
    } else {
      mouse_dx_ += float(x - prev_mouse_x_);
      mouse_dy_ += float(y - prev_mouse_y_);
    }
    // DP1: mirror the deltas for the camera hook (drained by the app).
    {
      const float cdx = relative_mouse_mode_ ? float(e.dx()) : float(x - prev_mouse_x_);
      const float cdy = relative_mouse_mode_ ? float(e.dy()) : float(y - prev_mouse_y_);
      mouse_camera_dx.store(mouse_camera_dx.load(std::memory_order_relaxed) + cdx,
                            std::memory_order_relaxed);
      mouse_camera_dy.store(mouse_camera_dy.load(std::memory_order_relaxed) + cdy,
                            std::memory_order_relaxed);
    }
  }
  prev_mouse_x_ = x;
  prev_mouse_y_ = y;
  // Without a pointer lock the cursor still has to be kept off the edges.
  if (mouse_captured_ && !relative_mouse_mode_) {
    RecenterCursorFromUIThread(x, y);
  }
}

// DP1 port 2026-09-05 (from dpour fork ce7d25d, mnk_input_driver.cpp).
// Window::OnMouseWheel already forwards to every WindowInputListener, so
// overriding it is all the plumbing the wheel needs. Same gating as
// OnMouseDown; e.scroll_y() is in kScrollPerDetent units.
void MnkInputDriver::OnMouseWheel(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  wheel_accumulator_y_ += e.scroll_y();
}

void MnkInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {
  has_focus_ = false;
  // Withdraw the request too, or an update queued before the focus loss grabs
  // the cursor straight back.
  mouse_capture_requested_.store(false, std::memory_order_relaxed);
  {
    std::lock_guard lock(state_mutex_);
    std::memset(key_down_, 0, sizeof(key_down_));
    mouse_dx_ = 0.0f;
    mouse_dy_ = 0.0f;
    wheel_accumulator_y_ = 0;  // DP1 port 2026-09-05
  }
  if (attached_window_) {
    ReleaseMouseCaptureFromUIThread(attached_window_);
  }
}

void MnkInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {
  has_focus_ = true;
}

}  // namespace rex::input::mnk

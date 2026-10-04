// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <fmt/format.h>

#include "citra_switch/applets/swkbd.h"
#include "citra_switch/config.h"
#include "citra_switch/emu_window.h"
#include "citra_switch/themes.h"
#include "common/file_util.h"
#include "common/horizon_boost.h"
#include "common/horizon_thread.h"
#include "common/thread.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/applets/default_applets.h"
#include "core/loader/loader.h"
#include "core/movie.h"
#include "core/perf_stats.h"
#include "core/savestate.h"
#include "video_core/gpu.h"
#include "video_core/overlay.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"

namespace SwitchFrontend {

namespace {

std::thread s_emu_thread;
std::atomic<bool> s_stop{true};
// Set true once system.Load succeeds.
// This lets the menu tell a crash/bad ROM apart from a clean exit.
std::atomic<bool> s_load_ok{false};
std::atomic<bool> s_artic_disconnected{false};
// Why the emulation thread stopped the game, when that should be told; read after it's joined.
std::string s_halt_reason;
// Freezes the guest between run loop slices.
std::atomic<bool> s_paused{false};
// Layout requests originate on the frontend thread and are consumed by the sole GPU producer.
std::atomic<bool> s_layout_update_pending{false};
std::atomic<bool> s_movie_throttle_enabled{false};
std::atomic<std::int32_t> s_movie_throttle_clock_percentage{45};
std::atomic<bool> s_movie_playback_detected{false};
std::atomic<bool> s_movie_throttle_active{false};

// How long the paused loop waits between repaints.
constexpr auto kPausedFrameInterval = std::chrono::milliseconds(8);
// The picture fades to black over this before the renderer is torn down (closing a game,
// loading a state), and back in over kFadeInMs once there's a picture again.
constexpr u32 kFadeOutMs = 180;
constexpr u32 kFadeInMs = 320;

constexpr std::int32_t kMovieThrottleClockMin = 10;
constexpr std::int32_t kMovieThrottleClockMax = 100;

void ApplyMovieCpuThrottle(Core::System& system) {
    if (!s_movie_throttle_enabled.load(std::memory_order_relaxed) || !system.IsPoweredOn()) {
        return;
    }
    const u32 percentage =
        static_cast<u32>(s_movie_throttle_clock_percentage.load(std::memory_order_relaxed));
    system.CoreTiming().UpdateClockSpeed(percentage);
    if (!s_movie_throttle_active.exchange(true, std::memory_order_relaxed)) {
        LOG_INFO(Frontend, "Experimental movie CPU throttle enabled at {}%", percentage);
    }
}

void RestoreMovieCpuClock(Core::System& system) {
    if (!s_movie_throttle_active.exchange(false, std::memory_order_relaxed) ||
        !system.IsPoweredOn()) {
        return;
    }
    const u32 percentage = Settings::values.cpu_clock_percentage.GetValue();
    system.CoreTiming().UpdateClockSpeed(percentage);
    LOG_INFO(Frontend, "Experimental movie CPU throttle ended. CPU clock restored to {}%",
             percentage);
}

void RegisterMovieCpuThrottle(Core::System& system) {
    s_movie_playback_detected = false;
    s_movie_throttle_active = false;
    system.RegisterMoviePlaybackStateChanged([&system](bool playing) {
        s_movie_playback_detected.store(playing, std::memory_order_relaxed);
        if (playing) {
            ApplyMovieCpuThrottle(system);
        } else {
            RestoreMovieCpuClock(system);
        }
    });
}

// The screen arrangements R3 cycles through.
struct ScreenLayoutPreset {
    Settings::LayoutOption layout;
    bool swap_screen;
    bool upright_screen;
    bool upright_flipped;
    Settings::SmallScreenPosition small_screen_position;
    const char* name;
};

constexpr std::array<ScreenLayoutPreset, 12> s_layout_presets{{
    {Settings::LayoutOption::Default, false, false, false,
     Settings::SmallScreenPosition::BottomRight, "Vertical stack"},
    {Settings::LayoutOption::SideScreen, false, false, false,
     Settings::SmallScreenPosition::MiddleRight, "Side by side"},
    {Settings::LayoutOption::LargeScreen, false, false, false,
     Settings::SmallScreenPosition::MiddleRight, "Large top, small bottom"},
    {Settings::LayoutOption::LargeScreen, true, false, false,
     Settings::SmallScreenPosition::MiddleRight, "Large bottom, small top"},
    {Settings::LayoutOption::TopScreenOnly, false, false, false,
     Settings::SmallScreenPosition::BottomRight, "Top screen only"},
    {Settings::LayoutOption::BottomScreenOnly, false, false, false,
     Settings::SmallScreenPosition::BottomRight, "Bottom screen only"},
    {Settings::LayoutOption::Default, false, true, false,
     Settings::SmallScreenPosition::BottomRight, "Vertical stack (rotate console)"},
    {Settings::LayoutOption::HybridScreen, false, false, false,
     Settings::SmallScreenPosition::BottomRight, "Hybrid screen"},
    {Settings::LayoutOption::Default, false, true, true,
     Settings::SmallScreenPosition::BottomRight, "Vertical stack (rotate console other way)"},
    {Settings::LayoutOption::OverlayScreen, false, false, false,
     Settings::SmallScreenPosition::BottomRight, "Bottom screen overlay"},
    {Settings::LayoutOption::OverlayScreen, false, false, false,
     Settings::SmallScreenPosition::TopRight, "Big top, small bottom in top right"},
    // Wherever the player put the screens in the quick menu's editor.
    {Settings::LayoutOption::CustomLayout, false, false, false,
     Settings::SmallScreenPosition::TopRight, "Custom"},
}};

constexpr std::size_t kCustomPresetIndex = 11;

// Written by the quick menu's editor on the main thread, read by the emulation thread.
std::mutex s_custom_layout_mutex;
SwitchFrontend::CustomScreenLayout s_custom_layout = SwitchFrontend::DefaultCustomScreenLayout();
bool s_start_in_custom_layout = false;

// Anchors the overlaid screen can snap to.
constexpr std::array<const char*, 8> s_overlay_position_names{
    "Top right",  "Middle right", "Bottom right", "Top left",
    "Middle left", "Bottom left", "Top centre",   "Bottom centre",
};

// Step sizes for the overlaid screen's size and opacity, in percent.
constexpr int kOverlaySizeStep = 5;
constexpr int kOverlayOpacityStep = 10;

// Step size for the gap between the two screens.
constexpr int kScreenGapStep = 4;

// Kept consistent with Settings so the first press advances past the boot default.
std::size_t s_layout_index = 0;

// Bitmask of presets R3 cycles through. Bit i set means preset i is in the rotation.
std::atomic<std::uint32_t> s_layout_cycle_mask{(1u << s_layout_presets.size()) - 1};

// Applies the preset at s_layout_index and requests a framebuffer relayout.
void ApplyCurrentLayout() {
    const ScreenLayoutPreset& preset = s_layout_presets[s_layout_index];
    // The choice sticks for the next game only when it is the custom one.
    s_start_in_custom_layout = s_layout_index == kCustomPresetIndex;
    if (preset.layout == Settings::LayoutOption::CustomLayout) {
        unsigned w = 0, h = 0;
        SwitchFrontend::GetWindowSize(w, h);
        SwitchFrontend::ApplyCustomLayoutPixels(w, h);
    }
    Settings::values.layout_option = preset.layout;
    Settings::values.swap_screen = preset.swap_screen;
    Settings::values.upright_screen = preset.upright_screen;
    Settings::values.upright_screen_flipped = preset.upright_flipped;
    Settings::values.small_screen_position = preset.small_screen_position;
    // The overlay presets differ only in which corner the small screen snaps to.
    if (preset.layout == Settings::LayoutOption::OverlayScreen) {
        Settings::values.overlay_screen_position = preset.small_screen_position;
    }
    s_layout_update_pending.store(true, std::memory_order_release);
    LOG_INFO(Frontend, "Screen layout: {}", preset.name);
}

// Points s_layout_index at whichever preset matches the live settings, so the name the menus
// report stays truthful after a swap lands on another preset's arrangement.
void SyncLayoutIndex() {
    const auto& v = Settings::values;
    for (std::size_t i = 0; i < s_layout_presets.size(); ++i) {
        const ScreenLayoutPreset& preset = s_layout_presets[i];
        if (preset.layout == v.layout_option.GetValue() &&
            preset.swap_screen == v.swap_screen.GetValue() &&
            preset.upright_screen == v.upright_screen.GetValue() &&
            preset.upright_flipped == v.upright_screen_flipped.GetValue() &&
            preset.small_screen_position == v.small_screen_position.GetValue()) {
            s_layout_index = i;
            return;
        }
    }
}

// Save states are keyed by the running title and, when replaying, the active movie.
bool CurrentSaveStateKey(u64& program_id, u64& movie_id) {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn()) {
        return false;
    }
    program_id = 0;
    if (system.GetAppLoader().ReadProgramId(program_id) != Loader::ResultStatus::Success) {
        return false;
    }
    movie_id = system.Movie().GetCurrentMovieID();
    return true;
}

// Turns a completed save state request into an on-screen message.
void ReportSaveStateEvent(Core::System& system) {
    const auto event = system.TakeSaveStateEvent();
    if (!event) {
        return;
    }
    if (event->loading) {
        VideoCore::FadeOverlayTo(0.0f, kFadeInMs);
    }
    const char* verb = event->loading ? "Load" : "Save";
    if (event->success) {
        VideoCore::PostOverlayToast(
            fmt::format("{}d state {}", verb, SaveStateSlotName(event->slot)));
    } else {
        VideoCore::PostOverlayToast(fmt::format("{} failed: {}", verb, event->message), 4000);
    }
}

/// Returns true if `path` is a 3DS title.
bool IsLoadableRom(const std::string& path) {
    auto loader = Loader::GetLoader(path);
    if (!loader) {
        return false;
    }
    bool executable = false;
    return loader->IsExecutable(executable) == Loader::ResultStatus::Success && executable;
}

/// Picks a ROM to boot
std::string ResolveRomPath(const std::string& rom_arg) {
    if (!rom_arg.empty()) {
        if (IsLoadableRom(rom_arg)) {
            return rom_arg;
        }
        LOG_WARNING(Frontend, "ROM argument '{}' is not a loadable 3DS title", rom_arg);
    }

    const std::string roms_dir = FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "roms/";
    FileUtil::CreateFullPath(roms_dir);

    std::string found;
    FileUtil::ForeachDirectoryEntry(
        nullptr, roms_dir,
        [&found](u64*, const std::string& directory, const std::string& virtual_name) {
            if (!found.empty()) {
                return true;
            }
            const std::string path = directory + virtual_name;
            if (!FileUtil::IsDirectory(path) && IsLoadableRom(path)) {
                found = path;
            }
            return true;
        });
    return found;
}

// Keeps presenting the paused picture (with whatever the overlay has on it) for `ms`.
void PresentFor(Core::System& system, u32 ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end && !s_stop) {
        Core::PerfStats::game_frames_updated = true;
        system.GPU().SwapBuffers();
        std::this_thread::sleep_for(kPausedFrameInterval);
    }
}

// A save or a load freezes the picture for a second or two. The quick menu has just closed, so
// the game is put back on screen first (with a note, for a save) rather than the menu staying
// frozen over it; a load, which rebuilds the renderer, fades to black first.
void PrepareStateOperation(Core::System& system, bool loading) {
    if (loading) {
        VideoCore::FadeOverlayTo(1.0f, kFadeOutMs);
        PresentFor(system, kFadeOutMs + 24);
    } else {
        VideoCore::PostOverlayToast("Saving state...", 10000);
        PresentFor(system, 40);
    }
}

void EmuThread(std::string path) {
    Common::SetCurrentThreadName("EmuThread");
    if (!Common::Horizon::PinCurrentThread(Common::Horizon::CoreEmu)) {
        LOG_WARNING(Frontend, "Failed to pin emulation thread to core 2");
    }

    auto& system = Core::System::GetInstance();
    EmuWindow_Switch* window = GetEmuWindow();
    if (!window) {
        LOG_CRITICAL(Frontend, "No EmuWindow available");
        s_stop = true;
        return;
    }

    // Mesa's Switch driver cannot reliably present renderbuffers across shared contexts
    window->MakeCurrent();

    u64 program_id = 0;
    {
        const Common::Horizon::CpuBoostScope boost;

        system.EjectCartridge();
        if (!GetInsertedCartridge().empty()) {
            system.InsertCartridge(GetInsertedCartridge());
        }

        const Core::System::ResultStatus load_result = system.Load(*window, path);
        if (load_result != Core::System::ResultStatus::Success) {
            s_artic_disconnected =
                load_result == Core::System::ResultStatus::ErrorArticDisconnected;
            LOG_CRITICAL(Frontend, "Failed to load ROM '{}' (error {})", path,
                         static_cast<int>(load_result));
            window->DoneCurrent();
            s_stop = true;
            return;
        }

        s_load_ok = true;

        system.GetAppLoader().ReadProgramId(program_id);
        system.GPU().ApplyPerProgramSettings(program_id);

        // Load any cached disk shaders
        system.GPU().Renderer().Rasterizer()->LoadDefaultDiskResources(
            s_stop,
            [](VideoCore::LoadCallbackStage, std::size_t, std::size_t, const std::string&) {});
    }

    LOG_INFO(Frontend, "Emulation started (program id {:016X})", program_id);
    VideoCore::FadeOverlayTo(0.0f, kFadeInMs);
    while (!s_stop) {
        if (s_layout_update_pending.exchange(false, std::memory_order_acq_rel)) {
            // The custom layout is kept as fractions: redo its pixels for the current output,
            // which changes size when the console is docked or undocked.
            if (Settings::values.layout_option.GetValue() == Settings::LayoutOption::CustomLayout) {
                unsigned w = 0, h = 0;
                GetWindowSize(w, h);
                ApplyCustomLayoutPixels(w, h);
            }
            system.GPU().UpdateCurrentFramebufferLayout();
            Core::PerfStats::game_frames_updated = true;
        }
        if (s_paused.load(std::memory_order_relaxed)) {
            // The overlay is only drawn as part of a frame present, so the last frame has to keep being presented.
            Core::PerfStats::game_frames_updated = true;
            system.GPU().SwapBuffers();
            std::this_thread::sleep_for(kPausedFrameInterval);
            continue;
        }

        const Core::System::ResultStatus result = system.RunLoop();
        ReportSaveStateEvent(system);
        if (result == Core::System::ResultStatus::Success) {
            continue;
        }
        // A rejected save or load leaves the guest untouched, so keep running.
        if (result == Core::System::ResultStatus::ErrorSavestate ||
            result == Core::System::ResultStatus::ErrorSavestateBuildMismatch) {
            LOG_ERROR(Frontend, "Save state operation failed: {}", system.GetStatusDetails());
            VideoCore::FadeOverlayTo(0.0f, kFadeInMs);
            if (result == Core::System::ResultStatus::ErrorSavestateBuildMismatch) {
                VideoCore::PostOverlayToast("That state is from a different version of EmuSwitch",
                                            4000);
            }
            continue;
        }
        if (result == Core::System::ResultStatus::ShutdownRequested) {
            LOG_INFO(Frontend, "Guest requested shutdown");
        } else {
            s_artic_disconnected =
                result == Core::System::ResultStatus::ErrorArticDisconnected;
            LOG_CRITICAL(Frontend, "Emulation halted: {} (error {})", system.GetStatusDetails(),
                         static_cast<int>(result));
            if (result == Core::System::ResultStatus::ErrorUnknown) {
                s_halt_reason = system.GetStatusDetails();
            }
        }
        break;
    }

    window->DoneCurrent();
    s_stop = true;
}

} // namespace

bool BootRom(const std::string& rom_arg) {
    const std::string path = ResolveRomPath(rom_arg);
    if (path.empty()) {
        LOG_CRITICAL(Frontend, "No loadable ROM found. Place a .3ds/.cci/.cxi/.3dsx in {}roms/",
                     FileUtil::GetUserPath(FileUtil::UserPath::UserDir));
        return false;
    }

    LOG_INFO(Frontend, "Booting ROM {}", path);

    s_load_ok = false;
    s_artic_disconnected = false;
    s_paused = false;
    auto& system = Core::System::GetInstance();
    FileUtil::SetCurrentRomPath(path);

    // Register the loader early so the core reuses it during Load.
    auto app_loader = Loader::GetLoader(path);
    if (app_loader) {
        system.RegisterAppLoaderEarly(app_loader);
    }

    u64 settings_program_id = 0;
    if (app_loader) {
        app_loader->ReadProgramId(settings_program_id);
    }
    ApplyPerGameConfig(settings_program_id);

    system.ApplySettings();
    Settings::LogSettings();

    // The quick menu and the messages over the game take the menus' theme.
    VideoCore::OverlayTheme overlay_theme;
    ThemeOverlayColors(GetMenuTheme(), overlay_theme.accent, overlay_theme.panel, overlay_theme.scrim);
    VideoCore::SetOverlayTheme(overlay_theme);
    VideoCore::SetOverlayFade(1.0f);

    // Hand text input to Horizon's swkbd.
    Frontend::RegisterDefaultApplets(system);
    RegisterKeyboard(system);
    RegisterMovieCpuThrottle(system);
    system.RegisterSaveStatePrepare(
        [&system](bool loading) { PrepareStateOperation(system, loading); });

    // Transfer ownership of the window context from the main thread to the emulation thread.
    auto* window = GetEmuWindow();
    if (!window) {
        LOG_CRITICAL(Frontend, "No EmuWindow available");
        return false;
    }
    window->DoneCurrent();

    s_stop = false;
    s_emu_thread = std::thread(EmuThread, path);
    return true;
}

bool IsRunning() {
    return !s_stop;
}

void SetEmulationPaused(bool paused) {
    s_paused.store(paused, std::memory_order_relaxed);
}

bool IsEmulationPaused() {
    return s_paused.load(std::memory_order_relaxed);
}

void StepScreenLayout(int delta) {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn()) {
        return;
    }

    const int count = static_cast<int>(s_layout_presets.size());
    s_layout_index = static_cast<std::size_t>(((static_cast<int>(s_layout_index) + delta) % count +
                                               count) %
                                              count);
    ApplyCurrentLayout();
}

bool IsOverlayScreenLayout() {
    return Settings::values.layout_option.GetValue() == Settings::LayoutOption::OverlayScreen;
}

void StepOverlayScreenPosition(int delta) {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn()) {
        return;
    }

    const int count = static_cast<int>(s_overlay_position_names.size());
    const int current = static_cast<int>(Settings::values.overlay_screen_position.GetValue());
    Settings::values.overlay_screen_position =
        static_cast<Settings::SmallScreenPosition>(((current + delta) % count + count) % count);
    s_layout_update_pending.store(true, std::memory_order_release);
}

const char* OverlayScreenPositionName() {
    const auto position = static_cast<std::size_t>(
        Settings::values.overlay_screen_position.GetValue());
    return position < s_overlay_position_names.size() ? s_overlay_position_names[position] : "";
}

void StepOverlayScreenSize(int delta) {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn()) {
        return;
    }

    Settings::values.overlay_screen_size = static_cast<u16>(std::max(
        0, Settings::values.overlay_screen_size.GetValue() + delta * kOverlaySizeStep));
    s_layout_update_pending.store(true, std::memory_order_release);
}

int GetOverlayScreenSize() {
    return Settings::values.overlay_screen_size.GetValue();
}

void StepOverlayScreenOpacity(int delta) {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn()) {
        return;
    }

    Settings::values.overlay_screen_opacity = static_cast<u16>(std::max(
        0, Settings::values.overlay_screen_opacity.GetValue() + delta * kOverlayOpacityStep));
    s_layout_update_pending.store(true, std::memory_order_release);
}

int GetOverlayScreenOpacity() {
    return Settings::values.overlay_screen_opacity.GetValue();
}

void StepScreenGap(int delta) {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn()) {
        return;
    }

    Settings::values.screen_gap =
        std::max(0, Settings::values.screen_gap.GetValue() + delta * kScreenGapStep);
    s_layout_update_pending.store(true, std::memory_order_release);
}

int GetScreenGap() {
    return Settings::values.screen_gap.GetValue();
}

bool RequestLayoutUpdate() {
    if (!Core::System::GetInstance().IsPoweredOn()) {
        return false;
    }
    s_layout_update_pending.store(true, std::memory_order_release);
    return true;
}

void CycleScreenLayout() {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn()) {
        return;
    }

    const std::uint32_t mask = s_layout_cycle_mask.load(std::memory_order_relaxed);
    const int count = static_cast<int>(s_layout_presets.size());
    // Advance to the next preset that is enabled in R3's rotation.
    for (int step = 1; step <= count; ++step) {
        const int idx = (static_cast<int>(s_layout_index) + step) % count;
        if ((mask & (1u << idx)) != 0) {
            s_layout_index = static_cast<std::size_t>(idx);
            ApplyCurrentLayout();
            return;
        }
    }
}

void ToggleSwapScreens() {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn()) {
        return;
    }

    // The single-screen layouts ignore swap_screen since there's only one screen.
    switch (Settings::values.layout_option.GetValue()) {
    case Settings::LayoutOption::TopScreenOnly:
        Settings::values.layout_option = Settings::LayoutOption::BottomScreenOnly;
        break;
    case Settings::LayoutOption::BottomScreenOnly:
        Settings::values.layout_option = Settings::LayoutOption::TopScreenOnly;
        break;
    default:
        Settings::values.swap_screen = !Settings::values.swap_screen.GetValue();
        break;
    }

    SyncLayoutIndex();
    s_layout_update_pending.store(true, std::memory_order_release);
}

bool IsFullscreenStretchEnabled() {
    return Settings::values.screen_top_stretch.GetValue() &&
           Settings::values.screen_bottom_stretch.GetValue();
}

void SetFullscreenStretchEnabled(bool enabled) {
    Settings::values.screen_top_stretch = enabled;
    Settings::values.screen_bottom_stretch = enabled;
    if (Core::System::GetInstance().IsPoweredOn()) {
        s_layout_update_pending.store(true, std::memory_order_release);
    }
}

const char* CurrentScreenLayoutName() {
    return s_layout_presets[s_layout_index].name;
}

int GetScreenLayoutCount() {
    return static_cast<int>(s_layout_presets.size());
}

CustomScreenLayout DefaultCustomScreenLayout() {
    return {{0.0f, 0.1f, 0.75f}, {0.75f, 0.1f, 0.25f}};
}

CustomScreenLayout GetCustomScreenLayout() {
    std::scoped_lock lock{s_custom_layout_mutex};
    return s_custom_layout;
}

void SetCustomScreenLayout(const CustomScreenLayout& layout) {
    {
        std::scoped_lock lock{s_custom_layout_mutex};
        s_custom_layout = layout;
    }
    if (Core::System::GetInstance().IsPoweredOn() &&
        Settings::values.layout_option.GetValue() == Settings::LayoutOption::CustomLayout) {
        s_layout_update_pending.store(true, std::memory_order_release);
    }
}

void UseCustomScreenLayout() {
    s_layout_index = kCustomPresetIndex;
    ApplyCurrentLayout();
}

int GetScreenLayoutIndex() {
    return static_cast<int>(s_layout_index);
}

void SetScreenLayoutIndex(int index) {
    if (index < 0 || index >= static_cast<int>(s_layout_presets.size())) {
        return;
    }
    s_layout_index = static_cast<std::size_t>(index);
    ApplyCurrentLayout();
}

bool IsCustomScreenLayoutActive() {
    return Settings::values.layout_option.GetValue() == Settings::LayoutOption::CustomLayout;
}

bool GetStartInCustomLayout() {
    return s_start_in_custom_layout;
}

void SetStartInCustomLayout(bool enabled) {
    s_start_in_custom_layout = enabled;
}

void PrepareBootScreenLayout() {
    if (s_start_in_custom_layout) {
        s_layout_index = kCustomPresetIndex;
        const ScreenLayoutPreset& preset = s_layout_presets[s_layout_index];
        Settings::values.layout_option = preset.layout;
        Settings::values.swap_screen = preset.swap_screen;
        Settings::values.upright_screen = preset.upright_screen;
        Settings::values.upright_screen_flipped = preset.upright_flipped;
    }
}

void ApplyCustomLayoutPixels(unsigned w, unsigned h) {
    if (w == 0 || h == 0) {
        w = 1280;
        h = 720;
    }
    // Screens keep their own shape: 400x240 on top, 320x240 below.
    const auto place = [w, h](const CustomScreenRect& r, float aspect, Settings::Setting<u16>& sx,
                              Settings::Setting<u16>& sy, Settings::Setting<u16>& sw,
                              Settings::Setting<u16>& sh) {
        const float pw = std::clamp(r.w, 0.05f, 1.0f) * static_cast<float>(w);
        const float ph = std::min(pw * aspect, static_cast<float>(h));
        const float px = std::clamp(r.x * static_cast<float>(w), 0.0f, static_cast<float>(w) - pw);
        const float py = std::clamp(r.y * static_cast<float>(h), 0.0f, static_cast<float>(h) - ph);
        sx = static_cast<u16>(px + 0.5f);
        sy = static_cast<u16>(py + 0.5f);
        sw = static_cast<u16>(pw + 0.5f);
        sh = static_cast<u16>(ph + 0.5f);
    };
    const CustomScreenLayout layout = GetCustomScreenLayout();
    auto& v = Settings::values;
    place(layout.top, 240.0f / 400.0f, v.custom_top_x, v.custom_top_y, v.custom_top_width,
          v.custom_top_height);
    place(layout.bottom, 240.0f / 320.0f, v.custom_bottom_x, v.custom_bottom_y,
          v.custom_bottom_width, v.custom_bottom_height);
}

const char* GetScreenLayoutName(int index) {
    if (index < 0 || index >= static_cast<int>(s_layout_presets.size())) {
        return "";
    }
    return s_layout_presets[index].name;
}

std::uint32_t GetLayoutCycleMask() {
    return s_layout_cycle_mask.load(std::memory_order_relaxed);
}

void SetLayoutCycleMask(std::uint32_t mask) {
    const std::uint32_t all = (1u << s_layout_presets.size()) - 1;
    s_layout_cycle_mask.store(mask & all, std::memory_order_relaxed);
}

bool IsMovieThrottleEnabled() {
    return s_movie_throttle_enabled.load(std::memory_order_relaxed);
}

void SetMovieThrottleEnabled(bool enabled) {
    s_movie_throttle_enabled.store(enabled, std::memory_order_relaxed);
    auto& system = Core::System::GetInstance();
    if (!enabled) {
        RestoreMovieCpuClock(system);
    } else if (s_movie_playback_detected.load(std::memory_order_relaxed)) {
        ApplyMovieCpuThrottle(system);
    }
}

std::int32_t GetMovieThrottleClockPercentage() {
    return s_movie_throttle_clock_percentage.load(std::memory_order_relaxed);
}

void SetMovieThrottleClockPercentage(std::int32_t percentage) {
    percentage = std::clamp(percentage, kMovieThrottleClockMin, kMovieThrottleClockMax);
    s_movie_throttle_clock_percentage.store(percentage, std::memory_order_relaxed);
    if (s_movie_throttle_active.load(std::memory_order_relaxed)) {
        ApplyMovieCpuThrottle(Core::System::GetInstance());
    }
}

std::string SaveStateSlotName(unsigned int slot) {
    return slot == 0 ? "Quick" : fmt::format("Slot {}", slot);
}

std::string SaveStateSlotStatus(unsigned int slot) {
    u64 program_id = 0;
    u64 movie_id = 0;
    if (!CurrentSaveStateKey(program_id, movie_id)) {
        return {};
    }

    for (const auto& info : Core::ListSaveStates(program_id, movie_id)) {
        if (info.slot != slot) {
            continue;
        }
        const auto time = static_cast<std::time_t>(info.time);
        std::tm tm{};
        if (localtime_r(&time, &tm) == nullptr) {
            return "Used";
        }
        std::string status = fmt::format("{:02}/{:02} {:02}:{:02}", tm.tm_mday, tm.tm_mon + 1,
                                         tm.tm_hour, tm.tm_min);
        if (info.status == Core::SaveStateInfo::ValidationStatus::RevisionMismatch) {
            status += " (old)";
        }
        return status;
    }
    return {};
}

bool RequestSaveState(unsigned int slot) {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn() || slot >= Core::SaveStateSlotCount) {
        return false;
    }
    return system.SendSignal(Core::System::Signal::Save, slot);
}

bool RequestLoadState(unsigned int slot) {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn() || slot >= Core::SaveStateSlotCount) {
        return false;
    }
    return system.SendSignal(Core::System::Signal::Load, slot);
}

bool DeleteSaveState(unsigned int slot) {
    u64 program_id = 0;
    u64 movie_id = 0;
    if (!CurrentSaveStateKey(program_id, movie_id) || slot >= Core::SaveStateSlotCount) {
        return false;
    }
    const std::string path = Core::GetSaveStatePath(program_id, movie_id, slot);
    return FileUtil::Exists(path) && FileUtil::Delete(path);
}

bool LoadFailed() {
    return !s_load_ok;
}

std::string TakeHaltReason() {
    return std::exchange(s_halt_reason, std::string{});
}

bool ArticDisconnected() {
    return s_artic_disconnected;
}

void StopRom() {
    // Each step of closing a game is logged, so a log from a close that never reaches the
    // library shows which step it stopped at.
    LOG_INFO(Frontend, "Closing the game");
    if (!s_stop && s_emu_thread.joinable()) {
        // The picture fades to black before the renderer goes, rather than cutting. Paused or
        // not, the emulation thread keeps presenting frames meanwhile.
        VideoCore::FadeOverlayTo(1.0f, kFadeOutMs);
        std::this_thread::sleep_for(std::chrono::milliseconds(kFadeOutMs + 30));
    }
    s_stop = true;
    s_paused = false;
    CancelKeyboard();
    if (s_emu_thread.joinable()) {
        s_emu_thread.join();
    }
    LOG_INFO(Frontend, "Emulation thread stopped");
    // Tear the core down after the window context current.
    auto* window = GetEmuWindow();
    if (window) {
        window->MakeCurrent();
    }
    auto& system = Core::System::GetInstance();
    system.ResetMoviePlaybackState();
    if (system.IsPoweredOn()) {
        system.Shutdown();
    }
    LOG_INFO(Frontend, "Emulated system shut down");
    system.EjectCartridge();
    ClearPerGameConfig();
    VideoCore::SetOverlayFade(0.0f);
}

} // namespace SwitchFrontend

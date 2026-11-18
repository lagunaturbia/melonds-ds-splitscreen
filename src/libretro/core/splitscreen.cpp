// SPDX-License-Identifier: GPL-3.0-or-later
#include "splitscreen.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <CRC32.h>
#include <NDS.h>
#include <LocalMP.h>
#include <pntr.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <vfs/vfs_implementation.h>

#include "config/config.hpp"
#include "config/console.hpp"
#include "config/constants.hpp"
#include "config/parse.hpp"
#include "constants.hpp"
#include "core.hpp"
#include "environment.hpp"
#include "exceptions.hpp"
#include "embedded/melondsds_error_title_font.h"
#include "input/input.hpp"
#include "retro/file.hpp"
#include "retro/info.hpp"

using namespace MelonDsDs;

namespace {
constexpr auto HostPacketWait = std::chrono::microseconds(2000);
constexpr unsigned DismissSplashAfter = 90;
constexpr unsigned DismissSplashInterval = 60;
constexpr unsigned GuestBootTimeout = 600;
constexpr unsigned TouchFrames = 6;
constexpr unsigned StableMenuFrames = 8;

void ConfigureGuestFirmware(melonDS::Firmware& firmware, int year) noexcept {
    constexpr unsigned SettingsLost = 1u << 9;
    constexpr unsigned SettingsConfigured = 0xec00;
    constexpr std::array<unsigned, 12> DaysInMonth{31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const melonDS::Firmware::UserData defaults(0);
    auto profile = firmware.GetEffectiveUserData();
    // Keep the touchscreen calibration already applied by CreateConsole.
    profile.Version = defaults.Version;
    if (profile.BirthdayMonth < 1 || profile.BirthdayMonth > DaysInMonth.size() || profile.BirthdayDay < 1
        || profile.BirthdayDay > DaysInMonth[profile.BirthdayMonth - 1]) {
        profile.BirthdayMonth = defaults.BirthdayMonth;
        profile.BirthdayDay = defaults.BirthdayDay;
    }
    if (profile.NameLength == 0 || profile.NameLength > std::size(profile.Nickname)) {
        std::copy(std::begin(defaults.Nickname), std::end(defaults.Nickname), profile.Nickname);
        profile.NameLength = defaults.NameLength;
    }
    if (profile.MessageLength > std::size(profile.Message)) profile.MessageLength = 0;
    if (profile.FavoriteColor > 15) profile.FavoriteColor = defaults.FavoriteColor;
    if ((profile.Settings & 7u) > melonDS::Firmware::Language::Spanish) {
        profile.Settings = (profile.Settings & ~7u) | melonDS::Firmware::Language::English;
    }

    // GBATEK DS Firmware User Settings. Store a valid profile in both flash slots.
    profile.Settings = (profile.Settings & ~(SettingsLost | melonDS::Firmware::BootMenu::Autostart))
        | SettingsConfigured;
    profile.Year = static_cast<uint8_t>(year % 100);
    profile.RTCOffset = 0;
    profile.UpdateCounter = 0;
    profile.UpdateChecksum();
    auto& slots = firmware.GetUserData();
    slots[0] = profile;
    slots[1] = profile;
    slots[1].UpdateCounter = 1;
}

bool HasColorEdge(uint32_t a, uint32_t b) noexcept {
    for (unsigned shift : {0u, 8u, 16u}) {
        const int first = (a >> shift) & 0xff;
        const int second = (b >> shift) & 0xff;
        if (std::abs(first - second) >= 18) return true;
    }
    return false;
}

bool HasPanelEdge(const uint32_t* pixels, unsigned width, unsigned firstRow, unsigned lastRow) noexcept {
    // The original DS and DS Lite menus both have adjacent PictoChat and Download Play panels.
    // Their borders span both columns, regardless of menu language and profile color.
    for (unsigned y = firstRow; y <= lastRow; ++y) {
        unsigned left = 0, right = 0;
        for (unsigned x = 24; x < 104; x += 4) {
            left += HasColorEdge(pixels[(y - 1) * width + x], pixels[(y + 1) * width + x]);
            right += HasColorEdge(pixels[(y - 1) * width + x + 128], pixels[(y + 1) * width + x + 128]);
        }
        if (left >= 14 && right >= 14) return true;
    }
    return false;
}

bool ShowsFirmwareMenu(melonDS::NDS& console, unsigned width) noexcept {
    void* top = nullptr;
    void* bottom = nullptr;
    if (!console.GPU.GetFramebuffers(&top, &bottom) || !bottom) return false;
    const auto* pixels = static_cast<const uint32_t*>(bottom);
    return HasPanelEdge(pixels, width, 70, 85) && HasPanelEdge(pixels, width, 116, 130);
}

class GuestDownloadBoot {
public:
    void Start() noexcept {
        _frame = 0;
        _menuFrames = 0;
        _downloadTouchFrames = 0;
    }

    void Cancel() noexcept {
        _frame = GuestBootTimeout;
        _downloadTouchFrames = 0;
    }

    bool ApplyTouch(melonDS::NDS& console, unsigned width, unsigned playerNumber) noexcept {
        if (_frame < GuestBootTimeout) {
            const unsigned frame = _frame++;
            if (frame >= DismissSplashAfter && ShowsFirmwareMenu(console, width)) {
                if (++_menuFrames == StableMenuFrames) {
                    _downloadTouchFrames = TouchFrames;
                    _frame = GuestBootTimeout;
                    retro::info("DS Download Play P{}: opening the firmware search panel.", playerNumber);
                }
            } else {
                _menuFrames = 0;
                if (frame >= DismissSplashAfter && (frame - DismissSplashAfter) % DismissSplashInterval < TouchFrames) {
                    // Dismiss the safety screen in a corner outside the menu's buttons.
                    console.TouchScreen(6, 6);
                    return true;
                }
            }
            if (_frame == GuestBootTimeout && _downloadTouchFrames == 0) {
                retro::info("DS Download Play P{}: firmware menu not recognized; use manual touch input.", playerNumber);
            }
        }
        if (_downloadTouchFrames == 0) return false;

        --_downloadTouchFrames;
        console.TouchScreen(190, 100);
        return true;
    }

private:
    unsigned _frame = GuestBootTimeout;
    unsigned _menuFrames = 0;
    unsigned _downloadTouchFrames = 0;
};

int ReplaceSave(const char* temporaryPath, const char* savePath) noexcept {
#ifdef _WIN32
    // Older Windows frontends cannot replace an existing file through VFS.
    // The bundled implementation uses MoveFileExW for that operation.
    if (retro_vfs_file_rename_impl(temporaryPath, savePath) == 0) return 0;
#endif
    return filestream_rename(temporaryPath, savePath);
}
}

struct Splitscreen::Player {
    Player(Splitscreen& session, unsigned index)
        : session(session), index(index), worker([this] { Work(); }) {}

    ~Player() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        changed.notify_one();
        worker.join();
    }

    void QueueFrame() noexcept {
        {
            std::lock_guard lock(mutex);
            pending = true;
        }
        changed.notify_one();
    }

    void Work() noexcept {
        std::unique_lock lock(mutex);
        while (true) {
            changed.wait(lock, [this] { return stopping || pending; });
            if (stopping) break;
            lock.unlock();
            console->RunFrame();
            session.FrameFinished(index);
            lock.lock();
            pending = false;
            changed.notify_one();
        }
        melonDS::NDS::Current = nullptr;
    }

    void WaitForFrame() noexcept {
        std::unique_lock lock(mutex);
        changed.wait(lock, [this] { return !pending; });
    }

    void FlushSave(bool force) noexcept {
        if (!dirty) return;
        const auto now = std::chrono::steady_clock::now();
        if (!force && now < nextSave) return;
        nextSave = now + std::chrono::seconds(2);

        const auto size = console->GetNDSSaveLength();
        if (!size || savePath.empty()) return;
        auto file = retro::make_rfile(temporaryPath, RETRO_VFS_FILE_ACCESS_WRITE);
        if (!file) {
            retro::warn("Could not open the temporary multiplayer save: {}", temporaryPath);
            return;
        }
        const bool written = filestream_write(file.get(), console->GetNDSSave(), size) == size
            && filestream_flush(file.get()) == 0;
        const bool closed = filestream_close(file.release()) == 0;
        if (!written || !closed) {
            filestream_delete(temporaryPath.c_str());
            retro::warn("Could not write or close the temporary multiplayer save: {}", temporaryPath);
            return;
        }
        if (ReplaceSave(temporaryPath.c_str(), savePath.c_str()) != 0) {
            retro::warn("Could not replace the multiplayer save {}; latest data is in {}", savePath, temporaryPath);
            return;
        }
        dirty = false;
    }

    Splitscreen& session;
    unsigned index;
    std::unique_ptr<melonDS::NDS> console;
    JoypadState joypad;
    std::string savePath;
    std::string temporaryPath;
    std::string romName;
    std::chrono::steady_clock::time_point nextSave{};
    float cursorX = 128;
    float cursorY = 96;
    bool dirty = false;
    GuestDownloadBoot guestBoot;

    std::mutex mutex;
    std::condition_variable changed;
    bool stopping = false;
    bool pending = false;
    std::thread worker;
};

struct Splitscreen::PlayerMenu {
    static constexpr int Width = ScreenWidth * MinPlayers;
    static constexpr int Height = FrameHeight;
    static constexpr uint32_t Confirm = (1u << RETRO_DEVICE_ID_JOYPAD_A)
        | (1u << RETRO_DEVICE_ID_JOYPAD_START);

    std::unique_ptr<pntr_image, decltype(&pntr_unload_image)> canvas{
        pntr_new_image(Width, Height), &pntr_unload_image};
    std::unique_ptr<pntr_font, decltype(&pntr_unload_font)> title{
        pntr_load_font_ttf_from_memory(embedded_melondsds_error_title_font,
            sizeof(embedded_melondsds_error_title_font), 30), &pntr_unload_font};
    std::unique_ptr<pntr_font, decltype(&pntr_unload_font)> body{
        pntr_load_font_ttf_from_memory(embedded_melondsds_error_title_font,
            sizeof(embedded_melondsds_error_title_font), 15), &pntr_unload_font};
    // Require a fresh press when loading from a frontend controlled with a gamepad.
    uint32_t previousButtons = UINT32_MAX;

    PlayerMenu() {
        if (!canvas || !title || !body) throw std::bad_alloc();
    }

    void Draw(unsigned selected, bool downloadPlay) noexcept {
        const auto bg = pntr_new_color(12, 19, 29, 255);
        const auto card = pntr_new_color(28, 40, 55, 255);
        const auto selectedCard = pntr_new_color(113, 224, 210, 255);
        const auto ink = pntr_new_color(237, 244, 251, 255);
        const auto hint = pntr_new_color(170, 190, 205, 255);
        auto center = [&](pntr_font* font, const char* label, int x, int y, pntr_color color) {
            pntr_draw_text(canvas.get(), font, label, x - pntr_measure_text(font, label) / 2, y, color);
        };

        pntr_clear_background(canvas.get(), bg);
        center(title.get(), "Select Players", Width / 2, 84, ink);
        for (unsigned players = MinPlayers; players <= MaxPlayers; ++players) {
            const int x = 60 + static_cast<int>(players - MinPlayers) * 136;
            const bool active = players == selected;
            const char label[]{static_cast<char>('0' + players), 0};
            pntr_draw_rectangle_rounded_fill(canvas.get(), x, 156, 120, 80, 9,
                active ? selectedCard : card);
            center(title.get(), label, x + 60, 178, active ? bg : ink);
        }
        center(body.get(), downloadPlay ? "Download Play" : "Multi-Card", Width / 2, 252, ink);
        center(body.get(), "LEFT / RIGHT: select players", Width / 2, 288, hint);
        center(body.get(), "X: switch mode    A / START: play", Width / 2, 310, hint);
        retro::video_refresh(canvas->data, Width, Height, Width * sizeof(uint32_t));
    }
};

Splitscreen::Splitscreen(CoreState& core, const CoreConfig& coreConfig, const retro::GameInfo& game)
    : _core(core), _config(coreConfig), _game(game)
{
    const auto rom = game.GetData();
    if (rom.size() < sizeof(melonDS::NDSHeader)) {
        throw content_exception("The ROM is smaller than a Nintendo DS cartridge header.");
    }
    const auto& header = *reinterpret_cast<const melonDS::NDSHeader*>(rom.data());
    if (RequiresDSi(header)) {
        throw content_exception("This cartridge requires DSi mode. The splitscreen core supports DS mode.");
    }
    if (rom.size() > 512u * 1024 * 1024) {
        throw content_exception("This file exceeds the maximum Nintendo DS cartridge size.");
    }
    _playerCount = ParseIntegerInRange(retro::get_variable(config::splitscreen::PLAYER_COUNT),
        MinPlayers, MaxPlayers).value_or(MinPlayers);
    _downloadPlay = retro::get_variable(config::splitscreen::LINK_MODE) == config::splitscreen::DOWNLOAD_PLAY;
    _menu = std::make_unique<PlayerMenu>();
}

void Splitscreen::RunMenu() noexcept {
    const uint32_t buttons = retro::joypad_state(0);
    const uint32_t pressed = buttons & ~_menu->previousButtons;
    _menu->previousButtons = buttons;
    if ((pressed & (1u << RETRO_DEVICE_ID_JOYPAD_LEFT)) && _playerCount > MinPlayers) --_playerCount;
    if ((pressed & (1u << RETRO_DEVICE_ID_JOYPAD_RIGHT)) && _playerCount < MaxPlayers) ++_playerCount;
    if (pressed & (1u << RETRO_DEVICE_ID_JOYPAD_X)) _downloadPlay = !_downloadPlay;

    if (pressed & PlayerMenu::Confirm) {
        try {
            StartPlayers();
            _menuButtonsToRelease = buttons & PlayerMenu::Confirm;
            _menu.reset();
            _frameWidth = ScreenWidth * _playerCount;
            if (!retro::set_geometry(AvInfo().geometry)) {
                retro::warn("Frontend did not accept the splitscreen geometry update.");
            }
            return;
        } catch (const emulator_exception& error) {
            retro::set_error_message(error.user_message());
        } catch (const std::exception& error) {
            retro::set_error_message(error.what());
        }
        for (auto& player : _players) player.reset();
        _link.reset();
        melonDS::NDS::Current = nullptr;
    }

    _menu->Draw(_playerCount, _downloadPlay);
}

void Splitscreen::StartPlayers() {
    _link = std::make_unique<melonDS::LocalMP>();
    _link->SetRecvTimeout(0);
    CoreConfig config = _config;
    const auto rom = _game.GetData();
    config.SetConsoleMode(ConsoleMode::DS);
    config.SetConfiguredRenderer(RenderMode::Software);
    config.SetDldiEnable(false);
    config.SetDldiFolderSync(false);
    config.SetBootMode(BootMode::Direct);
    config.SetNetworkMode(NetworkMode::None);
#ifdef HAVE_JIT_FASTMEM
    config.SetFastMemory(false);
#endif
    config.SetSlot2Device(Slot2Device::Auto);
    if (_downloadPlay && config.SysfileMode() != SysfileMode::Native) {
        throw content_exception("Download Play requires native Nintendo DS BIOS files and bootable firmware.");
    }

    const auto crc = melonDS::CRC32(reinterpret_cast<const melonDS::u8*>(rom.data()), static_cast<int>(rom.size()));
    for (unsigned i = 0; i < _playerCount; ++i) {
        auto& player = _players[i];
        player = std::make_unique<Player>(*this, i);
        const bool hasCart = !_downloadPlay || i == 0;
        const retro::GameInfo* content = hasCart ? &_game : nullptr;
        player->console = CreateConsole(_core, config, content, nullptr, nullptr, player.get());
        if (!hasCart && player->console->NeedsDirectBoot()) {
            throw content_exception("Download Play requires native Nintendo DS BIOS files and bootable firmware.");
        }
        if (const char* basename = path_basename(_game.GetPath().data())) player->romName = basename;
        if (player->romName.empty()) player->romName = "game.nds";

        auto& firmware = player->console->GetFirmware();
        auto& mac = firmware.GetHeader().MacAddr;
        mac[0] = (mac[0] & 0xfc) | 0x02; // Locally administered unicast.
        // Keep the Alpha 1 MACs for the first two consoles.
        mac[5] = ((mac[5] & 0xfe) | (i & 1)) ^ (i & 2);
        firmware.UpdateChecksums();

        Boot(*player);

        const auto size = player->console->GetNDSSaveLength();
        if (!size) continue;
        auto path = retro::get_save_path(fmt::format("{}.{:08x}.splitscreen-p{}.srm", player->romName, crc, i + 1));
        if (!path) {
            throw content_exception("RetroArch did not provide a usable save directory.");
        }
        player->savePath = std::move(*path);
        player->temporaryPath = player->savePath + ".tmp";
        auto file = retro::make_rfile(player->savePath, RETRO_VFS_FILE_ACCESS_READ);
        if (!file) {
            if (filestream_exists(player->savePath.c_str())) {
                throw content_exception("An existing splitscreen save could not be opened.");
            }
            continue;
        }

        std::vector<uint8_t> savedata(size);
        if (filestream_get_size(file.get()) != size
            || filestream_read(file.get(), savedata.data(), size) != size) {
            throw content_exception("A splitscreen save could not be read or has the wrong size.");
        }
        player->console->SetNDSSave(savedata.data(), size);
        player->dirty = false;
    }
    retro::info("Started {} Nintendo DS consoles with an in-process wireless link.", _playerCount);
    if (_downloadPlay) retro::info("DS Download Play: player 1 has the cartridge; guests open the Download Play search.");
}

Splitscreen::~Splitscreen() {
    for (auto& player : _players) {
        if (player) {
            player->FlushSave(true);
            const auto& stats = _replyStats[player->index];
            if (stats.requests) {
                retro::info("DS local link P{} replies: {} requests, {} with all payloads, {} finished with missing payloads, {} interrupted",
                            player->index + 1, stats.requests, stats.complete, stats.answeredWithoutAllPayloads,
                            stats.peersUnavailable);
            }
        }
    }
    melonDS::NDS::Current = nullptr;
}

void Splitscreen::Boot(Player& player) {
    auto& console = *player.console;
    melonDS::NDS::Current = &console;
    console.Reset();
    if (_downloadPlay && player.index != 0) {
        _core.SetConsoleTime(console);
        int year, month, day, hour, minute, second;
        console.RTC.GetDateTime(year, month, day, hour, minute, second);
        ConfigureGuestFirmware(console.GetFirmware(), year);
        player.guestBoot.Start();
    } else if (console.GetNDSCart()) {
        console.SetupDirectBoot(player.romName);
    }
    console.Start();
    melonDS::NDS::Current = nullptr;
}

void Splitscreen::Reset() {
    if (_menu) return;
    auto newLink = std::make_unique<melonDS::LocalMP>();
    newLink->SetRecvTimeout(0);
    _packets.fill(0);
    _totalPackets = 0;
    for (unsigned i = 0; i < _playerCount; ++i) _players[i]->FlushSave(true);
    _link = std::move(newLink);
    for (unsigned i = 0; i < _playerCount; ++i) Boot(*_players[i]);
}

retro_system_av_info Splitscreen::AvInfo() const noexcept {
    return {{_frameWidth, FrameHeight, MaxFrameWidth, FrameHeight, float(_frameWidth) / FrameHeight},
            {FPS, SAMPLE_RATE}};
}

void Splitscreen::SetController(unsigned port, unsigned device) noexcept {
    if (port < _controllerEnabled.size()) _controllerEnabled[port] = device != RETRO_DEVICE_NONE;
}

void Splitscreen::SaveChanged(const void* userdata) noexcept {
    if (userdata) _players[PlayerIndex(userdata)]->dirty = true;
}

melonDS::LocalMP& Splitscreen::Link() noexcept {
    return *_link;
}

int Splitscreen::PlayerIndex(const void* userdata) const noexcept {
    // Only console and cartridge callbacks use this session. Both carry the
    // Player pointer provided when the console was created.
    return static_cast<const Player*>(userdata)->index;
}

void Splitscreen::NotifyPacket(int player) noexcept {
    {
        std::lock_guard lock(_linkMutex);
        ++_packets[player];
        ++_totalPackets;
    }
    _linkChanged.notify_all();
}

void Splitscreen::FrameFinished(unsigned index) noexcept {
    {
        std::lock_guard lock(_linkMutex);
        _running[index] = false;
    }
    _linkChanged.notify_all();
}

uint64_t Splitscreen::PacketCount(int player) noexcept {
    std::lock_guard lock(_linkMutex);
    return _totalPackets - _packets[player];
}

bool Splitscreen::WaitForPeers(int player, uint64_t previousCount,
                              std::optional<std::chrono::steady_clock::time_point> deadline) noexcept {
    std::unique_lock lock(_linkMutex);
    const auto receivedTraffic = [&] { return _totalPackets - _packets[player] != previousCount; };
    const auto peerCanSend = [&] {
        for (unsigned i = 0; i < _playerCount; ++i) {
            if (static_cast<int>(i) == player || !_running[i]) continue;
            // A peer with new traffic is runnable even before it leaves
            // the condition-variable wait. In particular, it may have
            // just received the CMD we are about to wait for a reply to.
            if (!_waitingForPacketCount[i] ||
                *_waitingForPacketCount[i] != _totalPackets - _packets[i]) return true;
        }
        return false;
    };
    if (receivedTraffic()) return true;
    if (!peerCanSend()) return false;

    // A peer at the frame boundary cannot send again until the next retro_run.
    // Wake other waiters as soon as all active consoles have blocked or finished.
    _waitingForPacketCount[player] = previousCount;
    _linkChanged.notify_all();
    const auto ready = [&] { return receivedTraffic() || !peerCanSend(); };
    if (deadline) _linkChanged.wait_until(lock, *deadline, ready);
    else _linkChanged.wait(lock, ready);
    _waitingForPacketCount[player].reset();
    return receivedTraffic();
}

int Splitscreen::ReceiveHostPacket(int player, uint8_t* data, uint64_t* timestamp) noexcept {
    const auto deadline = std::chrono::steady_clock::now() + HostPacketWait;
    while (true) {
        const auto count = PacketCount(player);
        const auto size = Link().RecvHostPacket(player, data, timestamp);
        if (size != 0 || std::chrono::steady_clock::now() >= deadline
            || !WaitForPeers(player, count, deadline)) {
            return size;
        }
    }
}

uint16_t Splitscreen::ReceiveReplies(int player, uint8_t* data, uint64_t timestamp, uint16_t mask) noexcept {
    // A CMD is already on the link. Let runnable peers finish processing it
    // even when the OS did not schedule their threads for a few milliseconds.
    auto& stats = _replyStats[player];
    ++stats.requests;
    melonDS::LocalMP::ReplyPoll replies;
    const auto recordFinished = [&] {
        if ((replies.Payloads & mask) == mask) ++stats.complete;
        else ++stats.answeredWithoutAllPayloads;
        return replies.Payloads;
    };
    while (true) {
        const auto count = PacketCount(player);
        if (Link().PollReplies(player, data, timestamp, mask, replies)) return recordFinished();
        if (!WaitForPeers(player, count, std::nullopt)) {
            // One final poll covers a reply published as the wait was ending.
            if (Link().PollReplies(player, data, timestamp, mask, replies)) return recordFinished();
            ++stats.peersUnavailable;
            return replies.Payloads;
        }
    }
}

void Splitscreen::UpdateInput(Player& player, unsigned index) noexcept {
    InputPollResult input{};
    if (_controllerEnabled[index]) {
        input.JoypadButtons = retro::joypad_state(index);
        if (index == 0) {
            _menuButtonsToRelease &= input.JoypadButtons;
            input.JoypadButtons &= ~_menuButtonsToRelease;
        }
        input.AnalogCursorDirection = retro::analog_state(index, RETRO_DEVICE_INDEX_ANALOG_RIGHT);
    }
    player.joypad.Update(input);
    player.joypad.Apply(*player.console);

    auto moveCursor = [](float position, int axis, float limit) {
        if (axis > -5500 && axis < 5500) return position;
        return std::clamp(position + (axis / 32768.0f) * 4.0f, 0.0f, limit);
    };
    player.cursorX = moveCursor(player.cursorX, input.AnalogCursorDirection.x, ScreenWidth - 1);
    player.cursorY = moveCursor(player.cursorY, input.AnalogCursorDirection.y, ScreenHeight - 1);

    constexpr auto TouchButton = 1u << RETRO_DEVICE_ID_JOYPAD_R2;
    if (input.JoypadButtons & (TouchButton | (1u << RETRO_DEVICE_ID_JOYPAD_A) | (1u << RETRO_DEVICE_ID_JOYPAD_B))) {
        player.guestBoot.Cancel();
    }
    if (player.guestBoot.ApplyTouch(*player.console, ScreenWidth, index + 1)) return;
    if (input.JoypadButtons & TouchButton) {
        player.console->TouchScreen(static_cast<uint16_t>(player.cursorX), static_cast<uint16_t>(player.cursorY));
    } else {
        player.console->ReleaseScreen();
    }
}

void Splitscreen::DrawPlayer(const Player& player, unsigned index) noexcept {
    void* top = nullptr;
    void* bottom = nullptr;
    if (!player.console->GPU.GetFramebuffers(&top, &bottom) || !top || !bottom) return;

    const auto* topPixels = static_cast<const uint32_t*>(top);
    const auto* bottomPixels = static_cast<const uint32_t*>(bottom);
    auto column = _frame.begin() + index * ScreenWidth;
    for (unsigned row = 0; row < ScreenHeight; ++row) {
        std::copy_n(topPixels + row * ScreenWidth, ScreenWidth, column + row * _frameWidth);
        std::copy_n(bottomPixels + row * ScreenWidth, ScreenWidth, column + (row + ScreenHeight) * _frameWidth);
    }

    const auto x = static_cast<unsigned>(player.cursorX);
    const auto y = static_cast<unsigned>(player.cursorY) + ScreenHeight;
    for (int offset = -3; offset <= 3; ++offset) {
        if (const int px = static_cast<int>(x) + offset; px >= 0 && px < static_cast<int>(ScreenWidth)) {
            column[y * _frameWidth + px] ^= 0x00ffffff;
        }
        if (offset != 0) {
            if (const int py = static_cast<int>(y) + offset;
                py >= static_cast<int>(ScreenHeight) && py < static_cast<int>(FrameHeight)) {
                column[py * _frameWidth + x] ^= 0x00ffffff;
            }
        }
    }
}

void Splitscreen::Run() noexcept {
    retro::input_poll();
    if (_menu) {
        RunMenu();
        return;
    }
    for (unsigned i = 0; i < _playerCount; ++i) UpdateInput(*_players[i], i);

    // Let all consoles run before waiting, so any peer can answer a Wi-Fi command.
    {
        std::lock_guard lock(_linkMutex);
        std::fill_n(_running.begin(), _playerCount, true);
    }
    for (unsigned i = 0; i < _playerCount; ++i) _players[i]->QueueFrame();
    for (unsigned i = 0; i < _playerCount; ++i) _players[i]->WaitForFrame();

    for (unsigned i = 0; i < _playerCount; ++i) {
        DrawPlayer(*_players[i], i);
        _players[i]->FlushSave(false);
    }
    retro::video_refresh(_frame.data(), _frameWidth, FrameHeight, _frameWidth * sizeof(uint32_t));

    // Play player 1 audio and drain the other consoles to keep their queues moving.
    std::array<int16_t, 4096> audio;
    auto& audio1 = _players[0]->console->SPU;
    for (unsigned i = 1; i < _playerCount; ++i) {
        auto& other = _players[i]->console->SPU;
        other.ReadOutput(audio.data(), std::min(other.GetOutputSize(), 2048));
    }
    const auto count = audio1.ReadOutput(audio.data(), std::min(audio1.GetOutputSize(), 2048));
    retro::audio_sample_batch(audio.data(), count);
}

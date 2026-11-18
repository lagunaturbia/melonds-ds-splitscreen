// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

#include <libretro.h>

namespace melonDS {
    class LocalMP;
}

namespace retro {
    class GameInfo;
}

namespace MelonDsDs {
    class CoreConfig;
    class CoreState;

    class Splitscreen {
    public:
        Splitscreen(CoreState& core, const CoreConfig& config, const retro::GameInfo& game);
        ~Splitscreen();

        Splitscreen(const Splitscreen&) = delete;
        Splitscreen& operator=(const Splitscreen&) = delete;

        retro_system_av_info AvInfo() const noexcept;
        void Run() noexcept;
        void Reset();
        void SetController(unsigned port, unsigned device) noexcept;
        void SaveChanged(const void* userdata) noexcept;
        melonDS::LocalMP& Link() noexcept;
        int PlayerIndex(const void* userdata) const noexcept;
        void NotifyPacket(int player) noexcept;
        int ReceiveHostPacket(int player, uint8_t* data, uint64_t* timestamp) noexcept;
        uint16_t ReceiveReplies(int player, uint8_t* data, uint64_t timestamp, uint16_t mask) noexcept;

    private:
        struct Player;
        struct PlayerMenu;
        struct ReplyStats {
            uint64_t requests = 0;
            uint64_t complete = 0;
            uint64_t answeredWithoutAllPayloads = 0;
            uint64_t peersUnavailable = 0;
        };
        void RunMenu() noexcept;
        void StartPlayers();
        void Boot(Player& player);
        void DrawPlayer(const Player& player, unsigned index) noexcept;
        void UpdateInput(Player& player, unsigned index) noexcept;
        void FrameFinished(unsigned index) noexcept;
        uint64_t PacketCount(int player) noexcept;
        bool WaitForPeers(int player, uint64_t previousCount,
                          std::optional<std::chrono::steady_clock::time_point> deadline) noexcept;

        static constexpr unsigned ScreenWidth = 256;
        static constexpr unsigned ScreenHeight = 192;
        static constexpr unsigned MinPlayers = 2;
        static constexpr unsigned MaxPlayers = 4;
        static constexpr unsigned MaxFrameWidth = ScreenWidth * MaxPlayers;
        static constexpr unsigned FrameHeight = ScreenHeight * 2;

        std::mutex _linkMutex;
        std::condition_variable _linkChanged;
        std::array<uint64_t, MaxPlayers> _packets{};
        std::array<bool, MaxPlayers> _running{};
        std::array<std::optional<uint64_t>, MaxPlayers> _waitingForPacketCount{};
        std::array<ReplyStats, MaxPlayers> _replyStats{};
        uint64_t _totalPackets = 0;
        std::unique_ptr<melonDS::LocalMP> _link;
        std::array<std::unique_ptr<Player>, MaxPlayers> _players;
        std::array<bool, MaxPlayers> _controllerEnabled{true, true, true, true};
        std::array<uint32_t, MaxFrameWidth * FrameHeight> _frame{};
        CoreState& _core;
        const CoreConfig& _config;
        const retro::GameInfo& _game;
        std::unique_ptr<PlayerMenu> _menu;
        unsigned _playerCount = MinPlayers;
        unsigned _frameWidth = ScreenWidth * MinPlayers;
        bool _downloadPlay = false;
        uint32_t _menuButtonsToRelease = 0;
    };
}

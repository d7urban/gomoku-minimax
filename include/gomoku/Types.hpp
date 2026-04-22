#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace gomoku {

enum class Player {
    None,
    Black,
    White,
};

enum class Ruleset {
    Freestyle15,
    Standard15,
};

enum class GameResult {
    Ongoing,
    Draw,
    BlackWin,
    WhiteWin,
};

enum class ControllerKind {
    Human,
    RookieAI,
    ClubAI,
    TacticalAI,
    ExpertAI,
};

enum class AiTimeControlPreset {
    FixedPerMove,
    Blitz,
    Fast,
    Slow,
};

enum class Seat {
    Opener,
    Chooser,
};

enum class SwapChoice {
    KeepColors,
    SwapColors,
};

struct Move {
    int row {-1};
    int col {-1};

    bool operator==(const Move&) const = default;
};

struct Action {
    enum class Kind {
        Move,
        SwapChoice,
    };

    Kind kind {Kind::Move};
    Move move {};
    SwapChoice swapChoice {SwapChoice::KeepColors};

    static Action makeMove(Move move);
    static Action makeSwapChoice(SwapChoice choice);
};

struct AiTimeControlSpec {
    std::int64_t periodTimeMs {0};
    int periodMoves {0};
};

inline Player otherPlayer(Player player) {
    switch (player) {
        case Player::Black:
            return Player::White;
        case Player::White:
            return Player::Black;
        case Player::None:
        default:
            return Player::None;
    }
}
char playerGlyph(Player player);

std::string_view toString(Player player);
std::string_view toString(Ruleset ruleset);
std::string_view toString(GameResult result);
std::string_view toString(ControllerKind controller);
std::string_view toString(AiTimeControlPreset preset);
std::string_view toString(Seat seat);
std::string_view toString(SwapChoice choice);
AiTimeControlSpec aiTimeControlSpec(AiTimeControlPreset preset);

std::string moveToString(Move move);
bool tryParseMove(std::string_view text, Move& move);
bool tryParseRuleset(std::string_view text, Ruleset& ruleset);
bool tryParseController(std::string_view text, ControllerKind& controller);
bool tryParseAiTimeControlPreset(std::string_view text, AiTimeControlPreset& preset);
bool tryParseSwapChoice(std::string_view text, SwapChoice& choice);

}  // namespace gomoku

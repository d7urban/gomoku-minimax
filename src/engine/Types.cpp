#include "gomoku/Types.hpp"

#include <algorithm>
#include <cctype>

namespace gomoku {

Action Action::makeMove(Move move) {
    Action action;
    action.kind = Kind::Move;
    action.move = move;
    return action;
}

Action Action::makeSwapChoice(SwapChoice choice) {
    Action action;
    action.kind = Kind::SwapChoice;
    action.swapChoice = choice;
    return action;
}


char playerGlyph(Player player) {
    switch (player) {
        case Player::Black:
            return 'X';
        case Player::White:
            return 'O';
        case Player::None:
        default:
            return '.';
    }
}

std::string_view toString(Player player) {
    switch (player) {
        case Player::Black:
            return "black";
        case Player::White:
            return "white";
        case Player::None:
        default:
            return "none";
    }
}

std::string_view toString(Ruleset ruleset) {
    switch (ruleset) {
        case Ruleset::Freestyle15:
            return "freestyle15";
        case Ruleset::Standard15:
            return "standard15";
        default:
            return "unknown";
    }
}

std::string_view toString(GameResult result) {
    switch (result) {
        case GameResult::Ongoing:
            return "ongoing";
        case GameResult::Draw:
            return "draw";
        case GameResult::BlackWin:
            return "black_win";
        case GameResult::WhiteWin:
            return "white_win";
        default:
            return "unknown";
    }
}

std::string_view toString(ControllerKind controller) {
    switch (controller) {
        case ControllerKind::Human:
            return "human";
        case ControllerKind::RookieAI:
            return "rookie";
        case ControllerKind::ClubAI:
            return "club";
        case ControllerKind::TacticalAI:
            return "tactical";
        case ControllerKind::ExpertAI:
            return "expert";
        default:
            return "unknown";
    }
}

std::string_view toString(AiTimeControlPreset preset) {
    switch (preset) {
        case AiTimeControlPreset::FixedPerMove:
            return "fixed";
        case AiTimeControlPreset::Blitz:
            return "blitz";
        case AiTimeControlPreset::Fast:
            return "fast";
        case AiTimeControlPreset::Slow:
            return "slow";
        default:
            return "unknown";
    }
}

std::string_view toString(Seat seat) {
    switch (seat) {
        case Seat::Opener:
            return "opener";
        case Seat::Chooser:
            return "chooser";
        default:
            return "unknown";
    }
}

std::string_view toString(SwapChoice choice) {
    switch (choice) {
        case SwapChoice::KeepColors:
            return "keep";
        case SwapChoice::SwapColors:
            return "swap";
        default:
            return "unknown";
    }
}

AiTimeControlSpec aiTimeControlSpec(AiTimeControlPreset preset) {
    switch (preset) {
        case AiTimeControlPreset::Blitz:
            return {.periodTimeMs = 5LL * 60LL * 1000LL, .periodMoves = 40};
        case AiTimeControlPreset::Fast:
            return {.periodTimeMs = 15LL * 60LL * 1000LL, .periodMoves = 60};
        case AiTimeControlPreset::Slow:
            return {.periodTimeMs = 30LL * 60LL * 1000LL, .periodMoves = 80};
        case AiTimeControlPreset::FixedPerMove:
        default:
            return {};
    }
}

std::string moveToString(Move move) {
    if (move.row < 0 || move.col < 0 || move.col >= 26) {
        return "??";
    }

    std::string text;
    text.push_back(static_cast<char>('a' + move.col));
    text += std::to_string(move.row + 1);
    return text;
}

bool tryParseMove(std::string_view text, Move& move) {
    if (text.size() < 2) {
        return false;
    }

    const char file = static_cast<char>(std::tolower(static_cast<unsigned char>(text.front())));
    if (file < 'a' || file > 'z') {
        return false;
    }

    int rank = 0;
    for (std::size_t i = 1; i < text.size(); ++i) {
        const unsigned char digit = static_cast<unsigned char>(text[i]);
        if (!std::isdigit(digit)) {
            return false;
        }
        rank = rank * 10 + static_cast<int>(digit - '0');
    }

    if (rank <= 0) {
        return false;
    }

    move.col = file - 'a';
    move.row = rank - 1;
    return true;
}

bool tryParseRuleset(std::string_view text, Ruleset& ruleset) {
    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (lowered == "freestyle15" || lowered == "freestyle") {
        ruleset = Ruleset::Freestyle15;
        return true;
    }
    if (lowered == "standard15" || lowered == "standard") {
        ruleset = Ruleset::Standard15;
        return true;
    }
    return false;
}

bool tryParseController(std::string_view text, ControllerKind& controller) {
    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (lowered == "human") {
        controller = ControllerKind::Human;
        return true;
    }
    if (lowered == "rookie") {
        controller = ControllerKind::RookieAI;
        return true;
    }
    if (lowered == "ai" || lowered == "club") {
        controller = ControllerKind::ClubAI;
        return true;
    }
    if (lowered == "tactical") {
        controller = ControllerKind::TacticalAI;
        return true;
    }
    if (lowered == "expert") {
        controller = ControllerKind::ExpertAI;
        return true;
    }
    if (lowered == "analyst") {
        controller = ControllerKind::ExpertAI;
        return true;
    }
    return false;
}

bool tryParseAiTimeControlPreset(std::string_view text, AiTimeControlPreset& preset) {
    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (lowered == "fixed" || lowered == "fixedpermove") {
        preset = AiTimeControlPreset::FixedPerMove;
        return true;
    }
    if (lowered == "blitz") {
        preset = AiTimeControlPreset::Blitz;
        return true;
    }
    if (lowered == "fast") {
        preset = AiTimeControlPreset::Fast;
        return true;
    }
    if (lowered == "slow") {
        preset = AiTimeControlPreset::Slow;
        return true;
    }
    return false;
}

bool tryParseSwapChoice(std::string_view text, SwapChoice& choice) {
    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (lowered == "keep") {
        choice = SwapChoice::KeepColors;
        return true;
    }
    if (lowered == "swap") {
        choice = SwapChoice::SwapColors;
        return true;
    }
    return false;
}

}  // namespace gomoku

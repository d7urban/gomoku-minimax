#include "gomoku/Replay.hpp"

#include <sstream>

namespace gomoku {

namespace {

constexpr int kAnnotatedPositionFormatVersion = 1;
constexpr int kMatchSessionFormatVersion = 1;

std::string serializeMoveList(const std::vector<Move>& moves) {
    std::ostringstream output;
    for (std::size_t index = 0; index < moves.size(); ++index) {
        if (index > 0) {
            output << ' ';
        }
        output << moveToString(moves[index]);
    }
    return output.str();
}

bool parseMoveList(std::istringstream& input, std::vector<Move>& moves, std::string& badToken) {
    moves.clear();
    std::string token;
    while (input >> token) {
        Move move;
        if (!tryParseMove(token, move)) {
            badToken = token;
            return false;
        }
        moves.push_back(move);
    }
    badToken.clear();
    return true;
}

}  // namespace

std::string serializeReplay(const GameState& state) {
    std::ostringstream output;
    output << "ruleset=" << toString(state.rules().ruleset) << '\n';

    for (const Action& action : state.actions()) {
        if (action.kind == Action::Kind::Move) {
            output << "move " << moveToString(action.move) << '\n';
        } else {
            output << "swap " << toString(action.swapChoice) << '\n';
        }
    }

    return output.str();
}

bool deserializeReplay(const RulesSpec& rules, std::string_view text, GameState& state, std::string& error) {
    state = GameState(rules);

    std::istringstream input {std::string(text)};
    std::string line;
    int lineNumber = 0;

    while (std::getline(input, line)) {
        ++lineNumber;
        if (line.empty()) {
            continue;
        }

        if (line.rfind("ruleset=", 0) == 0) {
            continue;
        }

        std::istringstream lineStream(line);
        std::string command;
        lineStream >> command;

        if (command == "move") {
            std::string moveText;
            lineStream >> moveText;

            Move move;
            if (!tryParseMove(moveText, move) || !state.applyMove(move)) {
                error = "Invalid move on replay line " + std::to_string(lineNumber) + ": " + moveText;
                return false;
            }
            continue;
        }

        if (command == "swap") {
            std::string choiceText;
            lineStream >> choiceText;

            SwapChoice choice;
            if (!tryParseSwapChoice(choiceText, choice) || !state.applySwapChoice(choice)) {
                error = "Invalid swap choice on replay line " + std::to_string(lineNumber) + ": " + choiceText;
                return false;
            }
            continue;
        }

        error = "Unknown replay command on line " + std::to_string(lineNumber) + ": " + command;
        return false;
    }

    error.clear();
    return true;
}

std::string serializeMatchSession(const Match& match) {
    std::ostringstream output;
    output << "session_format=" << kMatchSessionFormatVersion << '\n';
    output << "ruleset=" << toString(match.config().ruleset) << '\n';
    output << "opener=" << toString(match.config().openerController) << '\n';
    output << "chooser=" << toString(match.config().chooserController) << '\n';
    output << "ai_move_time_ms=" << match.config().aiMoveTimeMs << '\n';
    output << "ai_search_threads=" << match.config().searchThreads << '\n';
    output << "ai_time_control=" << toString(match.config().aiTimeControlPreset) << '\n';
    if (const auto openerClock = match.aiClockForSeat(Seat::Opener)) {
        output << "opener_clock " << openerClock->timeLeftMs << ' ' << openerClock->movesPlayedInPeriod << '\n';
    }
    if (const auto chooserClock = match.aiClockForSeat(Seat::Chooser)) {
        output << "chooser_clock " << chooserClock->timeLeftMs << ' ' << chooserClock->movesPlayedInPeriod << '\n';
    }

    for (const Action& action : match.state().actions()) {
        if (action.kind == Action::Kind::Move) {
            output << "move " << moveToString(action.move) << '\n';
        } else {
            output << "swap " << toString(action.swapChoice) << '\n';
        }
    }

    return output.str();
}

bool deserializeMatchSession(std::string_view text, Match& match, std::string& error) {
    std::vector<std::string> lines;
    std::istringstream input {std::string(text)};
    std::string line;

    MatchConfig config;
    config.aiTimeControlPreset = AiTimeControlPreset::Blitz;
    std::optional<std::pair<std::int64_t, int>> openerClock;
    std::optional<std::pair<std::int64_t, int>> chooserClock;

    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        lines.push_back(line);

        if (line.rfind("session_format=", 0) == 0) {
            std::istringstream value(line.substr(15));
            int version = 0;
            if (!(value >> version)) {
                error = "Invalid match-session format version: " + line.substr(15);
                return false;
            }
            if (version > kMatchSessionFormatVersion) {
                error = "Unsupported match-session format version: " + std::to_string(version);
                return false;
            }
            continue;
        }
        if (line.rfind("ruleset=", 0) == 0) {
            if (!tryParseRuleset(line.substr(8), config.ruleset)) {
                error = "Invalid ruleset in match session: " + line.substr(8);
                return false;
            }
            continue;
        }
        if (line.rfind("opener=", 0) == 0) {
            if (!tryParseController(line.substr(7), config.openerController)) {
                error = "Invalid opener controller in match session: " + line.substr(7);
                return false;
            }
            continue;
        }
        if (line.rfind("chooser=", 0) == 0) {
            if (!tryParseController(line.substr(8), config.chooserController)) {
                error = "Invalid chooser controller in match session: " + line.substr(8);
                return false;
            }
            continue;
        }
        if (line.rfind("ai_move_time_ms=", 0) == 0) {
            std::istringstream value(line.substr(16));
            if (!(value >> config.aiMoveTimeMs) || config.aiMoveTimeMs <= 0) {
                error = "Invalid ai_move_time_ms in match session: " + line.substr(16);
                return false;
            }
            continue;
        }
        if (line.rfind("ai_search_threads=", 0) == 0) {
            std::istringstream value(line.substr(18));
            if (!(value >> config.searchThreads) || config.searchThreads < 0) {
                error = "Invalid ai_search_threads in match session: " + line.substr(18);
                return false;
            }
            continue;
        }
        if (line.rfind("ai_time_control=", 0) == 0) {
            if (!tryParseAiTimeControlPreset(line.substr(16), config.aiTimeControlPreset)) {
                error = "Invalid ai_time_control in match session: " + line.substr(16);
                return false;
            }
            continue;
        }
        if (line.rfind("opener_clock ", 0) == 0) {
            std::istringstream value(line.substr(13));
            std::int64_t timeLeftMs = -1;
            int movesPlayed = 0;
            if (!(value >> timeLeftMs >> movesPlayed)) {
                error = "Invalid opener_clock line in match session";
                return false;
            }
            openerClock = std::pair<std::int64_t, int> {timeLeftMs, movesPlayed};
            continue;
        }
        if (line.rfind("chooser_clock ", 0) == 0) {
            std::istringstream value(line.substr(14));
            std::int64_t timeLeftMs = -1;
            int movesPlayed = 0;
            if (!(value >> timeLeftMs >> movesPlayed)) {
                error = "Invalid chooser_clock line in match session";
                return false;
            }
            chooserClock = std::pair<std::int64_t, int> {timeLeftMs, movesPlayed};
            continue;
        }
    }

    Match loaded(config);
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const std::string& current = lines[index];
        if (current.empty()
            || current.rfind("session_format=", 0) == 0
            || current.rfind("ruleset=", 0) == 0
            || current.rfind("opener=", 0) == 0
            || current.rfind("chooser=", 0) == 0
            || current.rfind("ai_move_time_ms=", 0) == 0
            || current.rfind("ai_search_threads=", 0) == 0
            || current.rfind("ai_time_control=", 0) == 0
            || current.rfind("opener_clock ", 0) == 0
            || current.rfind("chooser_clock ", 0) == 0) {
            continue;
        }

        const int lineNumber = static_cast<int>(index + 1);
        std::istringstream lineStream(current);
        std::string command;
        lineStream >> command;

        if (command == "move") {
            std::string moveText;
            lineStream >> moveText;
            Move move;
            if (!tryParseMove(moveText, move) || !loaded.applyMove(move)) {
                error = "Invalid move in match session on line " + std::to_string(lineNumber) + ": " + moveText;
                return false;
            }
            continue;
        }

        if (command == "swap") {
            std::string choiceText;
            lineStream >> choiceText;
            SwapChoice choice;
            if (!tryParseSwapChoice(choiceText, choice) || !loaded.applySwapChoice(choice)) {
                error = "Invalid swap choice in match session on line " + std::to_string(lineNumber) + ": " + choiceText;
                return false;
            }
            continue;
        }

        error = "Unknown match-session command on line " + std::to_string(lineNumber) + ": " + command;
        return false;
    }

    if (openerClock.has_value()) {
        loaded.setAiClockState(Seat::Opener, openerClock->first, openerClock->second);
    }
    if (chooserClock.has_value()) {
        loaded.setAiClockState(Seat::Chooser, chooserClock->first, chooserClock->second);
    }
    loaded.clearUndoHistory();
    match = std::move(loaded);
    error.clear();
    return true;
}

std::string serializeAnnotatedPosition(const GameState& state, const PositionAnnotation& annotation) {
    std::ostringstream output;
    output << "format=" << kAnnotatedPositionFormatVersion << '\n';
    output << serializeReplay(state);
    if (annotation.analysisPlayer != Player::None) {
        output << "analysis_player=" << toString(annotation.analysisPlayer) << '\n';
    }
    if (!annotation.label.empty()) {
        output << "label " << annotation.label << '\n';
    }
    if (!annotation.principalVariation.empty()) {
        output << "pv " << serializeMoveList(annotation.principalVariation) << '\n';
    }
    return output.str();
}

bool deserializeAnnotatedPosition(std::string_view text, GameState& state, PositionAnnotation& annotation, std::string& error) {
    annotation = PositionAnnotation {};

    std::vector<std::string> lines;
    std::istringstream input {std::string(text)};
    std::string line;
    Ruleset ruleset = Ruleset::Freestyle15;

    while (std::getline(input, line)) {
        if (line.rfind("format=", 0) == 0) {
            std::istringstream value(line.substr(7));
            int version = 0;
            if (!(value >> version)) {
                error = "Invalid annotated-position format version: " + line.substr(7);
                return false;
            }
            if (version > kAnnotatedPositionFormatVersion) {
                error = "Unsupported annotated-position format version: " + std::to_string(version);
                return false;
            }
        }
        if (line.rfind("ruleset=", 0) == 0) {
            Ruleset parsed;
            if (!tryParseRuleset(line.substr(8), parsed)) {
                error = "Invalid ruleset in annotated position: " + line.substr(8);
                return false;
            }
            ruleset = parsed;
        }
        lines.push_back(line);
    }

    state = GameState(rulesFor(ruleset));
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const std::string& current = lines[index];
        const int lineNumber = static_cast<int>(index + 1);
        if (current.empty() || current.rfind("format=", 0) == 0 || current.rfind("ruleset=", 0) == 0) {
            continue;
        }
        if (current.rfind("analysis_player=", 0) == 0) {
            Player player = Player::None;
            const std::string value = current.substr(16);
            if (value == "black") {
                player = Player::Black;
            } else if (value == "white") {
                player = Player::White;
            } else if (value != "none") {
                error = "Invalid analysis player on line " + std::to_string(lineNumber) + ": " + value;
                return false;
            }
            annotation.analysisPlayer = player;
            continue;
        }
        if (current.rfind("label ", 0) == 0) {
            annotation.label = current.substr(6);
            continue;
        }

        std::istringstream lineStream(current);
        std::string command;
        lineStream >> command;

        if (command == "pv") {
            std::vector<Move> moves;
            std::string badToken;
            if (!parseMoveList(lineStream, moves, badToken)) {
                error = "Invalid move token on line " + std::to_string(lineNumber) + ": " + badToken;
                return false;
            }
            annotation.principalVariation = std::move(moves);
            continue;
        }

        if (command == "move") {
            std::string moveText;
            lineStream >> moveText;

            Move move;
            if (!tryParseMove(moveText, move) || !state.applyMove(move)) {
                error = "Invalid move on replay line " + std::to_string(lineNumber) + ": " + moveText;
                return false;
            }
            continue;
        }

        if (command == "swap") {
            std::string choiceText;
            lineStream >> choiceText;

            SwapChoice choice;
            if (!tryParseSwapChoice(choiceText, choice) || !state.applySwapChoice(choice)) {
                error = "Invalid swap choice on replay line " + std::to_string(lineNumber) + ": " + choiceText;
                return false;
            }
            continue;
        }

        error = "Unknown annotated-position command on line " + std::to_string(lineNumber) + ": " + command;
        return false;
    }

    error.clear();
    return true;
}

}  // namespace gomoku

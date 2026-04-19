// Gomocup-protocol adapter for the gomoku-minimax engine.
// Speaks the harness subset documented in
// ../../gomoku-harness/docs/protocol.md over stdin/stdout.
//
// Wire format: (X, Y) zero-indexed, X = column, Y = row, origin top-left.
// gomoku::Move uses (row, col); we map identically (wire X <-> col,
// wire Y <-> row). The internal display orientation differs from the wire
// orientation, but that is purely cosmetic -- gomoku is symmetric under
// reflection, so the engine plays equivalently in either frame.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "gomoku/Match.hpp"

namespace {

constexpr int kBoardSize = 15;
constexpr const char* kEngineName = "gomoku-minimax";
constexpr const char* kEngineVersion = "0.1.0-gomocup";
constexpr const char* kEngineAuthor = "gomoku-minimax";
constexpr const char* kEngineCountry = "—";

void writeLine(const std::string& line) {
    std::cout << line << '\n';
    std::cout.flush();
}

void logLine(const std::string& message) {
    std::cerr << "[minimax] " << message << '\n';
}

std::string toUpper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

struct EngineConfig {
    gomoku::ControllerKind controller {gomoku::ControllerKind::ExpertAI};
    int aiMoveTimeMs {500};
};

bool isInBounds(int x, int y) {
    return x >= 0 && x < kBoardSize && y >= 0 && y < kBoardSize;
}

bool byRowThenCol(const std::pair<int, int>& left, const std::pair<int, int>& right) {
    if (left.second != right.second) return left.second < right.second;
    return left.first < right.first;
}

class Engine {
public:
    explicit Engine(EngineConfig config) : config_(config) { rebuildMatch(); }

    void cmdStart(int size) {
        if (size != kBoardSize) {
            writeLine("ERROR unsupported size " + std::to_string(size));
            return;
        }
        rebuildMatch();
        writeLine("OK");
    }

    void cmdRestart() {
        rebuildMatch();
        writeLine("OK");
    }

    void cmdAbout() {
        std::ostringstream oss;
        oss << "name=\"" << kEngineName << "\""
            << ", version=\"" << kEngineVersion << "\""
            << ", author=\"" << kEngineAuthor << "\""
            << ", country=\"" << kEngineCountry << "\""
            << ", controller=\"" << gomoku::toString(config_.controller) << "\""
            << ", time_ms=" << config_.aiMoveTimeMs;
        writeLine(oss.str());
    }

    void cmdInfo(const std::string& key, const std::string& value) {
        if (key == "timeout_turn") {
            try {
                int ms = std::stoi(value);
                if (ms > 0) {
                    config_.aiMoveTimeMs = ms;
                    match_->setAiMoveTimeMs(ms);
                }
            } catch (const std::exception&) {
                // malformed value: silently ignore (per spec)
            }
        }
        // Other keys silently ignored.
    }

    void cmdBegin() { replyWithMove(); }

    void cmdTurn(int x, int y) {
        if (!applyOpponentMove(x, y)) return;
        replyWithMove();
    }

    void cmdBoard(const std::vector<std::tuple<int, int, int>>& cells) {
        std::vector<std::pair<int, int>> mine;
        std::vector<std::pair<int, int>> opp;
        std::vector<bool> occupied(static_cast<std::size_t>(kBoardSize * kBoardSize), false);
        for (const auto& cell : cells) {
            int x = std::get<0>(cell);
            int y = std::get<1>(cell);
            int field = std::get<2>(cell);
            if (!isInBounds(x, y)) {
                writeLine("ERROR BOARD cell out of bounds: " + std::to_string(x) + "," + std::to_string(y));
                return;
            }
            if (field != 1 && field != 2 && field != 3) {
                writeLine("ERROR BOARD field must be 1, 2, or 3: " + std::to_string(field));
                return;
            }
            if (field == 3) {
                continue;
            }
            const std::size_t index = static_cast<std::size_t>(y * kBoardSize + x);
            if (occupied[index]) {
                writeLine("ERROR duplicate BOARD cell: " + std::to_string(x) + "," + std::to_string(y));
                return;
            }
            occupied[index] = true;
            if (field == 1) mine.emplace_back(x, y);
            else opp.emplace_back(x, y);
        }

        bool brainIsBlack;
        if (mine.size() == opp.size()) brainIsBlack = true;
        else if (opp.size() == mine.size() + 1) brainIsBlack = false;
        else {
            writeLine("ERROR malformed BOARD: my=" + std::to_string(mine.size()) +
                      " opp=" + std::to_string(opp.size()));
            return;
        }

        rebuildMatch();
        auto blackList = brainIsBlack ? mine : opp;
        auto whiteList = brainIsBlack ? opp : mine;
        std::sort(blackList.begin(), blackList.end(), byRowThenCol);
        std::sort(whiteList.begin(), whiteList.end(), byRowThenCol);
        // Replay alternating: black, white, black, white, ...
        for (std::size_t i = 0; i < whiteList.size(); ++i) {
            if (!match_->applyMove(makeMove(blackList[i].first, blackList[i].second))) {
                writeLine("ERROR illegal black setup move");
                return;
            }
            if (!match_->applyMove(makeMove(whiteList[i].first, whiteList[i].second))) {
                writeLine("ERROR illegal white setup move");
                return;
            }
        }
        if (!brainIsBlack) {
            // black has one extra trailing stone (we are white, to move next)
            if (!match_->applyMove(makeMove(blackList.back().first, blackList.back().second))) {
                writeLine("ERROR illegal final black setup move");
                return;
            }
        }
        replyWithMove();
    }

private:
    static gomoku::Move makeMove(int x, int y) {
        return gomoku::Move{y, x};  // wire (X, Y) -> internal (row=y, col=x)
    }

    void rebuildMatch() {
        gomoku::MatchConfig mc;
        mc.ruleset = gomoku::Ruleset::Freestyle15;
        mc.openerController = config_.controller;
        mc.chooserController = config_.controller;
        mc.aiMoveTimeMs = config_.aiMoveTimeMs;
        match_.emplace(mc);
    }

    bool applyOpponentMove(int x, int y) {
        if (x < 0 || x >= kBoardSize || y < 0 || y >= kBoardSize) {
            writeLine("ERROR opponent move out of bounds: " + std::to_string(x) + "," + std::to_string(y));
            return false;
        }
        if (!match_->applyMove(makeMove(x, y))) {
            writeLine("ERROR opponent move rejected: " + std::to_string(x) + "," + std::to_string(y));
            return false;
        }
        return true;
    }

    void replyWithMove() {
        if (match_->state().isGameOver()) {
            writeLine("ERROR game over, cannot reply");
            return;
        }
        match_->stepAi();
        const auto last = match_->state().lastPlacedMove();
        if (!last) {
            writeLine("ERROR engine produced no move");
            return;
        }
        std::ostringstream oss;
        oss << last->col << "," << last->row;
        const auto& summary = match_->lastSearchSummary();
        if (summary) {
            logLine("move=" + std::to_string(last->col) + "," + std::to_string(last->row) +
                    " depth=" + std::to_string(summary->depthReached) +
                    " score=" + std::to_string(summary->score) +
                    " nodes=" + std::to_string(summary->nodes) +
                    " t=" + std::to_string(summary->elapsedMs) + "ms");
        }
        writeLine(oss.str());
    }

    EngineConfig config_;
    std::optional<gomoku::Match> match_;
};

std::vector<std::tuple<int, int, int>> readBoardBlock() {
    std::vector<std::tuple<int, int, int>> cells;
    std::string line;
    while (std::getline(std::cin, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (toUpper(line) == "DONE") return cells;
        std::stringstream ss(line);
        int x = 0;
        int y = 0;
        int field = 0;
        char comma1 = 0;
        char comma2 = 0;
        if ((ss >> x >> comma1 >> y >> comma2 >> field) && comma1 == ',' && comma2 == ',') {
            cells.emplace_back(x, y, field);
        }
    }
    return cells;
}

void printUsage() {
    std::cerr << "Usage: gomoku_gomocup [--controller rookie|club|tactical|expert|analyst] "
                 "[--time-ms N]\n";
}

}  // namespace

int main(int argc, char** argv) {
    EngineConfig config;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--controller" || arg == "-c") && i + 1 < argc) {
            gomoku::ControllerKind ctrl;
            const char* nextArg = argv[++i];
            if (!gomoku::tryParseController(nextArg, ctrl)) {
                std::cerr << "Unknown controller: " << nextArg << '\n';
                return 1;
            }
            config.controller = ctrl;
        } else if ((arg == "--time-ms" || arg == "-t") && i + 1 < argc) {
            config.aiMoveTimeMs = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            printUsage();
            return 1;
        }
    }

    Engine engine(config);
    logLine("ready (controller=" + std::string(gomoku::toString(config.controller)) +
            ", time_ms=" + std::to_string(config.aiMoveTimeMs) + ")");

    std::string line;
    while (std::getline(std::cin, line)) {
        line = trim(line);
        if (line.empty()) continue;

        std::string head;
        std::string rest;
        const auto sp = line.find(' ');
        if (sp == std::string::npos) {
            head = line;
        } else {
            head = line.substr(0, sp);
            rest = line.substr(sp + 1);
        }
        const std::string cmd = toUpper(head);

        try {
            if (cmd == "START") {
                engine.cmdStart(std::stoi(rest));
            } else if (cmd == "RESTART") {
                engine.cmdRestart();
            } else if (cmd == "BEGIN") {
                engine.cmdBegin();
            } else if (cmd == "TURN") {
                const auto comma = rest.find(',');
                if (comma == std::string::npos) {
                    writeLine("ERROR malformed TURN: " + rest);
                    continue;
                }
                int x = std::stoi(rest.substr(0, comma));
                int y = std::stoi(rest.substr(comma + 1));
                engine.cmdTurn(x, y);
            } else if (cmd == "BOARD") {
                auto cells = readBoardBlock();
                engine.cmdBoard(cells);
            } else if (cmd == "INFO") {
                const auto sp2 = rest.find(' ');
                if (sp2 == std::string::npos) engine.cmdInfo(rest, "");
                else engine.cmdInfo(rest.substr(0, sp2), rest.substr(sp2 + 1));
            } else if (cmd == "ABOUT") {
                engine.cmdAbout();
            } else if (cmd == "END") {
                return 0;
            } else {
                writeLine("UNKNOWN " + line);
            }
        } catch (const std::exception& exc) {
            writeLine(std::string("ERROR ") + cmd + ": " + exc.what());
            logLine(std::string("exception handling ") + cmd + ": " + exc.what());
        }
    }

    return 0;
}

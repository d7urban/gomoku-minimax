// Cross-process tournament harness.
//
// Spawns two gomocup-protocol binaries as child processes and arbitrates
// Freestyle15 matches between them. Each binary speaks the subset defined
// in src/tools/gomoku_gomocup.cpp: wire moves are `x,y` (col,row), START
// replies with OK, BEGIN/TURN reply with a single move line, END exits.
//
// Usage:
//   gomoku_tournament [options] <engineA> <engineB>
//     -n, --games N          Number of games to play (default 2, split by color).
//     -t, --turn-ms MS       Per-move soft time limit sent via INFO (default 1000).
//     -r, --read-ms MS       Hard wait cap per reply before declaring timeout
//                            (default: turn-ms + 5000).
//     -a, --name-a NAME      Display name for engineA (default: engineA path).
//     -b, --name-b NAME      Display name for engineB (default: engineB path).
//     --args-a "ARGS"        Extra CLI args passed to engineA, space-separated.
//     --args-b "ARGS"        Extra CLI args passed to engineB.
//     -v, --verbose          Print every move as it is relayed.
//
// The arbiter owns the authoritative GameState. It validates every move
// against gomoku::GameState::isLegalMove; an illegal move loses the game
// on the spot. Engines that time out or emit ERROR also forfeit. The
// adjudicator does not itself know gomoku tactics beyond the rule engine.

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "gomoku/GameState.hpp"
#include "gomoku/Rules.hpp"
#include "gomoku/Types.hpp"

namespace {

constexpr int kBoardSize = 15;

struct EngineSpec {
    std::string path;
    std::string name;
    std::vector<std::string> extraArgs;
};

struct EngineProcess {
    pid_t pid {-1};
    int writeFd {-1};
    int readFd {-1};
    std::string readBuffer;
    std::string name;
    bool alive {false};
    // Per-game clock state. Meaningful only when the enclosing GameConfig
    // enables global-clock mode; otherwise left at 0 / ignored.
    int64_t remainingMs {0};
};

std::vector<std::string> splitArgs(const std::string& raw) {
    std::vector<std::string> out;
    std::string current;
    for (char c : raw) {
        if (c == ' ' || c == '\t') {
            if (!current.empty()) {
                out.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        out.push_back(current);
    }
    return out;
}

[[noreturn]] void childExec(const EngineSpec& spec, int toChildRead, int fromChildWrite) {
    if (dup2(toChildRead, STDIN_FILENO) < 0) std::_Exit(127);
    if (dup2(fromChildWrite, STDOUT_FILENO) < 0) std::_Exit(127);
    // Leave stderr attached to our stderr so engine logs interleave visibly.
    close(toChildRead);
    close(fromChildWrite);

    std::vector<std::string> argv;
    argv.push_back(spec.path);
    for (const auto& a : spec.extraArgs) argv.push_back(a);

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (auto& s : argv) cargv.push_back(s.data());
    cargv.push_back(nullptr);

    execvp(cargv[0], cargv.data());
    std::fprintf(stderr, "tournament: execvp %s failed: %s\n", spec.path.c_str(), std::strerror(errno));
    std::_Exit(127);
}

bool spawnEngine(const EngineSpec& spec, EngineProcess& out) {
    int toChild[2];
    int fromChild[2];
    if (pipe(toChild) < 0) return false;
    if (pipe(fromChild) < 0) {
        close(toChild[0]);
        close(toChild[1]);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(toChild[0]); close(toChild[1]);
        close(fromChild[0]); close(fromChild[1]);
        return false;
    }
    if (pid == 0) {
        close(toChild[1]);
        close(fromChild[0]);
        childExec(spec, toChild[0], fromChild[1]);
    }

    close(toChild[0]);
    close(fromChild[1]);
    out.pid = pid;
    out.writeFd = toChild[1];
    out.readFd = fromChild[0];
    out.name = spec.name;
    out.alive = true;
    return true;
}

bool sendLine(EngineProcess& engine, const std::string& line) {
    std::string payload = line + "\n";
    const char* data = payload.data();
    std::size_t remaining = payload.size();
    while (remaining > 0) {
        ssize_t written = write(engine.writeFd, data, remaining);
        if (written < 0) {
            if (errno == EINTR) continue;
            engine.alive = false;
            return false;
        }
        data += written;
        remaining -= static_cast<std::size_t>(written);
    }
    return true;
}

// Reads a single line (without newline) within timeoutMs. Returns
// std::nullopt on timeout/EOF/error. Buffers leftover bytes for later.
std::optional<std::string> readLine(EngineProcess& engine, int timeoutMs) {
    const auto start = std::chrono::steady_clock::now();
    while (true) {
        const auto newlinePos = engine.readBuffer.find('\n');
        if (newlinePos != std::string::npos) {
            std::string line = engine.readBuffer.substr(0, newlinePos);
            engine.readBuffer.erase(0, newlinePos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        const int remaining = timeoutMs - static_cast<int>(elapsedMs);
        if (remaining <= 0) return std::nullopt;

        pollfd pfd{};
        pfd.fd = engine.readFd;
        pfd.events = POLLIN;
        const int rv = poll(&pfd, 1, remaining);
        if (rv < 0) {
            if (errno == EINTR) continue;
            return std::nullopt;
        }
        if (rv == 0) return std::nullopt;
        if ((pfd.revents & POLLIN) == 0 && (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            // Drain any last bytes then EOF.
        }

        std::array<char, 4096> buf{};
        const ssize_t got = read(engine.readFd, buf.data(), buf.size());
        if (got < 0) {
            if (errno == EINTR) continue;
            return std::nullopt;
        }
        if (got == 0) {
            engine.alive = false;
            return std::nullopt;
        }
        engine.readBuffer.append(buf.data(), static_cast<std::size_t>(got));
    }
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

std::string toUpper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool waitForOk(EngineProcess& engine, int timeoutMs) {
    while (true) {
        auto line = readLine(engine, timeoutMs);
        if (!line) return false;
        const std::string trimmed = trim(*line);
        if (trimmed.empty()) continue;
        const std::string upper = toUpper(trimmed);
        if (upper == "OK") return true;
        if (upper.rfind("ERROR", 0) == 0) return false;
        // Non-response chatter (UNKNOWN etc.): keep reading briefly.
    }
}

enum class MoveOutcome {
    Ok,
    Timeout,
    EngineError,
    Malformed,
};

struct MoveReply {
    MoveOutcome outcome {MoveOutcome::Ok};
    gomoku::Move move {};
    std::string message;
};

bool parseMoveLine(const std::string& line, gomoku::Move& out) {
    const auto comma = line.find(',');
    if (comma == std::string::npos) return false;
    try {
        int x = std::stoi(line.substr(0, comma));
        int y = std::stoi(line.substr(comma + 1));
        if (x < 0 || x >= kBoardSize || y < 0 || y >= kBoardSize) return false;
        out = gomoku::Move{y, x};  // wire (x,y) -> (row=y, col=x)
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

MoveReply readMoveReply(EngineProcess& engine, int timeoutMs) {
    MoveReply reply;
    auto line = readLine(engine, timeoutMs);
    if (!line) {
        reply.outcome = MoveOutcome::Timeout;
        return reply;
    }
    const std::string trimmed = trim(*line);
    if (trimmed.empty()) {
        reply.outcome = MoveOutcome::Malformed;
        reply.message = "empty reply";
        return reply;
    }
    const std::string upper = toUpper(trimmed);
    if (upper.rfind("ERROR", 0) == 0) {
        reply.outcome = MoveOutcome::EngineError;
        reply.message = trimmed;
        return reply;
    }
    if (parseMoveLine(trimmed, reply.move)) {
        reply.outcome = MoveOutcome::Ok;
        return reply;
    }
    reply.outcome = MoveOutcome::Malformed;
    reply.message = trimmed;
    return reply;
}

void shutdownEngine(EngineProcess& engine) {
    if (engine.pid <= 0) return;
    if (engine.alive) {
        sendLine(engine, "END");
    }
    if (engine.writeFd >= 0) { close(engine.writeFd); engine.writeFd = -1; }

    // Give it a moment to exit on its own.
    for (int i = 0; i < 20; ++i) {
        int status = 0;
        const pid_t rv = waitpid(engine.pid, &status, WNOHANG);
        if (rv == engine.pid || rv < 0) {
            engine.pid = -1;
            if (engine.readFd >= 0) { close(engine.readFd); engine.readFd = -1; }
            return;
        }
        usleep(50'000);  // 50 ms
    }

    kill(engine.pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        int status = 0;
        const pid_t rv = waitpid(engine.pid, &status, WNOHANG);
        if (rv == engine.pid || rv < 0) break;
        usleep(50'000);
    }
    kill(engine.pid, SIGKILL);
    int status = 0;
    waitpid(engine.pid, &status, 0);
    engine.pid = -1;
    if (engine.readFd >= 0) { close(engine.readFd); engine.readFd = -1; }
}

enum class GameOutcome {
    BlackWin,
    WhiteWin,
    Draw,
    BlackForfeit,   // Black engine misbehaved -> White wins
    WhiteForfeit,
};

const char* outcomeLabel(GameOutcome o) {
    switch (o) {
        case GameOutcome::BlackWin: return "B-wins";
        case GameOutcome::WhiteWin: return "W-wins";
        case GameOutcome::Draw:     return "draw";
        case GameOutcome::BlackForfeit: return "B-forfeit";
        case GameOutcome::WhiteForfeit: return "W-forfeit";
    }
    return "?";
}

struct GameRecord {
    GameOutcome outcome;
    int plies;
    std::string note;
};

struct GameConfig {
    int turnMs {0};         // 0 disables per-turn cap
    int matchMs {0};        // 0 disables global-clock mode
    int readSlackMs {5000}; // harness grace beyond engine's remaining budget
    bool verbose {false};
};

// Read one move reply while tracking wall-clock elapsed. outElapsedMs is
// always set, even on timeout — the caller needs it to decrement clocks
// and to distinguish clock-exhaustion from other failures.
MoveReply readMoveReplyTimed(EngineProcess& engine, int timeoutMs, int64_t& outElapsedMs) {
    const auto start = std::chrono::steady_clock::now();
    MoveReply reply = readMoveReply(engine, timeoutMs);
    const auto end = std::chrono::steady_clock::now();
    outElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    return reply;
}

GameRecord runGame(EngineProcess& black, EngineProcess& white, const GameConfig& cfg) {
    using namespace gomoku;
    GameState state(rulesFor(Ruleset::Freestyle15));

    const bool globalClock = cfg.matchMs > 0;
    black.remainingMs = globalClock ? cfg.matchMs : 0;
    white.remainingMs = globalClock ? cfg.matchMs : 0;

    const std::string startCmd = "START " + std::to_string(kBoardSize);

    // Boot sequence: START, wait OK, then announce the time control. INFO
    // keys we don't care about are silently dropped by gomocup-compliant
    // engines, so it is safe to always advertise both caps.
    const int bootReadMs = cfg.readSlackMs + (globalClock ? 2000 : (cfg.turnMs > 0 ? cfg.turnMs : 2000));
    auto boot = [&](EngineProcess& e) -> bool {
        if (!sendLine(e, startCmd)) return false;
        if (!waitForOk(e, bootReadMs)) return false;
        if (cfg.turnMs > 0) {
            sendLine(e, "INFO timeout_turn " + std::to_string(cfg.turnMs));
        }
        if (globalClock) {
            sendLine(e, "INFO timeout_match " + std::to_string(cfg.matchMs));
        }
        return true;
    };
    if (!boot(black)) return {GameOutcome::BlackForfeit, 0, "black failed START"};
    if (!boot(white)) return {GameOutcome::WhiteForfeit, 0, "white failed START"};

    EngineProcess* toMove = &black;
    EngineProcess* other = &white;
    Player movingPlayer = Player::Black;
    int plies = 0;

    auto forfeitOf = [&](Player player, const std::string& msg) -> GameRecord {
        return {
            player == Player::Black ? GameOutcome::BlackForfeit : GameOutcome::WhiteForfeit,
            plies,
            msg,
        };
    };

    // Send the "your move" prompt (BEGIN for ply 1, TURN x,y after).
    auto promptMove = [&](EngineProcess& engine, const std::optional<Move>& lastMove) -> bool {
        if (globalClock) {
            if (!sendLine(engine, "INFO time_left " + std::to_string(engine.remainingMs))) return false;
        }
        if (!lastMove) {
            return sendLine(engine, "BEGIN");
        }
        return sendLine(engine,
            "TURN " + std::to_string(lastMove->col) + "," + std::to_string(lastMove->row));
    };

    std::optional<Move> lastMove;  // what the previous side played, relayed to the other
    if (!promptMove(*toMove, lastMove)) {
        return forfeitOf(movingPlayer, toMove->name + " prompt write failed");
    }

    while (true) {
        // Per-reply read budget: in global-clock mode, wait up to the
        // engine's remaining game time plus slack; otherwise fall back to
        // the per-turn cap (or a fixed default) plus slack.
        const int waitMs = globalClock
            ? static_cast<int>(std::min<int64_t>(
                  std::numeric_limits<int>::max() / 2,
                  toMove->remainingMs + cfg.readSlackMs))
            : ((cfg.turnMs > 0 ? cfg.turnMs : 2000) + cfg.readSlackMs);

        int64_t elapsedMs = 0;
        const MoveReply reply = readMoveReplyTimed(*toMove, waitMs, elapsedMs);

        if (globalClock) {
            toMove->remainingMs -= elapsedMs;
            if (toMove->remainingMs <= 0 && reply.outcome != MoveOutcome::Ok) {
                // Ran out of clock before producing a legal reply.
                return forfeitOf(movingPlayer,
                    toMove->name + " flagged on time (elapsed=" + std::to_string(elapsedMs) + "ms)");
            }
        }

        if (reply.outcome == MoveOutcome::Timeout) {
            return forfeitOf(movingPlayer,
                "timeout waiting for " + toMove->name + " (elapsed=" + std::to_string(elapsedMs) + "ms)");
        }
        if (reply.outcome == MoveOutcome::EngineError) {
            return forfeitOf(movingPlayer, toMove->name + " error: " + reply.message);
        }
        if (reply.outcome == MoveOutcome::Malformed) {
            return forfeitOf(movingPlayer, toMove->name + " malformed reply: " + reply.message);
        }
        const Move move = reply.move;
        if (!state.isLegalMove(move)) {
            return forfeitOf(movingPlayer,
                toMove->name + " illegal move " + std::to_string(move.col) + "," + std::to_string(move.row));
        }
        if (!state.applyMove(move)) {
            return forfeitOf(movingPlayer,
                toMove->name + " move rejected " + std::to_string(move.col) + "," + std::to_string(move.row));
        }
        ++plies;
        if (cfg.verbose) {
            std::cerr << "  ply " << plies << " "
                      << (movingPlayer == Player::Black ? "B" : "W")
                      << "=" << toMove->name
                      << " " << move.col << "," << move.row
                      << " (t=" << elapsedMs << "ms";
            if (globalClock) std::cerr << " left=" << toMove->remainingMs << "ms";
            std::cerr << ")\n";
        }

        if (state.isGameOver()) {
            switch (state.result()) {
                case GameResult::BlackWin: return {GameOutcome::BlackWin, plies, ""};
                case GameResult::WhiteWin: return {GameOutcome::WhiteWin, plies, ""};
                case GameResult::Draw:     return {GameOutcome::Draw, plies, ""};
                default: break;
            }
        }

        lastMove = move;
        std::swap(toMove, other);
        movingPlayer = otherPlayer(movingPlayer);

        if (!promptMove(*toMove, lastMove)) {
            return forfeitOf(movingPlayer, toMove->name + " prompt write failed");
        }
    }
}

struct Tally {
    int games {0};
    int wins {0};
    int losses {0};
    int draws {0};
    int forfeitsFor {0};      // games where *this* engine forfeited
    int forfeitsAgainst {0};  // games where opponent forfeited
};

void recordResult(const EngineSpec& /*a*/, const EngineSpec& /*b*/,
                  bool aPlaysBlack, GameOutcome outcome, Tally& aTally, Tally& bTally) {
    ++aTally.games;
    ++bTally.games;
    const bool aWin = (aPlaysBlack && outcome == GameOutcome::BlackWin) ||
                      (!aPlaysBlack && outcome == GameOutcome::WhiteWin);
    const bool bWin = (aPlaysBlack && outcome == GameOutcome::WhiteWin) ||
                      (!aPlaysBlack && outcome == GameOutcome::BlackWin);
    const bool aForfeit = (aPlaysBlack && outcome == GameOutcome::BlackForfeit) ||
                          (!aPlaysBlack && outcome == GameOutcome::WhiteForfeit);
    const bool bForfeit = (aPlaysBlack && outcome == GameOutcome::WhiteForfeit) ||
                          (!aPlaysBlack && outcome == GameOutcome::BlackForfeit);
    if (outcome == GameOutcome::Draw) {
        ++aTally.draws; ++bTally.draws;
    } else if (aWin) {
        ++aTally.wins; ++bTally.losses;
    } else if (bWin) {
        ++bTally.wins; ++aTally.losses;
    } else if (aForfeit) {
        ++aTally.forfeitsFor; ++aTally.losses;
        ++bTally.forfeitsAgainst; ++bTally.wins;
    } else if (bForfeit) {
        ++bTally.forfeitsFor; ++bTally.losses;
        ++aTally.forfeitsAgainst; ++aTally.wins;
    }
}

double score(const Tally& t) {
    return t.wins + 0.5 * t.draws;
}

void printUsage() {
    std::cerr <<
        "Usage: gomoku_tournament [options] <engineA> <engineB>\n"
        "  -n, --games N          Games to play (default 2).\n"
        "  -t, --turn-ms MS       Per-move cap sent as INFO timeout_turn (default 1000,\n"
        "                         set 0 to omit). Also caps per-reply wait when no\n"
        "                         match clock is configured.\n"
        "  -m, --match-ms MS      Per-game total clock per engine. Enables global-clock\n"
        "                         mode: harness tracks remaining time and sends\n"
        "                         INFO time_left before each BEGIN/TURN. Default 0 (off).\n"
        "  -r, --read-slack-ms MS Grace period beyond the engine's remaining budget the\n"
        "                         harness will still wait for a reply (default 5000).\n"
        "  -a, --name-a NAME      Display name for engineA.\n"
        "  -b, --name-b NAME      Display name for engineB.\n"
        "      --args-a \"ARGS\"    Extra CLI args for engineA (space-separated).\n"
        "      --args-b \"ARGS\"    Extra CLI args for engineB.\n"
        "  -v, --verbose          Log every relayed move.\n";
}

}  // namespace

int main(int argc, char** argv) {
    EngineSpec a;
    EngineSpec b;
    int games = 2;
    int turnMs = 1000;
    int matchMs = 0;
    int readSlackMs = 5000;
    bool verbose = false;
    std::string rawArgsA;
    std::string rawArgsB;

    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "-n" || arg == "--games") {
            games = std::max(1, std::atoi(need("--games")));
        } else if (arg == "-t" || arg == "--turn-ms") {
            turnMs = std::max(0, std::atoi(need("--turn-ms")));
        } else if (arg == "-m" || arg == "--match-ms") {
            matchMs = std::max(0, std::atoi(need("--match-ms")));
        } else if (arg == "-r" || arg == "--read-slack-ms") {
            readSlackMs = std::max(0, std::atoi(need("--read-slack-ms")));
        } else if (arg == "-a" || arg == "--name-a") {
            a.name = need("--name-a");
        } else if (arg == "-b" || arg == "--name-b") {
            b.name = need("--name-b");
        } else if (arg == "--args-a") {
            rawArgsA = need("--args-a");
        } else if (arg == "--args-b") {
            rawArgsB = need("--args-b");
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage();
            return 2;
        } else {
            positional.push_back(arg);
        }
    }

    if (positional.size() != 2) {
        printUsage();
        return 2;
    }
    a.path = positional[0];
    b.path = positional[1];
    if (a.name.empty()) a.name = a.path;
    if (b.name.empty()) b.name = b.path;
    a.extraArgs = splitArgs(rawArgsA);
    b.extraArgs = splitArgs(rawArgsB);

    GameConfig gameCfg;
    gameCfg.turnMs = turnMs;
    gameCfg.matchMs = matchMs;
    gameCfg.readSlackMs = readSlackMs;
    gameCfg.verbose = verbose;

    std::cerr << "tournament: " << a.name << " vs " << b.name
              << "  games=" << games
              << "  turn=" << turnMs << "ms"
              << "  match=" << matchMs << "ms"
              << "  slack=" << readSlackMs << "ms\n";

    Tally tallyA;
    Tally tallyB;

    for (int g = 0; g < games; ++g) {
        const bool aPlaysBlack = (g % 2 == 0);
        const EngineSpec& blackSpec = aPlaysBlack ? a : b;
        const EngineSpec& whiteSpec = aPlaysBlack ? b : a;

        EngineProcess blackProc;
        EngineProcess whiteProc;
        if (!spawnEngine(blackSpec, blackProc)) {
            std::cerr << "failed to spawn " << blackSpec.path << "\n";
            return 3;
        }
        if (!spawnEngine(whiteSpec, whiteProc)) {
            std::cerr << "failed to spawn " << whiteSpec.path << "\n";
            shutdownEngine(blackProc);
            return 3;
        }

        std::cerr << "game " << (g + 1) << "/" << games << ": B=" << blackProc.name
                  << " W=" << whiteProc.name << "\n";
        const GameRecord record = runGame(blackProc, whiteProc, gameCfg);
        std::cerr << "  -> " << outcomeLabel(record.outcome) << " in "
                  << record.plies << " plies";
        if (!record.note.empty()) std::cerr << "  (" << record.note << ")";
        std::cerr << "\n";

        recordResult(a, b, aPlaysBlack, record.outcome, tallyA, tallyB);

        shutdownEngine(blackProc);
        shutdownEngine(whiteProc);
    }

    auto printTally = [](const std::string& label, const Tally& t) {
        std::cout << label
                  << "  games=" << t.games
                  << "  score=" << score(t)
                  << "  W-L-D=" << t.wins << "-" << t.losses << "-" << t.draws
                  << "  forfeits(self/opp)=" << t.forfeitsFor << "/" << t.forfeitsAgainst
                  << "\n";
    };
    std::cout << "\n== results ==\n";
    printTally(a.name, tallyA);
    printTally(b.name, tallyB);

    return 0;
}

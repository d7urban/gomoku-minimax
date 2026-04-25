#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <SFML/Graphics.hpp>

#include "gomoku/Match.hpp"
#include "gomoku/OpeningBook.hpp"
#include "gomoku/Replay.hpp"
#include "gomoku/ThreatSearch.hpp"
#include "gomoku/Threats.hpp"

namespace {

using gomoku::CandidateMove;
using gomoku::ControllerKind;
using gomoku::AiTimeControlPreset;
using gomoku::Match;
using gomoku::MatchConfig;
using gomoku::MatchAiClockView;
using gomoku::Move;
using gomoku::Ruleset;
using gomoku::SwapChoice;
using gomoku::ThreatSearchResult;
using gomoku::ThreatStep;
using gomoku::ThreatType;

constexpr std::array<ControllerKind, 6> kControllerCycle = {
    ControllerKind::Human,
    ControllerKind::RookieAI,
    ControllerKind::ClubAI,
    ControllerKind::TacticalAI,
    ControllerKind::ExpertAI,
};

constexpr std::array<AiTimeControlPreset, 3> kTimeControlChoices = {
    AiTimeControlPreset::Blitz,
    AiTimeControlPreset::Fast,
    AiTimeControlPreset::Slow,
};

bool isAiController(ControllerKind controller) {
    return controller != ControllerKind::Human;
}

bool usesSearchEngine(ControllerKind controller) {
    return controller == ControllerKind::ClubAI
        || controller == ControllerKind::TacticalAI
        || controller == ControllerKind::ExpertAI;
}

ControllerKind cycleController(ControllerKind controller, int delta) {
    auto found = std::find(kControllerCycle.begin(), kControllerCycle.end(), controller);
    const int index = found == kControllerCycle.end() ? 0 : static_cast<int>(std::distance(kControllerCycle.begin(), found));
    const int size = static_cast<int>(kControllerCycle.size());
    const int nextIndex = (index + delta + size) % size;
    return kControllerCycle[static_cast<std::size_t>(nextIndex)];
}

AiTimeControlPreset cycleTimeControl(AiTimeControlPreset current, int delta) {
    auto found = std::find(kTimeControlChoices.begin(), kTimeControlChoices.end(), current);
    const int index = found == kTimeControlChoices.end() ? 0 : static_cast<int>(std::distance(kTimeControlChoices.begin(), found));
    const int size = static_cast<int>(kTimeControlChoices.size());
    const int nextIndex = (index + delta + size) % size;
    return kTimeControlChoices[static_cast<std::size_t>(nextIndex)];
}

std::string formatClockMillis(std::int64_t timeMs) {
    if (timeMs < 0) {
        return "-";
    }
    const std::int64_t totalSeconds = timeMs / 1000;
    const std::int64_t minutes = totalSeconds / 60;
    const std::int64_t seconds = totalSeconds % 60;
    std::string text = std::to_string(minutes);
    text += ':';
    if (seconds < 10) {
        text += '0';
    }
    text += std::to_string(seconds);
    return text;
}

std::string formatTimeControl(AiTimeControlPreset preset) {
    const gomoku::AiTimeControlSpec spec = gomoku::aiTimeControlSpec(preset);
    if (spec.periodTimeMs <= 0 || spec.periodMoves <= 0) {
        return std::string(gomoku::toString(preset));
    }
    return std::string(gomoku::toString(preset)) + " " + formatClockMillis(spec.periodTimeMs) + "/"
        + std::to_string(spec.periodMoves);
}

std::string formatSeatClock(const MatchAiClockView& clock) {
    return formatClockMillis(clock.timeLeftMs) + " (" + std::to_string(clock.movesToReset) + " to reset)";
}

struct OverlayState {
    bool showHeatmap {true};
    bool showThreatLabels {true};
    bool showTopCandidates {true};
};

struct AnalysisOverlay {
    int boardSize {0};
    int actionCount {-1};
    bool swapPending {false};
    gomoku::GameResult result {gomoku::GameResult::Ongoing};
    gomoku::Player sideToMove {gomoku::Player::None};
    int staticEval {0};
    int maxCellScore {0};
    std::vector<CandidateMove> topCandidates;
};

struct ThreatAnalysisView {
    bool active {false};
    int actionCount {-1};
    gomoku::Player sideToMove {gomoku::Player::None};
    std::optional<ThreatSearchResult> result;
    std::size_t stepIndex {0};
};

struct UiControlState {
    bool autoplayEnabled {false};
    std::string feedbackText;
    sf::Clock feedbackClock;
};

struct AiSearchState {
    bool running {false};
    bool finished {false};
    bool moveSearch {false};
    int actionCount {-1};
    gomoku::Player sideToMove {gomoku::Player::None};
    std::thread worker;
    std::mutex mutex;
    std::optional<gomoku::SearchSummary> liveSummary;
    std::optional<gomoku::SearchResult> completedResult;
    std::optional<SwapChoice> completedSwapChoice;
    std::int64_t elapsedMs {0};
};

struct StatusLine {
    std::string text;
    bool bold {false};
};

struct UiLayout {
    float boardLeft {24.0f};
    float boardTop {24.0f};
    float cell {40.0f};
    sf::FloatRect statusPanel {820.0f, 56.0f, 250.0f, 790.0f};
    sf::FloatRect candidatePanel {1100.0f, 56.0f, 250.0f, 790.0f};
    float panelPadding {14.0f};
    unsigned statusCharacterSize {21U};
    unsigned candidateCharacterSize {16U};
};

constexpr int kFeedbackDurationMs = 2400;
constexpr std::string_view kSavedGameFilename = "gomoku_saved_game.txt";
bool bothSeatsAi(const Match& match) {
    return isAiController(match.config().openerController) && isAiController(match.config().chooserController);
}

bool autoplayActive(const Match& match, const UiControlState& controls) {
    return bothSeatsAi(match) && controls.autoplayEnabled;
}

bool feedbackVisible(const UiControlState& controls) {
    return !controls.feedbackText.empty() && controls.feedbackClock.getElapsedTime().asMilliseconds() < kFeedbackDurationMs;
}

void setFeedback(UiControlState& controls, std::string text) {
    controls.feedbackText = std::move(text);
    controls.feedbackClock.restart();
}

void finishAiSearchWorker(AiSearchState& state) {
    if (state.worker.joinable()) {
        state.worker.join();
    }
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.liveSummary.reset();
        state.completedResult.reset();
        state.completedSwapChoice.reset();
        state.elapsedMs = 0;
    }
    state.running = false;
    state.finished = false;
    state.moveSearch = false;
}

std::optional<gomoku::SearchSummary> currentLiveSummary(AiSearchState& state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.liveSummary;
}

void startAiSearch(const Match& match, AiSearchState& state) {
    finishAiSearchWorker(state);
    state.running = true;
    state.finished = false;
    state.moveSearch = !match.state().isSwapDecisionPending();
    state.actionCount = match.state().actionCount();
    state.sideToMove = match.state().sideToMove();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.liveSummary.reset();
        state.completedResult.reset();
        state.completedSwapChoice.reset();
        state.elapsedMs = 0;
    }

    Match snapshot = match;
    state.worker = std::thread([snapshot, &state]() mutable {
        const auto start = std::chrono::steady_clock::now();
        if (snapshot.state().isSwapDecisionPending()) {
            const SwapChoice choice = snapshot.chooseAiSwapChoice();
            const auto end = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(state.mutex);
            state.completedSwapChoice = choice;
            state.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            state.finished = true;
            return;
        }

        const gomoku::SearchResult result = snapshot.searchAiTurn([&state](const gomoku::SearchSummary& summary) {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.liveSummary = summary;
        });
        const auto end = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.completedResult = result;
        state.liveSummary = result.summary;
        state.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        state.finished = true;
    });
}

std::filesystem::path savedGamePath() {
    return std::filesystem::current_path() / std::string(kSavedGameFilename);
}

std::unique_ptr<sf::Font> loadUiFont() {
    static const std::array<const char*, 4> kCandidates = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
    };

    for (const char* path : kCandidates) {
        if (!std::filesystem::exists(path)) {
            continue;
        }
        auto font = std::make_unique<sf::Font>();
        if (font->loadFromFile(path)) {
            return font;
        }
    }

    return nullptr;
}

sf::Vector2f boardPoint(const Match& match, Move move, float left, float top, float cell) {
    return {
        left + cell * static_cast<float>(move.col),
        top + cell * static_cast<float>(match.state().boardSize() - 1 - move.row),
    };
}

std::string boardColumnLabel(int col) {
    const std::string label = gomoku::moveToString({0, col});
    return label.empty() ? "?" : std::string(1, label.front());
}

std::string boardRowLabel(int row) {
    const std::string label = gomoku::moveToString({row, 0});
    return label.size() <= 1 ? "?" : label.substr(1);
}

void centerTextOrigin(sf::Text& text) {
    const sf::FloatRect bounds = text.getLocalBounds();
    text.setOrigin(bounds.left + bounds.width * 0.5f, bounds.top + bounds.height * 0.5f);
}

std::optional<Move> pickMoveFromMouse(const Match& match, sf::Vector2i mouse, float left, float top, float cell) {
    const int boardSize = match.state().boardSize();
    const float boardPixels = cell * static_cast<float>(boardSize - 1);
    if (mouse.x < left - cell * 0.5f || mouse.y < top - cell * 0.5f || mouse.x > left + boardPixels + cell * 0.5f
        || mouse.y > top + boardPixels + cell * 0.5f) {
        return std::nullopt;
    }

    const int col = static_cast<int>((static_cast<float>(mouse.x) - left + cell * 0.5f) / cell);
    const int rowFromTop = static_cast<int>((static_cast<float>(mouse.y) - top + cell * 0.5f) / cell);
    const int row = boardSize - 1 - rowFromTop;

    if (!match.state().isInside(row, col)) {
        return std::nullopt;
    }

    return Move {row, col};
}

std::string threatShortLabel(ThreatType type) {
    switch (type) {
        case ThreatType::Five:
            return "5";
        case ThreatType::OpenFour:
            return "O4";
        case ThreatType::SimpleFour:
            return "4";
        case ThreatType::OpenThree:
            return "O3";
        case ThreatType::BrokenThree:
            return "B3";
        case ThreatType::Two:
            return "2";
        case ThreatType::One:
            return "1";
        case ThreatType::None:
        default:
            return "";
    }
}

sf::Color heatColorForPlayer(gomoku::Player player, std::uint8_t alpha) {
    if (player == gomoku::Player::Black) {
        return sf::Color(230, 90, 40, alpha);
    }
    if (player == gomoku::Player::White) {
        return sf::Color(70, 130, 255, alpha);
    }
    return sf::Color(128, 128, 128, alpha);
}

std::vector<int> boardGuideIndices(int boardSize) {
    if (boardSize == 15) {
        return {boardSize / 2};
    }

    return {};
}

void clearThreatAnalysis(ThreatAnalysisView& analysis) {
    analysis.active = false;
    analysis.actionCount = -1;
    analysis.sideToMove = gomoku::Player::None;
    analysis.result.reset();
    analysis.stepIndex = 0;
}

void invalidateThreatAnalysisIfStale(const Match& match, ThreatAnalysisView& analysis) {
    if (!analysis.active) {
        return;
    }

    if (match.state().actionCount() != analysis.actionCount || match.state().sideToMove() != analysis.sideToMove || match.state().isGameOver()
        || match.state().isSwapDecisionPending()) {
        clearThreatAnalysis(analysis);
    }
}

void runThreatAnalysis(const Match& match, ThreatAnalysisView& analysis) {
    if (match.state().isGameOver() || match.state().isSwapDecisionPending()) {
        clearThreatAnalysis(analysis);
        return;
    }

    gomoku::ThreatSequenceConfig config;
    config.maxDepth = 8;
    config.maxNodes = 60000;
    config.maxThreatMoves = 14;

    gomoku::ThreatSequenceSearcher searcher(config);
    analysis.active = true;
    analysis.actionCount = match.state().actionCount();
    analysis.sideToMove = match.state().sideToMove();
    analysis.result = searcher.searchWinningSequence(match.state(), analysis.sideToMove);
    analysis.stepIndex = 0;
}

std::string moveListText(const std::vector<Move>& moves, std::size_t limit = 4) {
    if (moves.empty()) {
        return "-";
    }

    std::string text;
    const std::size_t count = std::min(limit, moves.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (!text.empty()) {
            text += ", ";
        }
        text += gomoku::moveToString(moves[index]);
    }
    if (moves.size() > limit) {
        text += ", ...";
    }
    return text;
}

const ThreatStep* currentThreatStep(const ThreatAnalysisView& analysis) {
    if (!analysis.active || !analysis.result.has_value() || analysis.result->sequence.empty()) {
        return nullptr;
    }
    if (analysis.stepIndex >= analysis.result->sequence.size()) {
        return nullptr;
    }
    return &analysis.result->sequence[analysis.stepIndex];
}

void refreshAnalysis(const Match& match, AnalysisOverlay& analysis) {
    const auto& state = match.state();
    if (analysis.boardSize == state.boardSize() && analysis.actionCount == state.actionCount() && analysis.swapPending == state.isSwapDecisionPending()
        && analysis.result == state.result()) {
        return;
    }

    analysis.boardSize = state.boardSize();
    analysis.actionCount = state.actionCount();
    analysis.swapPending = state.isSwapDecisionPending();
    analysis.result = state.result();
    analysis.sideToMove = gomoku::Player::None;
    analysis.staticEval = 0;
    analysis.maxCellScore = 0;
    analysis.topCandidates.clear();

    if (state.isGameOver() || state.isSwapDecisionPending()) {
        return;
    }

    analysis.sideToMove = state.sideToMove();
    analysis.staticEval = gomoku::StaticEvaluator::evaluate(state, analysis.sideToMove);
    analysis.topCandidates = gomoku::StaticEvaluator::generateCandidateMoves(state, analysis.sideToMove, 8);

    for (int row = 0; row < state.boardSize(); ++row) {
        for (int col = 0; col < state.boardSize(); ++col) {
            const Move move {row, col};
            if (state.cellAt(row, col) != gomoku::Player::None) {
                continue;
            }
            analysis.maxCellScore = std::max(analysis.maxCellScore, state.threatInfoAt(move, analysis.sideToMove).totalScore);
        }
    }
}

void drawHeatmap(sf::RenderWindow& window, const Match& match, const AnalysisOverlay& analysis, float left, float top, float cell) {
    if (analysis.sideToMove == gomoku::Player::None || analysis.maxCellScore <= 0) {
        return;
    }

    for (int row = 0; row < match.state().boardSize(); ++row) {
        for (int col = 0; col < match.state().boardSize(); ++col) {
            const Move move {row, col};
            if (match.state().cellAt(row, col) != gomoku::Player::None) {
                continue;
            }

            const auto& info = match.state().threatInfoAt(move, analysis.sideToMove);
            if (info.totalScore <= 0) {
                continue;
            }

            const float normalized = std::sqrt(static_cast<float>(info.totalScore) / static_cast<float>(analysis.maxCellScore));
            const std::uint8_t alpha = static_cast<std::uint8_t>(50.0f + normalized * 140.0f);

            sf::CircleShape pulse(cell * (0.15f + normalized * 0.18f));
            pulse.setOrigin(pulse.getRadius(), pulse.getRadius());
            pulse.setPosition(boardPoint(match, move, left, top, cell));
            pulse.setFillColor(heatColorForPlayer(analysis.sideToMove, alpha));
            window.draw(pulse);
        }
    }
}

void drawThreatLabels(sf::RenderWindow& window, const Match& match, const AnalysisOverlay& analysis, const sf::Font& font, float left, float top, float cell) {
    if (analysis.sideToMove == gomoku::Player::None) {
        return;
    }

    for (int row = 0; row < match.state().boardSize(); ++row) {
        for (int col = 0; col < match.state().boardSize(); ++col) {
            const Move move {row, col};
            if (match.state().cellAt(row, col) != gomoku::Player::None) {
                continue;
            }

            const auto& info = match.state().threatInfoAt(move, analysis.sideToMove);
            if (gomoku::threatSeverity(info.best) < gomoku::threatSeverity(ThreatType::Two)) {
                continue;
            }

            sf::Text label;
            label.setFont(font);
            label.setCharacterSize(static_cast<unsigned>(std::max(12.0f, cell * 0.28f)));
            label.setFillColor(sf::Color::Black);
            label.setString(threatShortLabel(info.best));

            const sf::Vector2f point = boardPoint(match, move, left, top, cell);
            label.setPosition(point.x - cell * 0.18f, point.y - cell * 0.42f);

            sf::CircleShape background(cell * 0.18f);
            background.setOrigin(background.getRadius(), background.getRadius());
            background.setPosition(point.x, point.y - cell * 0.10f);
            background.setFillColor(sf::Color(255, 255, 255, 180));
            window.draw(background);
            window.draw(label);
        }
    }
}

void drawTopCandidateMarkers(sf::RenderWindow& window, const Match& match, const AnalysisOverlay& analysis, float left, float top, float cell) {
    for (std::size_t index = 0; index < analysis.topCandidates.size(); ++index) {
        const CandidateMove& candidate = analysis.topCandidates[index];
        const sf::Vector2f point = boardPoint(match, candidate.move, left, top, cell);

        sf::CircleShape ring(cell * (index == 0 ? 0.26f : 0.20f));
        ring.setOrigin(ring.getRadius(), ring.getRadius());
        ring.setPosition(point);
        ring.setFillColor(sf::Color::Transparent);
        ring.setOutlineThickness(index == 0 ? 3.0f : 2.0f);
        ring.setOutlineColor(index == 0 ? sf::Color(40, 170, 70, 220) : sf::Color(60, 60, 60, 160));
        window.draw(ring);
    }
}

void drawThreatSequenceOverlay(sf::RenderWindow& window, const Match& match, const ThreatAnalysisView& analysis, const sf::Font& font, float left, float top, float cell) {
    if (!analysis.active || !analysis.result.has_value() || analysis.result->sequence.empty()) {
        return;
    }

    const std::size_t visibleCount = std::min(analysis.stepIndex + 1, analysis.result->sequence.size());
    for (std::size_t index = 0; index < visibleCount; ++index) {
        const ThreatStep& step = analysis.result->sequence[index];
        const sf::Vector2f point = boardPoint(match, step.move, left, top, cell);

        sf::CircleShape ring(cell * (index == analysis.stepIndex ? 0.34f : 0.28f));
        ring.setOrigin(ring.getRadius(), ring.getRadius());
        ring.setPosition(point);
        ring.setFillColor(index == analysis.stepIndex ? sf::Color(255, 225, 120, 90) : sf::Color(255, 225, 120, 45));
        ring.setOutlineThickness(index == analysis.stepIndex ? 3.5f : 2.0f);
        ring.setOutlineColor(index == analysis.stepIndex ? sf::Color(225, 120, 25) : sf::Color(160, 110, 40));
        window.draw(ring);

        sf::Text number;
        number.setFont(font);
        number.setCharacterSize(static_cast<unsigned>(std::max(12.0f, cell * 0.28f)));
        number.setFillColor(sf::Color::Black);
        number.setString(std::to_string(index + 1));
        number.setPosition(point.x - cell * 0.10f, point.y - cell * 0.18f);
        window.draw(number);
    }

    const ThreatStep* step = currentThreatStep(analysis);
    if (step == nullptr) {
        return;
    }

    for (const Move& defense : step->defenseMoves) {
        sf::RectangleShape marker({cell * 0.38f, cell * 0.38f});
        marker.setOrigin(marker.getSize().x * 0.5f, marker.getSize().y * 0.5f);
        marker.setPosition(boardPoint(match, defense, left, top, cell));
        marker.setFillColor(sf::Color::Transparent);
        marker.setOutlineThickness(2.0f);
        marker.setOutlineColor(sf::Color(210, 50, 50, 220));
        window.draw(marker);
    }

    for (const Move& required : step->requiredEmpty) {
        sf::CircleShape marker(cell * 0.16f, 4);
        marker.setOrigin(marker.getRadius(), marker.getRadius());
        marker.setRotation(45.0f);
        marker.setPosition(boardPoint(match, required, left, top, cell));
        marker.setFillColor(sf::Color::Transparent);
        marker.setOutlineThickness(2.0f);
        marker.setOutlineColor(sf::Color(50, 170, 210, 220));
        window.draw(marker);
    }
}

void drawBoard(sf::RenderWindow& window, const Match& match, float left, float top, float cell, const sf::Font* font) {
    const int boardSize = match.state().boardSize();
    const float boardPixels = cell * static_cast<float>(boardSize - 1);

    sf::RectangleShape background({boardPixels + cell, boardPixels + cell});
    background.setPosition(left - cell * 0.5f, top - cell * 0.5f);
    background.setFillColor(sf::Color(224, 193, 122));
    window.draw(background);

    for (int index = 0; index < boardSize; ++index) {
        sf::Vertex horizontal[] = {
            {{left, top + cell * static_cast<float>(index)}, sf::Color::Black},
            {{left + boardPixels, top + cell * static_cast<float>(index)}, sf::Color::Black},
        };
        window.draw(horizontal, 2, sf::Lines);

        sf::Vertex vertical[] = {
            {{left + cell * static_cast<float>(index), top}, sf::Color::Black},
            {{left + cell * static_cast<float>(index), top + boardPixels}, sf::Color::Black},
        };
        window.draw(vertical, 2, sf::Lines);
    }

    const std::vector<int> guideIndices = boardGuideIndices(boardSize);
    for (int row : guideIndices) {
        for (int col : guideIndices) {
            sf::CircleShape guideDot(cell * 0.08f);
            guideDot.setOrigin(guideDot.getRadius(), guideDot.getRadius());
            guideDot.setPosition(boardPoint(match, {row, col}, left, top, cell));
            guideDot.setFillColor(sf::Color(55, 45, 25, 220));
            window.draw(guideDot);
        }
    }

    for (int row = 0; row < boardSize; ++row) {
        for (int col = 0; col < boardSize; ++col) {
            const auto cellValue = match.state().cellAt(row, col);
            if (cellValue == gomoku::Player::None) {
                continue;
            }

            sf::CircleShape stone(cell * 0.42f);
            stone.setOrigin(stone.getRadius(), stone.getRadius());
            stone.setPosition(boardPoint(match, {row, col}, left, top, cell));
            stone.setFillColor(cellValue == gomoku::Player::Black ? sf::Color(25, 25, 25) : sf::Color(245, 245, 245));
            stone.setOutlineColor(sf::Color::Black);
            stone.setOutlineThickness(1.5f);
            window.draw(stone);
        }
    }

    if (const auto lastMove = match.state().lastPlacedMove()) {
        sf::CircleShape marker(cell * 0.12f);
        marker.setOrigin(marker.getRadius(), marker.getRadius());
        marker.setPosition(boardPoint(match, *lastMove, left, top, cell));
        marker.setFillColor(sf::Color(220, 60, 60));
        window.draw(marker);
    }

    if (font == nullptr) {
        return;
    }

    sf::Text label;
    label.setFont(*font);
    label.setCharacterSize(static_cast<unsigned>(std::max(12.0f, cell * 0.30f)));
    label.setFillColor(sf::Color(55, 45, 25, 230));

    for (int col = 0; col < boardSize; ++col) {
        label.setString(boardColumnLabel(col));
        centerTextOrigin(label);

        const float x = left + cell * static_cast<float>(col);
        label.setPosition(x, top - cell * 0.35f);
        window.draw(label);

        label.setPosition(x, top + boardPixels + cell * 0.35f);
        window.draw(label);
    }

    for (int row = 0; row < boardSize; ++row) {
        label.setString(boardRowLabel(row));
        centerTextOrigin(label);

        const float y = top + cell * static_cast<float>(boardSize - 1 - row);
        label.setPosition(left - cell * 0.35f, y);
        window.draw(label);

        label.setPosition(left + boardPixels + cell * 0.35f, y);
        window.draw(label);
    }
}

std::vector<StatusLine> buildStatusLines(const Match& match, const UiControlState& controls, const OverlayState& overlay,
    const AnalysisOverlay& analysis, const ThreatAnalysisView& threatAnalysis,
    const std::optional<gomoku::SearchSummary>& liveSummary, bool aiThinking) {
    std::vector<StatusLine> lines;
    const auto addLine = [&](std::string text, bool bold = false) {
        lines.push_back({std::move(text), bold});
    };
    const auto addBlank = [&]() {
        lines.push_back({});
    };

    addLine("Rules: " + std::string(match.state().rules().name));
    addLine("Result: " + std::string(gomoku::toString(match.state().result())));
    addLine("Moves: " + std::to_string(match.state().moveCount()));
    addLine("AI clock: " + formatTimeControl(match.config().aiTimeControlPreset));
    addLine("Opener: " + std::string(gomoku::toString(match.config().openerController)));
    addLine("Chooser: " + std::string(gomoku::toString(match.config().chooserController)));
    if (bothSeatsAi(match)) {
        addLine(autoplayActive(match, controls) ? "AI vs AI autoplay" : "AI vs AI paused", autoplayActive(match, controls));
        if (!autoplayActive(match, controls)) {
            addLine("Press Space to start autoplay.");
        }
    }
    if (feedbackVisible(controls)) {
        addLine("Change: " + controls.feedbackText, true);
    }
    addBlank();

    if (!match.state().isGameOver()) {
        if (const auto openerClock = match.aiClockForSeat(gomoku::Seat::Opener)) {
            addLine("Opener clock: " + formatSeatClock(*openerClock));
        }
        if (const auto chooserClock = match.aiClockForSeat(gomoku::Seat::Chooser)) {
            addLine("Chooser clock: " + formatSeatClock(*chooserClock));
        }
        if (match.state().isSwapDecisionPending()) {
            addLine("Swap choice pending: " + std::string(gomoku::toString(match.seatToAct())));
            addLine("Press K to keep or S to swap.");
        } else {
            addLine("To move: " + std::string(gomoku::toString(match.state().sideToMove())));
            addLine("Seat: " + std::string(gomoku::toString(match.seatToAct())));
            addLine("Controller: " + std::string(gomoku::toString(match.controllerToAct())));
            if (const auto bookHit = gomoku::lookupOpeningBookMove(match.state())) {
                addLine("Book move: " + gomoku::moveToString(bookHit->move) + " (" + std::string(bookHit->lineName) + ")");
            }
        }
    }

    const std::optional<gomoku::SearchSummary> displayedSummary = liveSummary.has_value()
        ? liveSummary
        : match.lastSearchSummary();
    if (displayedSummary.has_value()) {
        const gomoku::SearchSummary& summary = *displayedSummary;
        addBlank();
        addLine(aiThinking ? "Current search:" : "Last search:");
        addLine("Max depth: " + std::to_string(summary.maxDepthVisited));
        addLine("Completed depth: " + std::to_string(summary.depthReached));
        if (aiThinking && summary.depthReached == 0) {
            addLine("Status: searching", true);
        }
        addLine("Eval: " + std::to_string(summary.score));
        addLine("Time: " + std::to_string(summary.elapsedMs) + " ms");
        addLine("Nodes: " + std::to_string(summary.nodes));
        addLine("TT hits: " + std::to_string(summary.ttHits));
        addLine("Threat nodes: " + std::to_string(summary.threatNodes));
        if (summary.usedThreatSequence) {
            addLine("Threat line: " + std::to_string(summary.threatSequenceLength) + " steps");
        }
        if (summary.usedOpeningBook) {
            addLine("Book line: " + summary.openingBookName);
        }
        if (summary.panicModeEntered) {
            addLine("Panic mode: soft limit ignored", true);
        }
        if (!summary.principalVariation.empty()) {
            addLine("PV: " + moveListText(summary.principalVariation, 6));
        }
    }

    if (analysis.sideToMove != gomoku::Player::None) {
        addBlank();
        addLine("Live eval:");
        addLine("Static eval: " + std::to_string(analysis.staticEval));
        addLine("Candidate moves: " + std::to_string(analysis.topCandidates.size()));
    }

    addBlank();
    addLine("Settings:");
    addLine("1 freestyle15", match.config().ruleset == Ruleset::Freestyle15);
    addLine("2 standard15", match.config().ruleset == Ruleset::Standard15);
    addLine("O opener: " + std::string(gomoku::toString(match.config().openerController)));
    addLine("P chooser: " + std::string(gomoku::toString(match.config().chooserController)));
    addLine("time (-/+): " + formatTimeControl(match.config().aiTimeControlPreset));
    addLine("Space autoplay", autoplayActive(match, controls));
    addLine("H heatmap", overlay.showHeatmap);
    addLine("T labels", overlay.showThreatLabels);
    addLine("C candidates", overlay.showTopCandidates);

    if (threatAnalysis.active && threatAnalysis.result.has_value()) {
        addBlank();
        addLine("Threat analysis:");
        addLine("Side: " + std::string(gomoku::toString(threatAnalysis.sideToMove)));
        addLine(std::string("Win found: ") + (threatAnalysis.result->foundWin ? "yes" : "no"));
        addLine("Graph nodes: " + std::to_string(threatAnalysis.result->graph.size()));
        addLine("Refutations: " + std::to_string(threatAnalysis.result->refutations.size()));
        if (!threatAnalysis.result->sequence.empty()) {
            addLine("Step: " + std::to_string(threatAnalysis.stepIndex + 1) + "/"
                + std::to_string(threatAnalysis.result->sequence.size()));
        }
    }

    addBlank();
    addLine("Actions:");
    addLine("R restart");
    addLine("U undo");
    addLine("Space toggle autoplay");
    addLine("A analyze threats");
    addLine("X save game");
    addLine("L load game");
    addLine("Left/Right threat step");
    addLine("Esc clear analysis");
    if (match.state().isSwapDecisionPending()) {
        addLine("K keep colors");
        addLine("S swap colors");
    }

    return lines;
}

void drawStatusLines(sf::RenderWindow& window, const sf::Font& font, const std::vector<StatusLine>& lines, sf::Vector2f position,
    unsigned characterSize, float lineSpacingFactor) {
    sf::Text lineText;
    lineText.setFont(font);
    lineText.setCharacterSize(characterSize);
    lineText.setFillColor(sf::Color::Black);

    float y = position.y;
    const float lineHeight = font.getLineSpacing(characterSize) * lineSpacingFactor;
    for (const StatusLine& line : lines) {
        if (!line.text.empty()) {
            lineText.setString(line.text);
            lineText.setStyle(line.bold ? sf::Text::Bold : sf::Text::Regular);
            lineText.setPosition(position.x, y);
            window.draw(lineText);
        }
        y += lineHeight;
    }
}

std::string buildCandidateText(const AnalysisOverlay& analysis) {
    std::string text = "Top candidates";
    if (analysis.sideToMove != gomoku::Player::None) {
        text += " (";
        text += std::string(gomoku::toString(analysis.sideToMove));
        text += ')';
    }
    text += ":\n";
    text += "#  mv   eval  th\n";

    if (analysis.topCandidates.empty()) {
        text += "  none\n";
        return text;
    }

    for (std::size_t index = 0; index < analysis.topCandidates.size(); ++index) {
        const CandidateMove& candidate = analysis.topCandidates[index];
        text += std::to_string(index + 1);
        text += "  ";
        text += gomoku::moveToString(candidate.move);
        text += "  ";
        text += std::to_string(candidate.score);
        text += "  ";
        text += threatShortLabel(candidate.threatInfo.best);
        text += '\n';
    }

    return text;
}

std::string buildThreatAnalysisText(const ThreatAnalysisView& analysis) {
    if (!analysis.active || !analysis.result.has_value()) {
        return "Threat analysis:\n  inactive\n";
    }

    std::string text = "Threat analysis";
    text += " (";
    text += std::string(gomoku::toString(analysis.sideToMove));
    text += "):\n";
    text += std::string("Win found: ") + (analysis.result->foundWin ? "yes" : "no") + "\n";
    text += "Graph nodes: ";
    text += std::to_string(analysis.result->graph.size());
    text += "\nSearch nodes: ";
    text += std::to_string(analysis.result->nodes);
    text += "\nRefutations: ";
    text += std::to_string(analysis.result->refutations.size());
    text += "\n";

    if (analysis.result->sequence.empty()) {
        if (!analysis.result->refutations.empty()) {
            text += "\nKey refutations:\n";
            text += moveListText(analysis.result->refutations, 8);
            text += '\n';
        }
        return text;
    }

    text += "\nSequence:\n";
    for (std::size_t index = 0; index < analysis.result->sequence.size(); ++index) {
        const ThreatStep& step = analysis.result->sequence[index];
        text += index == analysis.stepIndex ? "> " : "  ";
        text += std::to_string(index + 1);
        text += ". ";
        text += gomoku::moveToString(step.move);
        text += "  ";
        text += std::string(gomoku::toString(step.type));
        text += "\n";

        if (index == analysis.stepIndex) {
            text += "    defenses: ";
            text += moveListText(step.defenseMoves);
            text += "\n    required: ";
            text += moveListText(step.requiredEmpty);
            text += "\n    continue: ";
            text += moveListText(step.continuationMoves);
            text += '\n';
        }
    }

    return text;
}

UiLayout computeLayout(sf::Vector2u windowSize, int boardSize) {
    UiLayout layout;

    const float horizontalPadding = 24.0f;
    const float verticalPadding = 24.0f;
    const float boardGap = 24.0f;
    const float panelGap = 18.0f;
    const float minPanelWidthTwo = 220.0f;
    const float minPanelWidthOne = 250.0f;
    const float minCell = 20.0f;
    const float maxCell = 54.0f;

    const float availableWidth = std::max(400.0f, static_cast<float>(windowSize.x) - horizontalPadding * 2.0f);
    const float availableHeight = std::max(320.0f, static_cast<float>(windowSize.y) - verticalPadding * 2.0f);

    int panelColumns = windowSize.x >= 1320U ? 2 : 1;
    float chosenCell = minCell;
    float chosenSidebarWidth = minPanelWidthOne;

    while (true) {
        const float minPanelWidth = panelColumns == 2 ? minPanelWidthTwo : minPanelWidthOne;
        const float sidebarMinWidth = minPanelWidth * static_cast<float>(panelColumns) + panelGap * static_cast<float>(panelColumns - 1);
        const float boardWidthBudget = availableWidth - boardGap - sidebarMinWidth;
        const float cellByWidth = boardWidthBudget / static_cast<float>(boardSize);
        const float cellByHeight = availableHeight / static_cast<float>(boardSize);
        chosenCell = std::floor(std::min({cellByWidth, cellByHeight, maxCell}));

        if (chosenCell >= minCell || panelColumns == 1) {
            chosenCell = std::max(minCell, chosenCell);
            const float boardRenderedSize = chosenCell * static_cast<float>(boardSize);
            chosenSidebarWidth = std::max(minPanelWidth, availableWidth - boardRenderedSize - boardGap);
            break;
        }

        panelColumns = 1;
    }

    const float boardRenderedSize = chosenCell * static_cast<float>(boardSize);
    layout.boardLeft = horizontalPadding;
    layout.boardTop = verticalPadding + std::max(0.0f, (availableHeight - boardRenderedSize) * 0.5f);
    layout.cell = chosenCell;
    layout.panelPadding = std::max(10.0f, chosenSidebarWidth * 0.04f);

    const float sidebarLeft = layout.boardLeft + boardRenderedSize + boardGap;
    const float sidebarTop = verticalPadding;

    if (panelColumns == 2) {
        const float panelWidth = std::max(170.0f, (chosenSidebarWidth - panelGap) * 0.5f);
        layout.statusPanel = {sidebarLeft, sidebarTop, panelWidth, availableHeight};
        layout.candidatePanel = {sidebarLeft + panelWidth + panelGap, sidebarTop, panelWidth, availableHeight};
        layout.statusCharacterSize = static_cast<unsigned>(std::clamp(panelWidth / 11.5f, 15.0f, 21.0f));
        layout.candidateCharacterSize = static_cast<unsigned>(std::clamp(panelWidth / 17.5f, 12.0f, 16.0f));
        return layout;
    }

    const float panelHeight = (availableHeight - panelGap) * 0.5f;
    layout.statusPanel = {sidebarLeft, sidebarTop, chosenSidebarWidth, panelHeight};
    layout.candidatePanel = {sidebarLeft, sidebarTop + panelHeight + panelGap, chosenSidebarWidth, panelHeight};
    layout.statusCharacterSize = static_cast<unsigned>(std::clamp(chosenSidebarWidth / 15.0f, 14.0f, 20.0f));
    layout.candidateCharacterSize = static_cast<unsigned>(std::clamp(chosenSidebarWidth / 20.0f, 11.0f, 15.0f));
    return layout;
}

sf::RectangleShape makePanel(sf::Vector2f position, sf::Vector2f size) {
    sf::RectangleShape panel(size);
    panel.setPosition(position);
    panel.setFillColor(sf::Color(255, 250, 238, 220));
    panel.setOutlineColor(sf::Color(140, 120, 90, 180));
    panel.setOutlineThickness(1.5f);
    return panel;
}

}  // namespace

int main(int argc, char** argv) {
    MatchConfig config;
    config.aiTimeControlPreset = AiTimeControlPreset::Blitz;
    if (argc >= 2) {
        Ruleset ruleset;
        if (gomoku::tryParseRuleset(argv[1], ruleset)) {
            config.ruleset = ruleset;
        }
    }

    if (argc >= 3) {
        ControllerKind controller;
        if (gomoku::tryParseController(argv[2], controller)) {
            config.openerController = controller;
        }
    }

    if (argc >= 4) {
        ControllerKind controller;
        if (gomoku::tryParseController(argv[3], controller)) {
            config.chooserController = controller;
        }
    }

    if (argc >= 5) {
        try {
            config.aiMoveTimeMs = std::max(1, std::stoi(argv[4]));
        } catch (...) {
            config.aiMoveTimeMs = 500;
        }
    }

    Match match(config);
    UiControlState controls;
    OverlayState overlay;
    AnalysisOverlay analysis;
    ThreatAnalysisView threatAnalysis;
    AiSearchState aiSearch;

    sf::RenderWindow window(sf::VideoMode(1380, 900), "Gomoku - Checkpoint 5");
    window.setFramerateLimit(60);

    std::unique_ptr<sf::Font> font = loadUiFont();
    sf::Clock aiClock;

    const auto replaceMatch = [&](const MatchConfig& nextConfig) {
        finishAiSearchWorker(aiSearch);
        match = Match(nextConfig);
        clearThreatAnalysis(threatAnalysis);
        aiClock.restart();
    };

    const auto saveCurrentGame = [&]() {
        std::ofstream output(savedGamePath(), std::ios::out | std::ios::trunc);
        if (!output) {
            setFeedback(controls, "Save failed");
            return;
        }
        output << gomoku::serializeMatchSession(match);
        if (!output.good()) {
            setFeedback(controls, "Save failed");
            return;
        }
        setFeedback(controls, "Saved " + std::string(kSavedGameFilename));
    };

    const auto loadSavedGame = [&]() {
        std::ifstream input(savedGamePath());
        if (!input) {
            setFeedback(controls, "Load failed");
            return;
        }
        std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        Match loaded;
        std::string error;
        if (!gomoku::deserializeMatchSession(text, loaded, error)) {
            setFeedback(controls, "Load failed: " + error);
            return;
        }
        finishAiSearchWorker(aiSearch);
        match = std::move(loaded);
        clearThreatAnalysis(threatAnalysis);
        aiClock.restart();
        setFeedback(controls, "Loaded " + std::string(kSavedGameFilename));
    };

    while (window.isOpen()) {
        sf::Event event;
        while (window.pollEvent(event)) {
            if (event.type == sf::Event::Closed) {
                window.close();
            } else if (event.type == sf::Event::Resized) {
                window.setView(sf::View(sf::FloatRect(0.0f, 0.0f, static_cast<float>(event.size.width), static_cast<float>(event.size.height))));
            }

            if (event.type == sf::Event::KeyPressed) {
                const bool aiBusy = aiSearch.running;
                const bool mutatesMatch = event.key.code == sf::Keyboard::R
                    || event.key.code == sf::Keyboard::U
                    || event.key.code == sf::Keyboard::K
                    || event.key.code == sf::Keyboard::S
                    || event.key.code == sf::Keyboard::Num1
                    || event.key.code == sf::Keyboard::Num2
                    || event.key.code == sf::Keyboard::O
                    || event.key.code == sf::Keyboard::P
                    || event.key.code == sf::Keyboard::LBracket
                    || event.key.code == sf::Keyboard::Comma
                    || event.key.code == sf::Keyboard::RBracket
                    || event.key.code == sf::Keyboard::Period
                    || event.key.code == sf::Keyboard::L;
                if (aiBusy && mutatesMatch) {
                    setFeedback(controls, "Wait for AI to finish");
                    continue;
                }

                if (event.key.code == sf::Keyboard::R) {
                    match.reset();
                    clearThreatAnalysis(threatAnalysis);
                } else if (event.key.code == sf::Keyboard::U) {
                    match.smartUndo();
                    clearThreatAnalysis(threatAnalysis);
                } else if (event.key.code == sf::Keyboard::K) {
                    if (match.applySwapChoice(SwapChoice::KeepColors)) {
                        aiClock.restart();
                        clearThreatAnalysis(threatAnalysis);
                    }
                } else if (event.key.code == sf::Keyboard::S) {
                    if (match.applySwapChoice(SwapChoice::SwapColors)) {
                        aiClock.restart();
                        clearThreatAnalysis(threatAnalysis);
                    }
                } else if (event.key.code == sf::Keyboard::Num1) {
                    MatchConfig next = match.config();
                    next.ruleset = Ruleset::Freestyle15;
                    next.openerController = ControllerKind::Human;
                    next.chooserController = ControllerKind::ExpertAI;
                    replaceMatch(next);
                    setFeedback(controls, "Rules: freestyle15");
                } else if (event.key.code == sf::Keyboard::Num2) {
                    MatchConfig next = match.config();
                    next.ruleset = Ruleset::Standard15;
                    next.openerController = ControllerKind::Human;
                    next.chooserController = ControllerKind::ExpertAI;
                    replaceMatch(next);
                    setFeedback(controls, "Rules: standard15");
                } else if (event.key.code == sf::Keyboard::O) {
                    MatchConfig next = match.config();
                    next.openerController = cycleController(next.openerController, 1);
                    replaceMatch(next);
                    setFeedback(controls, "Opener: " + std::string(gomoku::toString(match.config().openerController)));
                } else if (event.key.code == sf::Keyboard::P) {
                    MatchConfig next = match.config();
                    next.chooserController = cycleController(next.chooserController, 1);
                    replaceMatch(next);
                    setFeedback(controls, "Chooser: " + std::string(gomoku::toString(match.config().chooserController)));
                } else if (event.key.code == sf::Keyboard::LBracket || event.key.code == sf::Keyboard::Comma) {
                    MatchConfig next = match.config();
                    next.aiTimeControlPreset = cycleTimeControl(next.aiTimeControlPreset, -1);
                    replaceMatch(next);
                    setFeedback(controls, "AI clock: " + formatTimeControl(match.config().aiTimeControlPreset));
                } else if (event.key.code == sf::Keyboard::RBracket || event.key.code == sf::Keyboard::Period) {
                    MatchConfig next = match.config();
                    next.aiTimeControlPreset = cycleTimeControl(next.aiTimeControlPreset, 1);
                    replaceMatch(next);
                    setFeedback(controls, "AI clock: " + formatTimeControl(match.config().aiTimeControlPreset));
                } else if (event.key.code == sf::Keyboard::Space) {
                    if (bothSeatsAi(match)) {
                        controls.autoplayEnabled = !controls.autoplayEnabled;
                        setFeedback(controls, controls.autoplayEnabled ? "Autoplay enabled" : "Autoplay paused");
                    } else {
                        setFeedback(controls, "Autoplay needs AI on both seats");
                    }
                    aiClock.restart();
                } else if (event.key.code == sf::Keyboard::H) {
                    overlay.showHeatmap = !overlay.showHeatmap;
                } else if (event.key.code == sf::Keyboard::T) {
                    overlay.showThreatLabels = !overlay.showThreatLabels;
                } else if (event.key.code == sf::Keyboard::C) {
                    overlay.showTopCandidates = !overlay.showTopCandidates;
                } else if (event.key.code == sf::Keyboard::A) {
                    runThreatAnalysis(match, threatAnalysis);
                } else if (event.key.code == sf::Keyboard::X) {
                    saveCurrentGame();
                } else if (event.key.code == sf::Keyboard::L) {
                    loadSavedGame();
                } else if (event.key.code == sf::Keyboard::Escape) {
                    clearThreatAnalysis(threatAnalysis);
                } else if (event.key.code == sf::Keyboard::Right) {
                    if (threatAnalysis.active && threatAnalysis.result.has_value() && threatAnalysis.stepIndex + 1 < threatAnalysis.result->sequence.size()) {
                        ++threatAnalysis.stepIndex;
                    }
                } else if (event.key.code == sf::Keyboard::Left) {
                    if (threatAnalysis.active && threatAnalysis.stepIndex > 0) {
                        --threatAnalysis.stepIndex;
                    }
                }
            }

            if (event.type == sf::Event::MouseButtonPressed && event.mouseButton.button == sf::Mouse::Left && match.isHumanTurn()
                && !match.state().isSwapDecisionPending()) {
                const UiLayout layout = computeLayout(window.getSize(), match.state().boardSize());
                if (const auto move = pickMoveFromMouse(match, {event.mouseButton.x, event.mouseButton.y}, layout.boardLeft, layout.boardTop, layout.cell)) {
                    if (match.applyMove(*move)) {
                        aiClock.restart();
                        clearThreatAnalysis(threatAnalysis);
                    }
                }
            }
        }

        if (aiSearch.running) {
            bool finished = false;
            std::optional<gomoku::SearchResult> completedResult;
            std::optional<SwapChoice> completedSwapChoice;
            std::int64_t elapsedMs = 0;
            const int expectedActionCount = aiSearch.actionCount;
            const gomoku::Player expectedSideToMove = aiSearch.sideToMove;
            {
                std::lock_guard<std::mutex> lock(aiSearch.mutex);
                finished = aiSearch.finished;
                if (finished) {
                    completedResult = aiSearch.completedResult;
                    completedSwapChoice = aiSearch.completedSwapChoice;
                    elapsedMs = aiSearch.elapsedMs;
                }
            }
            if (finished) {
                finishAiSearchWorker(aiSearch);
                if (match.state().actionCount() == expectedActionCount && match.state().sideToMove() == expectedSideToMove && match.isAiTurn()) {
                    if (completedResult.has_value()) {
                        match.commitAiSearchResult(*completedResult, elapsedMs);
                    } else if (completedSwapChoice.has_value()) {
                        match.commitAiSwapChoice(*completedSwapChoice, elapsedMs);
                    }
                    aiClock.restart();
                    clearThreatAnalysis(threatAnalysis);
                }
            }
        }

        if (!aiSearch.running && match.isAiTurn() && !match.state().isGameOver() && (!bothSeatsAi(match) || controls.autoplayEnabled)
            && aiClock.getElapsedTime().asMilliseconds() > 60) {
            if (match.state().isSwapDecisionPending() || !usesSearchEngine(match.controllerToAct())) {
                match.stepAi();
            } else {
                startAiSearch(match, aiSearch);
            }
            aiClock.restart();
            clearThreatAnalysis(threatAnalysis);
        }

        refreshAnalysis(match, analysis);
        invalidateThreatAnalysisIfStale(match, threatAnalysis);
        const std::optional<gomoku::SearchSummary> liveSearchSummary = currentLiveSummary(aiSearch);

        const UiLayout layout = computeLayout(window.getSize(), match.state().boardSize());

        window.clear(sf::Color(245, 235, 210));
        drawBoard(window, match, layout.boardLeft, layout.boardTop, layout.cell, font.get());

        if (overlay.showHeatmap) {
            drawHeatmap(window, match, analysis, layout.boardLeft, layout.boardTop, layout.cell);
        }
        if (overlay.showTopCandidates) {
            drawTopCandidateMarkers(window, match, analysis, layout.boardLeft, layout.boardTop, layout.cell);
        }
        if (overlay.showThreatLabels && font) {
            drawThreatLabels(window, match, analysis, *font, layout.boardLeft, layout.boardTop, layout.cell);
        }
        if (font) {
            drawThreatSequenceOverlay(window, match, threatAnalysis, *font, layout.boardLeft, layout.boardTop, layout.cell);
        }

        if (font) {
            const sf::Vector2f statusPanelPos {layout.statusPanel.left, layout.statusPanel.top};
            const sf::Vector2f candidatePanelPos {layout.candidatePanel.left, layout.candidatePanel.top};
            const sf::Vector2f statusPanelSize {layout.statusPanel.width, layout.statusPanel.height};
            const sf::Vector2f candidatePanelSize {layout.candidatePanel.width, layout.candidatePanel.height};

            const sf::RectangleShape statusPanel = makePanel(statusPanelPos, statusPanelSize);
            const sf::RectangleShape candidatePanel = makePanel(candidatePanelPos, candidatePanelSize);
            window.draw(statusPanel);
            window.draw(candidatePanel);

            drawStatusLines(window, *font,
                buildStatusLines(match, controls, overlay, analysis, threatAnalysis, liveSearchSummary, aiSearch.running),
                {statusPanelPos.x + layout.panelPadding, statusPanelPos.y + layout.panelPadding}, layout.statusCharacterSize, 1.05f);

            sf::Text candidateText;
            candidateText.setFont(*font);
            candidateText.setCharacterSize(layout.candidateCharacterSize);
            candidateText.setLineSpacing(1.08f);
            candidateText.setFillColor(sf::Color(25, 25, 25));
            candidateText.setPosition(candidatePanelPos.x + layout.panelPadding, candidatePanelPos.y + layout.panelPadding);
            candidateText.setString(threatAnalysis.active ? buildThreatAnalysisText(threatAnalysis)
                                                          : buildCandidateText(analysis));
            window.draw(candidateText);
        } else {
            window.setTitle("Gomoku - Checkpoint 5 (font missing)");
        }

        window.display();
    }

    finishAiSearchWorker(aiSearch);
    return 0;
}

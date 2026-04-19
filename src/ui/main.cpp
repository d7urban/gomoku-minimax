#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
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
using gomoku::Match;
using gomoku::MatchConfig;
using gomoku::Move;
using gomoku::PositionAnnotation;
using gomoku::ProofAnalysisResult;
using gomoku::ProofMoveSummary;
using gomoku::ProofOutcome;
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
    ControllerKind::AnalystAI,
};

constexpr std::array<int, 5> kMoveTimeChoices = {100, 250, 500, 1000, 2000};

bool isAiController(ControllerKind controller) {
    return controller != ControllerKind::Human;
}

ControllerKind cycleController(ControllerKind controller, int delta) {
    auto found = std::find(kControllerCycle.begin(), kControllerCycle.end(), controller);
    const int index = found == kControllerCycle.end() ? 0 : static_cast<int>(std::distance(kControllerCycle.begin(), found));
    const int size = static_cast<int>(kControllerCycle.size());
    const int nextIndex = (index + delta + size) % size;
    return kControllerCycle[static_cast<std::size_t>(nextIndex)];
}

int cycleMoveTime(int currentMs, int delta) {
    auto found = std::find(kMoveTimeChoices.begin(), kMoveTimeChoices.end(), currentMs);
    int index = found == kMoveTimeChoices.end() ? 0 : static_cast<int>(std::distance(kMoveTimeChoices.begin(), found));
    if (found == kMoveTimeChoices.end()) {
        for (std::size_t choice = 0; choice < kMoveTimeChoices.size(); ++choice) {
            if (currentMs <= kMoveTimeChoices[choice]) {
                index = static_cast<int>(choice);
                break;
            }
        }
    }
    const int size = static_cast<int>(kMoveTimeChoices.size());
    const int nextIndex = (index + delta + size) % size;
    return kMoveTimeChoices[static_cast<std::size_t>(nextIndex)];
}

std::string formatMoveTime(int moveTimeMs) {
    if (moveTimeMs < 1000) {
        return std::to_string(moveTimeMs) + " ms";
    }

    const int wholeSeconds = moveTimeMs / 1000;
    const int fraction = (moveTimeMs % 1000) / 100;
    if (fraction == 0) {
        return std::to_string(wholeSeconds) + " s";
    }
    return std::to_string(wholeSeconds) + "." + std::to_string(fraction) + " s";
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

struct ProofAnalysisView {
    bool active {false};
    int actionCount {-1};
    std::uint64_t positionHash {0};
    gomoku::Player sideToMove {gomoku::Player::None};
    std::optional<ProofAnalysisResult> result;
};

struct UiControlState {
    bool autoplayEnabled {false};
    std::string feedbackText;
    sf::Clock feedbackClock;
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
constexpr const char* kSavedAnalysisFilename = "gomoku_analysis_position.txt";

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

std::optional<sf::Font> loadUiFont() {
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
        sf::Font font;
        if (font.loadFromFile(path)) {
            return font;
        }
    }

    return std::nullopt;
}

sf::Vector2f boardPoint(const Match& match, Move move, float left, float top, float cell) {
    return {
        left + cell * static_cast<float>(move.col),
        top + cell * static_cast<float>(match.state().boardSize() - 1 - move.row),
    };
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

    if (boardSize == 16) {
        return {3, 6, 9, 12};
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

void clearProofAnalysis(ProofAnalysisView& analysis) {
    analysis.active = false;
    analysis.actionCount = -1;
    analysis.positionHash = 0;
    analysis.sideToMove = gomoku::Player::None;
    analysis.result.reset();
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

void invalidateProofAnalysisIfStale(const Match& match, ProofAnalysisView& analysis) {
    if (!analysis.active) {
        return;
    }

    if (match.state().actionCount() != analysis.actionCount || match.state().positionHash() != analysis.positionHash
        || match.state().sideToMove() != analysis.sideToMove || match.state().isGameOver() || match.state().isSwapDecisionPending()) {
        clearProofAnalysis(analysis);
    }
}

void runProofAnalysis(const Match& match, ProofAnalysisView& analysis) {
    if (match.state().isGameOver() || match.state().isSwapDecisionPending()) {
        clearProofAnalysis(analysis);
        return;
    }

    gomoku::ProofAnalysisConfig config;
    config.maxDepth = 8;
    config.maxNodes = std::max<std::uint64_t>(180000, static_cast<std::uint64_t>(match.config().aiMoveTimeMs) * 900ULL);
    config.timeLimitMs = std::max(200, match.config().aiMoveTimeMs);
    config.maxCandidateMoves = 8;
    config.maxThreatMoves = 10;

    gomoku::ProofAnalyzer analyzer(config);
    analysis.active = true;
    analysis.actionCount = match.state().actionCount();
    analysis.positionHash = match.state().positionHash();
    analysis.sideToMove = match.state().sideToMove();
    analysis.result = analyzer.analyze(match.state(), analysis.sideToMove);
}

std::filesystem::path savedAnalysisPath() {
    return std::filesystem::current_path() / kSavedAnalysisFilename;
}

PositionAnnotation makePositionAnnotation(const ProofAnalysisView& analysis) {
    PositionAnnotation annotation;
    annotation.label = "ui_analysis_snapshot";
    if (!analysis.active || !analysis.result.has_value()) {
        return annotation;
    }

    annotation.analysisPlayer = analysis.sideToMove;
    annotation.proofOutcome = analysis.result->outcome;
    annotation.proofNodes = analysis.result->nodes;
    annotation.principalVariation = analysis.result->principalVariation;
    for (const ProofMoveSummary& rootMove : analysis.result->rootMoves) {
        if (rootMove.outcome == ProofOutcome::ProvenWin) {
            annotation.provenWinningMoves.push_back(rootMove.move);
        } else if (rootMove.outcome == ProofOutcome::ProvenLoss) {
            annotation.provenLosingMoves.push_back(rootMove.move);
        }
    }
    return annotation;
}

std::optional<ProofAnalysisResult> proofResultFromAnnotation(const PositionAnnotation& annotation) {
    if (annotation.analysisPlayer == gomoku::Player::None && annotation.proofOutcome == ProofOutcome::Unknown
        && annotation.proofNodes == 0 && annotation.principalVariation.empty() && annotation.provenWinningMoves.empty()
        && annotation.provenLosingMoves.empty()) {
        return std::nullopt;
    }

    ProofAnalysisResult result;
    result.attacker = annotation.analysisPlayer;
    result.outcome = annotation.proofOutcome;
    result.nodes = annotation.proofNodes;
    result.principalVariation = annotation.principalVariation;
    if (!result.principalVariation.empty()) {
        result.bestMove = result.principalVariation.front();
    } else if (!annotation.provenWinningMoves.empty()) {
        result.bestMove = annotation.provenWinningMoves.front();
    }

    // Annotated-position files only persist proven move outcomes, not the live root threat types.
    // Reconstructed root moves therefore use ThreatType::None until the proof is recomputed.
    for (const Move& move : annotation.provenWinningMoves) {
        result.rootMoves.push_back({move, ThreatType::None, ProofOutcome::ProvenWin, 0, std::numeric_limits<std::uint64_t>::max() / 4});
    }
    for (const Move& move : annotation.provenLosingMoves) {
        result.rootMoves.push_back({move, ThreatType::None, ProofOutcome::ProvenLoss, std::numeric_limits<std::uint64_t>::max() / 4, 0});
    }
    return result;
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

void drawProofMarkers(sf::RenderWindow& window, const Match& match, const ProofAnalysisView& analysis, float left, float top, float cell) {
    if (!analysis.active || !analysis.result.has_value()) {
        return;
    }

    for (const ProofMoveSummary& rootMove : analysis.result->rootMoves) {
        const sf::Vector2f point = boardPoint(match, rootMove.move, left, top, cell);
        if (rootMove.outcome == ProofOutcome::ProvenWin) {
            sf::CircleShape ring(cell * 0.24f);
            ring.setOrigin(ring.getRadius(), ring.getRadius());
            ring.setPosition(point);
            ring.setFillColor(sf::Color(50, 180, 90, 60));
            ring.setOutlineThickness(3.0f);
            ring.setOutlineColor(sf::Color(40, 150, 70, 220));
            window.draw(ring);
        } else if (rootMove.outcome == ProofOutcome::ProvenLoss) {
            sf::RectangleShape marker({cell * 0.42f, cell * 0.42f});
            marker.setOrigin(marker.getSize().x * 0.5f, marker.getSize().y * 0.5f);
            marker.setPosition(point);
            marker.setFillColor(sf::Color::Transparent);
            marker.setOutlineThickness(2.5f);
            marker.setOutlineColor(sf::Color(190, 60, 60, 220));
            window.draw(marker);
        }
    }
}

void drawProofVariationOverlay(sf::RenderWindow& window, const Match& match, const ProofAnalysisView& analysis, const sf::Font& font, float left,
    float top, float cell) {
    if (!analysis.active || !analysis.result.has_value() || analysis.result->principalVariation.empty()) {
        return;
    }

    const std::size_t visibleCount = std::min<std::size_t>(6, analysis.result->principalVariation.size());
    for (std::size_t index = 0; index < visibleCount; ++index) {
        const sf::Vector2f point = boardPoint(match, analysis.result->principalVariation[index], left, top, cell);

        sf::CircleShape ring(cell * 0.18f);
        ring.setOrigin(ring.getRadius(), ring.getRadius());
        ring.setPosition(point.x + cell * 0.22f, point.y - cell * 0.22f);
        ring.setFillColor(sf::Color(90, 150, 245, 170));
        window.draw(ring);

        sf::Text number;
        number.setFont(font);
        number.setCharacterSize(static_cast<unsigned>(std::max(11.0f, cell * 0.22f)));
        number.setFillColor(sf::Color::White);
        number.setString(std::to_string(index + 1));
        number.setPosition(point.x + cell * 0.16f, point.y - cell * 0.33f);
        window.draw(number);
    }
}

void drawBoard(sf::RenderWindow& window, const Match& match, float left, float top, float cell) {
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
}

std::vector<StatusLine> buildStatusLines(const Match& match, const UiControlState& controls, const OverlayState& overlay,
    const AnalysisOverlay& analysis, const ThreatAnalysisView& threatAnalysis, const ProofAnalysisView& proofAnalysis) {
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
    addLine("AI time: " + formatMoveTime(match.config().aiMoveTimeMs));
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

    if (const auto& summary = match.lastSearchSummary()) {
        addBlank();
        addLine("Last search:");
        addLine("Depth: " + std::to_string(summary->depthReached));
        addLine("Eval: " + std::to_string(summary->score));
        addLine("Time: " + std::to_string(summary->elapsedMs) + " ms");
        addLine("Nodes: " + std::to_string(summary->nodes));
        addLine("TT hits: " + std::to_string(summary->ttHits));
        addLine("Threat nodes: " + std::to_string(summary->threatNodes));
        if (summary->usedThreatSequence) {
            addLine("Threat line: " + std::to_string(summary->threatSequenceLength) + " steps");
        }
        if (summary->usedOpeningBook) {
            addLine("Book line: " + summary->openingBookName);
        }
        if (!summary->principalVariation.empty()) {
            addLine("PV: " + moveListText(summary->principalVariation, 6));
        }
    }

    if (const auto& proof = match.lastProofAnalysis()) {
        addBlank();
        addLine("Last proof:");
        addLine("Outcome: " + std::string(gomoku::toString(proof->outcome)));
        addLine("Nodes: " + std::to_string(proof->nodes));
        addLine("Time: " + std::to_string(proof->elapsedMs) + " ms");
        if (proof->bestMove.has_value()) {
            addLine("Best move: " + gomoku::moveToString(*proof->bestMove));
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
    addLine("3 swap16", match.config().ruleset == Ruleset::Swap16);
    addLine("O opener: " + std::string(gomoku::toString(match.config().openerController)));
    addLine("P chooser: " + std::string(gomoku::toString(match.config().chooserController)));
    addLine("time (-/+): " + formatMoveTime(match.config().aiMoveTimeMs));
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

    if (proofAnalysis.active && proofAnalysis.result.has_value()) {
        addBlank();
        addLine("Proof analysis:");
        addLine("Outcome: " + std::string(gomoku::toString(proofAnalysis.result->outcome)),
            proofAnalysis.result->outcome != ProofOutcome::Unknown);
        addLine("Nodes: " + std::to_string(proofAnalysis.result->nodes));
        addLine("Time: " + std::to_string(proofAnalysis.result->elapsedMs) + " ms");
        if (proofAnalysis.result->bestMove.has_value()) {
            addLine("Best move: " + gomoku::moveToString(*proofAnalysis.result->bestMove));
        }
    }

    addBlank();
    addLine("Actions:");
    addLine("R restart");
    addLine("U undo");
    addLine("Space toggle autoplay");
    addLine("A analyze threats");
    addLine("F analyze proof");
    addLine("X save analysis");
    addLine("L load analysis");
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

std::string buildProofAnalysisText(const ProofAnalysisView& analysis) {
    if (!analysis.active || !analysis.result.has_value()) {
        return "Proof analysis:\n  inactive\n";
    }

    std::string text = "Proof analysis";
    text += " (";
    text += std::string(gomoku::toString(analysis.sideToMove));
    text += "):\n";
    text += "Outcome: ";
    text += std::string(gomoku::toString(analysis.result->outcome));
    text += "\nNodes: ";
    text += std::to_string(analysis.result->nodes);
    text += "\nTime: ";
    text += std::to_string(analysis.result->elapsedMs);
    text += " ms\nDepth: ";
    text += std::to_string(analysis.result->maxDepthReached);
    text += "\nRoot pn/dn: ";
    text += std::to_string(analysis.result->rootProofNumber);
    text += " / ";
    text += std::to_string(analysis.result->rootDisproofNumber);
    if (analysis.result->bestMove.has_value()) {
        text += "\nBest move: ";
        text += gomoku::moveToString(*analysis.result->bestMove);
    }
    if (analysis.result->usedThreatShortcut) {
        text += "\nShortcut: threat win";
    }

    if (!analysis.result->principalVariation.empty()) {
        text += "\nPV: ";
        text += moveListText(analysis.result->principalVariation, 6);
    }

    if (!analysis.result->rootMoves.empty()) {
        text += "\n\nRoot moves:\n";
        const std::size_t count = std::min<std::size_t>(8, analysis.result->rootMoves.size());
        for (std::size_t index = 0; index < count; ++index) {
            const ProofMoveSummary& move = analysis.result->rootMoves[index];
            text += "  ";
            text += gomoku::moveToString(move.move);
            text += "  ";
            text += std::string(gomoku::toString(move.outcome));
            text += "  ";
            text += std::to_string(move.proofNumber);
            text += "/";
            text += std::to_string(move.disproofNumber);
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
    ProofAnalysisView proofAnalysis;

    sf::RenderWindow window(sf::VideoMode(1380, 900), "Gomoku - Checkpoint 5");
    window.setFramerateLimit(60);

    const std::optional<sf::Font> font = loadUiFont();
    sf::Clock aiClock;

    const auto replaceMatch = [&](const MatchConfig& nextConfig) {
        match = Match(nextConfig);
        clearThreatAnalysis(threatAnalysis);
        clearProofAnalysis(proofAnalysis);
        aiClock.restart();
    };

    const auto loadIntoMatch = [&](const gomoku::GameState& loadedState) {
        MatchConfig next = match.config();
        next.ruleset = loadedState.rules().ruleset;
        replaceMatch(next);
        for (const auto& action : loadedState.actions()) {
            if (action.kind == gomoku::Action::Kind::Move) {
                if (!match.applyMove(action.move)) {
                    return false;
                }
            } else {
                if (!match.applySwapChoice(action.swapChoice)) {
                    return false;
                }
            }
        }
        return true;
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
                if (event.key.code == sf::Keyboard::R) {
                    match.reset();
                    clearThreatAnalysis(threatAnalysis);
                    clearProofAnalysis(proofAnalysis);
                } else if (event.key.code == sf::Keyboard::U) {
                    match.undo();
                    clearThreatAnalysis(threatAnalysis);
                    clearProofAnalysis(proofAnalysis);
                } else if (event.key.code == sf::Keyboard::K) {
                    if (match.applySwapChoice(SwapChoice::KeepColors)) {
                        aiClock.restart();
                        clearThreatAnalysis(threatAnalysis);
                        clearProofAnalysis(proofAnalysis);
                    }
                } else if (event.key.code == sf::Keyboard::S) {
                    if (match.applySwapChoice(SwapChoice::SwapColors)) {
                        aiClock.restart();
                        clearThreatAnalysis(threatAnalysis);
                        clearProofAnalysis(proofAnalysis);
                    }
                } else if (event.key.code == sf::Keyboard::Num1) {
                    MatchConfig next = match.config();
                    next.ruleset = Ruleset::Freestyle15;
                    next.openerController = ControllerKind::Human;
                    next.chooserController = ControllerKind::AnalystAI;
                    replaceMatch(next);
                    setFeedback(controls, "Rules: freestyle15");
                } else if (event.key.code == sf::Keyboard::Num2) {
                    MatchConfig next = match.config();
                    next.ruleset = Ruleset::Standard15;
                    next.openerController = ControllerKind::Human;
                    next.chooserController = ControllerKind::AnalystAI;
                    replaceMatch(next);
                    setFeedback(controls, "Rules: standard15");
                } else if (event.key.code == sf::Keyboard::Num3) {
                    MatchConfig next = match.config();
                    next.ruleset = Ruleset::Swap16;
                    next.openerController = ControllerKind::Human;
                    next.chooserController = ControllerKind::AnalystAI;
                    replaceMatch(next);
                    setFeedback(controls, "Rules: swap16");
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
                } else if (event.key.code == sf::Keyboard::LBracket || event.key.code == sf::Keyboard::Comma || event.key.code == sf::Keyboard::Left) {
                    MatchConfig next = match.config();
                    next.aiMoveTimeMs = cycleMoveTime(next.aiMoveTimeMs, -1);
                    replaceMatch(next);
                    setFeedback(controls, "AI time: " + formatMoveTime(match.config().aiMoveTimeMs));
                } else if (event.key.code == sf::Keyboard::RBracket || event.key.code == sf::Keyboard::Period || event.key.code == sf::Keyboard::Right) {
                    MatchConfig next = match.config();
                    next.aiMoveTimeMs = cycleMoveTime(next.aiMoveTimeMs, 1);
                    replaceMatch(next);
                    setFeedback(controls, "AI time: " + formatMoveTime(match.config().aiMoveTimeMs));
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
                    clearProofAnalysis(proofAnalysis);
                } else if (event.key.code == sf::Keyboard::F) {
                    runProofAnalysis(match, proofAnalysis);
                    clearThreatAnalysis(threatAnalysis);
                } else if (event.key.code == sf::Keyboard::X) {
                    std::ofstream output(savedAnalysisPath());
                    if (!output) {
                        setFeedback(controls, "Failed to save analysis file");
                    } else {
                        output << serializeAnnotatedPosition(match.state(), makePositionAnnotation(proofAnalysis));
                        setFeedback(controls, "Saved " + savedAnalysisPath().filename().string());
                    }
                } else if (event.key.code == sf::Keyboard::L) {
                    std::ifstream input(savedAnalysisPath());
                    if (!input) {
                        setFeedback(controls, "No saved analysis file");
                    } else {
                        std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
                        gomoku::GameState loadedState;
                        PositionAnnotation annotation;
                        std::string error;
                        if (!deserializeAnnotatedPosition(text, loadedState, annotation, error) || !loadIntoMatch(loadedState)) {
                            setFeedback(controls, error.empty() ? "Failed to load saved analysis" : error);
                        } else {
                            clearThreatAnalysis(threatAnalysis);
                            clearProofAnalysis(proofAnalysis);
                            if (const auto proof = proofResultFromAnnotation(annotation)) {
                                proofAnalysis.active = true;
                                proofAnalysis.actionCount = match.state().actionCount();
                                proofAnalysis.positionHash = match.state().positionHash();
                                proofAnalysis.sideToMove = proof->attacker;
                                proofAnalysis.result = *proof;
                            }
                            setFeedback(controls, "Loaded " + savedAnalysisPath().filename().string());
                        }
                    }
                } else if (event.key.code == sf::Keyboard::Escape) {
                    clearThreatAnalysis(threatAnalysis);
                    clearProofAnalysis(proofAnalysis);
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
                        clearProofAnalysis(proofAnalysis);
                    }
                }
            }
        }

        if (match.isAiTurn() && !match.state().isGameOver() && (!bothSeatsAi(match) || controls.autoplayEnabled)
            && aiClock.getElapsedTime().asMilliseconds() > 60) {
            match.stepAi();
            aiClock.restart();
            clearThreatAnalysis(threatAnalysis);
            clearProofAnalysis(proofAnalysis);
        }

        refreshAnalysis(match, analysis);
        invalidateThreatAnalysisIfStale(match, threatAnalysis);
        invalidateProofAnalysisIfStale(match, proofAnalysis);

        const UiLayout layout = computeLayout(window.getSize(), match.state().boardSize());

        window.clear(sf::Color(245, 235, 210));
        drawBoard(window, match, layout.boardLeft, layout.boardTop, layout.cell);

        if (overlay.showHeatmap) {
            drawHeatmap(window, match, analysis, layout.boardLeft, layout.boardTop, layout.cell);
        }
        if (overlay.showTopCandidates) {
            drawTopCandidateMarkers(window, match, analysis, layout.boardLeft, layout.boardTop, layout.cell);
        }
        if (overlay.showThreatLabels && font.has_value()) {
            drawThreatLabels(window, match, analysis, *font, layout.boardLeft, layout.boardTop, layout.cell);
        }
        if (font.has_value()) {
            drawThreatSequenceOverlay(window, match, threatAnalysis, *font, layout.boardLeft, layout.boardTop, layout.cell);
            drawProofVariationOverlay(window, match, proofAnalysis, *font, layout.boardLeft, layout.boardTop, layout.cell);
        }
        drawProofMarkers(window, match, proofAnalysis, layout.boardLeft, layout.boardTop, layout.cell);

        if (font.has_value()) {
            const sf::Vector2f statusPanelPos {layout.statusPanel.left, layout.statusPanel.top};
            const sf::Vector2f candidatePanelPos {layout.candidatePanel.left, layout.candidatePanel.top};
            const sf::Vector2f statusPanelSize {layout.statusPanel.width, layout.statusPanel.height};
            const sf::Vector2f candidatePanelSize {layout.candidatePanel.width, layout.candidatePanel.height};

            const sf::RectangleShape statusPanel = makePanel(statusPanelPos, statusPanelSize);
            const sf::RectangleShape candidatePanel = makePanel(candidatePanelPos, candidatePanelSize);
            window.draw(statusPanel);
            window.draw(candidatePanel);

            drawStatusLines(window, *font, buildStatusLines(match, controls, overlay, analysis, threatAnalysis, proofAnalysis),
                {statusPanelPos.x + layout.panelPadding, statusPanelPos.y + layout.panelPadding}, layout.statusCharacterSize, 1.05f);

            sf::Text candidateText;
            candidateText.setFont(*font);
            candidateText.setCharacterSize(layout.candidateCharacterSize);
            candidateText.setLineSpacing(1.08f);
            candidateText.setFillColor(sf::Color(25, 25, 25));
            candidateText.setPosition(candidatePanelPos.x + layout.panelPadding, candidatePanelPos.y + layout.panelPadding);
            candidateText.setString(proofAnalysis.active ? buildProofAnalysisText(proofAnalysis)
                                                         : (threatAnalysis.active ? buildThreatAnalysisText(threatAnalysis)
                                                                                  : buildCandidateText(analysis)));
            window.draw(candidateText);
        } else {
            window.setTitle("Gomoku - Checkpoint 5 (font missing)");
        }

        window.display();
    }

    return 0;
}

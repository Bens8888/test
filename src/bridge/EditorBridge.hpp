#pragma once
// Bridge between core LevelObjects and GD's LevelEditorLayer: applying generated objects as a
// single undo step on a fresh editor layer, one-click undo, and level backups.
#include <Geode/Geode.hpp>

#include <al/level/level.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace al::mod {

struct StartSettings {
    int mode = 0, speed = 0; // speed: GD's Speed enum value
    bool mini = false, dual = false, flipped = false;
};

struct AppliedBatch {
    std::vector<geode::Ref<GameObject>> objects;
    std::filesystem::path backup;
    int editorLayer = 0;
    std::string label;
    std::optional<StartSettings> previousStart; // restored by undo
};

class EditorBridge {
public:
    static EditorBridge& get();

    // Writes the objects into the editor on a fresh editor layer as one undo step.
    // Returns the number of objects created.
    geode::Result<size_t> apply(LevelEditorLayer* lel, std::vector<level::LevelObject> objects, std::string const& label);

    // A whole generated level: its objects plus the start settings (gamemode, speed, size,
    // gravity, dual) of the level start; one undo step, the previous start settings are restored
    // by undoLast.
    geode::Result<size_t> applyLevel(LevelEditorLayer* lel, level::Level const& lvl, std::string const& label);

    // Highest group ID used in the editor level + 1 (generated groups start there).
    int firstFreeGroup(LevelEditorLayer* lel) const;
    // Number of gameplay (non-trigger) objects in the editor.
    int gameplayObjectCount(LevelEditorLayer* lel) const;

    // Saves the editor level as .gmd (+ a copy of its song and a label file) into the training
    // dataset folder (<save dir>/dataset/levels/<difficulty>/). Returns the .gmd path.
    geode::Result<std::filesystem::path> addToDataset(LevelEditorLayer* lel, std::filesystem::path const& song, int difficulty, int style);
    static std::filesystem::path datasetDir();

    // Removes the objects of the most recent AutoLayout batch (still present in the editor).
    bool undoLast(LevelEditorLayer* lel);
    size_t historySize() const { return m_history.size(); }
    std::string lastLabel() const { return m_history.empty() ? std::string() : m_history.back().label; }

    // Saves the editor's live level string to <save dir>/backups. Returns the file path.
    std::filesystem::path backupLevel(LevelEditorLayer* lel);

    // First editor layer above every layer used in the level.
    int freeEditorLayer(LevelEditorLayer* lel) const;

    // Rightmost x of any gameplay object currently in the editor.
    float lastObjectX(LevelEditorLayer* lel) const;

    // Live, uncompressed level string of the editor.
    std::string levelString(LevelEditorLayer* lel) const;

    void reset() { m_history.clear(); }

private:
    std::vector<AppliedBatch> m_history;
};

} // namespace al::mod

#include "EditorBridge.hpp"

#include "../recorder/TapAlong.hpp"

#include <Geode/binding/EditorUI.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>
#include <Geode/binding/UndoObject.hpp>

#include <al/gen/test_pattern.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>

using namespace geode::prelude;

namespace al::mod {

EditorBridge& EditorBridge::get() {
    static EditorBridge b;
    return b;
}

std::string EditorBridge::levelString(LevelEditorLayer* lel) const {
    if (!lel) return {};
    return std::string(lel->getLevelString());
}

int EditorBridge::freeEditorLayer(LevelEditorLayer* lel) const {
    int maxLayer = 0;
    if (!lel || !lel->m_objects) return 1;
    for (auto obj : CCArrayExt<GameObject*>(lel->m_objects)) {
        maxLayer = std::max<int>(maxLayer, obj->m_editorLayer);
        maxLayer = std::max<int>(maxLayer, obj->m_editorLayer2);
    }
    return maxLayer + 1;
}

float EditorBridge::lastObjectX(LevelEditorLayer* lel) const {
    float m = 0;
    if (!lel || !lel->m_objects) return 0;
    for (auto obj : CCArrayExt<GameObject*>(lel->m_objects)) {
        if (obj->m_isTrigger) continue;
        m = std::max(m, obj->getPositionX());
    }
    return m;
}

std::filesystem::path EditorBridge::backupLevel(LevelEditorLayer* lel) {
    auto dir = Mod::get()->getSaveDir() / "backups";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto stamp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::string name = "level";
    if (lel && lel->m_level) {
        name.clear();
        for (char c : std::string(lel->m_level->m_levelName)) name += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
        if (name.empty()) name = "level";
    }
    auto path = dir / fmt::format("{}-{}.txt", name, stamp);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f) {
        auto s = levelString(lel);
        f.write(s.data(), std::streamsize(s.size()));
    }
    return path;
}

geode::Result<size_t> EditorBridge::apply(LevelEditorLayer* lel, std::vector<level::LevelObject> objects, std::string const& label) {
    if (!lel) return Err("Not in the editor");
    if (objects.empty()) return Err("Nothing to place");
    AppliedBatch batch;
    batch.label = label;
    if (Mod::get()->getSettingValue<bool>("backup-before-apply")) batch.backup = backupLevel(lel);
    batch.editorLayer = freeEditorLayer(lel);
    std::string str;
    str.reserve(objects.size() * 40);
    for (auto& o : objects) {
        o.editorLayer = batch.editorLayer;
        if (!str.empty()) str += ';';
        str += level::serializeObject(o);
    }
    auto created = lel->createObjectsFromString(gd::string(str), true, true);
    if (!created || created->count() == 0) return Err("The editor did not create any objects");
    lel->addToUndoList(UndoObject::createWithArray(created, UndoCommand::Paste), false);
    for (auto obj : CCArrayExt<GameObject*>(created)) batch.objects.emplace_back(obj);
    size_t n = batch.objects.size();
    m_history.push_back(std::move(batch));
    log::info("AutoLayout: applied {} objects ({}) on editor layer {}", n, label, m_history.back().editorLayer);
    return Ok(n);
}


int EditorBridge::gameplayObjectCount(LevelEditorLayer* lel) const {
    int n = 0;
    if (!lel || !lel->m_objects) return 0;
    for (auto obj : CCArrayExt<GameObject*>(lel->m_objects)) if (!obj->m_isTrigger) ++n;
    return n;
}

namespace {
// our speed index (0 = 0.5x .. 4 = 4x) <-> GD's Speed enum (Normal = 1x, Slow = 0.5x, Fast, Faster, Fastest)
int toGdSpeed(int idx) { static const int m[5] = {1, 0, 2, 3, 4}; return m[std::clamp(idx, 0, 4)]; }
} // namespace

geode::Result<size_t> EditorBridge::applyLevel(LevelEditorLayer* lel, level::Level const& lvl, std::string const& label) {
    if (!lel || !lel->m_levelSettings) return Err("Not in the editor");
    auto st = lel->m_levelSettings;
    StartSettings prev;
    prev.mode = st->m_startMode;
    prev.speed = int(st->m_startSpeed);
    prev.mini = st->m_startMini;
    prev.dual = st->m_startDual;
    prev.flipped = st->m_isFlipped;
    prev.songOffset = st->m_songOffset;
    TapAlong::get().track(lel); // read the guidelines with the offset they were made for
    auto res = apply(lel, lvl.objects, label);
    if (res.isErr()) return res;
    m_history.back().previousStart = prev;
    st->m_startMode = int(lvl.settings.startMode);
    st->m_startSpeed = static_cast<Speed>(toGdSpeed(lvl.settings.startSpeed));
    st->m_startMini = lvl.settings.startMini;
    st->m_startDual = lvl.settings.startDual;
    st->m_isFlipped = lvl.settings.startFlipped;
    // the layout is timed to the song from the level's song offset (0 = the song's start): with
    // any other offset every click would be off by that much
    st->m_songOffset = float(lvl.settings.songOffset);
    lel->levelSettingsUpdated();
    TapAlong::get().showGuidelines(lel); // the tap lines follow the offset
    return res;
}

geode::Result<size_t> EditorBridge::placePieceTest(LevelEditorLayer* lel) {
    if (!lel) return Err("Not in the editor");
    double x0 = 0;
    if (auto lvl = level::Level::fromLevelString(levelString(lel)))
        for (auto& o : lvl->objects) x0 = std::max(x0, o.x);
    x0 = std::ceil((x0 + 300) / 30.0) * 30.0;
    return apply(lel, gen::makePieceTest(x0), "AutoLayout piece test");
}

std::filesystem::path EditorBridge::datasetDir() {
    auto dir = Mod::get()->getSaveDir() / "dataset" / "levels";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

geode::Result<std::filesystem::path> EditorBridge::addToDataset(LevelEditorLayer* lel, std::filesystem::path const& song, int difficulty, int style) {
    if (!lel || !lel->m_level) return Err("Not in the editor");
    auto lvl = level::Level::fromLevelString(levelString(lel));
    if (!lvl) return Err("cannot read the level");
    lvl->name = std::string(lel->m_level->m_levelName);
    static char const* folders[] = {"easy", "normal", "hard", "harder", "insane", "easy_demon", "medium_demon", "hard_demon", "insane_demon", "extreme_demon"};
    auto dir = datasetDir() / (difficulty >= 0 && difficulty <= 9 ? folders[difficulty] : "unrated");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string base;
    for (char c : lvl->name) base += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    if (base.empty()) base = "level";
    base += fmt::format("_{}", int(lel->m_level->m_levelID.value()));
    auto gmd = dir / (base + ".gmd");
    GEODE_UNWRAP(file::writeString(gmd, lvl->toGmd()));
    if (!song.empty()) {
        auto dst = dir / (base + geode::utils::string::pathToString(song.extension()));
        std::filesystem::copy_file(song, dst, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) return Err("level saved, but the song could not be copied: " + ec.message());
    }
    static char const* styles[] = {"auto", "flow", "tight", "chaotic", "minimal", "retro", "modern", "memory"};
    GEODE_UNWRAP(file::writeString(dir / (base + ".json"),
                                   fmt::format("{{\"difficulty\": {}, \"style\": \"{}\"}}", difficulty, styles[std::clamp(style, 0, 7)])));
    return Ok(gmd);
}

bool EditorBridge::undoLast(LevelEditorLayer* lel) {
    if (!lel || m_history.empty()) return false;
    auto batch = std::move(m_history.back());
    m_history.pop_back();
    if (auto ui = lel->m_editorUI) ui->deselectAll();
    size_t removed = 0;
    for (auto& ref : batch.objects) {
        GameObject* obj = ref;
        if (!obj || !lel->m_objects || !lel->m_objects->containsObject(obj)) continue; // already deleted by the user
        lel->removeObject(obj, true);
        ++removed;
    }
    if (batch.previousStart && lel->m_levelSettings) {
        auto st = lel->m_levelSettings;
        st->m_startMode = batch.previousStart->mode;
        st->m_startSpeed = static_cast<Speed>(batch.previousStart->speed);
        st->m_startMini = batch.previousStart->mini;
        st->m_startDual = batch.previousStart->dual;
        st->m_isFlipped = batch.previousStart->flipped;
        st->m_songOffset = batch.previousStart->songOffset;
        lel->levelSettingsUpdated();
        TapAlong::get().showGuidelines(lel);
    }
    log::info("AutoLayout: undo removed {} objects ({})", removed, batch.label);
    return removed > 0;
}

} // namespace al::mod

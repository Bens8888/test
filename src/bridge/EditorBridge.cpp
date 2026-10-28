#include "EditorBridge.hpp"

#include <Geode/binding/EditorUI.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/UndoObject.hpp>

#include <chrono>
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
    log::info("AutoLayout: undo removed {} objects ({})", removed, batch.label);
    return removed > 0;
}

} // namespace al::mod

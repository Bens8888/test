# AutoLayout: Geode / GD 2.2081 API Reference (Windows + macOS)

Target: **Geode SDK v5.11.2**, **Geometry Dash 2.2081**, platforms **win (x64)**, **mac (imac = x86_64, m1 = arm64)**.

Sources used (all local, under `/tmp/claude-0/dl/`):
- `bindings/bindings/2.2081/*.bro` + `bindings/bindings/2.2081/inline/*.cpp`: every signature below was copied from these files.
- `bindings/bindings/include/Geode/Enums.hpp`: enums.
- `geode/` (v5.11.2): loader headers (`loader/include/Geode/...`), CMake, CHANGELOG.
- `geode-sdk_docs/`: `tutorials/migrate-v5.md`, `tutorials/async.md`, `tutorials/popup.md`, `mods/settings.md`, `mods/resources.md`, `mods/savedata.md`, `mods/configuring.md`, `mods/dependencies.md`.
- `geode-sdk_build-geode-mod/`, `geode-sdk_example-mod/`, `geode-sdk_NodeIDs/` (cloned, v1.23.3), `camila314_pathfinder/`, `camila314_gdp/` (physics decomp), `zilko_xdBot/` (2.2074 macro bot), `HJfod_GDShare-mod/` + `hjfod_GMD-API/` (cloned), `maxnut_GDReplayFormat/`, `gddocs/`.

---

## 0. How to read availability (important)

A binding line looks like this:

```
GameObject* createObject(int key, cocos2d::CCPoint position, bool noUndo) = win 0x2d46a0, imac 0xe02d0, m1 0xc5e74, ios 0x35a0ac;
```

The codegen rules come from `bindings/codegen/src/{Shared.hpp,BindingGen.cpp,SourceGen.cpp}`:

| Marker for a platform | Meaning | Call? | Hook? |
|---|---|---|---|
| `win 0x...` / `imac 0x...` / `m1 0x...` | Real address | yes | yes |
| `win inline`, body in `2.2081/inline/<Class>.cpp` under `#if defined(GEODE_IS_WINDOWS)` | GD inlined the function. Geode ships a re-implementation. | yes (runs Geode's copy) | **no** (the function does not exist in the binary, so a `$modify` override is never called by GD) |
| `= inline;` (all platforms) | Header/inline body on every platform | yes | no |
| platform missing, e.g. `= m1 0xd1b6c, imac 0xee240` with no `win` | Declared but **not defined** on that platform | **link error** | no |
| `inline` with no body in `inline/` | Same: link error | no | no |
| `[[missing(win, mac)]]` | Not present | no | no |

**Cocos2d / FMOD / DS_Dictionary / ZipUtils classes** are tagged `[[link(win, android)]]` (FMOD: `[[link(win, android, mac)]]`). On Windows they resolve through `libcocos2d.dll` exports, so they have no `win` address and still work. On macOS, cocos is statically linked into GD, so each function needs `imac`/`m1` addresses. I checked every cocos class used below (CCNode, CCSprite, CCMenu, CCLabelBMFont, CCScale9Sprite, CCDrawNode, CCFileUtils, CCDirector, ...). All have mac addresses, except the **variadic** `CCMenu::create(CCMenuItem*, ...)` and `alignItemsIn*(...)`, which are `mac inline`. Use `CCMenu::create()` followed by `addChild` instead.

Legend used in this doc: **A** = address (callable and hookable), **I** = inline re-implementation (callable, NOT hookable on that platform), **X** = unavailable.

I used a small checker script (`avail.py` in my scratchpad) to cross-check every function listed below against the `.bro` files and `inline/` bodies. **Every API recommended in this document is available on win + imac + m1.** The few gaps are listed in section 9.

---

## 1. Project setup

### 1.1 mod.json (Geode 5 schema)

Required keys: `geode`, `gd`, `id`, `name`, `version`, `developer` (or `developers`). Geode 5 changes compared with v4:
- `dependencies` and `incompatibilities` are **objects only**. The array form was removed.
- `"importance"` was replaced by `"required": bool` (dependencies) and `"breaking": bool` (incompatibilities).
- The `path` setting type was removed. Use `file` or `folder`.
- **`keybind` is a native setting type in Geode 5.** Custom Keybinds is no longer needed (from `migrate-v5.md`).

```jsonc
{
  "geode": "5.11.2",
  "gd": { "win": "2.2081", "mac": "2.2081" },
  "id": "yourname.autolayout",
  "name": "AutoLayout",
  "version": "v0.1.0",
  "developer": "YourName",
  "description": "Generate a level layout from a song.",
  "tags": ["editor", "music", "utility"],
  "links": { "source": "https://github.com/you/autolayout" },

  "dependencies": {
    // v1.22.0 = first Node IDs release for Geode 5.0.0 + GD 2.2081 (NodeIDs changelog)
    "geode.node-ids": ">=v1.22.0"
    // optional .gmd export helper (Geode 5, 2.2081, all platforms):
    // "hjfod.gmd-api": { "version": ">=v1.5.0", "required": true }
  },

  "resources": {
    "sprites": ["resources/sprites/*.png"],          // used as "name.png"_spr
    "files":   ["resources/models/*.onnx", "resources/models/*.json"], // flattened into getResourcesDir()
    "folders": ["resources/presets"]                 // copied recursively, keeps structure
  },

  "settings": {
    "open-panel": {
      "type": "keybind",
      "name": "Open AutoLayout",
      "default": { "win": "Ctrl+Alt+L", "mac": "Cmd+Alt+L" },  // per-platform default allowed
      "category": "editor"
    },
    "tap-key": {
      "type": "keybind",
      "name": "Tap-along key",
      "default": ["Space", "J"],
      "category": "universal",
      "allow-in-text-inputs": false
    },
    "difficulty": {
      "type": "int", "name": "Difficulty", "default": 3, "min": 1, "max": 10,
      "control": { "slider": true }
    },
    "custom-song": {
      "type": "file", "name": "Custom song", "default": "",
      "control": { "dialog": "open",
                   "filters": [ { "files": ["*.mp3", "*.ogg", "*.wav"], "description": "Audio" } ] }
    },
    "export-dir": { "type": "folder", "name": "Export folder", "default": "{mod_save_dir}/exports" }
  }
}
```

Notes:
- The `geode` value: the example template uses the bare version (`"5.11.2"`), the docs use `"v1.0.0"`. Both forms are accepted. The build action's default `sdk: given` reads this field, so pin it to `5.11.2`.
- Setting value types in C++: `int` maps to `int64_t`, `float` maps to `double`, `file`/`folder` map to `std::filesystem::path`, and `keybind` maps to `std::vector<geode::Keybind>`.
- `listenForSettingChanges<T>(key, cb)` **requires the explicit `<T>` in v5**. The settings.md snippet without `<T>` is outdated (header `loader/SettingV3.hpp:845`).
- `resources.files` are copied **flattened** (only the file name is kept; CLI `package.rs`). Read them from `Mod::get()->getResourcesDir() / "model.onnx"`. Use `"folders"` if you need the directory structure.

### 1.2 CMakeLists.txt template

```cmake
cmake_minimum_required(VERSION 3.25)   # Geode's own root CMakeLists requires 3.25 (FATAL_ERROR)
set(CMAKE_CXX_STANDARD 23)             # Geode v5 REQUIRES C++23 (loader target: cxx_std_23 PUBLIC)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")   # universal mac build; must be set BEFORE project()
endif()
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(AutoLayout VERSION 0.1.0)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

# Optional arc (async) features: must come BEFORE add_subdirectory(GEODE_SDK)
# set(ARC_FEATURE_FULL ON CACHE BOOL "" FORCE)

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# ---- extra static libraries (pure C++, no Geode headers: unit-testable on host) ----
add_subdirectory(core)          # defines target autolayout-core (STATIC)
# third-party via CPM (CPM is already available after the Geode add_subdirectory)
CPMAddPackage("gh:maxnut/GDReplayFormat#<pin-a-commit>")   # target: libGDR (INTERFACE, C++23)

target_link_libraries(${PROJECT_NAME} autolayout-core libGDR)

setup_geode_mod(${PROJECT_NAME})   # signature: setup_geode_mod(target [DONT_INSTALL] [EXTERNALS id:ver ...] [LINK_TYPE ...])
```

`core/CMakeLists.txt`:

```cmake
add_library(autolayout-core STATIC src/analysis.cpp src/generator.cpp)
target_include_directories(autolayout-core PUBLIC include)
target_compile_features(autolayout-core PUBLIC cxx_std_23)
set_target_properties(autolayout-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Windows: Geode mods use the dynamic CRT (/MD). Keep static libs on the same runtime:
#   set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL") in the root (pathfinder does this)
# If core needs Geode headers: target_link_libraries(autolayout-core PRIVATE geode-sdk)
```

Pitfalls:
- `CMAKE_OSX_ARCHITECTURES` must be set before `project()` so that sub-libraries are also built universal.
- Do **not** compile static libs with `-fno-rtti` if they share types with the mod. Geode uses RTTI-like `typeinfo_cast`. Pathfinder's gd-sim only does this in Release with isolated types.

### 1.3 GitHub Actions (from `geode-sdk/build-geode-mod/examples/multi-platform.yml`, trimmed to win + mac)

```yaml
name: Build Geode Mod

on:
  workflow_dispatch:
  push:
    branches:
      - "**"

jobs:
  build:
    strategy:
      fail-fast: false
      matrix:
        config:
        - name: Windows
          os: windows-latest

        - name: macOS
          os: macos-latest

    name: ${{ matrix.config.name }}
    runs-on: ${{ matrix.config.os }}

    steps:
      - uses: actions/checkout@v7          # README example pins v7; pathfinder uses v4

      - name: Build the mod
        uses: geode-sdk/build-geode-mod@main
        with:
          combine: true
          target: ${{ matrix.config.target }}
          # sdk: given (default) -> uses "geode" version in mod.json
          # build-config: Release (default); export-pdb / bundle-pdb for crash symbols

  package:
    name: Package builds
    runs-on: ubuntu-latest
    needs: ['build']

    steps:
      - uses: geode-sdk/build-geode-mod/combine@main
        id: build

      - uses: actions/upload-artifact@v7
        with:
          name: Build Output
          path: ${{ steps.build.outputs.build-output }}
```

- Each build job uploads an artifact named **`geode-build-<target_id>`** (`geode-build-win`, `geode-build-mac`; see action.yml line 347). The combine job downloads them all, merges them into one `.geode`, and you upload that as **"Build Output"**.
- Compilers (from `action.yml`): `cmake -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++` on **every** runner. On Windows this is LLVM clang targeting the MSVC ABI/STL. On macOS it is Apple clang from Xcode with `-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0`, building universal arm64+x86_64. LTO is on by default (`use-lto: true`).
- Geode's CMake rejects Clang < 19 and MSVC < 19.44 (needed for coroutine/async support). **C++23 is required**, which also covers C++20. Deducing-`this` lambdas (`[](this auto self) -> arc::Future<>`), as used by pathfinder, need C++23.

---

## 2. Editor integration

### 2.1 Getting the editor

```cpp
static LevelEditorLayer* LevelEditorLayer::get() = inline;   // -> GameManager::sharedState()->m_levelEditorLayer   [A on all, inline body]
static EditorUI* EditorUI::get() = inline;                   // -> LevelEditorLayer::get()->m_editorUI
EditorUI*        LevelEditorLayer::m_editorUI;
LevelEditorLayer* EditorUI::m_editorLayer;
GJGameLevel*      GJBaseGameLayer::m_level;          // LevelEditorLayer : GJBaseGameLayer
LevelSettingsObject* GJBaseGameLayer::m_levelSettings;
cocos2d::CCArray* GJBaseGameLayer::m_objects;        // ALL objects in the editor (GameObject*)
cocos2d::CCLayer* GJBaseGameLayer::m_objectLayer;    // parent for world-space overlays (CCDrawNode etc.)
short LevelEditorLayer::m_currentLayer;              // current editor layer (-1 = "All")
```

### 2.2 Adding a button to the editor UI

Hook `EditorUI::init(LevelEditorLayer*)` (win 0xdf390, imac 0x9070, m1 0x7d7c: **A**). Node IDs assigns IDs inside its own `EditorUI::init` hook. Depending on `geode.node-ids` guarantees its hook runs first, and you can also call `NodeIDs::provideFor(this)`.

EditorUI menu IDs (from `geode-sdk_NodeIDs/src/EditorUI.cpp`):

| ID | What / layout |
|---|---|
| `settings-menu` | top-right row: `pause-button`, `settings-button`, `edit-tools-button`, `grid-controls-button` (SimpleRowLayout, RightToLeft). **Best spot for a mod button.** |
| `undo-menu` | top-left: `undo-button`, `redo-button`, `delete-button`, `hide-ui-button` (SimpleRowLayout) |
| `playtest-menu` | `playtest-button`, `stop-playtest-button` |
| `playback-menu` | `music-playback-button` |
| `editor-buttons-menu` | right-side column of edit buttons (copy-paste, edit-object, ...). ColumnLayout, buttons are 37 wide |
| `toolbar-categories-menu` | build/edit/delete/view mode buttons |
| `toolbar-toggles-menu` | swipe/rotate/free-move/snap |
| `layer-menu`, `lock-layer-menu`, `link-menu`, `zoom-menu`, `delete-category-menu` | misc |

```cpp
#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
using namespace geode::prelude;

class $modify(ALEditorUI, EditorUI) {
    bool init(LevelEditorLayer* lel) {
        if (!EditorUI::init(lel)) return false;
        if (auto menu = this->getChildByID("settings-menu")) {
            auto spr = CircleButtonSprite::createWithSpriteFrameName("GJ_musicOnBtn_001.png", 1.f,
                         CircleBaseColor::Green, CircleBaseSize::Small);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(ALEditorUI::onAutoLayout));
            btn->setID("autolayout-button"_spr);
            menu->addChild(btn);
            menu->updateLayout();
        }
        return true;
    }
    void onAutoLayout(CCObject*) { /* AutoLayoutPopup::create(m_editorLayer)->show(); */ }
};
```

Note: the local NodeIDs clone is v1.23.3, which targets 2.2091. IDs for 2.2081 come from v1.22.x. The IDs above existed in both.

### 2.3 Creating objects

| Signature (2.2081) | win | imac | m1 |
|---|---|---|---|
| `GameObject* LevelEditorLayer::createObject(int key, cocos2d::CCPoint position, bool noUndo)` | A 0x2d46a0 | A | A |
| `GameObject* EditorUI::createObject(int objectID, cocos2d::CCPoint position)` | A 0x10ff20 | A | A |
| `cocos2d::CCArray* LevelEditorLayer::createObjectsFromString(gd::string const& str, bool noUndo, bool noLimit)` | A 0x2d4020 | A 0xdf360 | A 0xc5090 |
| `cocos2d::CCArray* EditorUI::pasteObjects(gd::string str, bool withColor, bool noUndo)` | A 0x1135b0 | A | A |
| `GameObject* LevelEditorLayer::addObjectFromVector(gd::vector<gd::string>& values, gd::vector<void*>& exists)` | **I** | A | A |
| `void LevelEditorLayer::createObjectsFromSetup(gd::string& setup)` (level load path) | A | A | A |
| `gd::string EditorUI::copyObjects(cocos2d::CCArray* objects, bool copyColors, bool sort)` | A | A | A |
| `virtual gd::string GameObject::getSaveString(GJBaseGameLayer* layer)` | A | A | A |
| `static GameObject* GameObject::createWithKey(int key)` | A | A | A |

**Recommendation: build a level-string fragment and call `createObjectsFromString`.** It exists on all platforms with real addresses. It goes through GD's own `GameObject::objectFromVector` parser, which handles every property (groups, z, colours, HSV, scale, trigger fields). It also handles section registration, `addSpecial`, colour sprites and z-batch placement. Setting members by hand after creation misses those side effects.

```cpp
// object string: "key,value,key,value,..." ; objects separated by ';' (no header!)
std::string s = fmt::format("1,{},2,{},3,{},20,{},57,{}", id, x, y, editorLayer, groupsDotSeparated);
auto lel = LevelEditorLayer::get();
size_t undoBefore = lel->m_undoObjects->count();
CCArray* created = lel->createObjectsFromString(gd::string(s), /*noUndo*/ true, /*noLimit*/ true);
// one undo step for the whole batch:
lel->addToUndoList(UndoObject::createWithArray(created, UndoCommand::Paste), /*keepRedo*/ false);
```

**Risk:** the exact semantics of `noUndo`/`noLimit` are inferred from parameter names (no decomp available locally). Verify once in-game by comparing `m_undoObjects->count()` before and after the call with `noUndo=false`. If GD adds its own undo entry, drop the manual one. `EditorUI::pasteObjects(str, true, false)` is the "user paste" path: it creates the undo entry and (most likely) selects the new objects. Use it if you want GD's exact UX.

Common object-string keys (gddocs `level-components/level-string.md`):
`1` id, `2` x, `3` y, `4` flipX, `5` flipY, `6` rotation (deg, CW+), `20` editor layer, `61` editor layer 2, `21` main colour channel, `22` detail colour channel, `24` Z layer, `25` Z order, `32` scale (legacy uniform), `128` scaleX, `129` scaleY, `57` groups (`.`-separated), `64` don't fade, `67` don't enter, `116` no effects.
Triggers: `51` target group, `10` duration, `28`/`29` move offset X/Y, `30` easing, `85` easing rate, `11` touch-triggered, `62` spawn-triggered, `87` multi-trigger, `71` center group, `35` opacity, `45/46/47` pulse fade-in/hold/out, `7/8/9` RGB, `50` copied colour.
Coordinates: 1 grid cell = 30 units. The centre of the bottom row on the ground is `y = 15`, and column *c* is at `x = 15 + 30*c`.

### 2.4 Setting properties on an existing GameObject (if you must)

Fields (GameObject, all platforms):
```cpp
int m_objectID;  int m_uniqueID;  GameObjectType m_objectType;  GameObjectClassType m_classType;  bool m_isTrigger;
short m_editorLayer; short m_editorLayer2;
ZLayer m_zLayer; ZLayer m_defaultZLayer; int m_zOrder; int m_defaultZOrder;
GJSpriteColor* m_baseColor;   GJSpriteColor* m_detailColor;   // GJSpriteColor{ int m_colorID; int m_defaultColorID; float m_opacity; ccHSVValue m_hsv; bool m_usesHSV; ... }
float m_scaleX; float m_scaleY; float m_customScaleX; float m_customScaleY;
bool m_isFlipX; bool m_isFlipY;
std::array<short, 10>* m_groups;  short m_groupCount;
bool m_isDontFade; bool m_isDontEnter; bool m_hasNoGlow; bool m_isNoTouch; bool m_isHide; bool m_isHighDetail; bool m_isPassable;
```
Methods (all **A** on win/imac/m1 unless noted):
```cpp
virtual void setPosition(cocos2d::CCPoint const& position);
virtual void setRotation(float rotation);
virtual void setFlipX(bool flipX);  virtual void setFlipY(bool flipY);
virtual void setRScaleX(float scaleX); virtual void setRScaleY(float scaleY); virtual void setRScale(float scale);
void updateCustomScaleX(float scaleX); void updateCustomScaleY(float scaleY);
virtual int  addToGroup(int id);     // object-side; returns 1 if added
virtual void removeFromGroup(int id);
void copyGroups(GameObject* object);
int getGroupID(int index);            // I on win
gd::string getGroupString();          // I on win
// layer-side registration (needed so triggers find the object):
virtual void LevelEditorLayer::addToGroup(GameObject* object, int groupID, bool triggerGroup);  // A
virtual void LevelEditorLayer::removeFromGroup(GameObject* object, int groupID);                // A
void LevelEditorLayer::addObjectToGroup(GameObject* object, int group); // I on win: object->addToGroup + layer addToGroup
int  LevelEditorLayer::getNextFreeGroupID(const gd::unordered_set<int>& exclude);              // A
int  LevelEditorLayer::getNextColorChannel();                                                    // A
static void LevelEditorLayer::updateObjectLabel(GameObject* object);                            // A
```
`ZLayer` enum: `B5=-5, B4=-3, B3=-1, B2=1, B1=3, PortalBack=4, Default=0, T1=5, T2=7, T3=9, T4=11`.

Risk: changing `m_zLayer`/colour IDs after creation does not re-parent the sprite into the right batch node or refresh colour sprites. Prefer the string route, or delete and recreate the object.

### 2.5 Trigger objects (EffectGameObject : EnhancedGameObject : GameObject)

Cast with `typeinfo_cast<EffectGameObject*>(obj)` (or check `obj->m_isTrigger`). Fields (with level-string key):
```cpp
float m_duration;            // 10
float m_opacity;             // 35
int   m_targetGroupID;       // 51
int   m_centerGroupID;       // 71
bool  m_isTouchTriggered;    // 11
bool  m_isSpawnTriggered;    // 62
bool  m_isMultiTriggered;    // 87
cocos2d::CCPoint m_moveOffset;  // 28, 29
EasingType m_easingType;     // 30
float m_easingRate;          // 85
bool  m_lockToPlayerX, m_lockToPlayerY;   // 58, 59
bool  m_useMoveTarget;       // 100
float m_rotationDegrees; int m_times360; bool m_lockObjectRotation; // 68, 69, 70
float m_fadeInDuration, m_holdDuration, m_fadeOutDuration;  // 45, 46, 47
cocos2d::ccColor3B m_triggerTargetColor;  bool m_usesBlending; int m_copyColorID;  ccHSVValue m_hsvValue;
float m_shakeStrength, m_shakeInterval;   // 75, 84
bool  m_targetPlayer1, m_targetPlayer2;
virtual gd::string getSaveString(GJBaseGameLayer* layer);   // A
static EffectGameObject* create(char const* frame);          // I on win
```
`EasingType`: 0 None, 1 EaseInOut, 2 EaseIn, 3 EaseOut, 4..6 Elastic, 7..9 Bounce, 10..12 Exponential, 13..15 Sine, 16..18 Back.
**Recommendation:** emit triggers as strings, e.g. move trigger `1,901,2,X,3,Y,51,G,28,DX,29,DY,10,DUR,30,EASE,85,RATE`. Verify the move-offset units once with a manually placed trigger. I believe they are game units (30 = 1 block), but check `getSaveString()` output.

### 2.6 Removing objects

```cpp
void LevelEditorLayer::removeObject(GameObject* object, bool noUndo);      // A all
void EditorUI::deleteObject(GameObject* object, bool noUndo);              // I on win (removeObject + deactivate rotate/scale/transform controls), A mac
void LevelEditorLayer::removeAllObjects();                                 // I on win; clears undo/redo too!
void LevelEditorLayer::removeAllObjectsOfType(int objectID);               // A
void EditorUI::deselectAll();                                              // A  (call before removing selected objects)
void EditorUI::onDeleteSelected(cocos2d::CCObject* sender);                // A
```
Bulk delete with a single undo step:
```cpp
ui->deselectAll();
lel->addToUndoList(UndoObject::createWithArray(arr, UndoCommand::DeleteMulti), false);
for (auto o : CCArrayExt<GameObject*>(arr)) lel->removeObject(o, true);
```

### 2.7 Undo system

```cpp
cocos2d::CCArray* LevelEditorLayer::m_undoObjects;   // CCArray<UndoObject*>, capped at 200 (1000 if m_increaseMaxUndoRedo)
cocos2d::CCArray* LevelEditorLayer::m_redoObjects;
void LevelEditorLayer::addToUndoList(UndoObject* object, bool keepRedo);   // I on win
void LevelEditorLayer::addToRedoList(UndoObject* object);                  // I on win
void LevelEditorLayer::undoLastAction();  void LevelEditorLayer::redoLastAction();   // I on win (handleAction)
void LevelEditorLayer::handleAction(bool undo, cocos2d::CCArray* undoObjects);       // A
UndoObject* EditorUI::createUndoObject(UndoCommand command, bool addToList);         // A (uses current selection)
void EditorUI::undoLastAction(cocos2d::CCObject* object);                            // A (button handler)

class UndoObject : CCObject {
    static UndoObject* create(GameObject* object, UndoCommand command);                       // I on win
    static UndoObject* createWithArray(cocos2d::CCArray* array, UndoCommand command);         // I on win
    static UndoObject* createWithTransformObjects(cocos2d::CCArray* objects, UndoCommand command); // I on win
    GameObjectCopy* m_objectCopy; UndoCommand m_command; cocos2d::CCArray* m_objects; bool m_redo; bool m_undoTransform; GJTransformState m_transformState;
};
enum class UndoCommand { Delete = 1, New = 2, Paste = 3, DeleteMulti = 4, Transform = 5, Select = 6 };
```

### 2.8 Level settings / song / level string

`LevelSettingsObject` (`lel->m_levelSettings`), with level-string header key:
```cpp
int m_startMode;   // kA2  gamemode: 0 cube,1 ship,2 ball,3 ufo,4 wave,5 robot,6 spider,7 swing
Speed m_startSpeed;// kA4  enum Speed { Normal=0, Slow=1, Fast=2, Faster=3, Fastest=4 }
bool m_startMini;  // kA3
bool m_startDual;  // kA8
bool m_mirrorMode; bool m_rotateGameplay; bool m_twoPlayerMode /*kA10*/; bool m_platformerMode /*kA22*/;
float m_songOffset;// kA13 (seconds)
bool m_fadeIn, m_fadeOut; bool m_isFlipped /*kA11*/; bool m_reverseGameplay;
GJGameLevel* m_level;
gd::string getSaveString();                                   // A
static LevelSettingsObject* objectFromString(gd::string const& str);  // I on win
virtual void LevelEditorLayer::levelSettingsUpdated();        // A (call/hook after changing settings)
```
Song fields live on **GJGameLevel**, not on LevelSettingsObject:
```cpp
int GJGameLevel::m_audioTrack;       // official song index (k8)  (used when m_songID == 0)
int GJGameLevel::m_songID;           // custom song: Newgrounds or music-library ID (k45)
gd::string GJGameLevel::m_songIDs;   // k104: comma-separated list of all songs used (2.2 song triggers)
gd::string GJGameLevel::m_sfxIDs;    // k105
gd::string GJGameLevel::getAudioFileName();      // A  (file the level plays)
gd::string GJGameLevel::getSongName();           // A
gd::string LevelEditorLayer::getSongIDs(bool& multipleSongs);   // A (computed from triggers)
gd::string LevelEditorLayer::getSFXIDs();                       // A
```
Level string:
```cpp
gd::string LevelEditorLayer::getLevelString();   // A. Live, UNCOMPRESSED "header;obj;obj;..."
gd::string GJGameLevel::m_levelString;          // saved, COMPRESSED (base64 + gzip, starts "H4sI")
static gd::string cocos2d::ZipUtils::decompressString(gd::string const& data, bool encrypt, int encryptionKey); // win via dll, A mac
static gd::string cocos2d::ZipUtils::compressString(gd::string const& data, bool encrypt, int encryptionKey);
// decompress: ZipUtils::decompressString(level->m_levelString, false, 0)
// (pathfinder passes (…, true, 0); with key 0 the XOR step is a no-op, so both work)
```
There is no "set level string" call for a live editor. Either `removeAllObjects()` + `createObjectsFromString(...)` (note: this wipes the undo history), or set `level->m_levelString = ZipUtils::compressString(raw, false, 0)` before opening the editor (`LevelEditorLayer::scene(GJGameLevel*, bool noUI)`, I on win).

Time ↔ X mapping (critical for a song-driven layout; this is what GD's editor uses, and it respects speed portals):
```cpp
virtual float LevelEditorLayer::timeForPos(cocos2d::CCPoint position, int order, int channel, bool songTriggers, int id); // A (pass 0,0,false,0)
virtual cocos2d::CCPoint LevelEditorLayer::posForTime(float time);                                                      // A
static float LevelTools::timeForPos(cocos2d::CCPoint position, cocos2d::CCArray* objects, int speed, int order, int channel, bool songTriggers, bool platformer, bool ignoreWarp, bool ignoreRotate, int id); // A
static cocos2d::CCPoint LevelTools::posForTimeInternal(float time, cocos2d::CCArray* objects, int speed, bool platformer, bool ignoreMinorAxis, bool ignoreWarp, int& rotateChannel, int unused); // A
static float LevelTools::valueForSpeedMod(int speed);   // I on win: 0->311.5801, 1->251.16008, 2->387.42014, 3->468.00015, 4->576.00018 (units/s)
```
(The gddocs Speed table labels 0 and 1 the wrong way round. Trust `valueForSpeedMod`. Speed portal object IDs: 200 = 0.5x, 201 = 1x, 202 = 2x, 203 = 3x, 1334 = 4x.)

### 2.9 Playtest

```cpp
void LevelEditorLayer::onPlaytest();        // A
void LevelEditorLayer::onStopPlaytest();    // A
void LevelEditorLayer::onPausePlaytest();   // I on win
void LevelEditorLayer::onResumePlaytest();  // I on win
void EditorUI::onPlaytest(cocos2d::CCObject* sender);      // A (the button)
void EditorUI::onStopPlaytest(cocos2d::CCObject* sender);  // A
void EditorUI::playtestStopped();                          // A (good hook point)
void EditorUI::onPlayback(cocos2d::CCObject* sender);      // A (music preview button)
PlaybackMode GJBaseGameLayer::m_playbackMode;              // enum PlaybackMode { Not=0, Playing=1, Paused=2 }
```

---

## 3. Song files and FMOD

### 3.1 Resolving the song file

```cpp
static MusicDownloadManager* MusicDownloadManager::sharedState();     // A
gd::string MusicDownloadManager::pathForSong(int id);                 // A  (NG and music-library songs)
gd::string MusicDownloadManager::pathForSongFolder(int id);           // A
gd::string MusicDownloadManager::pathForSFX(int id);                  // A
gd::string MusicDownloadManager::pathForSFXFolder(int id);            // A
bool MusicDownloadManager::isSongDownloaded(int id);                  // A
SongInfoObject* MusicDownloadManager::getSongInfoObject(int id);      // A
bool MusicDownloadManager::isResourceSong(int id);                    // I on win (song shipped in Resources)
bool MusicDownloadManager::isMusicLibraryLoaded();                    // I on win
void MusicDownloadManager::downloadSong(int id);                      // A
static gd::string LevelTools::getAudioFileName(int id);               // A (official track -> e.g. "StereoMadness.mp3", relative)
static gd::string LevelTools::getAudioTitle(int id);                  // A
static SongInfoObject* LevelTools::getSongObject(int id);             // A
virtual gd::string cocos2d::CCFileUtils::fullPathForFilename(char const* pszFileName, bool skipSuffix); // win dll, A mac
```
Recommended resolver:
```cpp
std::filesystem::path resolveSong(GJGameLevel* lvl) {
    if (lvl->m_songID > 0) {
        auto mdm = MusicDownloadManager::sharedState();
        if (!mdm->isSongDownloaded(lvl->m_songID)) return {};   // offer mdm->downloadSong(id)
        return std::string(mdm->pathForSong(lvl->m_songID));
    }
    auto rel = LevelTools::getAudioFileName(lvl->m_audioTrack);
    return std::string(CCFileUtils::get()->fullPathForFilename(rel.c_str(), false));
}
```
`GJGameLevel::getAudioFileName()` does the same branch internally (inferred from GMD-API, which zips that path as the song file). It may return a relative name for official songs, so pass it through `fullPathForFilename`.
Newgrounds and music-library songs both go through `pathForSong`. Library songs use large IDs (≥ 10,000,000) and their metadata is in `SongInfoObject`:
```cpp
int m_songID; gd::string m_songName; gd::string m_artistName; gd::string m_songUrl; float m_fileSize;
int m_duration /*seconds*/; int m_BPM; int m_nongType; bool m_isNew; gd::set<int> m_tags;
```
Risk: NONG/Jukebox-style mods replace songs by hooking these functions, so the file you get may be a replacement. That is usually what the user wants.
`CustomSongWidget` (song box in level settings): `SongInfoObject* m_songInfoObject; int m_customSongID; bool m_isRobtopSong; bool m_isMusicLibrary; gd::map<int,bool> m_songs;`. Node IDs: `buttons-menu`, `play-song-button`, `song-name-label`, `artist-label`, `id-and-size-label`, `use-button`, `download-button`.

### 3.2 Playing, pausing and position (FMODAudioEngine)

```cpp
static FMODAudioEngine* FMODAudioEngine::sharedEngine();       // I on win (static instance), A mac
void FMODAudioEngine::playMusic(gd::string path, bool shouldLoop, float fadeInTime, int channel);   // A
void FMODAudioEngine::loadMusic(gd::string path, float speed, float unknown, float volume, bool shouldLoop, int musicID, int channelID, bool dontReset); // A
void FMODAudioEngine::pauseMusic(int musicChannel);     // I on win (getActiveMusicChannel(id)->setPaused(true))
void FMODAudioEngine::resumeMusic(int musicChannel);    // I on win
void FMODAudioEngine::stopMusic(int id);                // I on win
void FMODAudioEngine::stopAllMusic(bool clear);         // A
bool FMODAudioEngine::isMusicPlaying(int musicID);      // A
unsigned int FMODAudioEngine::getMusicTimeMS(int channel);   // A
float FMODAudioEngine::getMusicTime(int channel);            // I on win (= getMusicTimeMS/1000)
unsigned int FMODAudioEngine::getMusicLengthMS(int channel); // A
void FMODAudioEngine::setMusicTimeMS(unsigned int time, bool dontWait, int musicID); // A
FMOD::Channel* FMODAudioEngine::getActiveMusicChannel(int musicID);  // I on win
FMOD::System* FMODAudioEngine::m_system;  FMOD::ChannelGroup* m_backgroundMusicChannel;  float m_musicVolume;
int FMODAudioEngine::playEffect(gd::string path);   // A (SFX)
```
The main level music is music ID/channel **0**. Example preview:
```cpp
auto fae = FMODAudioEngine::sharedEngine();
fae->playMusic(path.string(), false, 0.f, 0);
fae->setMusicTimeMS(startMs, true, 0);
// per frame: auto ms = fae->getMusicTimeMS(0);
fae->pauseMusic(0); fae->resumeMusic(0); fae->stopMusic(0);
```
The FMOD C++ API itself is linked on win+mac (`[[link(win, android, mac)]]`, macOS links `libfmod.dylib`). For a private, independent preview that does not fight GD's music state:
```cpp
FMOD::Sound* snd; FMOD::Channel* ch;
fae->m_system->createStream(path.string().c_str(), FMOD_DEFAULT | FMOD_ACCURATETIME, nullptr, &snd);
fae->m_system->playSound(snd, nullptr, /*paused*/ false, &ch);
unsigned pos; ch->getPosition(&pos, FMOD_TIMEUNIT_MS); ch->setPaused(true); snd->release();
```
Tap-along accuracy: FMOD position only advances once per mix block (about 10–20 ms). Store the input's own timestamp from Geode's `KeyboardInputEvent` (`KeyboardInputData{ double timestamp; enumKeyCodes key; Action action; ... }`, `utils/Keyboard.hpp`) or from the keybind callback (`double timestamp`). Map it to song time by anchoring `(fmodPosMs, timestampAtSample)` pairs, not by reading the position at callback time.
Waveform decoding: do it **off the main thread** in your core lib (minimp3 / stb_vorbis are already in `/tmp/claude-0/dl/`). Alternatively, create your own `FMOD::System` with `FMOD_OPENONLY` + `Sound::readData`. Never use GD's `m_system` off-thread.

---

## 4. Gameplay hooks: recorder, replay, validation

### 4.1 Signatures (2.2081, all A on win/imac/m1 unless noted)

```cpp
// GJBaseGameLayer (PlayLayer and LevelEditorLayer both derive from it)
virtual void update(float dt);                                       // win 0x237850, imac 0x151310, m1 0x1229e8
void processCommands(float dt, bool isHalfTick, bool isLastTick);    // win 0x239c60, imac 0x1535e0, m1 0x124490  ** 2.2081 has 3 args **
void handleButton(bool down, int button, bool isPlayer1);            // win 0x2338e0, imac 0x14aba0, m1 0x11d578
void queueButton(int button, bool push, bool isPlayer2, double timestamp); // I on win (pushes PlayerButtonCommand into m_queuedButtons)
double getModifiedDelta(float dt);                                   // A
void updateCamera(float dt);                                         // A (called once per physics step: pathfinder's per-step hook)
int  checkCollisions(PlayerObject* object, float dt, bool ignoreDamage);   // A
void playerTouchedRing(PlayerObject* player, RingObject* object);    // A  (overlap with an orb)
void bumpPlayer(PlayerObject* player, EffectGameObject* object);     // A  (pads)
void playerWillSwitchMode(PlayerObject* player, GameObject* object); // A  (portals)
void playerTouchedTrigger(PlayerObject* player, EffectGameObject* object); // A
void playerTouchedObject(PlayerObject* player, GameObject* object);  // I on win: NOT hookable on Windows
void toggleDualMode(GameObject* object, bool dual, PlayerObject* player, bool noEffects); // A
void teleportPlayer(TeleportPortalObject* object, PlayerObject* player);                  // A
void resetLevelVariables();                                          // A
static GJBaseGameLayer* get();                                       // inline (GameManager::get()->m_gameLayer)
// fields
GJGameState m_gameState;  PlayerObject* m_player1; PlayerObject* m_player2;
bool m_isPracticeMode; bool m_isTestMode; bool m_isEditor; bool m_isPlatformer; bool m_playerDied; bool m_started;
int m_attempts; int m_clicks; int m_currentStep; double m_timePlayed; double m_extraDelta; double m_timestamp;
gd::vector<PlayerButtonCommand> m_queuedButtons;   // PlayerButtonCommand{ PlayerButton m_button; bool m_isPush; bool m_isPlayer2; int m_step; double m_timestamp; }
bool m_isBetweenSteps; bool m_clickBetweenSteps; bool m_clickOnSteps;   // 2.2081 native "click between steps"
GameObject* m_anticheatSpike;

// PlayLayer
static PlayLayer* get();                                             // inline
bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects);   // A
virtual void resetLevel();                                           // A win 0x3b8eb0
void resetLevelFromStart(); void fullReset();                        // A
void loadFromCheckpoint(CheckpointObject* object);                   // A win 0x3b7640
CheckpointObject* markCheckpoint(); CheckpointObject* createCheckpoint();  // A
void storeCheckpoint(CheckpointObject* checkpoint); void removeCheckpoint(bool first); // A
CheckpointObject* getLastCheckpoint(); CheckpointObject* loadLastCheckpoint();         // I on win
void togglePracticeMode(bool practiceMode);                          // A
virtual void destroyPlayer(PlayerObject* player, GameObject* object);// A
void levelComplete();  void setupHasCompleted();  void startMusic(); // A
void pauseGame(bool unfocused); void resume(); void onQuit();        // A
virtual void postUpdate(float dt);                                   // A
float getCurrentPercent();                                           // A
bool m_hasCompletedLevel; int m_jumps; double m_attemptTime; cocos2d::CCArray* m_checkpointArray; CheckpointObject* m_currentCheckpoint; bool m_isPaused;

// CheckpointObject : CCNode
GJGameState m_gameState; PlayerCheckpoint* m_player1Checkpoint; PlayerCheckpoint* m_player2Checkpoint;
```
`PlayerButton` enum: `Jump = 1, Left = 2, Right = 3` (matches GDR's button byte).

### 4.2 PlayerObject fields (all present, public on win/mac)

```cpp
double m_yVelocity;  float m_playerSpeed /*0.7,0.9,1.1,1.3,1.6 for 0.5x..4x*/;  float m_vehicleSize /*1.0 normal, 0.6 mini*/;
bool m_isShip, m_isBird /*UFO*/, m_isBall, m_isDart /*wave*/, m_isRobot, m_isSpider, m_isSwing;   // cube = none set
bool m_isUpsideDown, m_isOnGround, m_isDead, m_isGoingLeft, m_isSideways, m_isDashing, m_isOnSlope, m_isLocked;
bool m_isPlatformer, m_isSecondPlayer;  double m_gravity; float m_gravityMod; double m_speedMultiplier;
cocos2d::CCPoint m_position;  cocos2d::CCPoint m_lastPortalPos;  GameObject* m_lastActivatedPortal;
cocos2d::CCArray* m_touchingRings;  gd::unordered_set<int> m_touchedRings;  bool m_touchedRing; bool m_jumpBuffered;
gd::map<int, bool> m_holdingButtons;
```
Methods: `bool pushButton(PlayerButton)`, `bool releaseButton(PlayerButton)`, `void ringJump(RingObject* object, bool skipCheck)`, `void bumpPlayer(float bumpMod, int objectType, bool noEffects, GameObject* object)`, `void boostPlayer(float yVelocity)`, `void playerDestroyed(bool noEffects)`, `void switchedToMode(GameObjectType type)`, `void flipGravity(bool flip, bool noEffects)`, `void hitGround(GameObject* object, bool notFlipped)`, `void saveToCheckpoint(PlayerCheckpoint*)`, `void loadFromCheckpoint(PlayerCheckpoint*)`, `virtual void update(float dt)`: all **A**. `GameObjectType getActiveMode()`, `bool isFlying()` and `void touchedObject(GameObject*)` are **I on win** (callable, not hookable).
Inside per-step hooks use `getPosition()`. `m_position` is synced at the end of `update` (see `camila314_gdp/GJBaseGameLayer_update.cpp`).

### 4.3 Ticks, frames, TPS

- Physics runs at **240 steps/s**. `getModifiedDelta(dt)` quantises the frame delta to whole steps, carrying the remainder in `m_extraDelta`. Steps per rendered frame ≈ 240/FPS: 4 at 60 FPS, 1 at 240 FPS. The decompiled loop (`gdp`): `stepCount = max(1, delta*240/min(timewarp,1))`. Per step it calls `processCommands(stepDt)` (handles queued inputs via `handleButton`), then the players' `update`, `checkCollisions`, ..., and `updateCamera(stepDt*60)`.
- **Frame counter:** use `m_gameState.m_currentProgress` (`unsigned int`, the per-step counter). This is what xdBot uses as the frame when playing back. Fallback: `(int)(m_gameState.m_levelTime * 240)`. `GJBaseGameLayer::m_currentStep` belongs to GD's built-in replay recorder (it is reset in `resetRecordFull`). Do not rely on it.
- **2.2081 change:** `processCommands(float dt, bool isHalfTick, bool isLastTick)`. 2.2074 had `processCommands(float dt)`, so xdBot's code will not compile against 2.2081. With native click-between-steps (`m_clickBetweenSteps`), GD can split a step in half when a queued button's timestamp falls in the second half of the step (`shouldUseSubstepForButton`, inline/GJBaseGameLayer.cpp:1179). **Risk:** the recorded frame is then ambiguous. For deterministic record and replay, set `m_clickBetweenSteps = false` after `PlayLayer::init` while your recorder/replayer is active, or record the half-tick flag too. Verify in-game.

### 4.4 Recorder / replayer pattern

```cpp
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

struct Input { uint32_t frame; uint8_t button; bool p2; bool down; };
static std::vector<Input> g_rec, g_play; static size_t g_idx = 0;
static bool g_recording = false, g_replaying = false;

class $modify(ALGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (g_replaying && !m_fields->m_injecting) return;              // block real input during replay
        if (g_recording && !g_replaying)
            g_rec.push_back({ m_gameState.m_currentProgress, (uint8_t)button, !isPlayer1, down });
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (g_replaying && !isHalfTick) {
            auto f = m_gameState.m_currentProgress;
            m_fields->m_injecting = true;
            while (g_idx < g_play.size() && g_play[g_idx].frame <= f) {
                auto& in = g_play[g_idx++];
                this->handleButton(in.down, in.button, !in.p2);
            }
            m_fields->m_injecting = false;
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
    struct Fields { bool m_injecting = false; };
};

class $modify(ALPlayLayer, PlayLayer) {
    void resetLevel() {
        PlayLayer::resetLevel();
        if (!m_isPracticeMode) { g_rec.clear(); g_idx = 0; }
    }
    void loadFromCheckpoint(CheckpointObject* cp) {
        PlayLayer::loadFromCheckpoint(cp);
        auto f = cp->m_gameState.m_currentProgress;               // truncate inputs after checkpoint
        std::erase_if(g_rec, [f](Input const& i) { return i.frame >= f; });
        // also re-apply held state (m_player1->m_holdingButtons) if needed
    }
    void destroyPlayer(PlayerObject* p, GameObject* obj) {
        if (obj == m_anticheatSpike) return PlayLayer::destroyPlayer(p, obj);  // GD's anticheat probe, not a real death
        PlayLayer::destroyPlayer(p, obj);
        // record death at m_gameState.m_currentProgress
    }
};
```
- The `isPlayer1` argument: bindings name the 3rd parameter `isPlayer1`, and xdBot and its autoclicker treat it as "player 1". 2-player levels flip controls when `GameManager::get()->getGameVariable("0010")` is set (`bool getGameVariable(char const* key)`, A). Pathfinder and xdBot both XOR player 2 with this flag.
- Detect contexts: `PlayLayer::get()` (normal play), `LevelEditorLayer::get()` + `m_playbackMode` (editor playtest), `m_isPracticeMode`, `m_isTestMode` (start-pos).
- Death and completion: `m_playerDied`, `m_hasCompletedLevel`, `PlayLayer::levelComplete()`.
- Per-step state sampling (ghost or validation): hook `GJBaseGameLayer::updateCamera(float)`, which is called once per step. Pathfinder logs player x/y/velocity there and resets its frame counter in `PlayLayer::resetLevel`.

### 4.5 Orbs, pads and portals touched

- `GJBaseGameLayer::playerTouchedRing(PlayerObject*, RingObject*)` fires while the player overlaps an orb. `PlayerObject::ringJump(RingObject* object, bool skipCheck)` is the actual activation: hook it to log "orb used at frame N".
- Pads: `GJBaseGameLayer::bumpPlayer(PlayerObject*, EffectGameObject*)` and `PlayerObject::bumpPlayer(float bumpMod, int objectType, bool noEffects, GameObject* object)`.
- Portals: `GJBaseGameLayer::playerWillSwitchMode(PlayerObject*, GameObject*)`, `PlayerObject::switchedToMode(GameObjectType)`.
- `RingObject : EffectGameObject` (fields `bool m_claimTouch; bool m_isSpawnOnly;`). Identify the type by `m_objectID`: 36 yellow orb, 84 blue, 141 pink, 1022 green, 1330 black, 1333 red, 1594 toggle, 1704 green dash, 1751 pink dash, 3004 spider orb, 3027 teleport orb. Pads: 35 yellow, 67 blue, 140 pink, 1332 red, 3005 spider. You can also use `m_objectType` (`GameObjectType::YellowJumpRing`=11, `PinkJumpRing`=12, `GravityRing`=13, `GreenRing`=29, `DropRing`=32, `RedJumpRing`=35, `CustomRing`=36, `DashRing`=37, `GravityDashRing`=38, `SpiderOrb`=43, `TeleportOrb`=46; pads 8/9/10/34/44).

### 4.6 How pathfinder works (camila314_pathfinder, Geode 5.3, 2.2081)

- **It does not inject inputs into GD in release builds.** `main.cpp` hooks `EditLevelLayer::init(GJGameLevel*)` and `LevelInfoLayer::init(GJGameLevel*, bool)` to add a button. The button decompresses `m_level->m_levelString` (`ZipUtils::decompressString(str, true, 0)`) and runs `pathfind(lvlString, std::atomic_bool& stop, std::function<void(double)> progress)` on `std::async`. That function lives in its own static lib **gd-sim**, a re-implementation of GD physics stepped per 240 Hz frame: `Level::runFrame(bool pressed)`, `rollback(int frame)`, `currentFrame()`. A schedule on the node polls the `std::future` on the main thread. The result is exported as a GDR2 macro (`gdr::Replay<...>::exportData()`, frames as `uint64_t` ticks, `framerate = 240`). The save dialog uses `co_await file::pick(PickMode::SaveFile, opts)` inside `async::wrapSpawn([..](this auto self) -> arc::Future<void> {...})`.
- `debug.cpp` (debug tooling): `$modify(GJBaseGameLayer) void updateCamera(float dt)` logs per step `Frame N X Y Vel Accel Rot` from `m_player1` (`m_yVelocity`, `m_isUpsideDown`, `getPositionX()`) and increments a `frames` counter. `$modify(PlayLayer) void resetLevel()` resets the counter. It also adds a `CCDrawNode` to `LevelEditorLayer::m_objectLayer` (ID `pathfind-node`) to draw sim vs real paths, and an `EditorPauseLayer::customSetup` button in `guidelines-menu`.
- `checker.cpp` (commented out) shows headless validation: `PlayLayer::create(lvl,false,false)`, `resetLevel()`, then a loop of `handleButton(input.down, input.button, input.player2 ^ flipP2)` and `pl->update(1/240.)` until `m_hasCompletedLevel || m_playerDied`. **Risky:** an off-scene PlayLayer creates audio, objects and so on. It is a good idea for an offline validator only if you create it on the main thread and tear it down carefully.
- For AutoLayout: a pure-C++ simulator in your core lib is the most robust way to validate generated layouts (deterministic, runs off-thread). In-game ghost playback via the hooks in 4.4 then serves as ground truth.

---

## 5. UI

### 5.1 geode::Popup (v5: NOT templated; no `setup()`; no `initAnchored`)

```cpp
class GEODE_DLL Popup : public FLAlertLayer {
protected:
    cocos2d::CCSize m_size; NineSlice* m_bgSprite; cocos2d::CCLabelBMFont* m_title = nullptr; CCMenuItemSpriteExtra* m_closeBtn;
    bool init(float width, float height, char const* bg = "GJ_square01.png", cocos2d::CCRect bgRect = {});
    bool init(cocos2d::CCSize size, char const* bg = "GJ_square01.png", cocos2d::CCRect bgRect = {});
    virtual void onClose(CCObject*);
    void setTitle(ZStringView title, const char* font = "goldFont.fnt", float scale = .7f, float offset = 20.f);
    void setCloseButtonSpr(cocos2d::CCSprite* spr, float scale = 1.f);
    CloseEvent listenForClose();
};
// inherited from FLAlertLayer: cocos2d::CCLayer* m_mainLayer; cocos2d::CCMenu* m_buttonMenu; bool m_noElasticity; CCNode* m_scene;
```
`m_mainLayer` has size `m_size` and is centred. `m_buttonMenu` and `m_bgSprite` copy its size (CopySizeLayout), so use `addChildAtPosition(node, Anchor::..., offset)`.
```cpp
class AutoLayoutPopup : public geode::Popup {
protected:
    bool init(LevelEditorLayer* lel) {
        if (!Popup::init(420.f, 280.f)) return false;
        this->setTitle("AutoLayout");
        auto gen = CCMenuItemSpriteExtra::create(ButtonSprite::create("Generate"), this, menu_selector(AutoLayoutPopup::onGenerate));
        m_buttonMenu->addChildAtPosition(gen, Anchor::Bottom, {0.f, 25.f});
        return true;
    }
    void onGenerate(CCObject*) {}
public:
    static AutoLayoutPopup* create(LevelEditorLayer* lel) {
        auto ret = new AutoLayoutPopup();
        if (ret->init(lel)) { ret->autorelease(); return ret; }
        delete ret; return nullptr;
    }
};
// AutoLayoutPopup::create(lel)->show();
```
Quick confirm:
```cpp
FLAlertLayer* geode::createQuickPopup(char const* title, std::string content, char const* btn1, char const* btn2,
    geode::Function<void(FLAlertLayer*, bool)> selected, bool doShow = true, bool cancelledByEscape = false);
// + overload with `float width` after btn2
```

### 5.2 Widgets (signatures)

```cpp
// GD
static CCMenuItemSpriteExtra* CCMenuItemSpriteExtra::create(cocos2d::CCNode* sprite, cocos2d::CCObject* target, cocos2d::SEL_MenuHandler callback); // inline
static ButtonSprite* ButtonSprite::create(char const* caption);                                         // inline (many overloads, all usable)
static ButtonSprite* ButtonSprite::create(char const* caption, const char* font, const char* texture, float scale); // inline
static CCMenuItemToggler* CCMenuItemToggler::createWithStandardSprites(cocos2d::CCObject* target, cocos2d::SEL_MenuHandler callback, float scale); // inline
bool CCMenuItemToggler::isToggled(); void CCMenuItemToggler::toggle(bool on);
static Slider* Slider::create(cocos2d::CCNode* target, cocos2d::SEL_MenuHandler handler, float scale);  // I on win
float Slider::getValue() /*I on win*/; void Slider::setValue(float val);  // 0..1
static CCTextInputNode* CCTextInputNode::create(float width, float height, char const* placeholder, char const* fontPath); // inline
// Geode lambda helpers (geode::cocos::CCMenuItemExt, in prelude)
static CCMenuItemSpriteExtra* CCMenuItemExt::createSpriteExtra(cocos2d::CCNode* normalSprite, geode::Function<void(CCMenuItemSpriteExtra*)> callback);
static CCMenuItemSpriteExtra* CCMenuItemExt::createSpriteExtraWithFrameName(ZStringView name, float scale, geode::Function<void(CCMenuItemSpriteExtra*)> cb);
static CCMenuItemToggler* CCMenuItemExt::createTogglerWithStandardSprites(float scale, geode::Function<void(CCMenuItemToggler*)> callback);
// Geode
static geode::TextInput* TextInput::create(float width, ZStringView placeholder, ZStringView font = "bigFont.fnt");
void TextInput::setCallback(geode::Function<void(std::string const&)>); void setCommonFilter(CommonFilter); void setFilter(gd::string);
gd::string TextInput::getString() const; void setString(gd::string str, bool triggerCallback = false);
//   CommonFilter: Uint, Int, Float, ID, Name, Any, Hex, Base64Normal, Base64URL, Alphanumeric, Alphabetic
static geode::SliderNode* SliderNode::create(SliderCallback callback, bool alt = false);   // SliderCallback = geode::Function<void(SliderNode*, float)>
static geode::TabButton* TabButton::create(TabBaseColor unselected, TabBaseColor selected, char const* text, cocos2d::CCObject* target, cocos2d::SEL_MenuHandler callback);
static geode::TabButton* TabButton::create(char const* text, cocos2d::CCObject* target, cocos2d::SEL_MenuHandler callback);  // : CCMenuItemToggler
static geode::ScrollLayer* ScrollLayer::create(cocos2d::CCSize const& size, bool scrollWheelEnabled = true, bool vertical = true);
//   list->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout());
static geode::NineSlice* NineSlice::create(geode::ZStringView file, cocos2d::CCRect const& rect = {}, Insets const& insets = {});  // Geode 5 replacement used by Popup
//   cocos2d::extension::CCScale9Sprite::create(...) still exists (win dll / mac addresses)
static geode::Notification* Notification::create(ZStringView text, NotificationIcon icon = NotificationIcon::None, float time = NOTIFICATION_DEFAULT_TIME);
//   ->show();  NotificationIcon: None, Loading, Success, Warning, Error, Info
static CircleButtonSprite* CircleButtonSprite::createWithSpriteFrameName(char const* sprName, float sprScale = 1.f, CircleBaseColor color = CircleBaseColor::Green, CircleBaseSize size = CircleBaseSize::Medium);
static EditorButtonSprite* EditorButtonSprite::createWithSpriteFrameName(char const* sprName, float sprScale = 1.f, EditorBaseColor color = EditorBaseColor::Green, EditorBaseSize size = EditorBaseSize::Normal);
```
v5 note: all callbacks are `geode::Function` (**move-only**). Use `std::move` and do not copy lambdas that hold move-only state.

### 5.3 File picker (Geode 5 async API, no more `Task`)

```cpp
#include <Geode/utils/file.hpp>
#include <Geode/utils/async.hpp>
// enum class PickMode { OpenFile, SaveFile, OpenFolder };
// struct FilePickOptions { struct Filter { std::string description; std::unordered_set<std::string> files; };
//                          std::optional<std::filesystem::path> defaultPath; std::vector<Filter> filters; };
// using PickResult = Result<std::optional<std::filesystem::path>>;
// GEODE_DLL arc::Future<PickResult> file::pick(PickMode mode, FilePickOptions options);
// GEODE_DLL arc::Future<Result<std::vector<std::filesystem::path>>> file::pickMany(FilePickOptions options);

file::FilePickOptions opts;
opts.filters.push_back({ "Audio", { "*.mp3", "*.ogg", "*.wav" } });
m_pickTask.spawn(                                  // async::TaskHolder<file::PickResult> m_pickTask; (member: auto-cancels on destroy)
    file::pick(file::PickMode::OpenFile, std::move(opts)),
    [this](file::PickResult res) {                 // runs on the MAIN thread
        if (res.isErr()) { Notification::create(res.unwrapErr(), NotificationIcon::Error)->show(); return; }
        auto opt = std::move(res).unwrap();
        if (!opt) return;                          // user cancelled
        this->loadSong(*opt);
    });
// fire-and-forget alternative: async::spawn(file::pick(...), [](file::PickResult r) { ... });
// coroutine form: co_await file::pick(...) inside an arc::Future<> lambda (see pathfinder main.cpp)
```

### 5.4 Waveform timeline (CCDrawNode, all mac addresses present)

```cpp
static cocos2d::CCDrawNode* create();
void clear();
bool drawSegment(cocos2d::CCPoint const&, cocos2d::CCPoint const&, float radius, cocos2d::ccColor4F const&);
bool drawRect(cocos2d::CCPoint const&, cocos2d::CCPoint const&, cocos2d::ccColor4F const& fill, float borderWidth, cocos2d::ccColor4F const& border, cocos2d::BorderAlignment);
bool drawPolygon(cocos2d::CCPoint*, unsigned int, cocos2d::ccColor4F const& fill, float borderWidth, cocos2d::ccColor4F const& border, cocos2d::BorderAlignment);
bool drawLines(cocos2d::CCPoint*, unsigned int, float, cocos2d::ccColor4F const&);
bool drawDot(cocos2d::CCPoint const& pos, float radius, cocos2d::ccColor4F const& color);
void enableDrawArea(cocos2d::CCRect&); void disableDrawArea();   // cull to visible region
```
Downsample the waveform to about 1 sample per pixel column (min/max pairs) and rebuild it only when zoom or scroll changes. Clip the visible window with `CCClippingNode`, or use `enableDrawArea`. For the in-editor overlay (beat markers aligned to X), add the node to `LevelEditorLayer::m_objectLayer` (world space), as pathfinder does.

### 5.5 Pause menu button

`virtual void PauseLayer::customSetup()` (win 0x37c870, imac 0x3d5d30, m1 0x355300: **A**). Node IDs creates `left-button-menu`, `right-button-menu` (ColumnLayout), `bottom-button-menu` (RowLayout) and `center-button-menu`.
```cpp
#include <Geode/modify/PauseLayer.hpp>
class $modify(PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (auto m = this->getChildByID("right-button-menu")) {
            auto b = CCMenuItemExt::createSpriteExtraWithFrameName("GJ_replayBtn_001.png", 0.6f, [](auto) { /* ghost toggle */ });
            b->setID("ghost-button"_spr); m->addChild(b); m->updateLayout();
        }
    }
};
```
EditorPauseLayer: `virtual void customSetup()` (A). Menus: `resume-menu`, `settings-menu`, `info-menu`, `top-menu` (pathfinder also uses `guidelines-menu`).

### 5.6 Keybinds: use the native Geode 5 `keybind` setting (recommended)

Custom Keybinds is deprecated by v5 (`migrate-v5.md`: "If your mod depends on Custom Keybinds, well, it doesn't anymore!"). Declare the keybind in `mod.json` (section 1.1), then:
```cpp
#include <Geode/loader/SettingV3.hpp>
#include <Geode/loader/GameEvent.hpp>
$on_game(Loaded) {
    listenForKeybindSettingPresses("open-panel", [](Keybind const& kb, bool down, bool repeat, double timestamp) {
        if (down && !repeat) if (auto lel = LevelEditorLayer::get()) AutoLayoutPopup::create(lel)->show();
    });
}
// node-scoped: this->addEventListener(KeybindSettingPressedEventV3(Mod::get(), "tap-key"), [this](Keybind const&, bool down, bool repeat, double ts) {...});
// read: Mod::get()->getSettingValue<std::vector<geode::Keybind>>("tap-key");
```
The `timestamp` argument is what tap-along should use (section 3.2). Raw input is also available via `KeyboardInputEvent` / `MouseInputEvent` (`utils/Keyboard.hpp`).

---

## 6. Threading

- **Cocos and GD objects are main-thread only.** That covers any `CCNode`, `CCObject::retain/release`, `GameManager`, `FMODAudioEngine`, `CCFileUtils`, and any binding call. Geode 5.11.0 changelog: "Fix rare wrapper crash caused by calling a GD/Cocos function from multiple threads". Event listeners run on the thread that sent the event (use `ThreadSafeEvent` for cross-thread events).
- Post back to the main thread:
  ```cpp
  inline void geode::queueInMainThread(ScheduledFunction&& func);   // usable from any thread
  co_await async::waitForMainThread<T>(fn);                          // inside a coroutine; returns std::optional<T>
  ```
- Geode 5 async (`<Geode/utils/async.hpp>`, Arc runtime). `geode::Task` was **replaced** by coroutines:
  ```cpp
  arc::Runtime& geode::async::runtime();
  arc::TaskHandle<void> geode::async::spawn(Fut&& future, Cb&& cb);    // cb runs on MAIN thread with the result
  auto geode::async::spawn(F&& f);                                     // plain task
  auto geode::async::wrapSpawn(F&& f);                                 // returns a callable that spawns f (handy for button callbacks)
  template<class T> class geode::async::TaskHolder { void spawn(F&& future, Cb&& cb); void cancel(); };   // RAII cancellation
  // CPU-heavy or blocking work (model inference, audio decoding):
  auto h = async::runtime().spawnBlocking<Result>([] { return heavy(); });  // then co_await h; never blockOn() inside the runtime
  ```
  Never block an Arc worker (`std::this_thread::sleep_for`, `std::mutex` held across `co_await`). Use `arc::sleep`, `arc::Mutex`, or the blocking pool.
- `std::thread` / `std::async` are fine for long-running analysis. Pathfinder uses `std::async` + `std::atomic_bool` stop flag + polling the future in a `schedule` callback. **Lifetime gotcha:** do not let a worker thread own a `Ref<CCNode>` or `WeakRef`, because its destructor (release) would run off-thread. Move it into the `queueInMainThread` lambda so it is created and destroyed on the main thread, or keep worker results in plain C++ objects and look up the UI by ID.
  ```cpp
  std::thread([path, cancel = m_cancelFlag /* shared_ptr<atomic_bool> */] {
      auto result = autolayout::analyze(path, *cancel);           // pure C++ (core lib)
      geode::queueInMainThread([result = std::move(result)]() mutable {
          if (auto ui = AutoLayoutPopup::current()) ui->onAnalysisDone(std::move(result));
      });
  }).detach();
  ```

---

## 7. Saving

```cpp
std::filesystem::path Mod::getSaveDir() const;                    // geode/mods/<id>/        (data; removed on uninstall)
std::filesystem::path Mod::getConfigDir(bool create = true) const;// geode/config/<id>/      (user-editable config)
std::filesystem::path Mod::getPersistentDir(bool create = true) const; // survives uninstall
std::filesystem::path Mod::getResourcesDir() const;               // dirs::getModRuntimeDir()/<id>/resources/<id>  (shipped files)
std::filesystem::path Mod::getTempDir() const;
template<class T> T Mod::getSavedValue(std::string_view key);
template<class T> T Mod::getSavedValue(std::string_view key, T const& defaultValue);
template<class T> T Mod::setSavedValue(std::string_view key, T const& value);   // returns the previous value
bool Mod::hasSavedValue(std::string_view key);
template<class T> T Mod::getSettingValue(std::string_view key) const;
Result<> Mod::saveData();
// file helpers (geode::utils::file):
Result<std::string> readString(path); Result<ByteVector> readBinary(path); Result<matjson::Value> readJson(path);
Result<> writeString(path, std::string_view); Result<> writeStringSafe(...); Result<> writeBinary(path, ByteSpan); Result<> writeBinarySafe(...);
Result<> createDirectoryAll(path); template<class T> Result<T> readFromJson(path); template<class T> Result<> writeToJson(path, T const&);
```
- Saved values must implement `matjson::Serialize<T>` (`fromJson` / `toJson`). They are saved on game close.
- Shipping model files: `"resources": { "files": ["resources/models/*.onnx"] }` puts them, **flattened**, under `getResourcesDir()`. `"folders": [...]` copies directories recursively. Sprites go in `"sprites"` / `"spritesheets"` and are referenced as `"file.png"_spr`.
- Do not write into the GD folder. Use `getSaveDir()` for recordings and generated layouts, and `getConfigDir()` for user-editable presets.

---

## 8. Exporting a .gmd

- **Easiest:** depend on `hjfod.gmd-api` (v1.5.0, Geode 5, 2.2081, all platforms; this is what GDShare uses):
  ```cpp
  #include <hjfod.gmd-api/include/GMD.hpp>
  geode::Result<> gmd::exportLevelAsGmd(GJGameLevel* level, std::filesystem::path const& to, gmd::GmdFileType type = gmd::GmdFileType::Gmd);
  geode::Result<GJGameLevel*> gmd::importGmdAsLevel(std::filesystem::path const& from);
  // ExportGmdFile::from(level).setType(GmdFileType::Gmd2).setIncludeSong(true).intoFile(path)  -> zip with level.data + level.meta + song
  ```
- **DIY (what GMD-API does internally):**
  ```cpp
  auto dict = std::make_unique<DS_Dictionary>();          // DS_Dictionary(): win via libcocos2d, A mac
  level->encodeWithCoder(dict.get());                      // virtual void GJGameLevel::encodeWithCoder(DS_Dictionary*)  A
  std::string xml = dict->saveRootSubDictToString();       // gd::string DS_Dictionary::saveRootSubDictToString()  A mac
  file::writeString(path, xml);
  ```
  The output is a plist-style XML `<d>` of `<k>key</k><s|i|r|t/>` pairs. On import GMD-API prepends `<?xml version="1.0"?><plist version="1.0" gjver="2.0">` if it is missing, because DS_Dictionary needs `gjver`.
- Keys (gddocs `resources/client/level.md`): `kCEK`=4 (GJGameLevel type), `k1` level ID, `k2` name, `k3` description (base64), `k4` level string (compressed, `H4sI...`), `k5` creator, `k8` official song, `k45` custom song ID, `k104` song list, `k105` SFX list, `k13` editable, `k14` verified, `k16` version, `k21` level type (2 = local), `k23` length, `k47` hasBeenModified, `k48` object count, `k50` binary version, `k80` seconds editing, `k95` verification time in steps.
- To create a new local level programmatically: `GJGameLevel* GameLevelManager::createNewLevel()` (A), then set `m_levelName` and `m_levelString = ZipUtils::compressString(raw, false, 0)`, then `m_songID` / `m_audioTrack`. `LocalLevelManager::get()->m_localLevels` holds the created levels.

---

## 9. Platform gaps (win / macOS) and workarounds

Everything recommended above is available on win + imac + m1. Items to **avoid** or handle specially:

| Item | Status | Workaround |
|---|---|---|
| `LevelEditorLayer::addTriggersWithGroupID(...)`, `addTriggersWithTargetGroupID(...)`, `objectMoved(GameObject*)`, `processLoadedMoveActions()` | **X on Windows** (mac only) | Don't call. Do your own group bookkeeping. |
| `GJBaseGameLayer::isButtonAllowed(bool, int, bool)`, `checkSpawnAbuse()` | `[[missing(win, mac)]]` | Don't use. |
| `PlayerObject::spiderTestJumpX/Y(bool)`, `EffectGameObject::hasSpawnTargetID(int)` | X on Windows | Not needed. |
| `PauseLayer::onHelp/onRecordReplays/onReplay/onTime`, `GameManager` ad/icon-async functions | X on Windows | Not needed. |
| `PlayerObject::touchedObject`, `isFlying`, `getActiveMode`; `GJBaseGameLayer::playerTouchedObject`, `queueButton`; `EditorUI::deleteObject`, `create`, `centerCameraOnObject`; `LevelEditorLayer::addToUndoList`, `undoLastAction`, `redoLastAction`, `getAllObjects`, `removeAllObjects`, `addObjectFromVector`, `addObjectToGroup`, `onPausePlaytest`, `onResumePlaytest`, `findStartPosObject`, `scene`; `UndoObject::create*`; `FMODAudioEngine::sharedEngine/pauseMusic/resumeMusic/stopMusic/getMusicTime/getActiveMusicChannel`; `Slider::create(…,scale)/getValue`; `CCTextInputNode::getString/setAllowedChars`; `PlayLayer::getLastCheckpoint/loadLastCheckpoint`; `MusicDownloadManager::isResourceSong`; `LevelTools::valueForSpeedMod/posForTime` | **I on Windows** (Geode re-implementation) | Safe to **call**. **Cannot be hooked on Windows**: hook the real caller instead, e.g. `ringJump`/`playerTouchedRing` instead of `touchedObject`, `processCommands`/`handleButton` instead of `queueButton`, `removeObject` instead of `EditorUI::deleteObject`. |
| Variadic `CCMenu::create(CCMenuItem*, ...)`, `CCMenu::alignItemsInColumns/Rows(...)`, `CCRenderTexture::saveToFile` | `mac inline` | Use `CCMenu::create()` + `addChild` + Geode layouts. |
| `processCommands` signature | Changed in 2.2081 (3 args) | Copy the 2.2081 signature exactly. Older bots' code (1 arg) will not compile. |
| Native click-between-steps (`m_clickBetweenSteps`, half ticks) | 2.2081 behaviour | Disable while recording/replaying, or record the half-tick flag. Verify determinism. |
| `GJGameLevel::m_cbsOverride` | android/ios only field | Don't use on win/mac. |
| mac keybinds | GD maps Ctrl and Cmd to the same actions | Give per-platform defaults (`"mac": "Cmd+..."`). |

General risks:
- `createObjectsFromString`'s `noUndo`/`noLimit` semantics are unverified. Test once.
- Node IDs differ slightly between node-ids versions. Always null-check `getChildByID`.
- Never touch cocos or GD from worker threads, including `Ref` destruction.
- The `GJGameLevel::m_levelString` (compressed) and `getLevelString()` (raw) formats differ.
- Song paths can be replaced by NONG mods.
- `destroyPlayer` is also called for `m_anticheatSpike`. Filter it out.

#include <al/gen/modules.hpp>

#include <algorithm>

namespace al::gen {

#include "module_library.inc"

const Module* moduleTable(size_t& count) {
    count = sizeof(kModules) / sizeof(kModules[0]);
    return kModules;
}

const ModulePiece* modulePieces() { return kPieces; }

const StyleStats& structureStyle(int style) { return kStyles[std::clamp(style, 0, kStructureStyles - 1)]; }

} // namespace al::gen

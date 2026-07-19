#include "SdCardFontManager.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <SdCardFontRegistry.h>

#include <algorithm>
#include <climits>

SdCardFontManager::~SdCardFontManager() {
  for (auto& lf : loaded_) {
    if (lf.ownsFont) delete lf.font;
  }
}

// FNV-1a continuation: seeds with contentHash, then hashes family name + point size.
// Produces a deterministic ID that is stable across load/unload cycles and reboots,
// and changes when font content changes (different header/TOC = different contentHash).
int SdCardFontManager::computeFontId(uint32_t contentHash, const char* familyName, uint8_t pointSize) {
  static constexpr uint32_t FNV_PRIME = 16777619u;
  uint32_t hash = contentHash;
  while (*familyName) {
    hash ^= static_cast<uint8_t>(*familyName++);
    hash *= FNV_PRIME;
  }
  hash ^= pointSize;
  hash *= FNV_PRIME;
  int id = static_cast<int>(hash);
  return id != 0 ? id : 1;  // 0 is reserved as "not found" sentinel
}

// Map fontSizeEnum / role enum to the target point size used for "closest" matching.
// Must match the mapping in SdCardFontFamilyInfo::findClosestReaderSize.
uint8_t SdCardFontManager::targetPointSize(uint8_t fontSizeEnum) {
  switch (fontSizeEnum) {
    case 0:  return 12;   // SMALL
    case 1:  return 14;   // MEDIUM
    case 2:  return 16;   // LARGE
    case 3:  return 18;   // EXTRA_LARGE
    case 4:  return 10;   // TABLE_SIZE
    case 5:  return 8;    // RUBY_SIZE
    case 6:  return 10;   // FOOTNOTE_SIZE
    default: return 14;
  }
}

int SdCardFontManager::findSlotByEnum(uint8_t fontSizeEnum) const {
  for (size_t i = 0; i < loaded_.size(); i++) {
    if (loaded_[i].loadedFor == fontSizeEnum) return static_cast<int>(i);
  }
  return -1;
}

bool SdCardFontManager::loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t fontSizeEnum) {
  // If a different family is loaded, unload everything first.
  if (!loadedFamilyName_.empty() && loadedFamilyName_ != family.name) {
    unloadAll(renderer);
  }

  // If this exact fontSizeEnum is already loaded for the same family, skip.
  if (!loaded_.empty() && loadedFamilyName_ == family.name && findSlotByEnum(fontSizeEnum) >= 0) {
    return true;
  }

  // Select the physical point size closest to the target for this fontSizeEnum.
  const SdCardFontFileInfo* selected = family.findClosestReaderSize(fontSizeEnum);
  if (!selected) {
    LOG_ERR("SDMGR", "Family %s has no files to load (enum %u)", family.name.c_str(), fontSizeEnum);
    return false;
  }

  // If another role already resolved to the same physical point size, alias it
  // instead of loading/registering the same .cpfont twice.
  for (const auto& lf : loaded_) {
    if (lf.size == selected->pointSize) {
      loaded_.push_back({lf.font, lf.fontId, lf.size, fontSizeEnum, false});
      loadedFamilyName_ = family.name;
      if (loaded_.size() == 1 || fontSizeEnum == 1) {
        loadedPointSize_ = selected->pointSize;
      }
      LOG_DBG("SDMGR", "Aliased %s size=%u pt to existing id=%d (enum=%u) [%zu total roles]",
              selected->path.c_str(), selected->pointSize, lf.fontId, fontSizeEnum, loaded_.size());
      return true;
    }
  }

  auto* font = new (std::nothrow) SdCardFont();
  if (!font) {
    LOG_ERR("SDMGR", "Failed to allocate SdCardFont for %s", selected->path.c_str());
    return false;
  }

  if (!font->load(selected->path.c_str())) {
    LOG_ERR("SDMGR", "Failed to load %s", selected->path.c_str());
    delete font;
    return false;
  }

  int fontId = computeFontId(font->contentHash(), family.name.c_str(), selected->pointSize);
  if (renderer.getFontMap().count(fontId) != 0) {
    LOG_ERR("SDMGR", "Font ID %d collides with existing font, skipping %s", fontId, selected->path.c_str());
    delete font;
    return false;
  }
  renderer.registerSdCardFont(fontId, font);

  EpdFontFamily fontFamily(font->getEpdFont(0), font->getEpdFont(1), font->getEpdFont(2), font->getEpdFont(3));
  renderer.insertFont(fontId, fontFamily);

  loaded_.push_back({font, fontId, selected->pointSize, fontSizeEnum, true});
  loadedFamilyName_ = family.name;

  // Keep body (primary) size metadata in the dedicated field for backward compat.
  if (loaded_.size() == 1 || fontSizeEnum == 1) {
    loadedPointSize_ = selected->pointSize;
  }

  LOG_DBG("SDMGR", "Loaded %s size=%u pt id=%d styles=%u (enum=%u) [%zu total sizes]",
          selected->path.c_str(), selected->pointSize, fontId, font->styleCount(),
          fontSizeEnum, loaded_.size());

  return true;
}

void SdCardFontManager::unloadAll(GfxRenderer& renderer) {
  renderer.clearSdCardFonts();
  std::vector<int> removedIds;
  for (auto& lf : loaded_) {
    if (std::find(removedIds.begin(), removedIds.end(), lf.fontId) == removedIds.end()) {
      renderer.removeFont(lf.fontId);
      removedIds.push_back(lf.fontId);
    }
    if (lf.ownsFont) delete lf.font;
  }
  loaded_.clear();
  loadedFamilyName_.clear();
  loadedPointSize_ = 0;
}

int SdCardFontManager::getFontId(const std::string& familyName, uint8_t fontSizeEnum) const {
  if (familyName != loadedFamilyName_ || loaded_.empty()) return 0;

  // Exact match on fontSizeEnum.
  int slot = findSlotByEnum(fontSizeEnum);
  if (slot >= 0) return loaded_[slot].fontId;

  // Otherwise resolve to the closest loaded physical point size.
  const uint8_t target = targetPointSize(fontSizeEnum);
  const LoadedFont* best = &loaded_.front();
  int bestDiff = INT_MAX;
  for (const auto& lf : loaded_) {
    const int diff = (lf.size > target) ? static_cast<int>(lf.size - target)
                                        : static_cast<int>(target - lf.size);
    if (diff < bestDiff) {
      bestDiff = diff;
      best = &lf;
    }
  }
  return best->fontId;
}

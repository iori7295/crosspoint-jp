#include "SdCardFontSystem.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include "CrossPointSettings.h"

namespace {

static uint8_t fontSizeEnumFromSettings(bool isVertical) {
  uint8_t e = SETTINGS.getDirectionSettings(isVertical).fontSize;
  if (e >= CrossPointSettings::FONT_SIZE_COUNT) e = 1;
  return e;
}

static uint8_t headingSizeEnum(bool isVertical) {
  uint8_t body = fontSizeEnumFromSettings(isVertical);
  return (body < CrossPointSettings::EXTRA_LARGE)
             ? static_cast<uint8_t>(body + 1)
             : static_cast<uint8_t>(CrossPointSettings::EXTRA_LARGE);
}

static uint8_t smallSizeEnum(bool isVertical) {
  uint8_t body = fontSizeEnumFromSettings(isVertical);
  return (body > 0) ? static_cast<uint8_t>(body - 1) : 0;
}

static constexpr uint8_t tableSizeEnum() { return CrossPointSettings::TABLE_SIZE; }
static constexpr uint8_t rubySizeEnum() { return CrossPointSettings::RUBY_SIZE; }
static constexpr uint8_t footnoteSizeEnum() { return CrossPointSettings::FOOTNOTE_SIZE; }

// Load one size for the given family.  Returns true on success.
static bool loadOneSize(SdCardFontManager& mgr, GfxRenderer& renderer,
                        const SdCardFontFamilyInfo& family, uint8_t sizeEnum,
                        const char* label) {
  if (!mgr.loadFamily(family, renderer, sizeEnum)) {
    LOG_ERR("SDFS", "Failed to load %s size (enum=%u) for %s", label, sizeEnum, family.name.c_str());
    return false;
  }
  LOG_DBG("SDFS", "Loaded %s font (enum=%u) for %s", label, sizeEnum, family.name.c_str());
  return true;
}

static bool loadReaderRoleSet(SdCardFontManager& mgr, GfxRenderer& renderer,
                              const SdCardFontFamilyInfo& family, bool isVertical) {
  const uint8_t body = fontSizeEnumFromSettings(isVertical);
  const uint8_t small = smallSizeEnum(isVertical);
  const uint8_t heading = headingSizeEnum(isVertical);
  if (!loadOneSize(mgr, renderer, family, body, "body")) return false;
  loadOneSize(mgr, renderer, family, small, "small");
  loadOneSize(mgr, renderer, family, heading, "heading");
  loadOneSize(mgr, renderer, family, tableSizeEnum(), "table");
  loadOneSize(mgr, renderer, family, rubySizeEnum(), "ruby");
  loadOneSize(mgr, renderer, family, footnoteSizeEnum(), "footnote");
  return true;
}

}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();

  // Register this system as the SD font ID resolver in settings.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t fontSizeEnum) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, fontSizeEnum);
  };
  SETTINGS.sdFontResolverCtx = this;

  // Load the horizontal-direction SD font at startup (body + auxiliaries).
  if (SETTINGS.horizontal.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.horizontal.sdFontFamilyName);
    if (family) {
      if (loadReaderRoleSet(manager_, renderer, *family, false)) {
        lastLoadedFamily_[0] = family->name;
        lastLoadedBodyEnum_[0] = fontSizeEnumFromSettings(false);
      }
    } else {
      LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.horizontal.sdFontFamilyName);
      SETTINGS.horizontal.sdFontFamilyName[0] = '\0';
      SETTINGS.saveToFile();
    }
  }

  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer, bool isVertical) {
  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
  }

  auto& ds = SETTINGS.getDirectionSettings(isVertical);
  const char* wantedFamily = ds.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();
  const uint8_t bodyEnum = fontSizeEnumFromSettings(isVertical);
  const int dirIndex = isVertical ? 1 : 0;

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    lastLoadedFamily_[dirIndex].clear();
    lastLoadedBodyEnum_[dirIndex] = 0xFF;
    return;
  }

  const bool needsReload =
      registryWasDirty ||
      !manager_.isFamilyLoaded(wantedFamily) ||
      currentFamily != wantedFamily ||
      lastLoadedFamily_[dirIndex] != wantedFamily ||
      lastLoadedBodyEnum_[dirIndex] != bodyEnum;

  if (!needsReload) return;

  // Family changed or registry was dirty — reload everything.
  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (!family) {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    ds.sdFontFamilyName[0] = '\0';
    SETTINGS.saveToFile();
    return;
  }

  if (!loadReaderRoleSet(manager_, renderer, *family, isVertical)) {
    LOG_ERR("SDFS", "Failed to load reader role set for %s (clearing)", wantedFamily);
    ds.sdFontFamilyName[0] = '\0';
    SETTINGS.saveToFile();
    return;
  }

  lastLoadedFamily_[dirIndex] = wantedFamily;
  lastLoadedBodyEnum_[dirIndex] = bodyEnum;
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t fontSizeEnum) const {
  return manager_.getFontId(familyName, fontSizeEnum);
}

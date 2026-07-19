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

// Small/annotation size: one step below the body size, clamped to SMALL.
static uint8_t smallSizeEnum(bool isVertical) {
  uint8_t body = fontSizeEnumFromSettings(isVertical);
  return (body > 0) ? static_cast<uint8_t>(body - 1) : 0;
}

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

}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();

  // Register this system as the SD font ID resolver in settings.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t fontSizeEnum) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, fontSizeEnum);
  };
  SETTINGS.sdFontResolverCtx = this;

  // Load the horizontal-direction SD font at startup (body + small).
  if (SETTINGS.horizontal.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.horizontal.sdFontFamilyName);
    if (family) {
      loadOneSize(manager_, renderer, *family, fontSizeEnumFromSettings(false), "body");
      loadOneSize(manager_, renderer, *family, smallSizeEnum(false), "small");
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
  const uint8_t smallEnum = smallSizeEnum(isVertical);

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    return;
  }

  // If family hasn't changed and both sizes are already loaded, nothing to do.
  if (manager_.isFamilyLoaded(wantedFamily)) {
    // Both sizes are loaded only when the registry is clean and the font
    // settings haven't changed.  We can't cheaply check "are both enums
    // loaded" here without a manager API, so we just return early and
    // rely on re-discovery / version mismatch below to trigger a reload.
    if (!registryWasDirty) return;
    // Fall through to re-load below when registry was dirty.
  }

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

  if (!loadOneSize(manager_, renderer, *family, bodyEnum, "body")) {
    LOG_ERR("SDFS", "Failed to load body font for %s (clearing)", wantedFamily);
    ds.sdFontFamilyName[0] = '\0';
    SETTINGS.saveToFile();
    return;
  }
  loadOneSize(manager_, renderer, *family, smallEnum, "small");
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t fontSizeEnum) const {
  return manager_.getFontId(familyName, fontSizeEnum);
}

#pragma once

#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;
class SdCardFont;
struct SdCardFontFamilyInfo;

class SdCardFontManager {
 public:
  SdCardFontManager() = default;
  ~SdCardFontManager();
  SdCardFontManager(const SdCardFontManager&) = delete;
  SdCardFontManager& operator=(const SdCardFontManager&) = delete;

  // Load the font file whose physical point size is closest to
  // fontSizeEnum.  Multiple sizes of the same family can be loaded by
  // calling loadFamily with different fontSizeEnum values; the manager
  // keeps all of them in loaded_ and resolves getFontId(fontSizeEnum)
  // against the closest match in the family.
  // Returns true on success.
  bool loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t fontSizeEnum);

  // Unload everything, unregister from renderer.
  void unloadAll(GfxRenderer& renderer);

  // Look up the font ID for the given family + fontSizeEnum.
  // Searches loaded_ for the best matching size; returns 0 if nothing
  // is loaded for the given family.
  int getFontId(const std::string& familyName, uint8_t fontSizeEnum) const;

  // Returns true when at least one size is loaded for the given family.
  bool isFamilyLoaded(const std::string& familyName) const { return !loaded_.empty() && familyName == loadedFamilyName_; };

  // Get name of currently loaded family (empty if none).
  const std::string& currentFamilyName() const { return loadedFamilyName_; };

  // The point size that was loaded for the body (primary) fontSizeEnum.
  // 0 if nothing loaded.
  uint8_t currentPointSize() const { return loadedPointSize_; };

 private:
  struct LoadedFont {
    SdCardFont* font;
    int fontId;
    uint8_t size;         // actual point size of the font file
    uint8_t loadedFor;    // fontSizeEnum / role enum used when loading
    bool ownsFont;        // false = alias sharing another slot's font
  };
  static int computeFontId(uint32_t contentHash, const char* familyName, uint8_t pointSize);
  static uint8_t targetPointSize(uint8_t fontSizeEnum);

  int findSlotByEnum(uint8_t fontSizeEnum) const;

  std::string loadedFamilyName_;
  uint8_t loadedPointSize_ = 0;
  std::vector<LoadedFont> loaded_;
};

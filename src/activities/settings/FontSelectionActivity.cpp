#include "FontSelectionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "fontIds.h"

extern void setupUiFontFallback();

namespace {
constexpr const char* ELLIPSIS_UTF8 = "\xe2\x80\xa6";

int findCurrentFontIndex(const SdCardFontRegistry* registry, const CrossPointSettings::DirectionSettings& ds) {
  if (ds.sdFontFamilyName[0] != '\0' && registry) {
    const auto& families = registry->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      if (families[i].name == ds.sdFontFamilyName) {
        return CrossPointSettings::BUILTIN_FONT_COUNT + i;
      }
    }
  }

  return ds.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? ds.fontFamily : 0;
}
}  // namespace

FontSelectionActivity::FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const SdCardFontRegistry* registry, bool isVertical)
    : Activity("FontSelect", renderer, mappedInput), registry_(registry), isVertical_(isVertical) {}

void FontSelectionActivity::onEnter() {
  Activity::onEnter();

  // Get metrics and calculate layout dimensions
  metrics_ = UITheme::getInstance().getMetrics();
  afterHeader = metrics_.topPadding + metrics_.headerHeight + metrics_.verticalSpacing;
  bottomReserved = metrics_.buttonHintsHeight + metrics_.verticalSpacing;
  usableHeight = renderer.getScreenHeight() - afterHeader - bottomReserved;
  previewHeight = usableHeight * metrics_.previewHeightPercent / 100;

  auto& ds = SETTINGS.getDirectionSettings(isVertical_);
  originalFontFamily_ = ds.fontFamily;
  strncpy(originalSdFontFamilyName_, ds.sdFontFamilyName, sizeof(originalSdFontFamilyName_) - 1);
  originalSdFontFamilyName_[sizeof(originalSdFontFamilyName_) - 1] = '\0';

  fonts_.clear();
  fonts_.reserve(CrossPointSettings::BUILTIN_FONT_COUNT + (registry_ ? registry_->getFamilyCount() : 0));

  fonts_.push_back({I18N.get(StrId::STR_NOTO_SERIF), true, static_cast<uint8_t>(CrossPointSettings::NOTOSERIF)});
  fonts_.push_back({I18N.get(StrId::STR_NOTO_SANS), true, static_cast<uint8_t>(CrossPointSettings::NOTOSANS)});

  if (registry_) {
    const auto& families = registry_->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      fonts_.push_back({families[i].name, false, static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i)});
    }
  }

  selectedIndex_ = findCurrentFontIndex(registry_, SETTINGS.getDirectionSettings(isVertical_));
  previewFontIndex_ = selectedIndex_;

  requestUpdate();
}

void FontSelectionActivity::onExit() { Activity::onExit(); }

void FontSelectionActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    auto& ds = SETTINGS.getDirectionSettings(isVertical_);
    ds.fontFamily = originalFontFamily_;
    strncpy(ds.sdFontFamilyName, originalSdFontFamilyName_, sizeof(ds.sdFontFamilyName) - 1);
    ds.sdFontFamilyName[sizeof(ds.sdFontFamilyName) - 1] = '\0';
    sdFontSystem.ensureLoaded(renderer);
    setupUiFontFallback();
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedIndex_ == previewFontIndex_) {
      handleSelection();
    } else {
      auto& ds = SETTINGS.getDirectionSettings(isVertical_);
      previewFontIndex_ = selectedIndex_;
      const auto& font = fonts_[selectedIndex_];
      if (font.isBuiltin) {
        ds.fontFamily = font.settingIndex;
        ds.sdFontFamilyName[0] = '\0';
      } else if (registry_) {
        const int sdIdx = font.settingIndex - CrossPointSettings::BUILTIN_FONT_COUNT;
        const auto& families = registry_->getFamilies();
        if (sdIdx < static_cast<int>(families.size())) {
          strncpy(ds.sdFontFamilyName, families[sdIdx].name.c_str(), sizeof(ds.sdFontFamilyName) - 1);
          ds.sdFontFamilyName[sizeof(ds.sdFontFamilyName) - 1] = '\0';
          sdFontSystem.ensureLoaded(renderer);
          setupUiFontFallback();
        }
      }
      requestUpdate();
    }
    return;
  }

  const int listSize = static_cast<int>(fonts_.size());
  const int pageItems =
      UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, previewHeight + metrics_.verticalSpacing);

  buttonNavigator_.onNextRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, listSize);
    requestUpdate();
  });

  buttonNavigator_.onPreviousRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, listSize);
    requestUpdate();
  });

  buttonNavigator_.onNextContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator_.onPreviousContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems);
    requestUpdate();
  });
}

void FontSelectionActivity::handleSelection() {
  auto& ds = SETTINGS.getDirectionSettings(isVertical_);
  const auto& font = fonts_[selectedIndex_];
  if (font.settingIndex < CrossPointSettings::BUILTIN_FONT_COUNT) {
    ds.fontFamily = font.settingIndex;
    ds.sdFontFamilyName[0] = '\0';
  } else if (registry_) {
    const int sdIdx = font.settingIndex - CrossPointSettings::BUILTIN_FONT_COUNT;
    const auto& families = registry_->getFamilies();
    if (sdIdx < static_cast<int>(families.size())) {
      strncpy(ds.sdFontFamilyName, families[sdIdx].name.c_str(), sizeof(ds.sdFontFamilyName) - 1);
      ds.sdFontFamilyName[sizeof(ds.sdFontFamilyName) - 1] = '\0';
    }
  }
  finish();
}

void FontSelectionActivity::renderPreviewPane(int top, int height, int fontId, const char* fontName) const {
  const int left = metrics_.previewPadding;
  const int width = renderer.getScreenWidth() - (metrics_.previewPadding * 2);
  if (width <= 0 || height <= 0) return;

  const int labelFontId = UI_10_FONT_ID;
  const int labelH = renderer.getTextHeight(labelFontId);
  const int labelGap = 4;
  const int labelReserved = labelH + labelGap + metrics_.previewPadding;

  char labelBuf[128];
  snprintf(labelBuf, sizeof(labelBuf), "%s \"%s\"", tr(STR_PREVIEW), fontName ? fontName : "");
  const int labelY = top + height - metrics_.previewPadding - labelH;
  renderer.drawText(labelFontId, left, labelY, labelBuf);

  if (fontId == 0) return;

  const int lineH = renderer.getTextHeight(fontId);
  if (lineH <= 0) return;

  const int innerHeight = height - metrics_.previewPadding - labelReserved;
  const int maxLines = std::max(1, innerHeight / (lineH + 2));

  const char* previewText = I18N.get(StrId::STR_FONT_PREVIEW_TEXT);
  if (auto* fcm = renderer.getFontCacheManager()) {
    char prewarmBuf[256];
    snprintf(prewarmBuf, sizeof(prewarmBuf), "%s %s", previewText, ELLIPSIS_UTF8);
    fcm->prewarmCache(fontId, prewarmBuf, 0x01);
  }

  const auto lines = renderer.wrappedText(fontId, previewText, width, maxLines);

  int y = top + metrics_.previewPadding;
  const int textBottomLimit = top + height - labelReserved;
  for (const auto& line : lines) {
    if (y + lineH > textBottomLimit) break;
    renderer.drawText(fontId, left, y, line.c_str());
    y += lineH + 2;
  }
}

void FontSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics_.topPadding, pageWidth, metrics_.headerHeight}, tr(STR_FONT_FAMILY));

  const int previewTop = afterHeader;
  const int listTop = previewTop + previewHeight + metrics_.verticalSpacing;
  const int listHeight = usableHeight - previewHeight - metrics_.verticalSpacing;

  const int previewFontId = SETTINGS.getReaderFontId(isVertical_);
  const char* previewFontName = (previewFontIndex_ >= 0 && previewFontIndex_ < static_cast<int>(fonts_.size()))
                                    ? fonts_[previewFontIndex_].name.c_str()
                                    : nullptr;
  renderPreviewPane(previewTop, previewHeight, previewFontId, previewFontName);

  renderer.drawLine(0, listTop - metrics_.verticalSpacing / 2, pageWidth, listTop - metrics_.verticalSpacing / 2);

  CrossPointSettings::DirectionSettings origDs;
  origDs.fontFamily = originalFontFamily_;
  strncpy(origDs.sdFontFamilyName, originalSdFontFamilyName_, sizeof(origDs.sdFontFamilyName) - 1);
  origDs.sdFontFamilyName[sizeof(origDs.sdFontFamilyName) - 1] = '\0';
  const int currentFontIndex = findCurrentFontIndex(registry_, origDs);
  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listHeight}, static_cast<int>(fonts_.size()), selectedIndex_,
      [this](int index) { return fonts_[index].name; }, nullptr, nullptr,
      [this, currentFontIndex](int index) -> std::string {
        if (index == previewFontIndex_ && index != currentFontIndex) return tr(STR_PREVIEW);
        if (index == currentFontIndex) return tr(STR_SELECTED);
        return "";
      },
      true);

  const bool onPreviewed = selectedIndex_ == previewFontIndex_;
  const char* confirmLabel = onPreviewed ? tr(STR_SELECT) : tr(STR_PREVIEW);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

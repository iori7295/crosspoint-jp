#include "LanguageSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <iterator>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "fontIds.h"

void LanguageSelectActivity::onEnter() {
  Activity::onEnter();

  // Filter to just English + Japanese for simplicity.
  // All 27 languages are still compiled in (translations are always present),
  // but only these two are shown in the picker.
  visibleLanguages.clear();
  const auto langCount = getLanguageCount();
  for (uint8_t i = 0; i < langCount; i++) {
    const auto idx = SORTED_LANGUAGE_INDICES[i];
    if (idx == static_cast<uint8_t>(Language::EN) || idx == static_cast<uint8_t>(Language::JAPANESE)) {
      visibleLanguages.push_back(idx);
    }
  }
  totalItems = static_cast<int>(visibleLanguages.size());

  // Set current selection based on current language
  const auto currentLang = static_cast<uint8_t>(I18N.getLanguage());
  const auto* begin = visibleLanguages.data();
  const auto* end = begin + visibleLanguages.size();
  const auto* it = std::find(begin, end, currentLang);
  selectedIndex = (it != end) ? std::distance(begin, it) : 0;

  requestUpdate();
}

void LanguageSelectActivity::onExit() { Activity::onExit(); }

void LanguageSelectActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);

  // Handle navigation
  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(static_cast<int>(selectedIndex), totalItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(static_cast<int>(selectedIndex), totalItems);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectedIndex), totalItems, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectedIndex), totalItems, pageItems);
    requestUpdate();
  });
}

void LanguageSelectActivity::handleSelection() {
  const uint8_t langIndex = visibleLanguages[selectedIndex];

  {
    RenderLock lock(*this);
    I18N.setLanguage(static_cast<Language>(langIndex));
  }

  SETTINGS.language = langIndex;
  SETTINGS.saveToFile();

  // Return to previous page
  onBack();
}

void LanguageSelectActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LANGUAGE));

  // Current language marker
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const auto currentLang = static_cast<uint8_t>(I18N.getLanguage());
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, totalItems, selectedIndex,
      [this](int index) {
        const uint8_t langIdx = visibleLanguages[index];
        return std::string(LANGUAGE_CODES[langIdx]) + " " + I18N.getLanguageName(static_cast<Language>(langIdx));
      },
      nullptr, nullptr,
      [this, currentLang](int index) { return visibleLanguages[index] == currentLang ? tr(STR_SELECTED) : ""; },
      true);

  // Button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

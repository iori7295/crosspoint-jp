#include "TextBlock.h"

#include <BidiUtils.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>

int TextBlock::rubyFontId = -1;

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y) const {
  if (isVertical) {
    if (words.size() != wordStyles.size() ||
        (!wordXpos.empty() && words.size() != wordXpos.size()) ||
        (!wordYpos.empty() && words.size() != wordYpos.size())) {
      LOG_ERR("TXB", "Vertical render skipped: size mismatch (words=%u styles=%u xpos=%u ypos=%u)",
              (uint32_t)words.size(), (uint32_t)wordStyles.size(),
              (uint32_t)wordXpos.size(), (uint32_t)wordYpos.size());
      return;
    }
    const int logicalMax = std::max<int>(renderer.getDisplayWidth(), renderer.getDisplayHeight());
    const int baseLineHeight = renderer.getLineHeight(fontId);
    const int rubyLineHeight = (rubyFontId >= 0) ? renderer.getLineHeight(rubyFontId) : 0;
    const int lowerClip = -std::max(baseLineHeight, rubyLineHeight) * 2;
    const int columnWidth = baseLineHeight;
    for (size_t i = 0; i < words.size(); i++) {
      const int wordX = wordXpos.empty() ? x : wordXpos[i] + x;
      const int wordY = (i < wordYpos.size()) ? y + wordYpos[i] : y;
      if (wordY > logicalMax || wordY < lowerClip) continue;
      if (wordX > logicalMax || wordX < -logicalMax / 2) continue;

      const auto vb = (i < wordVerticalBehaviors.size())
                          ? wordVerticalBehaviors[i]
                          : VerticalTextUtils::VerticalBehavior::Upright;

      if (vb == VerticalTextUtils::VerticalBehavior::Sideways) {
        renderer.drawTextSideways(fontId, wordX, wordY, words[i].c_str(), true, wordStyles[i], columnWidth);
      } else {
        renderer.drawTextVertical(fontId, wordX, wordY, words[i].c_str(), true, wordStyles[i]);
      }

      if (i < rubyTexts.size() && !rubyTexts[i].empty() && rubyFontId >= 0) {
        const int rubyX = wordX + renderer.getLineHeight(fontId);
        if (wordY <= logicalMax && wordY >= lowerClip &&
            rubyX <= logicalMax && rubyX >= -logicalMax / 2) {
          renderer.drawTextVertical(rubyFontId, rubyX, wordY, rubyTexts[i].c_str(), true,
                                    EpdFontFamily::REGULAR);
        }
      }
    }
    return;
  }

  const bool hasFocus = !wordFocusBoundary.empty();
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size() ||
      (hasFocus && (words.size() != wordFocusBoundary.size() || words.size() != wordFocusSuffixX.size()))) {
    LOG_ERR("TXB", "Render skipped: size mismatch (words=%u, xpos=%u, styles=%u, boundary=%u, suffixX=%u)\n",
            (uint32_t)words.size(), (uint32_t)wordXpos.size(), (uint32_t)wordStyles.size(),
            (uint32_t)wordFocusBoundary.size(), (uint32_t)wordFocusSuffixX.size());
    return;
  }

  const bool scanning = renderer.isFontCacheScanning();
  const int ascender = renderer.getFontAscenderSize(fontId);
  for (size_t i = 0; i < words.size(); i++) {
    const int wordX = wordXpos[i] + x;
    const EpdFontFamily::Style currentStyle = wordStyles[i];
    const auto baseDir = static_cast<BidiUtils::BidiBaseDir>(
        BidiUtils::detectParagraphLevel(words[i].c_str(), blockStyle.isRtl ? 1 : 0));
    const uint8_t boundary = hasFocus ? wordFocusBoundary[i] : 0;

    int wordY = y;
    if ((currentStyle & EpdFontFamily::SUP) != 0) {
      wordY -= ascender * 2 / 5;
    } else if ((currentStyle & EpdFontFamily::SUB) != 0) {
      wordY += ascender / 4;
    }

    if (boundary > 0) {
      static constexpr size_t MAX_FOCUS_PREFIX_BYTES = 9 * 4 + 1;
      char boldBuf[40];
      static_assert(sizeof(boldBuf) >= MAX_FOCUS_PREFIX_BYTES,
                    "boldBuf too small for max focus prefix (9 codepoints * 4 UTF-8 bytes + null)");
      const auto boldStyle = static_cast<EpdFontFamily::Style>(currentStyle | EpdFontFamily::BOLD);
      const size_t boldLen = std::min<size_t>({static_cast<size_t>(boundary), words[i].size(), sizeof(boldBuf) - 1});
      memcpy(boldBuf, words[i].c_str(), boldLen);
      boldBuf[boldLen] = '\0';
      renderer.drawText(fontId, wordX, wordY, boldBuf, true, boldStyle, baseDir);
      const int suffixX = wordX + wordFocusSuffixX[i];
      renderer.drawText(fontId, suffixX, wordY, words[i].c_str() + boldLen, true, currentStyle, baseDir);
    } else {
      renderer.drawText(fontId, wordX, wordY, words[i].c_str(), true, currentStyle, baseDir);
    }

    if (i < rubyTexts.size() && !rubyTexts[i].empty() && rubyFontId >= 0) {
      const int baseWidth = renderer.getTextAdvanceX(fontId, words[i].c_str(), currentStyle);
      const int rubyWidth = renderer.getTextWidth(rubyFontId, rubyTexts[i].c_str(), EpdFontFamily::REGULAR);
      const int rubyX = wordX + (baseWidth - rubyWidth) / 2;
      const int rubyY = wordY - renderer.getLineHeight(rubyFontId) - 1;
      renderer.drawText(rubyFontId, rubyX, rubyY, rubyTexts[i].c_str(), true, EpdFontFamily::REGULAR);
    }

    if (!scanning && (currentStyle & EpdFontFamily::UNDERLINE) != 0) {
      const std::string& w = words[i];
      int underlineWidth = renderer.getTextWidth(fontId, w.c_str(), currentStyle, baseDir);
      const int underlineY = wordY + ascender + 2;

      if ((currentStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
        underlineWidth = (underlineWidth + 1) / 2;
      }

      renderer.drawLine(wordX, underlineY, wordX + underlineWidth, underlineY, true);
    }
  }

  // Full-width separator below h1/h2 headings
  if (!scanning && blockStyle.drawSeparatorBelow && !blockStyle.isRtl && !words.empty()) {
    const int firstWordX = wordXpos.empty() ? x : wordXpos[0] + x;
    const int lastWordEnd = renderer.getTextWidth(fontId, words.back().c_str(), wordStyles.back());
    const int lastWordX = wordXpos.empty() ? x : wordXpos.back() + x;
    // Draw from the first word left edge to the last word right edge
    int sepX = firstWordX;
    int sepW = lastWordX + lastWordEnd - firstWordX;
    // On the first line of the block, draw below the ascender line
    const int firstAscender = renderer.getFontAscenderSize(fontId);
    // Find the first visible word's Y (could be SUP/SUB adjusted)
    int firstVisibleY = y;
    const auto firstStyle = words.empty() ? EpdFontFamily::REGULAR : wordStyles[0];
    if ((firstStyle & EpdFontFamily::SUP) != 0) firstVisibleY -= firstAscender * 2 / 5;
    else if ((firstStyle & EpdFontFamily::SUB) != 0) firstVisibleY += firstAscender / 4;
    const int sepY = firstVisibleY + firstAscender + 3;
    renderer.drawLine(sepX, sepY, sepX + sepW, sepY, true);
  }
}

bool TextBlock::hasRuby() const {
  for (const auto& rt : rubyTexts) {
    if (!rt.empty()) return true;
  }
  return false;
}

bool TextBlock::serialize(HalFile& file) const {
  // Focus annotations are optional; vectors are either empty (no splits in this block)
  // or sized in lockstep with words[].
  const bool hasFocus = !wordFocusBoundary.empty();
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size() ||
      (hasFocus && (words.size() != wordFocusBoundary.size() || words.size() != wordFocusSuffixX.size()))) {
    LOG_ERR("TXB", "Serialization failed: size mismatch (words=%u, xpos=%u, styles=%u, boundary=%u, suffixX=%u)\n",
            static_cast<uint32_t>(words.size()), static_cast<uint32_t>(wordXpos.size()),
            static_cast<uint32_t>(wordStyles.size()), static_cast<uint32_t>(wordFocusBoundary.size()),
            static_cast<uint32_t>(wordFocusSuffixX.size()));
    return false;
  }

  // Word data
  serialization::writePod(file, static_cast<uint16_t>(words.size()));
  for (const auto& w : words) serialization::writeString(file, w);
  for (auto x : wordXpos) serialization::writePod(file, x);
  for (auto s : wordStyles) serialization::writePod(file, s);
  // Focus block: 1-byte presence flag, followed by per-word vectors only when present.
  // Saves 3 bytes/word when focus reading is disabled or no word on this line was split.
  serialization::writePod(file, static_cast<uint8_t>(hasFocus ? 1 : 0));
  if (hasFocus) {
    for (auto b : wordFocusBoundary) serialization::writePod(file, b);
    for (auto sx : wordFocusSuffixX) serialization::writePod(file, sx);
  }

  // Style (alignment + margins/padding/indent)
  serialization::writePod(file, blockStyle.alignment);
  serialization::writePod(file, blockStyle.textAlignDefined);
  serialization::writePod(file, blockStyle.marginTop);
  serialization::writePod(file, blockStyle.marginBottom);
  serialization::writePod(file, blockStyle.marginLeft);
  serialization::writePod(file, blockStyle.marginRight);
  serialization::writePod(file, blockStyle.paddingTop);
  serialization::writePod(file, blockStyle.paddingBottom);
  serialization::writePod(file, blockStyle.paddingLeft);
  serialization::writePod(file, blockStyle.paddingRight);
  serialization::writePod(file, blockStyle.textIndent);
  serialization::writePod(file, blockStyle.textIndentDefined);
  serialization::writePod(file, blockStyle.isRtl);
  serialization::writePod(file, blockStyle.directionDefined);

  serialization::writePod(file, isVertical);
  if (isVertical) {
    for (auto y : wordYpos) serialization::writePod(file, y);
    // Persist per-word vertical behaviour so Sideways/TateChuYoko survive
    // section cache reload. Presence flag + array to maintain backward compat
    // with old cache files (presence=0 → skip in deserialize).
    const bool hasBehaviors = (wordVerticalBehaviors.size() == words.size());
    serialization::writePod(file, static_cast<uint8_t>(hasBehaviors ? 1 : 0));
    if (hasBehaviors) {
      for (auto vb : wordVerticalBehaviors)
        serialization::writePod(file, static_cast<uint8_t>(vb));
    }
  }

  for (size_t i = 0; i < words.size(); i++) {
    serialization::writeString(file, (i < rubyTexts.size()) ? rubyTexts[i] : std::string());
  }

  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(HalFile& file) {
  uint16_t wc;
  std::vector<std::string> words;
  std::vector<int16_t> wordXpos;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<uint8_t> wordFocusBoundary;
  std::vector<uint16_t> wordFocusSuffixX;
  BlockStyle blockStyle;

  // Word count
  serialization::readPod(file, wc);

  // Sanity check: prevent allocation of unreasonably large vectors (max 10000 words per block)
  if (wc > 10000) {
    LOG_ERR("TXB", "Deserialization failed: word count %u exceeds maximum", wc);
    return nullptr;
  }

  // Word data
  words.resize(wc);
  wordXpos.resize(wc);
  wordStyles.resize(wc);
  for (auto& w : words) serialization::readString(file, w);
  for (auto& x : wordXpos) serialization::readPod(file, x);
  for (auto& s : wordStyles) serialization::readPod(file, s);
  // Focus block: presence flag, then vectors only if present. Empty vectors when absent
  // signal "no splits in this block" to render() (zero per-word RAM cost).
  uint8_t hasFocus;
  serialization::readPod(file, hasFocus);
  if (hasFocus) {
    wordFocusBoundary.resize(wc);
    wordFocusSuffixX.resize(wc);
    for (auto& b : wordFocusBoundary) serialization::readPod(file, b);
    for (auto& sx : wordFocusSuffixX) serialization::readPod(file, sx);
  }

  // Style (alignment + margins/padding/indent)
  serialization::readPod(file, blockStyle.alignment);
  serialization::readPod(file, blockStyle.textAlignDefined);
  serialization::readPod(file, blockStyle.marginTop);
  serialization::readPod(file, blockStyle.marginBottom);
  serialization::readPod(file, blockStyle.marginLeft);
  serialization::readPod(file, blockStyle.marginRight);
  serialization::readPod(file, blockStyle.paddingTop);
  serialization::readPod(file, blockStyle.paddingBottom);
  serialization::readPod(file, blockStyle.paddingLeft);
  serialization::readPod(file, blockStyle.paddingRight);
  serialization::readPod(file, blockStyle.textIndent);
  serialization::readPod(file, blockStyle.textIndentDefined);
  serialization::readPod(file, blockStyle.isRtl);
  serialization::readPod(file, blockStyle.directionDefined);

  bool vertical = false;
  serialization::readPod(file, vertical);
  std::vector<int16_t> wordYpos;
  std::vector<VerticalTextUtils::VerticalBehavior> wordVerticalBehaviors;
  if (vertical) {
    wordYpos.resize(wc);
    for (auto& y : wordYpos) serialization::readPod(file, y);
    // Read per-word vertical behaviour if present (new format, presence flag).
    uint8_t hasBehaviors = 0;
    serialization::readPod(file, hasBehaviors);
    if (hasBehaviors) {
      wordVerticalBehaviors.resize(wc);
      for (auto& vb : wordVerticalBehaviors) {
        uint8_t raw = 0;
        serialization::readPod(file, raw);
        vb = static_cast<VerticalTextUtils::VerticalBehavior>(raw);
      }
    }
  }

  std::vector<std::string> rubyTexts(wc);
  for (auto& rt : rubyTexts) serialization::readString(file, rt);

  auto tb = new (std::nothrow) TextBlock(std::move(words), std::move(wordXpos), std::move(wordStyles),
                                          std::move(wordFocusBoundary), std::move(wordFocusSuffixX),
                                          blockStyle, std::move(wordYpos), vertical, std::move(rubyTexts),
                                          std::move(wordVerticalBehaviors));
  if (!tb) return nullptr;
  return std::unique_ptr<TextBlock>(tb);
}

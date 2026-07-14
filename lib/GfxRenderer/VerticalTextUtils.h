#pragma once

#include <cstdint>

namespace VerticalTextUtils {

enum class VerticalBehavior : uint8_t {
  Upright,
  Sideways,
  TateChuYoko,
};

struct PunctuationOffset {
  uint32_t codepoint;
  int8_t dxEighths;
  int8_t dyEighths;
  bool rotate;
};

static constexpr PunctuationOffset VERTICAL_PUNCTUATION[] = {
    {0x3001, 3, -3, false},
    {0x3002, 3, -3, false},
    {0xFF0C, 3, -3, false},
    {0xFF0E, 3, -3, false},
    {0x30FC, 0, 0, true},
    {0x2014, 0, 0, true},
    {0x2015, 0, 0, true},
    {0x2026, 0, 0, true},
    {0xFF5E, 0, 0, true},
};
static constexpr int VERTICAL_PUNCTUATION_COUNT =
    sizeof(VERTICAL_PUNCTUATION) / sizeof(VERTICAL_PUNCTUATION[0]);

inline const PunctuationOffset* getVerticalPunctuationOffset(uint32_t cp) {
  for (int i = 0; i < VERTICAL_PUNCTUATION_COUNT; i++) {
    if (VERTICAL_PUNCTUATION[i].codepoint == cp) return &VERTICAL_PUNCTUATION[i];
  }
  return nullptr;
}

inline bool isUprightInVertical(uint32_t cp) {
  if (cp >= 0x4E00 && cp <= 0x9FFF) return true;
  if (cp >= 0x3400 && cp <= 0x4DBF) return true;
  if (cp >= 0x3040 && cp <= 0x309F) return true;
  if (cp >= 0x30A0 && cp <= 0x30FF) return true;
  if (cp >= 0x3000 && cp <= 0x303F) return true;
  if (cp >= 0xFF00 && cp <= 0xFFEF) return true;
  if (cp >= 0xF900 && cp <= 0xFAFF) return true;
  if (cp >= 0xFFE0 && cp <= 0xFFEF) return true;
  if (cp >= 0x3200 && cp <= 0x32FF) return true;
  if (cp >= 0x3300 && cp <= 0x33FF) return true;
  if (cp >= 0x3100 && cp <= 0x312F) return true;
  if (cp >= 0xAC00 && cp <= 0xD7AF) return true;
  return false;
}

}  // namespace VerticalTextUtils

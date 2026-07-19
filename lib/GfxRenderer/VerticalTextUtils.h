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
    // Commas and periods (移す)
    {0x3001, 3, -3, false},   // 、
    {0x3002, 3, -3, false},   // 。
    {0xFF0C, 3, -3, false},   // ，
    {0xFF0E, 3, -3, false},   // ．
    // Middle dot
    {0x30FB, 0, 0, false},    // ・
    {0xFF65, 0, 0, false},    // ･
    // Colon, semicolon, exclamation, question
    {0xFF1A, 2, -2, false},   // ：
    {0xFF1B, 2, -2, false},   // ；
    {0xFF01, 2, -2, false},   // ！
    {0xFF1F, 2, -2, false},   // ？
    // ASCII punctuation
    {'.', 2, -2, false},      // .
    {',', 2, -2, false},      // ,
    {':', 2, -2, false},      // :
    {';', 2, -2, false},      // ;
    {'!', 2, -2, false},      // !
    {'?', 2, -2, false},      // ?
    // Opening brackets (上げ)
    {0x300C, -3, -2, false},  // 「
    {0x300E, -3, -2, false},  // 『
    {0x3008, -3, -2, false},  // 〈
    {0x300A, -3, -2, false},  // 《
    {0xFF08, -3, -2, false},  // （
    {0xFF3B, -3, -2, false},  // ［
    {0x3014, -3, -2, false},  // 〔
    {0x3010, -3, -2, false},  // 【
    {'(', -2, -1, false},     // (
    {'[', -2, -1, false},     // [
    {'{', -2, -1, false},     // {
    // Closing brackets (下げ)
    {0x300D, 3, -2, false},   // 」
    {0x300F, 3, -2, false},   // 』
    {0x3009, 3, -2, false},   // 〉
    {0x300B, 3, -2, false},   // 》
    {0xFF09, 3, -2, false},   // ）
    {0xFF3D, 3, -2, false},   // ］
    {0x3015, 3, -2, false},   // 〕
    {0x3011, 3, -2, false},   // 】
    {')', 2, -1, false},      // )
    {']', 2, -1, false},      // ]
    {'}', 2, -1, false},      // }
    // Long marks and dashes (回転)
    {0x30FC, 0, 0, true},     // ー
    {0x2014, 0, 0, true},     // —
    {0x2015, 0, 0, true},     // ―
    {0x2026, 0, 0, true},     // …
    {0xFF5E, 0, 0, true},     // ～
    {0x301C, 0, 0, true},     // 〜 (wave dash)
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
  switch (cp) {
    case '.': case ',': case ':': case ';': case '!': case '?':
    case '(' : case ')' : case '[' : case ']' : case '{' : case '}':
      return true;
    default:
      break;
  }
  return false;
}

/// Returns true if the font may have a vertical-specific glyph (via OTVert /
/// 'vert' feature) for this codepoint.  Callers that have access to font
/// metadata can use this to decide between the default upright glyph and the
/// font-provided vertical variant.
inline bool shouldUseVertGlyph(uint32_t cp) {
  // CJK brackets, punctuation, and fullwidth forms commonly have vert
  // glyphs in CJK fonts (e.g. BIZ UDGothic, Source Han Sans).
  return (cp >= 0x3000 && cp <= 0x303F) ||  // CJK Symbols & Punctuation
         (cp >= 0x3099 && cp <= 0x309C) ||  // Combining marks, long vowel
         (cp >= 0x30A0 && cp <= 0x30FF) ||  // Katakana (some have vert)
         (cp >= 0xFF01 && cp <= 0xFF60) ||  // Fullwidth forms
         (cp == 0x2014 || cp == 0x2015 || cp == 0x2026 || cp == 0x301C ||
          cp == 0x30FC || cp == 0xFF5E);     // Dashes, ellipsis, wave dash
}

/// Characters that cannot appear at the start of a line in Japanese typesetting.
inline bool isKinsokuHead(uint32_t cp) {
  // Closing brackets, punctuation, small kana, long vowel mark, etc.
  // Includes all entries from zrn-ns plus CJK Compatibility closed brackets.
  return cp == 0x3001 || cp == 0x3002 ||  // 、。
         cp == 0xFF0C || cp == 0xFF0E ||  // ，
         cp == 0x300D || cp == 0x300F ||  // 」』
         cp == 0x3009 || cp == 0x300B ||  // 〉》
         cp == 0x3015 || cp == 0x3011 ||  // 〕】
         cp == 0xFF09 || cp == 0xFF3D ||  // ）］
         cp == 0xFF5D ||                  // ｝
         cp == 0x3017 || cp == 0x3019 ||  // 〗〙
         cp == 0x301B ||                  // 〛
         cp == 0x301D ||                  // 〝
         cp == 0x30FC ||                  // ー (chōonpu)
         cp == 0x3041 || cp == 0x3043 ||  // ぁぃ
         cp == 0x3045 || cp == 0x3047 ||  // ぅぇ
         cp == 0x3049 ||                  // ぉ
         cp == 0x3063 ||                  // っ
         cp == 0x3083 || cp == 0x3085 ||  // ゃゅ
         cp == 0x3087 || cp == 0x308E ||  // ょゎ
         cp == 0x30A1 || cp == 0x30A3 ||  // ァィ
         cp == 0x30A5 || cp == 0x30A7 ||  // ゥェ
         cp == 0x30A9 ||                  // ォ
         cp == 0x30C3 ||                  // ッ
         cp == 0x30E3 || cp == 0x30E5 ||  // ャュ
         cp == 0x30E7 || cp == 0x30EE ||  // ョヮ
         cp == 0x30F5 || cp == 0x30F6 ||  // ヵヶ
         cp == '.' || cp == ',' || cp == ':' || cp == ';' ||
         cp == '!' || cp == '?' ||
         cp == ')' || cp == ']' || cp == '}';
}

/// Characters that cannot appear at the end of a line in Japanese typesetting.
inline bool isKinsokuTail(uint32_t cp) {
  // Opening brackets that should not end a line
  return cp == 0x300C || cp == 0x300E ||  // 「『
         cp == 0x3008 || cp == 0x300A ||  // 〈《
         cp == 0x3014 || cp == 0x3010 ||  // 〔【
         cp == 0xFF08 || cp == 0xFF3B ||  // （［
         cp == 0xFF5B ||                  // ｛
         cp == 0x3016 || cp == 0x3018 ||  // 〖〘
         cp == 0x301A ||                  // 〚
         cp == '(' || cp == '[' || cp == '{';
}

}  // namespace VerticalTextUtils

// Imported from matcha-reader (https://github.com/eszter007/matcha-reader) - MIT License
#include "VerticalSection.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <FsHelpers.h>

#include <cstring>
#include <string>

#include "Epub/converters/ImageDecoderFactory.h"
#include "GfxRenderer.h"

namespace {

// v51: bumped again past every v50 cache -- confirmed on a real device that v50's retry logic ran
// correctly (logged "removing partial cache file" for several broken images) but several STILL
// failed extraction on that same rebuild pass: the zip inflate's 32KB contiguous block was
// competing with the font decompressor's hot-group buffer, still resident from this same
// chapter's column-fitting text measurements. Now frees that buffer immediately before each image
// extraction attempt. A chapter rebuilt under v50 can have images that failed even with the retry
// logic and are now stuck cached as "extraction already tried, still missing" until a fresh
// rebuild gives the fix (more headroom, not just a retry) a chance to actually help. See cache
// format comment below.
// v52: not a format change -- forces a rebuild of vertical caches that were built while the CSS
// rule table was still held resident (see Epub::load): its heap fragmentation made the layout's
// stream reserve fail on long chapters, silently truncating them into sparse pages ON DISK.
constexpr uint8_t VSECTION_FILE_VERSION = 59;

// Helper to detect gaiji (external character) image paths.  Gaiji images are
// small PNGs used for rare kanji not covered by the font; missing them should
// not break the entire chapter build.  Returns true when the path suggests an
// EPUB gaiji or external-character reference.
bool isLikelyGaijiImagePath(const std::string& path) {
  // Common EPUB gaiji path patterns.
  if (path.find("/gaiji/") != std::string::npos) return true;
  if (path.find("/外字/") != std::string::npos) return true;
  // Some EPUBs put gaiji directly in OEBPF/gaiji*.png.
  const size_t slash = path.rfind('/');
  const std::string name = (slash != std::string::npos) ? path.substr(slash + 1) : path;
  return name.rfind("gaiji", 0) == 0 || name.rfind("GAJI", 0) == 0 || name.rfind("外字", 0) == 0;
}
static constexpr uint16_t VSECTION_FLAG_PARTIAL = 0x0001;
// Header layout: u8 version, i32 fontId, u16 viewportW, u16 viewportH, u16 pageCount, u32 indexOffset, u16 flags
static constexpr uint32_t VSECTION_HEADER_PCOUNT_OFF = sizeof(uint8_t) + sizeof(int32_t) + sizeof(uint16_t) + sizeof(uint16_t);
static constexpr uint32_t VSECTION_HEADER_IDX_OFF = VSECTION_HEADER_PCOUNT_OFF + sizeof(uint16_t);
static constexpr uint32_t VSECTION_HEADER_FLAGS_OFF = VSECTION_HEADER_IDX_OFF + sizeof(uint32_t);
// 4KB, not 1KB: chapter builds are SD-latency-bound -- the inflate staging write, the
// staging read-back, and the expat feed each touch the card once per chunk, so quadrupling
// the chunk quarters the transaction count for ~12KB of transient buffers.
constexpr size_t PARSE_BUFFER_SIZE = 2048;

using RubyRun = VerticalParsedText::RubyRun;

// Receives extraction output in document order, one paragraph or image at a time. The extractor
// deliberately has no whole-chapter storage: a real Japanese chapter's text plus its laid-out
// glyph pages runs to megabytes, which can never fit in the ESP32-C3's ~220KB heap (the previous
// accumulate-everything design only worked in the desktop emulator's 8MB heap). See
// VerticalSection.h for the full memory model.
struct ParagraphSink {
  virtual ~ParagraphSink() = default;
  // Takes ownership of the runs; the extractor's buffer is cleared after the call.
  // Consumes the runs' contents (moves the strings out) and clear()s the vector, so the
  // caller's buffer keeps its capacity across paragraphs. continuesPrevious=true means these
  // runs seamlessly continue the previously delivered paragraph (streaming cadence) -- no
  // paragraph break must be recorded before them.
  virtual void onParagraph(std::vector<RubyRun>& runs, bool continuesPrevious) = 0;
  virtual void onImage(const std::string& src) = 0;
};

struct TextExtractor {
  // Each paragraph is a sequence of RubyRun entries. Unannotated text has
  // empty ruby; annotated text (<ruby>base<rt>reading</rt></ruby>) maps
  // base -> rubyText.
  ParagraphSink* sink = nullptr;

  std::vector<RubyRun> currentRuns;
  std::string currentText;
  int blockDepth = 0;
  int skipDepth = -1;

  // Pathological books put an entire chapter in one <p>/<div>. currentText/currentRuns are the
  // only unbounded-by-markup buffers left in extraction, so force a paragraph split once the
  // accumulated text passes this size -- the split hands the text to the sink (which lays out and
  // flushes pages to SD), keeping extraction O(bounded-paragraph) instead of O(chapter). A forced
  // split starts a new column mid-paragraph; harmless compared to the alternative (OOM).
  static constexpr size_t MAX_PARAGRAPH_BYTES = 16 * 1024;

  // Ruby parsing state
  bool inRuby = false;
  bool inRt = false;
  bool inRp = false;
  std::string rubyBase;
  std::string rubyAnnotation;

  // Style tracking — each entry records the elementDepth at which
  // bold/italic was activated. On endElement, if we're leaving that
  // depth, pop and flush.
  int boldDepth = 0;
  int italicDepth = 0;
  int elementDepth = 0;
  static constexpr int MAX_STYLE_STACK = 8;
  int boldOpenedAtDepth[MAX_STYLE_STACK] = {};
  int boldStackSize = 0;
  int italicOpenedAtDepth[MAX_STYLE_STACK] = {};
  int italicStackSize = 0;
  int emphasisDepth = 0;
  int emphasisOpenedAtDepth[MAX_STYLE_STACK] = {};
  int emphasisStackSize = 0;

  bool hasEmphasis() const { return emphasisDepth > 0; }

  // Diagnostic: bisects a ~11KB drop seen accumulating within a single furigana-dense paragraph's
  // SAX processing, too large to be explained by RubyRun/string vector growth alone. Call at the
  // top of each SAX callback -- logs only on a drop since the last checkpoint, so a normal chapter
  // stays quiet and only the actual culprit tag/callback prints. Remove once found.
  uint32_t lastCheckpointMaxAlloc = 0;
  void checkHeap(const char* phase, const char* tag = "") {
    const uint32_t now = ESP.getMaxAllocHeap();
    if (lastCheckpointMaxAlloc != 0 && now + 256 < lastCheckpointMaxAlloc) {
      LOG_DBG("VSC", "heap drop at %s(%s): %u -> %u", phase, tag, lastCheckpointMaxAlloc, now);
    }
    lastCheckpointMaxAlloc = now;
  }

  static bool isBoldTag(const char* name) {
    return strcasecmp(name, "b") == 0 || strcasecmp(name, "strong") == 0;
  }
  static bool isItalicTag(const char* name) {
    return strcasecmp(name, "i") == 0 || strcasecmp(name, "em") == 0;
  }
  uint8_t currentStyle() const {
    uint8_t s = 0;
    if (boldDepth > 0) s |= 1;    // EpdFontFamily::BOLD
    if (italicDepth > 0) s |= 2;  // EpdFontFamily::ITALIC
    return s;
  }

  static bool isSkipTag(const char* name) {
    return strcasecmp(name, "head") == 0 || strcasecmp(name, "style") == 0 || strcasecmp(name, "script") == 0;
  }

  // std::move()-ing currentText/currentRuns into the sink hands off their heap buffer and leaves
  // the local variable at capacity 0 -- so without a reserve() right after, the NEXT run/paragraph
  // has to regrow from scratch via std::string/vector's own doubling, one malloc+free cycle at a
  // time. For furigana-dense text (ruby annotations on nearly every kanji, e.g. "kyokasho"-style
  // textbook readings) that's dozens to hundreds of tiny alloc/free cycles per paragraph -- exactly
  // the kind of churn that fragments a ~220KB heap. These hints are small requests sized for the
  // common case (a handful of CJK characters / a handful of runs between markup boundaries), not a
  // correctness requirement -- an unusually long run still grows normally via doubling.
  static constexpr size_t TEXT_RESERVE_HINT = 128;   // bytes
  static constexpr size_t RUBY_RESERVE_HINT = 32;    // bytes
  static constexpr size_t RUNS_RESERVE_HINT = 16;    // elements

  void flushCurrentText() {
    if (!currentText.empty()) {
      // COPY, don't move: moving handed currentText's grown buffer to a transient RubyRun and
      // restarted this one at TEXT_RESERVE_HINT, so every paragraph re-grew it by doubling
      // (alloc-copy-free per step, hundreds of times per chapter) -- the main planter of the
      // persistent fragments that shredded maxAlloc on long single-file books. The copy is a
      // transient that coalesces back; currentText's capacity now lives for the whole build.
      currentRuns.push_back(RubyRun{currentText, {}, currentStyle(), hasEmphasis()});
      currentText.clear();
    }
  }

  // Streaming accumulation bounds: hand runs to the sink every ~SOFT_FLUSH_BYTES (or
  // SOFT_FLUSH_RUNS for furigana-dense text) as a seamless paragraph CONTINUATION, instead of
  // buffering whole paragraphs SAX-side. A 238KB single-file novel accumulated 16KB text +
  // its RubyRun copies here per forced-split "paragraph" -- transient peaks and buffer growth
  // that shredded the heap's largest block. With a ~2KB cadence every buffer on this layer
  // stays small and stable for the whole build.
  static constexpr size_t SOFT_FLUSH_BYTES = 2048;
  static constexpr size_t SOFT_FLUSH_RUNS = 48;

  static size_t currentSoftFlushBytes() {
    return ESP.getMaxAllocHeap() < 24 * 1024 ? 1024 : SOFT_FLUSH_BYTES;
  }

  static size_t currentSoftFlushRuns() {
    return ESP.getMaxAllocHeap() < 24 * 1024 ? 24 : SOFT_FLUSH_RUNS;
  }
  bool midParagraph = false;

  // paragraphEnds=false streams a partial paragraph: the sink lays it out with no break
  // recorded, and the next emit continues it seamlessly (continuesPrevious=true).
  void emitRuns(const bool paragraphEnds) {
    flushCurrentText();
    if (!currentRuns.empty()) {
      if (sink) {
        // Diagnostic: bisects where a chapter's heap actually drops -- was the paragraph itself
        // already large/run-heavy going INTO onParagraph (accumulation phase, this SAX callback
        // layer), or does the drop happen INSIDE onParagraph (stream-building/layout phase,
        // VerticalParsedText)? Remove once the sparse-page root cause is found.
        size_t totalBytes = 0;
        for (const auto& r : currentRuns) totalBytes += r.baseText.size() + r.rubyText.size();
        LOG_DBG("VSC", "flushParagraph: %u runs, %u bytes, maxAlloc=%u before onParagraph",
                static_cast<unsigned>(currentRuns.size()), static_cast<unsigned>(totalBytes),
                ESP.getMaxAllocHeap());
        // By reference: onParagraph moves the individual runs' strings out and clear()s the
        // vector, handing its capacity back -- currentRuns keeps one stable buffer for the
        // whole build instead of re-growing from RUNS_RESERVE_HINT every paragraph.
        sink->onParagraph(currentRuns, midParagraph);
        LOG_DBG("VSC", "flushParagraph: maxAlloc=%u after onParagraph", ESP.getMaxAllocHeap());
      }
      currentRuns.clear();
      midParagraph = !paragraphEnds;
    } else if (paragraphEnds) {
      midParagraph = false;
    }
  }

  void flushParagraph() { emitRuns(true); }

  static bool isBlockTag(const char* name) {
    static constexpr const char* blockTags[] = {"p",  "div", "h1", "h2",   "h3",  "h4",
                                                "h5", "h6",  "li", "blockquote", "section", "article"};
    for (const auto* tag : blockTags) {
      if (strcasecmp(name, tag) == 0) return true;
    }
    return false;
  }

  static bool hasClass(const char** atts, const char* cls) {
    if (!atts) return false;
    for (int i = 0; atts[i]; i += 2) {
      if (strcasecmp(atts[i], "class") == 0 && atts[i + 1]) {
        const char* val = atts[i + 1];
        const size_t clsLen = strlen(cls);
        while (*val) {
          while (*val == ' ') val++;
          if (strncasecmp(val, cls, clsLen) == 0 && (val[clsLen] == ' ' || val[clsLen] == '\0'))
            return true;
          while (*val && *val != ' ') val++;
        }
      }
    }
    return false;
  }

  static void XMLCALL startElement(void* userData, const char* name, const char** atts) {
    auto* self = static_cast<TextExtractor*>(userData);
    self->checkHeap("startElement", name);
    self->elementDepth++;
    if (self->skipDepth >= 0) {
      self->skipDepth++;
      return;
    }
    if (isSkipTag(name)) {
      self->skipDepth = 1;
      return;
    }
    if (isBlockTag(name)) {
      if (self->blockDepth == 0) {
        self->flushParagraph();
      }
      self->blockDepth++;
    }
    if (strcasecmp(name, "ruby") == 0) {
      self->flushCurrentText();
      self->inRuby = true;
      self->rubyBase.clear();
      self->rubyBase.reserve(RUBY_RESERVE_HINT);
      self->rubyAnnotation.clear();
      self->rubyAnnotation.reserve(RUBY_RESERVE_HINT);
    } else if (strcasecmp(name, "rt") == 0) {
      self->inRt = true;
      self->rubyAnnotation.clear();
      self->rubyAnnotation.reserve(RUBY_RESERVE_HINT);
    } else if (strcasecmp(name, "rp") == 0) {
      self->inRp = true;
    }
    if (isBoldTag(name) || hasClass(atts, "bold")) {
      self->flushCurrentText();
      self->boldDepth++;
      if (self->boldStackSize < MAX_STYLE_STACK)
        self->boldOpenedAtDepth[self->boldStackSize++] = self->elementDepth;
    }
    if (isItalicTag(name) || hasClass(atts, "italic")) {
      self->flushCurrentText();
      self->italicDepth++;
      if (self->italicStackSize < MAX_STYLE_STACK)
        self->italicOpenedAtDepth[self->italicStackSize++] = self->elementDepth;
    }
    if (hasClass(atts, "em-sesame") || hasClass(atts, "em-dot") || hasClass(atts, "em-circle") ||
        hasClass(atts, "em-sesame-open") || hasClass(atts, "em-dot-open") || hasClass(atts, "em-circle-open") ||
        hasClass(atts, "em-triangle") || hasClass(atts, "em-double-circle")) {
      self->flushCurrentText();
      self->emphasisDepth++;
      if (self->emphasisStackSize < MAX_STYLE_STACK)
        self->emphasisOpenedAtDepth[self->emphasisStackSize++] = self->elementDepth;
    }
    if (strcasecmp(name, "img") == 0 || strcasecmp(name, "image") == 0) {
      const char* src = nullptr;
      if (atts) {
        for (int i = 0; atts[i]; i += 2) {
          if (strcasecmp(atts[i], "src") == 0 || strcasecmp(atts[i], "xlink:href") == 0) {
            src = atts[i + 1];
            break;
          }
        }
      }
      if (src && src[0] != '\0') {
        // Complete the paragraph built so far, then emit the image in document order. (For the
        // rare mid-paragraph image this places the partial text before the image, where the old
        // accumulate-then-interleave code placed the image before the whole paragraph; identical
        // for the usual block-level images.)
        self->flushParagraph();
        if (self->sink) self->sink->onImage(std::string(src));
      }
    }
    if (strcasecmp(name, "br") == 0 || strcasecmp(name, "br/") == 0) {
      if (!self->inRuby) {
        self->currentText.push_back('\n');
      }
    }
  }

  static void XMLCALL endElement(void* userData, const char* name) {
    auto* self = static_cast<TextExtractor*>(userData);
    self->checkHeap("endElement", name);
    self->elementDepth--;
    if (self->skipDepth > 0) {
      self->skipDepth--;
      if (self->skipDepth == 0) self->skipDepth = -1;
      return;
    }
    if (strcasecmp(name, "rp") == 0) {
      self->inRp = false;
      return;
    }
    if (strcasecmp(name, "rt") == 0) {
      self->inRt = false;
      // Emit a RubyRun for the base text accumulated so far with this annotation.
      if (!self->rubyBase.empty()) {
        self->currentRuns.push_back(
            RubyRun{std::move(self->rubyBase), std::move(self->rubyAnnotation), self->currentStyle(), self->hasEmphasis()});
        self->rubyBase.clear();
        self->rubyBase.reserve(RUBY_RESERVE_HINT);
      }
      self->rubyAnnotation.clear();
      self->rubyAnnotation.reserve(RUBY_RESERVE_HINT);
      return;
    }
    if (strcasecmp(name, "ruby") == 0) {
      // Flush any remaining base text that had no <rt> (malformed markup).
      if (!self->rubyBase.empty()) {
        self->currentRuns.push_back(RubyRun{std::move(self->rubyBase), {}, self->currentStyle(), self->hasEmphasis()});
        self->rubyBase.clear();
        self->rubyBase.reserve(RUBY_RESERVE_HINT);
      }
      self->inRuby = false;
      // Furigana-dense text accumulates many small runs without currentText ever growing;
      // stream them onward at the same cadence as the byte bound (see SOFT_FLUSH_RUNS).
      if (self->currentRuns.size() >= self->currentSoftFlushRuns()) self->emitRuns(false);
      return;
    }
    if (self->boldStackSize > 0 && self->boldOpenedAtDepth[self->boldStackSize - 1] == self->elementDepth) {
      self->flushCurrentText();
      self->boldDepth--;
      self->boldStackSize--;
    }
    if (self->italicStackSize > 0 && self->italicOpenedAtDepth[self->italicStackSize - 1] == self->elementDepth) {
      self->flushCurrentText();
      self->italicDepth--;
      self->italicStackSize--;
    }
    if (self->emphasisStackSize > 0 && self->emphasisOpenedAtDepth[self->emphasisStackSize - 1] == self->elementDepth) {
      self->flushCurrentText();
      self->emphasisDepth--;
      self->emphasisStackSize--;
    }
    if (isBlockTag(name)) {
      self->blockDepth--;
      if (self->blockDepth <= 0) {
        self->blockDepth = 0;
        self->flushParagraph();
      }
    }
  }

  static void XMLCALL characterData(void* userData, const char* s, int len) {
    auto* self = static_cast<TextExtractor*>(userData);
    self->checkHeap("characterData");
    if (self->skipDepth >= 0) return;
    if (self->inRp) return;
    if (self->inRt) {
      self->rubyAnnotation.append(s, static_cast<size_t>(len));
    } else if (self->inRuby) {
      self->rubyBase.append(s, static_cast<size_t>(len));
    } else {
      // Forced split for markup-less mega-paragraphs; see MAX_PARAGRAPH_BYTES. (Not applied
      // inside <ruby> -- ruby runs are a handful of characters by nature.)
      if (self->currentText.size() + static_cast<size_t>(len) > MAX_PARAGRAPH_BYTES) {
        LOG_DBG("VSC", "MAX_PARAGRAPH_BYTES forced split at %u bytes", static_cast<unsigned>(self->currentText.size()));
        self->flushParagraph();
      }
      // Drop inter-tag whitespace (the "\n" text nodes between <p>/<div> in pretty-printed
      // xhtml): laid out as real paragraphs they produced phantom blank pages -- e.g. an
      // empty page BEFORE a cover chapter's image page. Only leading whitespace is dropped;
      // intentional blank lines arrive as explicit '\n' from <br/> in startElement, and
      // whitespace inside running text lands with currentText already non-empty.
      if (self->currentText.empty()) {
        int firstInk = 0;
        while (firstInk < len && (s[firstInk] == '\n' || s[firstInk] == '\r' || s[firstInk] == '\t' || s[firstInk] == ' ')) {
          firstInk++;
        }
        if (firstInk == len) return;
        s += firstInk;
        len -= firstInk;
      }
      self->currentText.append(s, static_cast<size_t>(len));
      // Streaming cadence: hand the buffered text onward as a seamless continuation well
      // before it grows large (see SOFT_FLUSH_BYTES).
      if (self->currentText.size() >= self->currentSoftFlushBytes()) self->emitRuns(false);
    }
  }

  static void XMLCALL defaultHandler(void* userData, const char* s, int len) {
    if (len >= 4 && s[0] == '&') {
      auto* self = static_cast<TextExtractor*>(userData);
      std::string entity(s, static_cast<size_t>(len));
      std::string resolved;
      if (entity == "&nbsp;") {
        resolved = " ";
      } else if (entity == "&mdash;") {
        resolved = "\xe2\x80\x94";
      } else if (entity == "&ndash;") {
        resolved = "\xe2\x80\x93";
      } else if (entity == "&hellip;") {
        resolved = "\xe2\x80\xa6";
      } else if (entity == "&amp;") {
        resolved = "&";
      } else if (entity == "&lt;") {
        resolved = "<";
      } else if (entity == "&gt;") {
        resolved = ">";
      } else if (entity == "&quot;") {
        resolved = "\"";
      } else if (entity == "&apos;") {
        resolved = "'";
      } else {
        return;
      }

      if (self->inRp) return;
      if (self->inRt) {
        self->rubyAnnotation.append(resolved);
      } else if (self->inRuby) {
        self->rubyBase.append(resolved);
      } else {
        self->currentText.append(resolved);
      }
    }
  }
};

}  // namespace

namespace {

// ---- Page (de)serialization (cache format v37) -----------------------------------------------
// File layout:
//   header: u8 version, i32 fontId, u16 viewportWidth, u16 viewportHeight,
//           u16 pageCount, u32 indexOffset          (pageCount/indexOffset patched post-stream)
//   page records (variable length, written as pages are laid out)
//   footer at indexOffset: pageCount x u32 file offset of each page record
// The footer lets loadSectionFile() open a chapter by reading only the header + 4 bytes/page,
// and getPage() seek straight to one page -- pages are never all resident in RAM.

bool writePage(HalFile& file, const VerticalPage& page) {
  const bool isImg = page.isImagePage();
  serialization::writePod(file, isImg);
  if (isImg) {
    serialization::writeString(file, page.imagePath);
    serialization::writePod(file, page.imageWidth);
    serialization::writePod(file, page.imageHeight);
    serialization::writePod(file, page.imageRotated);
    return true;
  }
  const auto glyphCount = static_cast<uint32_t>(page.glyphs.size());
  serialization::writePod(file, glyphCount);
  serialization::writePod(file, page.columnCount);
  serialization::writePod(file, page.rowsPerColumn);

  for (const auto& g : page.glyphs) {
    serialization::writePod(file, g.codepoint);
    serialization::writePod(file, g.column);
    serialization::writePod(file, g.row);
    serialization::writePod(file, g.x);
    serialization::writePod(file, g.y);
    serialization::writePod(file, g.paragraphIndex);
    serialization::writePod(file, g.byteOffset);
    serialization::writePod(file, g.renderKind);
    serialization::writePod(file, g.style);
    serialization::writePod(file, g.emphasis);

    if (g.renderKind == VerticalGlyph::RotatedRun || g.renderKind == VerticalGlyph::UprightRun) {
      const auto runLen = static_cast<uint16_t>(g.rotatedRunText.size());
      serialization::writePod(file, runLen);
      if (runLen > 0) {
        file.write(reinterpret_cast<const uint8_t*>(g.rotatedRunText.data()), runLen);
      }
    }

    const auto rubyLen = static_cast<uint16_t>(g.rubyText.size());
    serialization::writePod(file, rubyLen);
    if (rubyLen > 0) {
      file.write(reinterpret_cast<const uint8_t*>(g.rubyText.data()), rubyLen);
    }
  }
  return true;
}

bool readPage(HalFile& file, VerticalPage& page) {
  page.glyphs.clear();
  page.imagePath.clear();

  bool isImg = false;
  serialization::readPod(file, isImg);
  if (isImg) {
    serialization::readString(file, page.imagePath);
    serialization::readPod(file, page.imageWidth);
    serialization::readPod(file, page.imageHeight);
    serialization::readPod(file, page.imageRotated);
    return !page.imagePath.empty();
  }

  uint32_t glyphCount;
  serialization::readPod(file, glyphCount);
  serialization::readPod(file, page.columnCount);
  serialization::readPod(file, page.rowsPerColumn);

  // One page is bounded by screen geometry (a few hundred cells); a corrupt count must not
  // drive a huge reserve on a heap that can't take it.
  constexpr uint32_t MAX_GLYPHS_PER_PAGE = 4096;
  if (glyphCount > MAX_GLYPHS_PER_PAGE) {
    LOG_ERR("VSC", "Corrupt page record: %u glyphs", glyphCount);
    return false;
  }
  page.glyphs.reserve(glyphCount);

  for (uint32_t gi = 0; gi < glyphCount; gi++) {
    VerticalGlyph g;
    serialization::readPod(file, g.codepoint);
    serialization::readPod(file, g.column);
    serialization::readPod(file, g.row);
    serialization::readPod(file, g.x);
    serialization::readPod(file, g.y);
    serialization::readPod(file, g.paragraphIndex);
    serialization::readPod(file, g.byteOffset);
    serialization::readPod(file, g.renderKind);
    serialization::readPod(file, g.style);
    serialization::readPod(file, g.emphasis);

    if (g.renderKind == VerticalGlyph::RotatedRun || g.renderKind == VerticalGlyph::UprightRun) {
      uint16_t runLen;
      serialization::readPod(file, runLen);
      if (runLen > 0) {
        g.rotatedRunText.resize(runLen);
        file.read(reinterpret_cast<uint8_t*>(g.rotatedRunText.data()), runLen);
      }
    }

    uint16_t rubyLen;
    serialization::readPod(file, rubyLen);
    if (rubyLen > 0) {
      g.rubyText.resize(rubyLen);
      file.read(reinterpret_cast<uint8_t*>(g.rubyText.data()), rubyLen);
    }
    page.glyphs.push_back(std::move(g));
  }
  return true;
}

// Streams the temp HTML once looking for "<rt", to decide ruby layout geometry (column gap /
// right padding) before the first paragraph is laid out. The old design scanned the fully
// accumulated paragraph list for ruby runs; a streaming pipeline has to know up front. A false
// positive (e.g. "<rt" inside a comment) merely pads columns slightly -- harmless.
bool fileContainsRubyTag(const std::string& path) {
  HalFile f;
  if (!Storage.openFileForRead("VSC", path, f)) return false;
  uint8_t buf[512];
  int state = 0;  // matched prefix length of "<rt"
  size_t n;
  while ((n = f.read(buf, sizeof(buf))) > 0) {
    for (size_t i = 0; i < n; i++) {
      const char c = static_cast<char>(buf[i]);
      if (state == 0) {
        state = (c == '<') ? 1 : 0;
      } else if (state == 1) {
        state = (c == 'r') ? 2 : (c == '<' ? 1 : 0);
      } else {
        if (c == 't') {
          f.close();
          return true;
        }
        state = (c == '<') ? 1 : 0;
      }
    }
  }
  f.close();
  return false;
}

// Concrete sink: feeds each extracted paragraph straight into the column layout, and whenever a
// batch of characters is buffered (or an image forces a boundary), lays out the batch and writes
// each resulting page to the cache file immediately. Nothing here is O(chapter): the layout
// stream, the produced pages, and the paragraph being extracted are all O(batch).
struct LayoutPageSink final : ParagraphSink {
  VerticalParsedText& layout;
  HalFile& out;
  std::vector<uint32_t>& pageOffsets;
  Epub& epub;
  GfxRenderer& renderer;
  const std::string& chapterDir;
  const std::string& imageBasePath;
  const uint16_t viewportWidth;
  const uint16_t viewportHeight;
  const int fontId;
  size_t imgIdx = 0;
  bool failed = false;
  size_t pagesToSkip = 0;
  int pageBudget = 0;
  bool hitBudget = false;
  bool frontierStop = false;  // set by writeOne when a page is too sparse (forced break + low heap)
  XML_Parser* parserRef = nullptr;  // points to BuildState::parser

  // ~1-2 screens of text per layout batch. A batch boundary lands between paragraphs, which
  // already force a fresh column, so the only observable effect is an occasional page that ends
  // at a paragraph boundary instead of mid-paragraph -- same behavior as an image boundary.
  //
  // Was 640. Confirmed on a real device that at 640, the stream_ buffer needed to hold one batch
  // (33-43KB for furigana-dense text, each RubyRun carrying its own PendingChar-per-character
  // cost) and layoutPages()'s own per-page glyph buffers (13824+ bytes each) are BOTH alive at
  // flush time -- stream_ isn't freed until after layoutPages() returns and reset() runs. Peak
  // memory is the SUM of both, and for dense chapters that sum exceeded available heap, dropping
  // glyphs on the hardest paragraphs even after the chunking fix bounded stream_'s own reserve.
  // Halving the batch roughly halves stream_'s peak size, leaving proportionally more headroom
  // for layoutPages() at the cost of more (individually smaller, safer) flush cycles. Was 640,
  // then 320 -- still measurably dropped glyphs on the single most furigana-dense paragraph in a
  // real test chapter (71 runs), on top of the chunk-sizing estimate bug fixed alongside this
  // (see onParagraph()). Halved again; the trend across both prior reductions has been strictly
  // more real content preserved with each halving.  Reduced to 96 for the same reason: long
  // paragraphs on X3 collapse maxAlloc from ~40 KB to ~3 KB inside a single onParagraph() call.
  static constexpr size_t BATCH_CHARS = 96;

  static size_t currentChunkCharBudget() {
    const uint32_t m = ESP.getMaxAllocHeap();
    if (m < 12 * 1024) return 8;
    if (m < 16 * 1024) return 24;
    if (m < 24 * 1024) return 48;
    if (m < 48 * 1024) return 64;
    return BATCH_CHARS;
  }
  static size_t currentChunkByteBudget() {
    const uint32_t m = ESP.getMaxAllocHeap();
    if (m < 12 * 1024) return 128;
    if (m < 16 * 1024) return 384;
    if (m < 24 * 1024) return 512;
    if (m < 48 * 1024) return 768;
    return 1024;
  }
  static size_t currentRunSliceChars() {
    const uint32_t m = ESP.getMaxAllocHeap();
    if (m < 12 * 1024) return 8;
    if (m < 16 * 1024) return 24;
    if (m < 24 * 1024) return 48;
    if (m < 48 * 1024) return 64;
    return 96;
  }
  static size_t currentRubyByteLimit() {
    const uint32_t m = ESP.getMaxAllocHeap();
    if (m < 12 * 1024) return 48;
    if (m < 16 * 1024) return 128;
    if (m < 24 * 1024) return 192;
    if (m < 48 * 1024) return 256;
    return 384;
  }

  LayoutPageSink(VerticalParsedText& layout, HalFile& out, std::vector<uint32_t>& pageOffsets, Epub& epub,
                 GfxRenderer& renderer, const std::string& chapterDir, const std::string& imageBasePath,
                 uint16_t viewportWidth, uint16_t viewportHeight, int fontId)
      : layout(layout),
        out(out),
        pageOffsets(pageOffsets),
        epub(epub),
        renderer(renderer),
        chapterDir(chapterDir),
        imageBasePath(imageBasePath),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        fontId(fontId) {}

  void onParagraph(std::vector<RubyRun>& runs, const bool continuesPrevious) override {
    if (failed) return;

    std::vector<RubyRun> chunk;
    size_t chunkEstimatedChars = 0;
    size_t chunkEstimatedBytes = 0;
    bool firstChunkOfParagraph = !continuesPrevious;

    auto utf8Chars = [](const std::string& s) {
      size_t n = 0;
      for (const char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) n++;
      }
      return n;
    };

    auto flushChunk = [&]() {
      if (failed || chunk.empty()) return;
      layout.addAnnotatedParagraph(chunk, !firstChunkOfParagraph);
      firstChunkOfParagraph = false;
      chunk.clear();
      chunkEstimatedChars = 0;
      chunkEstimatedBytes = 0;
      // If heap is already low, push pages out immediately instead of accumulating more.
      if (ESP.getMaxAllocHeap() < 20 * 1024 || layout.pendingCount() >= currentChunkCharBudget()) {
        flushText();
      }
    };

    auto pushRun = [&](RubyRun&& run) {
      if (failed) return;
      const uint32_t freeNow = ESP.getMaxAllocHeap();
      const size_t chunkCharBudget = currentChunkCharBudget();
      const size_t chunkByteBudget = currentChunkByteBudget();
      const size_t rubyByteLimit = currentRubyByteLimit();

      const size_t runEstimatedChars = utf8Chars(run.baseText);
      size_t runEstimatedBytes = run.baseText.size() + run.rubyText.size();

      if (!run.rubyText.empty() && runEstimatedBytes > rubyByteLimit) {
        LOG_ERR("VSC",
                "Oversized ruby run; dropping ruby annotation (base=%u ruby=%u free=%u)",
                static_cast<unsigned>(run.baseText.size()),
                static_cast<unsigned>(run.rubyText.size()),
                freeNow);
        run.rubyText.clear();
        runEstimatedBytes = run.baseText.size();
      }

      // Flush BEFORE adding if over budget.
      if (!chunk.empty() &&
          (layout.pendingCount() + chunkEstimatedChars + runEstimatedChars >= chunkCharBudget ||
           chunkEstimatedBytes + runEstimatedBytes >= chunkByteBudget)) {
        flushChunk();
      }

      chunk.push_back(std::move(run));
      chunkEstimatedChars += runEstimatedChars;
      chunkEstimatedBytes += runEstimatedBytes;

      if (layout.pendingCount() + chunkEstimatedChars >= chunkCharBudget ||
          chunkEstimatedBytes >= chunkByteBudget ||
          ESP.getMaxAllocHeap() < 20 * 1024) {
        flushChunk();
      }
    };

    for (auto& run : runs) {
      if (failed) return;
      const size_t sliceChars = currentRunSliceChars();

      if (run.rubyText.empty() && utf8Chars(run.baseText) > sliceChars) {
        const std::string base = std::move(run.baseText);
        size_t pos = 0;
        while (pos < base.size() && !failed) {
          size_t end = pos;
          size_t chars = 0;
          while (end < base.size()) {
            if ((static_cast<unsigned char>(base[end]) & 0xC0) != 0x80) {
              if (chars == sliceChars) break;
              chars++;
            }
            end++;
          }
          RubyRun slice;
          slice.baseText = base.substr(pos, end - pos);
          slice.style = run.style;
          slice.emphasis = run.emphasis;
          pushRun(std::move(slice));
          pos = end;
        }
        continue;
      }

      if (!run.rubyText.empty() &&
          (run.baseText.size() + run.rubyText.size()) > currentChunkByteBudget() &&
          utf8Chars(run.baseText) > currentRunSliceChars()) {
        const std::string base = std::move(run.baseText);
        std::string ruby = std::move(run.rubyText);
        size_t pos = 0;
        bool first = true;
        while (pos < base.size() && !failed) {
          size_t end = pos;
          size_t chars = 0;
          const size_t sliceCharsRuby = currentRunSliceChars();
          while (end < base.size()) {
            if ((static_cast<unsigned char>(base[end]) & 0xC0) != 0x80) {
              if (chars == sliceCharsRuby) break;
              chars++;
            }
            end++;
          }
          RubyRun slice;
          slice.baseText = base.substr(pos, end - pos);
          slice.style = run.style;
          slice.emphasis = run.emphasis;
          if (first) {
            slice.rubyText = std::move(ruby);
            first = false;
          }
          pushRun(std::move(slice));
          pos = end;
        }
        continue;
      }

      pushRun(std::move(run));
    }
    if (failed) return;
    if (!chunk.empty()) {
      flushChunk();
    }
    runs.clear();
  }

  void onImage(const std::string& src) override {
    if (failed) return;
    // Lay out any buffered text, then FINALIZE the in-progress page before the image page is
    // written. Without this, the image page was written while the half-filled text page stayed
    // pending: the pending page (whose content PRECEDES the image) landed in the cache AFTER
    // the image page, and post-image text silently merged onto it -- confirmed on a real device
    // as dialogue continuing mid-column across a scene-break graphic instead of starting fresh.
    flushText();
    VerticalPage pendingTail;
    if (layout.finalizePendingPage(pendingTail)) writeOne(pendingTail);
    VerticalPage imgPage = makeImagePage(src);
    // Skip empty gaiji pages (low-heap image fallback) instead of writing
    // a broken image page that corrupts the chapter cache.
    if (imgPage.imagePath.empty()) return;
    writeOne(imgPage);
  }

  static void writePageCallback(void* ctx, VerticalPage&& page) { static_cast<LayoutPageSink*>(ctx)->writeOne(page); }

  // isFinalFlush must be true ONLY for the chapter's true last flush (see the caller in
  // streamParseAndLayout(), after extractor.flushParagraph()) -- every mid-chapter call (the
  // BATCH_CHARS trigger below, onImage()'s pre-image flush) must pass false so a batch boundary
  // that lands mid-page continues that page on the next call instead of finalizing it early. See
  // VerticalParsedText::layoutPages()'s isFinalFlush doc comment for the full rationale.
  void flushText(bool isFinalFlush = false) {
    if (failed) return;
    if (!isFinalFlush && layout.pendingCount() == 0) return;
    // TEMP diagnostics: bisect which stage of the flush plants persistent allocations
    // (maxAlloc collapsed 82K -> 2K over one chapter on a real device; strip with the rest).
    const uint32_t maBefore = ESP.getMaxAllocHeap();
    // Streaming pages out via callback as they're finalized keeps at most ~2 pages' worth of
    // glyph buffers resident at once instead of the whole batch's -- see PageReadyCallback in
    // VerticalParsedText.h for why this is safe (oikomi only ever looks one page back).
    auto pages = layout.layoutPages(this, &writePageCallback, isFinalFlush);
    const uint32_t maLayout = ESP.getMaxAllocHeap();
    layout.reset();
    for (const auto& p : pages) writeOne(p);
    const uint32_t maAfter = ESP.getMaxAllocHeap();
    if (maAfter + 2048 < maBefore) {
      LOG_DBG("VSC", "flushText maxAlloc: %u -> %u (layout) -> %u (reset+write), free=%u", maBefore, maLayout, maAfter,
              ESP.getFreeHeap());
    }
  }

  // Helper: append a single codepoint as UTF-8 to a string.
  static void appendUtf8(uint32_t cp, std::string& out) {
    if (cp < 0x80) { out.push_back(static_cast<char>(cp)); }
    else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  // Preload glyph BITMAP data (not just advance) for every character on this
  // page, so render-time on-demand (overflow) reads are eliminated.  Call
  // BEFORE writePage() serialises the page, since that serialization may be
  // immediately followed by rendering.
  // Build-time advance-table warming (glyph bitmap NOT loaded — that would
  // spike heap by ~16 KB per page during the build).  The renderer will warm
  // bitmaps on demand when the page is first displayed; advance-only warming
  // is enough to keep column measurement fast and avoid layout OOM.
  void warmAdvanceTableForPage(const VerticalPage& p, int fontId_) {
    if (p.isImagePage() || p.glyphs.empty()) return;
    if (!renderer.isSdCardFont(fontId_)) return;
    // Advance-table warming is much cheaper than glyph-bitmap prewarm so we
    // always run it.  The old ensureSdCardFontGlyphsReady was the heap killer;
    // ensureSdCardFontReady only builds the advance-metadata table (~2 KB).
    std::string utf8;
    utf8.reserve(p.glyphs.size() * 3);
    uint8_t styleMask = 0;
    for (const auto& g : p.glyphs) {
      styleMask |= (1u << (g.style & 3));
      appendUtf8(g.codepoint, utf8);
      for (size_t i = 0; i < g.rubyText.size();) {
        const auto c0 = static_cast<unsigned char>(g.rubyText[i]);
        size_t clen = 1;
        if (c0 >= 0xF0) clen = 4; else if (c0 >= 0xE0) clen = 3; else if (c0 >= 0xC0) clen = 2;
        if (i + clen > g.rubyText.size()) break;
        utf8.append(g.rubyText.data() + i, clen);
        i += clen;
      }
    }
    renderer.ensureSdCardFontReady(fontId_, utf8.c_str(), styleMask ? styleMask : 0x01);
  }

  void writeOne(const VerticalPage& p) {
    if (pagesToSkip > 0) { pagesToSkip--; return; }
    if (frontierStop) {
      // Already committed to stopping; just drain the callback stream.
      return;
    }

    pageOffsets.push_back(static_cast<uint32_t>(out.position()));
    warmAdvanceTableForPage(p, fontId);
    if (!writePage(out, p)) {
      LOG_ERR("VSC", "Failed to write page %zu to cache", pageOffsets.size() - 1);
      failed = true;
      return;
    }

    // If glyph dropping was detected during this page's layout, discard the
    // page and stop — the section becomes partial at the previous frontier.
    if (layout.everDroppedForHeap()) {
      frontierStop = true;
      pageOffsets.pop_back();
      LOG_ERR("VSC", "Glyph drop detected — discarding page %zu", pageOffsets.size());
      if (parserRef && *parserRef) XML_StopParser(*parserRef, XML_TRUE);
      return;
    }

    // Low-heap forced break: the page split at an unnatural position.  Treat
    // it as a frontier so the reader gets only well-formed pages.  12 KB is
    // the threshold where forced breaks start producing visible artefacts
    // (confirmed on a real device with 210 KB CJK chapters).
    const uint32_t ma = ESP.getMaxAllocHeap();
    if (p.forcedBreaks > 0 && ma < 12 * 1024) {
      frontierStop = true;
      hitBudget = true;
      pageOffsets.pop_back();
      LOG_ERR("VSC", "Frontier at page %zu (forcedBreaks=%u maxAlloc=%u)",
              pageOffsets.size(), p.forcedBreaks, ma);
      if (parserRef && *parserRef) XML_StopParser(*parserRef, XML_TRUE);
      return;
    }

    if (pageBudget > 0 && --pageBudget == 0) {
      LOG_DBG("VSC", "Page budget reached, stopping chunk");
      hitBudget = true;
      failed = true;
      if (parserRef && *parserRef) {
        XML_StopParser(*parserRef, XML_TRUE);
      }
    }
  }

  VerticalPage makeImagePage(const std::string& src) {
    std::string resolvedSrc = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(chapterDir + src));

    // Determine extension and cached path
    std::string ext;
    const size_t extPos = resolvedSrc.rfind('.');
    if (extPos != std::string::npos) ext = resolvedSrc.substr(extPos);
    const std::string cachedPath = imageBasePath + std::to_string(imgIdx++) + ext;

    // Extract image from EPUB to cache if not already present. A previous attempt that failed
    // partway (e.g. the zip inflate's 32KB ring buffer allocation failing under the same heap
    // pressure this session spent all night fixing elsewhere) can leave behind a file that
    // EXISTS but is empty/truncated -- Storage.exists() alone can't tell a genuinely-cached image
    // from that broken leftover, so a broken file was never retried, permanently blanking that
    // page. Checking size > 0 makes this self-healing for images broken by an earlier session.
    bool needsExtraction = true;
    if (Storage.exists(cachedPath.c_str())) {
      HalFile existingFile;
      if (Storage.openFileForRead("VSC", cachedPath, existingFile) && existingFile.size() > 0) {
        needsExtraction = false;
      }
    }
    const bool isGaiji = isLikelyGaijiImagePath(resolvedSrc);

    if (needsExtraction) {
      HalFile cachedFile;
      if (Storage.openFileForWrite("VSC", cachedPath, cachedFile)) {
        const bool extracted = epub.readItemContentsToStream(resolvedSrc, cachedFile, 4096);
        cachedFile.flush();
        cachedFile.close();
        if (!extracted) {
          LOG_ERR("VSC", "Failed to extract image %s; removing partial cache file", resolvedSrc.c_str());
          Storage.remove(cachedPath.c_str());
          if (isGaiji) {
            LOG_ERR("VSC", "Skipping gaiji image after extract failure: %s", resolvedSrc.c_str());
            return VerticalPage{};
          }
        }
      }
    }

    {
      // Verify cache file exists and is non-empty before proceeding.
      HalFile verifyFile;
      if (!Storage.openFileForRead("VSC", cachedPath, verifyFile) || verifyFile.size() == 0) {
        if (isGaiji) {
          LOG_ERR("VSC", "Skipping gaiji image with missing/empty cache: %s", resolvedSrc.c_str());
          return VerticalPage{};
        }
      }
    }

    // Get actual image dimensions. Store natural (unrotated) dimensions --
    // ImageBlock::render handles rotation, scaling, and centering itself.
    int displayW = viewportWidth;
    int displayH = viewportHeight;
    bool rotated = false;
    ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(cachedPath);
    if (!decoder) {
      if (isGaiji) {
        LOG_ERR("VSC", "Skipping gaiji image with no decoder: %s", resolvedSrc.c_str());
        return VerticalPage{};
      }
    } else {
      ImageDimensions dims = {0, 0};
      if (decoder->getDimensions(cachedPath, dims) && dims.width > 0 && dims.height > 0) {
        const bool viewportIsPortrait = (viewportHeight > viewportWidth);
        const bool imageIsLandscape = (dims.width > dims.height);
        rotated = (viewportIsPortrait == imageIsLandscape);
        displayW = dims.width;
        displayH = dims.height;
      } else if (isGaiji) {
        LOG_ERR("VSC", "Skipping gaiji image after dimension probe failure: %s", resolvedSrc.c_str());
        return VerticalPage{};
      }
    }

    VerticalPage page;
    page.imagePath = cachedPath;
    page.imageWidth = static_cast<int16_t>(displayW);
    page.imageHeight = static_cast<int16_t>(displayH);
    page.imageRotated = rotated;
    return page;
  }
};

// Byte offset of the pageCount field in the header: u8 version + i32 fontId + 2x u16 viewport.
constexpr size_t HEADER_PAGECOUNT_OFFSET = 1 + sizeof(int) + 2 * sizeof(uint16_t);

}  // namespace

struct VerticalSection::BuildState {
  HalFile out;           // cache file (.bin)
  HalFile htmlFile;      // pre-extracted temp HTML (open across calls)

  // Persistent parse pipeline (all survive across buildSomeMore calls).
  XML_Parser parser = nullptr;
  std::unique_ptr<VerticalParsedText> layout;
  std::unique_ptr<LayoutPageSink> sink;
  TextExtractor extractor;

  std::string htmlPath;
  int fontId = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  std::string chapterDir;
  std::string imageBasePath;
  bool lowMemMode = false;
  bool hasRuby = false;
  bool eof = false;

  uint16_t pagesAlreadyBuilt = 0;
  uint8_t frontierRetryCount = 0;  // how many times we hit glyph drop at this build frontier

  ~BuildState() { destroyXmlParser(parser); }
};

// Forward declarations
static bool extractChapterHtml(Epub& epub, int spineIndex, const std::string& tmpHtmlPath);
static bool finalizeVerticalCache(HalFile& file, const std::vector<uint32_t>& pageOffsets, uint16_t& pageCount,
                                   uint16_t flags);

VerticalSection::VerticalSection(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer)
    : epub(epub), spineIndex(spineIndex), renderer(renderer),
      filePath(epub->getCachePath() + "/vsections/" + std::to_string(spineIndex) + ".bin") {}
VerticalSection::~VerticalSection() { suspendBuild(); }

static std::string binTmpPath(const std::string& fp) { return fp + ".part"; }

// Write the page-offset index and patch the header pageCount field.
static bool patchVerticalCacheHeader(HalFile& file, uint16_t pageCount, uint32_t indexOffset, uint16_t flags) {
  if (!file.seek(VSECTION_HEADER_PCOUNT_OFF)) return false;
  serialization::writePod(file, pageCount);
  serialization::writePod(file, indexOffset);
  serialization::writePod(file, flags);
  return true;
}

static bool finalizeVerticalCache(HalFile& file, const std::vector<uint32_t>& pageOffsets, uint16_t& pageCount,
                                   uint16_t flags) {
  if (pageOffsets.empty()) {
    LOG_ERR("VSC", "Refusing to finalize zero-page vertical cache");
    pageCount = 0;
    return false;
  }
  const auto indexOffset = static_cast<uint32_t>(file.position());
  for (const uint32_t off : pageOffsets) {
    serialization::writePod(file, off);
  }
  pageCount = static_cast<uint16_t>(pageOffsets.size());
  return patchVerticalCacheHeader(file, pageCount, indexOffset, flags);
}

// Extract the chapter's HTML from the EPUB zip to a temp file.
// Returns true if the file was written (or already exists).
static bool extractChapterHtml(Epub& epub, int spineIndex, const std::string& tmpHtmlPath) {
  // Reuse an already-extracted file (from a partial previuos build).
  if (Storage.exists(tmpHtmlPath.c_str())) return true;
  bool success = false;
  const auto localPath = epub.getSpineItem(spineIndex).href;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    if (attempt > 0) delay(50);
    if (Storage.exists(tmpHtmlPath.c_str())) Storage.remove(tmpHtmlPath.c_str());
    HalFile tmpHtml;
    if (!Storage.openFileForWrite("VSC", tmpHtmlPath, tmpHtml)) continue;
    success = epub.readItemContentsToStream(localPath, tmpHtml, 2048);
    tmpHtml.close();
    if (!success && Storage.exists(tmpHtmlPath.c_str())) Storage.remove(tmpHtmlPath.c_str());
  }
  return success;
}

bool VerticalSection::startBuild(const int fontId, const uint16_t viewportWidth,
                                  const uint16_t viewportHeight) {
  const auto vsectionsDir = epub->getCachePath() + "/vsections";
  Storage.mkdir(vsectionsDir.c_str());

  auto bs = std::make_unique<BuildState>();
  bs->fontId = fontId;
  bs->viewportWidth = viewportWidth;
  bs->viewportHeight = viewportHeight;
  bs->htmlPath = epub->getCachePath() + "/.tmp_v" + std::to_string(spineIndex) + ".html";

  // Extract HTML to temp file ONCE (reused across buildSomeMore calls).
  if (!extractChapterHtml(*epub, spineIndex, bs->htmlPath)) {
    return false;
  }

  // Write to a temp .part file so the existing .bin stays readable during
  // the build (getPage() can still serve previously cached pages).  On
  // finalize or suspend the .part is promoted to .bin via rename.
  const std::string tmpPath = binTmpPath(filePath);
  if (!Storage.openFileForWrite("VSC", tmpPath, bs->out)) {
    return false;
  }
  serialization::writePod(bs->out, VSECTION_FILE_VERSION);
  serialization::writePod(bs->out, fontId);
  serialization::writePod(bs->out, viewportWidth);
  serialization::writePod(bs->out, viewportHeight);
  const uint16_t pageCountPH = 0;
  const uint32_t indexOffsetPH = 0;
  const uint16_t flagsPH = 0;
  serialization::writePod(bs->out, pageCountPH);
  serialization::writePod(bs->out, indexOffsetPH);
  serialization::writePod(bs->out, flagsPH);

  // Open the temp HTML for incremental reading.
  if (!Storage.openFileForRead("VSC", bs->htmlPath, bs->htmlFile)) {
    return false;
  }

  // Heap guard: match the horizontal Section's approach of letting
  // individual allocations (parser, layout) fail naturally if they OOM,
  // rather than guessing a single threshold.  A token guard against the
  // degenerate case where there's truly nothing to work with.
  const uint32_t maxAlloc = ESP.getMaxAllocHeap();
  constexpr uint32_t kTokenGuard = 8 * 1024;
  constexpr uint32_t kLowMemBytes  = 40 * 1024;
  if (maxAlloc < kTokenGuard) {
    LOG_ERR("VSC", "Insufficient heap (maxAlloc=%u < %u), deferring", maxAlloc, kTokenGuard);
    return false;
  }
  bs->lowMemMode = (maxAlloc < kLowMemBytes);

  LOG_DBG("VSC", "after readItemContentsToStream: maxAlloc=%u", maxAlloc);

  // One-time chapter metadata.
  bs->hasRuby = fileContainsRubyTag(bs->htmlPath);
  LOG_DBG("VSC", "after fileContainsRubyTag: maxAlloc=%u", ESP.getMaxAllocHeap());

  const auto& spineItem = epub->getSpineItem(spineIndex);
  {
    const size_t slash = spineItem.href.rfind('/');
    if (slash != std::string::npos) bs->chapterDir = spineItem.href.substr(0, slash + 1);
  }
  bs->imageBasePath = epub->getCachePath() + "/img_v" + std::to_string(spineIndex) + "_";

  // Persistent layout engine.
  bs->layout = std::make_unique<VerticalParsedText>(renderer, fontId, viewportWidth, viewportHeight);
  bs->layout->preallocateStream();
  const int lineH = renderer.getLineHeight(fontId);
  bs->layout->setColumnGapPx((lineH / 3) < 4 ? 4 : (lineH / 3));
  if (bs->hasRuby) {
    bs->layout->setColumnGapPx(lineH * 2 / 3);
    bs->layout->setRightPaddingPx((lineH / 2) < 2 ? 2 : (lineH / 2));
  }

  // Persistent page sink.
  bs->sink = std::make_unique<LayoutPageSink>(*bs->layout, bs->out, pageOffsets_, *epub, renderer,
                                               bs->chapterDir, bs->imageBasePath,
                                               viewportWidth, viewportHeight, fontId);
  bs->sink->parserRef = &bs->parser;

  // Persistent text extractor, wired to the sink.
  bs->extractor.sink = bs->sink.get();
  bs->extractor.currentText.reserve(TextExtractor::SOFT_FLUSH_BYTES + 512);
  bs->extractor.currentRuns.reserve(TextExtractor::SOFT_FLUSH_RUNS + 8);
  bs->extractor.rubyBase.reserve(TextExtractor::RUBY_RESERVE_HINT);
  bs->extractor.rubyAnnotation.reserve(TextExtractor::RUBY_RESERVE_HINT);

  // Expat parser.
  bs->parser = XML_ParserCreate(nullptr);
  if (!bs->parser) {
    LOG_ERR("VSC", "OOM: XML parser");
    return false;
  }
  LOG_DBG("VSC", "after XML_ParserCreate: maxAlloc=%u", ESP.getMaxAllocHeap());

  XML_SetDefaultHandlerExpand(bs->parser, TextExtractor::defaultHandler);
  XML_SetUserData(bs->parser, &bs->extractor);
  XML_SetElementHandler(bs->parser, TextExtractor::startElement, TextExtractor::endElement);
  XML_SetCharacterDataHandler(bs->parser, TextExtractor::characterData);

  build_ = std::move(bs);
  partial_ = false;
  pageOffsets_.clear();
  loadedPageIndex_ = -1;
  return true;
}

bool VerticalSection::buildSomeMore(int maxPages) {
  if (!build_) return false;

  GfxRenderer::FrameBufferLoan loan(renderer);

  // If the frontier has been retried too many times without progress,
  // abandon the build — the heap is too fragmented to continue.
  if (build_->frontierRetryCount >= 5) {
    LOG_ERR("VSC", "Frontier retry limit reached (%u), abandoning spine %d",
            static_cast<unsigned>(build_->frontierRetryCount), spineIndex);
    abandonBuild();
    return false;
  }

  // Light retry mode: smaller budget so the heap has a chance to recover
  // between chunks.
  if (build_->frontierRetryCount > 0 && maxPages > 2) {
    maxPages = 2;
    LOG_DBG("VSC", "Frontier retry %u, budget reduced to %d",
            static_cast<unsigned>(build_->frontierRetryCount), maxPages);
  }

  const size_t pagesBefore = pageOffsets_.size();
  const size_t targetPages = (maxPages <= 0)
      ? std::numeric_limits<size_t>::max()
      : (pagesBefore + static_cast<size_t>(maxPages));

  build_->sink->pagesToSkip = static_cast<size_t>(pagesBefore);
  build_->sink->pageBudget = (maxPages > 0) ? maxPages : 0;
  build_->sink->hitBudget = false;
  build_->sink->frontierStop = false;

  // Reset the one-shot glyph-drop guard for this chunk.
  build_->layout->clearDropFlag();

  // If the parser was suspended by XML_StopParser (page budget reached),
  // resume it before feeding the next chunk.
  if (!build_->eof && build_->parser) {
    XML_ResumeParser(build_->parser);
  }

  while (!build_->eof && !build_->sink->hitBudget && pageOffsets_.size() < targetPages) {
    void* const buf = XML_GetBuffer(build_->parser, PARSE_BUFFER_SIZE);
    if (!buf) {
      LOG_ERR("VSC", "OOM: parse buffer");
      abandonBuild();
      return false;
    }
    const size_t len = build_->htmlFile.read(buf, PARSE_BUFFER_SIZE);
    if (len == 0 && build_->htmlFile.available() > 0) {
      LOG_ERR("VSC", "File read error");
      abandonBuild();
      return false;
    }
    const bool isFinal = (build_->htmlFile.available() == 0);
    if (XML_ParseBuffer(build_->parser, static_cast<int>(len), isFinal) == XML_STATUS_ERROR) {
      LOG_ERR("VSC", "XML parse error at line %lu: %s",
              XML_GetCurrentLineNumber(build_->parser),
              XML_ErrorString(XML_GetErrorCode(build_->parser)));
      abandonBuild();
      return false;
    }
    if (isFinal && !build_->sink->hitBudget) build_->eof = true;
  }

  build_->out.flush();
  loan.end();

  if (build_->sink->frontierStop && !build_->eof) {
    // Keep the parser/pipeline alive so the next buildSomeMore call can
    // resume from this frontier without re-creating the Expat parser
    // (which may fail on a fragmented heap).  The partial cache file is
    // not finalised until the activity exits (suspendBuild in ~VerticalSection).
    partial_ = true;
    LOG_ERR("VSC", "Frontier at %u pages — keeping build alive for spine %d",
            static_cast<unsigned>(pageOffsets_.size()), spineIndex);
    return true;
  }

  if (build_->eof) {
    build_->extractor.flushParagraph();
    build_->sink->flushText(/*isFinalFlush=*/true);
  }

  bool hitDropThisCall = build_->layout->everDroppedForHeap();

  if (hitDropThisCall) {
    build_->frontierRetryCount++;
    LOG_ERR("VSC", "Build dropped glyphs for spine %d (retry=%u)", spineIndex,
            static_cast<unsigned>(build_->frontierRetryCount));
  }

  build_->pagesAlreadyBuilt = static_cast<uint16_t>(pageOffsets_.size());
  pageCount = build_->pagesAlreadyBuilt;

  if (pageCount == 0) {
    const auto href = epub ? epub->getSpineItem(spineIndex).href : std::string();
    LOG_DBG("VSC", "No renderable vertical pages for spine %d: %s", spineIndex, href.c_str());
    build_->out.close();
    Storage.remove(binTmpPath(filePath).c_str());
    build_.reset();
    partial_ = false;
    return true;
  }

  if (!build_->eof) {
    partial_ = true;
    LOG_DBG("VSC", "Chunk build paused at %u pages (maxPages=%d)", pageCount, maxPages);
    return true;
  }

  // EOF reached: finalize cache and promote .part → .bin.
  if (!finalizeVerticalCache(build_->out, pageOffsets_, pageCount, 0)) {
    abandonBuild();
    return false;
  }
  build_->out.close();
  build_->htmlFile.close();
  const std::string tmpPath = binTmpPath(filePath);
  if (!Storage.rename(tmpPath.c_str(), filePath.c_str())) {
    LOG_ERR("VSC", "Failed to promote %s → %s", tmpPath.c_str(), filePath.c_str());
  }
  Storage.remove(build_->htmlPath.c_str());
  LOG_DBG("VSC", "Cached %u vertical pages (complete)", pageCount);
  build_.reset();
  partial_ = false;
  return true;
}

void VerticalSection::abandonBuild() {
  if (!build_) return;
  build_->out.close();
  Storage.remove(binTmpPath(filePath).c_str());
  // If there was no good .bin yet, drop the cache dir too so a future
  // load doesn't pick up a stale partial from a previous session.
  if (!Storage.exists(filePath.c_str())) {
    Storage.remove(filePath.c_str());
  }
  Storage.remove(build_->htmlPath.c_str());
  pageOffsets_.clear();
  pageCount = 0;
  build_.reset();
  partial_ = false;
}
void VerticalSection::suspendBuild() {
  if (!build_) return;
  if (pageOffsets_.empty()) {
    LOG_DBG("VSC", "suspendBuild(): no built pages yet, abandoning");
    abandonBuild();
    return;
  }
  build_->pagesAlreadyBuilt = static_cast<uint16_t>(pageOffsets_.size());
  pageCount = build_->pagesAlreadyBuilt;
  if (!finalizeVerticalCache(build_->out, pageOffsets_, pageCount, VSECTION_FLAG_PARTIAL)) {
    abandonBuild();
    return;
  }
  build_->out.close();
  // Promote .part → .bin so the partial survives on disk.
  const std::string tmpPath = binTmpPath(filePath);
  Storage.rename(tmpPath.c_str(), filePath.c_str());
  // Keep the temp HTML for cross-session resume (startBuild reuses it).
  build_.reset();
  partial_ = true;
  LOG_DBG("VSC", "Suspended vertical build at %u pages (HTML kept for resume)",
          static_cast<unsigned>(pageCount));
}

uint16_t VerticalSection::estimatedTotalPages() const {
  if (pageCount > 0) return pageCount;
  return 1;
}

bool VerticalSection::createSectionFile(const int fontId, const uint16_t viewportWidth,
                                         const uint16_t viewportHeight) {
  GfxRenderer::FrameBufferLoan loan(renderer);
  if (!startBuild(fontId, viewportWidth, viewportHeight)) return false;
  if (!buildSomeMore(0)) return false;
  return true;
}

bool VerticalSection::loadSectionFile(const int fontId, const uint16_t viewportWidth, const uint16_t viewportHeight) {
  // Clean up any stale .part file from a crash-interrupted build.
  const std::string tmpPath = binTmpPath(filePath);
  if (Storage.exists(tmpPath.c_str())) {
    Storage.remove(tmpPath.c_str());
  }
  // A missing cache file is the NORMAL case here, not an error: the book-progress counter probes
  // every spine's section on each page turn, and unbuilt chapters simply don't have one yet.
  // openFileForRead would print "File does not exist" per spine per probe -- pure log spam.
  if (!Storage.exists(filePath.c_str())) return false;
  HalFile file;
  if (!Storage.openFileForRead("VSC", filePath, file)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(file, version);
  if (version != VSECTION_FILE_VERSION) {
    file.close();
    LOG_DBG("VSC", "Version mismatch: %u vs %u", version, VSECTION_FILE_VERSION);
    clearCache();
    return false;
  }

  int cachedFontId;
  uint16_t cachedWidth, cachedHeight;
  serialization::readPod(file, cachedFontId);
  serialization::readPod(file, cachedWidth);
  serialization::readPod(file, cachedHeight);

  if (cachedFontId != fontId || cachedWidth != viewportWidth || cachedHeight != viewportHeight) {
    file.close();
    LOG_DBG("VSC", "Cache mismatch:");
    if (cachedFontId != fontId) LOG_DBG("VSC", "  fontId: cached=%d current=%d", cachedFontId, fontId);
    if (cachedWidth != viewportWidth) LOG_DBG("VSC", "  viewportWidth: cached=%d current=%d", cachedWidth, viewportWidth);
    if (cachedHeight != viewportHeight) LOG_DBG("VSC", "  viewportHeight: cached=%d current=%d", cachedHeight, viewportHeight);
    clearCache();
    return false;
  }

  uint16_t cachedPageCount;
  uint32_t indexOffset;
  uint16_t flags = 0;
  serialization::readPod(file, cachedPageCount);
  serialization::readPod(file, indexOffset);
  serialization::readPod(file, flags);

  pageOffsets_.clear();
  loadedPageIndex_ = -1;

  if (cachedPageCount > 0) {
    if (indexOffset == 0 || !file.seek(indexOffset)) {
      file.close();
      LOG_ERR("VSC", "Bad page index offset in cache");
      clearCache();
      return false;
    }
    pageOffsets_.resize(cachedPageCount);
    const size_t want = static_cast<size_t>(cachedPageCount) * sizeof(uint32_t);
    const size_t got = file.read(reinterpret_cast<uint8_t*>(pageOffsets_.data()), want);
    if (got != want) {
      pageOffsets_.clear();
      file.close();
      LOG_ERR("VSC", "Truncated page index in cache");
      clearCache();
      return false;
    }
  }

  file.close();
  pageCount = cachedPageCount;
  partial_ = (flags & VSECTION_FLAG_PARTIAL) != 0;

  // Zero-page caches are always invalid — a valid vertical cache must have at least page 0.
  if (pageCount == 0) {
    LOG_ERR("VSC", "Invalid zero-page vertical cache");
    clearCache();
    partial_ = false;
    return false;
  }

  LOG_DBG("VSC", "Opened cache: %u vertical pages (partial=%d)", pageCount, partial_ ? 1 : 0);
  return true;
}

bool VerticalSection::clearCache() const {
  if (!Storage.exists(filePath.c_str())) {
    return true;
  }
  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("VSC", "Failed to clear cache");
    return false;
  }
  LOG_DBG("VSC", "Cache cleared");
  return true;
}

const VerticalPage* VerticalSection::getPage() const { return getPage(currentPage); }

const VerticalPage* VerticalSection::getPage(int pageIndex) const {
  if (pageIndex < 0 || pageIndex >= static_cast<int>(pageOffsets_.size())) {
    return nullptr;
  }
  if (pageIndex == loadedPageIndex_) {
    return &loadedPage_;
  }

  // Fault the page in from the SD cache. The previous pointer returned by getPage() is
  // invalidated here -- all callers fetch-and-render one page at a time.
  loadedPageIndex_ = -1;

  // During an active build the cache file is written to .part (so the
  // existing .bin from a previous session stays readable).  If the .bin
  // hasn't been promoted yet, read from .part instead.
  const char* openPath = filePath.c_str();
  std::string partPath;
  if (!Storage.exists(filePath.c_str()) && build_) {
    partPath = binTmpPath(filePath);
    if (Storage.exists(partPath.c_str())) openPath = partPath.c_str();
  }

  HalFile file;
  if (!Storage.openFileForRead("VSC", openPath, file)) {
    return nullptr;
  }
  if (!file.seek(pageOffsets_[static_cast<size_t>(pageIndex)])) {
    file.close();
    return nullptr;
  }
  const bool ok = readPage(file, loadedPage_);
  file.close();
  if (!ok) {
    LOG_ERR("VSC", "Failed to read page %d from cache", pageIndex);
    return nullptr;
  }
  loadedPageIndex_ = pageIndex;
  return &loadedPage_;
}

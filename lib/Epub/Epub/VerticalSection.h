#pragma once
// Imported from matcha-reader (https://github.com/eszter007/matcha-reader) - MIT License

#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "VerticalParsedText.h"

class GfxRenderer;
class HalFile;

// A vertical-text chapter, backed by an on-SD page cache.
//
// Memory model: pages are NEVER all held in RAM. On an ESP32-C3 (~220KB usable heap) a single
// real Japanese chapter (~30k characters) would need ~1.3MB of laid-out VerticalGlyphs -- the
// original hold-everything design only ever worked in the desktop emulator's 8MB heap. Instead:
//   - createSectionFile() streams: XML parse feeds paragraphs directly into layout, and each
//     batch of laid-out pages is serialized to the cache file immediately and freed.
//   - loadSectionFile() reads only the header + a per-page offset table (4 bytes/page).
//   - getPage() loads the one requested page from SD on demand into a single-page cache.
//
// Build lifecycle (1.5-compatible stubs): createSectionFile() is synchronous, so
// isBuilding() is only true inside that call and isBuildComplete() means no build
// is running.  Future work can add true incremental build via startBuild/buildSomeMore.
class VerticalSection {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;

  // File offset of each serialized page record within the cache file.
  std::vector<uint32_t> pageOffsets_;

  // Single-page read cache backing getPage()'s returned pointer. Mutable because getPage() is
  // const to callers (a read) but faults the page in from SD. The pointer returned by getPage()
  // is invalidated by the next getPage() call for a different index -- all existing callers
  // fetch-and-render one page at a time, never holding two pages.
  mutable VerticalPage loadedPage_;
  mutable int loadedPageIndex_ = -1;

  bool streamParseAndLayout(HalFile& out, int fontId, uint16_t viewportWidth, uint16_t viewportHeight);

  // Set by streamParseAndLayout when the layout dropped chars/glyphs on low heap. The pages that
  // made it to disk are readable (this session keeps working).
  bool lastBuildDroppedForHeap_ = false;

  // Build state held while incremental build (startBuild / buildSomeMore) is running.
  struct BuildState {
    HalFile out;
    std::vector<uint32_t> pageOffsets;
    int fontId = 0;
    uint16_t viewportWidth = 0;
    uint16_t viewportHeight = 0;
    bool failed = false;
  };
  std::unique_ptr<BuildState> build_;

  // True while the build has written at least one page but hasn't finalized yet.
  // loadSectionFile sets this for cached partial files.
  bool partial_ = false;

  // Set before streamParseAndLayout to disable heavy optimisations when the
  // heap is too tight for large reserves but still above the hard-fail floor.
  bool lowMemMode_ = false;

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  explicit VerticalSection(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer)
      : epub(epub),
        spineIndex(spineIndex),
        renderer(renderer),
        filePath(epub->getCachePath() + "/vsections/" + std::to_string(spineIndex) + ".bin") {}

  bool loadSectionFile(int fontId, uint16_t viewportWidth, uint16_t viewportHeight);
  bool createSectionFile(int fontId, uint16_t viewportWidth, uint16_t viewportHeight);
  bool clearCache() const;
  const VerticalPage* getPage() const;
  const VerticalPage* getPage(int pageIndex) const;

  // Incremental build lifecycle (1.5-compatible).
  bool startBuild(int fontId, uint16_t viewportWidth, uint16_t viewportHeight);
  bool buildSomeMore(int maxPages);
  bool isBuilding() const { return build_ != nullptr; }
  bool isBuildComplete() const { return build_ == nullptr; }
  bool isPartial() const { return partial_; }
  void suspendBuild();
  void abandonBuild();
  uint16_t estimatedTotalPages() const { return pageCount > 0 ? pageCount : 1; }
};

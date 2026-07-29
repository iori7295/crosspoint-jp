#pragma once
// Imported from matcha-reader (https://github.com/eszter007/matcha-reader) - MIT License

#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "VerticalParsedText.h"

class GfxRenderer;
class HalFile;
struct XML_ParserStruct;
typedef struct XML_ParserStruct* XML_Parser;

struct VerticalPage;

// A vertical-text chapter, backed by an on-SD page cache.
//
// Build lifecycle (1.5-compatible):
//   startBuild()  — extract HTML, create parser, open output file
//   buildSomeMore(N) — read up to N pages (0 = all), stop when target reached
//   suspendBuild() / abandonBuild() — persist or discard partial progress
//   loadSectionFile() — load a complete or partial cache
class VerticalSection {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;

  std::vector<uint32_t> pageOffsets_;

  mutable VerticalPage loadedPage_;
  mutable int loadedPageIndex_ = -1;

  bool lastBuildDroppedForHeap_ = false;
  bool partial_ = false;

  struct BuildState;
  std::unique_ptr<BuildState> build_;

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  explicit VerticalSection(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer);
  ~VerticalSection();

  bool loadSectionFile(int fontId, uint16_t viewportWidth, uint16_t viewportHeight);
  bool createSectionFile(int fontId, uint16_t viewportWidth, uint16_t viewportHeight);
  bool clearCache() const;
  const VerticalPage* getPage() const;
  const VerticalPage* getPage(int pageIndex) const;

  // Incremental build lifecycle.
  bool startBuild(int fontId, uint16_t viewportWidth, uint16_t viewportHeight);
  bool buildSomeMore(int maxPages);
  bool isBuilding() const { return build_ != nullptr; }
  bool isBuildComplete() const { return build_ == nullptr; }
  bool isPartial() const { return partial_; }
  void suspendBuild();
  void abandonBuild();
  uint16_t estimatedTotalPages() const;
};

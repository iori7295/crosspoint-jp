#include "AozoraActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

struct KanaRow {
  StrId label;
  const char* apiParam;
};

static const KanaRow KANA_ROWS[] = {
    {StrId::STR_KANA_A, "\xe3\x82\xa2"},  {StrId::STR_KANA_KA, "\xe3\x82\xab"},
    {StrId::STR_KANA_SA, "\xe3\x82\xb5"},  {StrId::STR_KANA_TA, "\xe3\x82\xbf"},
    {StrId::STR_KANA_NA, "\xe3\x83\x8a"},  {StrId::STR_KANA_HA, "\xe3\x83\x8f"},
    {StrId::STR_KANA_MA, "\xe3\x83\x9e"},  {StrId::STR_KANA_YA, "\xe3\x83\xa4"},
    {StrId::STR_KANA_RA, "\xe3\x83\xa9"},  {StrId::STR_KANA_WA, "\xe3\x83\xaf"},
};
static constexpr int KANA_ROW_COUNT = 10;

struct GenreRow {
  StrId label;
  const char* ndc;
};

static const GenreRow GENRES[] = {
    {StrId::STR_GENRE_NOVEL, "913"},
    {StrId::STR_GENRE_POETRY, "911"},
    {StrId::STR_GENRE_ESSAY, "914"},
    {StrId::STR_GENRE_DRAMA, "912"},
    {StrId::STR_GENRE_FAIRY_TALE, "388"},
};
static constexpr int GENRE_COUNT = 5;
static constexpr int TOP_MENU_COUNT = 5;
static constexpr const char* API_TMP_FILE = "/aozora_api.tmp";

AozoraActivity::AozoraActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Aozora", renderer, mappedInput) {}

void AozoraActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void AozoraActivity::onExit() {
  Activity::onExit();
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

void AozoraActivity::onWifiSelectionComplete(const bool success) {
  if (!success) { finish(); return; }
  AozoraIndexManager::ensureDirectory();
  indexManager_.loadAndPurge();
  { RenderLock lock(*this); state_ = TOP_MENU; selectedIndex_ = 0; }
}

void AozoraActivity::pushState(State newState) {
  stateStack_.push_back(state_);
  selectedIndexStack_.push_back(selectedIndex_);
  state_ = newState;
  selectedIndex_ = 0;
}

void AozoraActivity::popState() {
  if (stateStack_.empty()) { finish(); return; }
  state_ = stateStack_.back();
  selectedIndex_ = selectedIndexStack_.back();
  stateStack_.pop_back();
  selectedIndexStack_.pop_back();
}

static bool fetchApiJson(const char* url, JsonDocument& doc) {
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) delay(1000);
    bool cancel = false;
    auto result = HttpDownloader::downloadToFile(url, API_TMP_FILE, nullptr, &cancel);
    if (result == HttpDownloader::OK) break;
    Storage.remove(API_TMP_FILE);
    if (attempt == 2) return false;
  }
  HalFile file;
  if (!Storage.openFileForRead("AOZORA", API_TMP_FILE, file)) {
    Storage.remove(API_TMP_FILE);
    return false;
  }
  DeserializationError err = deserializeJson(doc, file);
  file.close();
  Storage.remove(API_TMP_FILE);
  if (err) return false;
  return true;
}

bool AozoraActivity::fetchAuthors(const char* kanaPrefix) {
  works_.clear();
  works_.shrink_to_fit();
  char url[256];
  snprintf(url, sizeof(url), "%s/api/authors?kana_prefix=%s", API_BASE, kanaPrefix);
  JsonDocument doc;
  if (!fetchApiJson(url, doc)) return false;
  return parseAuthorsJson(doc);
}

bool AozoraActivity::parseAuthorsJson(JsonDocument& doc) {
  if (doc["error"].is<const char*>()) {
    errorMessage_ = doc["message"] | "API error";
    return false;
  }
  authors_.clear();
  JsonArray arr = doc["authors"].as<JsonArray>();
  authors_.reserve(arr.size());
  for (JsonObject obj : arr) {
    AuthorEntry e;
    e.id = obj["id"] | 0;
    snprintf(e.name, sizeof(e.name), "%s", (const char*)(obj["name"] | ""));
    snprintf(e.kana, sizeof(e.kana), "%s", (const char*)(obj["kana"] | ""));
    e.workCount = obj["work_count"] | 0;
    authors_.push_back(e);
  }
  return true;
}

bool AozoraActivity::fetchWorks(const char* queryParam) {
  char url[320];
  snprintf(url, sizeof(url), "%s/api/works?%s", API_BASE, queryParam);
  JsonDocument doc;
  if (!fetchApiJson(url, doc)) return false;
  return parseWorksJson(doc);
}

bool AozoraActivity::parseWorksJson(JsonDocument& doc) {
  if (doc["error"].is<const char*>()) {
    errorMessage_ = doc["message"] | "API error";
    return false;
  }
  works_.clear();
  JsonArray arr = doc["works"].as<JsonArray>();
  works_.reserve(arr.size());
  for (JsonObject obj : arr) {
    WorkEntry e;
    e.id = obj["id"] | 0;
    snprintf(e.title, sizeof(e.title), "%s", (const char*)(obj["title"] | ""));
    snprintf(e.author, sizeof(e.author), "%s", (const char*)(obj["author"] | ""));
    works_.push_back(e);
  }
  return true;
}

bool AozoraActivity::downloadBook() {
  AozoraIndexManager::ensureAuthorDirectory(selectedWorkAuthor_);
  std::string relPath = AozoraIndexManager::makeRelativePath(selectedWorkId_, selectedWorkTitle_, selectedWorkAuthor_);
  char destPath[160];
  snprintf(destPath, sizeof(destPath), "%s/%s", AozoraIndexManager::AOZORA_DIR, relPath.c_str());
  char url[256];
  snprintf(url, sizeof(url), "%s/api/convert?work_id=%d", API_BASE, selectedWorkId_);
  bool cancel = false;
  auto result = HttpDownloader::downloadToFile(std::string(url), std::string(destPath),
                                                [this](size_t downloaded, size_t total) {
                                                  downloadProgress_ = downloaded;
                                                  downloadTotal_ = total;
                                                  requestUpdate(true);
                                                }, &cancel);
  if (result != HttpDownloader::OK) {
    Storage.remove(destPath);
    return false;
  }
  return indexManager_.addEntry(selectedWorkId_, selectedWorkTitle_, selectedWorkAuthor_, relPath.c_str());
}

void AozoraActivity::loop() {
  if (state_ == WIFI_SELECTION || state_ == LOADING || state_ == DOWNLOADING) return;
  mappedInput.update();

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (state_ != TOP_MENU) { RenderLock lock(*this); popState(); requestUpdate(); }
    else { finish(); }
    return;
  }

  const int count = [this]() -> int {
    switch (state_) {
      case TOP_MENU: return TOP_MENU_COUNT;
      case KANA_SELECT: return KANA_ROW_COUNT;
      case GENRE_SELECT: return GENRE_COUNT;
      case AUTHOR_LIST: return static_cast<int>(authors_.size());
      case WORK_LIST: return static_cast<int>(works_.size());
      case DOWNLOADED_LIST: return static_cast<int>(indexManager_.entries().size());
      case WORK_DETAIL: return 1;
      default: return 0;
    }
  }();

  buttonNavigator_.onNextRelease([this, count] {
    if (selectedIndex_ < count - 1) { selectedIndex_++; requestUpdate(); }
  });
  buttonNavigator_.onPreviousRelease([this] {
    if (selectedIndex_ > 0) { selectedIndex_--; requestUpdate(); }
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    switch (state_) {
      case TOP_MENU: {
        RenderLock lock(*this);
        switch (selectedIndex_) {
          case 0: searchMode_ = SEARCH_AUTHOR; pushState(KANA_SELECT); break;
          case 1: searchMode_ = SEARCH_TITLE; pushState(KANA_SELECT); break;
          case 2: pushState(GENRE_SELECT); break;
          case 3: pushState(LOADING); break;
          case 4: pushState(DOWNLOADED_LIST); break;
        }
        requestUpdate();
        if (selectedIndex_ == 3) {
          requestUpdateAndWait();
          bool ok = fetchWorks("sort=newest&limit=50");
          { RenderLock lock(*this); if (ok) state_ = WORK_LIST; else state_ = ERROR; }
          requestUpdate();
        }
        break;
      }
      case KANA_SELECT: {
        const char* prefix = KANA_ROWS[selectedIndex_].apiParam;
        {
          RenderLock lock(*this);
          pushState(LOADING);
        }
        requestUpdateAndWait();
        bool ok;
        if (searchMode_ == SEARCH_AUTHOR) ok = fetchAuthors(prefix);
        else ok = fetchWorks(prefix);
        {
          RenderLock lock(*this);
          if (ok) state_ = (searchMode_ == SEARCH_AUTHOR) ? AUTHOR_LIST : WORK_LIST;
          else state_ = ERROR;
        }
        requestUpdate();
        break;
      }
      case GENRE_SELECT: {
        const char* ndc = GENRES[selectedIndex_].ndc;
        { RenderLock lock(*this); pushState(LOADING); }
        requestUpdateAndWait();
        char param[32]; snprintf(param, sizeof(param), "ndc=%s", ndc);
        bool ok = fetchWorks(param);
        { RenderLock lock(*this);
          if (ok) state_ = WORK_LIST; else state_ = ERROR; }
        requestUpdate();
        break;
      }
      case AUTHOR_LIST: {
        if (authors_.empty()) break;
        selectedAuthorId_ = authors_[selectedIndex_].id;
        snprintf(selectedAuthorName_, sizeof(selectedAuthorName_), "%s", authors_[selectedIndex_].name);
        { RenderLock lock(*this); pushState(LOADING); }
        requestUpdateAndWait();
        char param[32]; snprintf(param, sizeof(param), "author_id=%d", selectedAuthorId_);
        bool ok = fetchWorks(param);
        { RenderLock lock(*this);
          if (ok) state_ = WORK_LIST; else state_ = ERROR; }
        requestUpdate();
        break;
      }
      case WORK_LIST: {
        if (works_.empty()) break;
        selectedWorkId_ = works_[selectedIndex_].id;
        snprintf(selectedWorkTitle_, sizeof(selectedWorkTitle_), "%s", works_[selectedIndex_].title);
        snprintf(selectedWorkAuthor_, sizeof(selectedWorkAuthor_), "%s", works_[selectedIndex_].author);
        RenderLock lock(*this); pushState(WORK_DETAIL); requestUpdate();
        break;
      }
      case WORK_DETAIL: {
        if (indexManager_.isDownloaded(selectedWorkId_)) {
          indexManager_.removeEntry(selectedWorkId_);
          requestUpdate();
        } else {
          { RenderLock lock(*this); state_ = DOWNLOADING; downloadProgress_ = 0; downloadTotal_ = 0; }
          requestUpdateAndWait();
          bool ok = downloadBook();
          { RenderLock lock(*this);
            if (ok) state_ = WORK_DETAIL; else { errorMessage_ = "Download failed"; state_ = ERROR; } }
          requestUpdate();
        }
        break;
      }
      case DOWNLOADED_LIST: {
        const auto& entries = indexManager_.entries();
        if (entries.empty()) break;
        selectedWorkId_ = entries[selectedIndex_].workId;
        snprintf(selectedWorkTitle_, sizeof(selectedWorkTitle_), "%s", entries[selectedIndex_].title);
        snprintf(selectedWorkAuthor_, sizeof(selectedWorkAuthor_), "%s", entries[selectedIndex_].author);
        RenderLock lock(*this); pushState(WORK_DETAIL); requestUpdate();
        break;
      }
      default: break;
    }
  }
}

void AozoraActivity::render(RenderLock&&) {
  const auto pw = renderer.getScreenWidth();
  const auto ph = renderer.getScreenHeight();
  renderer.clearScreen();
  const auto& m = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, m.topPadding, pw, m.headerHeight}, I18n::getInstance().get(StrId::STR_AOZORA_BUNKO));

  const Rect cr{m.contentSidePadding, m.topPadding + m.headerHeight + m.verticalSpacing,
                    pw - m.contentSidePadding * 2,
                    ph - m.topPadding - m.headerHeight - m.verticalSpacing - m.buttonHintsHeight};

  switch (state_) {
    case TOP_MENU: {
      const StrId labels[] = {StrId::STR_SEARCH_BY_AUTHOR, StrId::STR_SEARCH_BY_TITLE, StrId::STR_SEARCH_BY_GENRE,
                               StrId::STR_NEWEST_WORKS, StrId::STR_DOWNLOADED_BOOKS};
      GUI.drawList(renderer, cr, TOP_MENU_COUNT, selectedIndex_,
                    [&labels](int i) -> std::string { return I18n::getInstance().get(labels[i]); },
                    nullptr, nullptr, nullptr, true, nullptr);
      break;
    }
    case KANA_SELECT:
      GUI.drawList(renderer, cr, KANA_ROW_COUNT, selectedIndex_,
                    [](int i) -> std::string { return I18n::getInstance().get(KANA_ROWS[i].label); },
                    nullptr, nullptr, nullptr, true, nullptr);
      break;
    case GENRE_SELECT:
      GUI.drawList(renderer, cr, GENRE_COUNT, selectedIndex_,
                    [](int i) -> std::string { return I18n::getInstance().get(GENRES[i].label); },
                    nullptr, nullptr, nullptr, true, nullptr);
      break;
    case AUTHOR_LIST: {
      if (authors_.empty()) { renderer.drawCenteredText(UI_10_FONT_ID, ph / 2, "No results"); break; }
      GUI.drawList(renderer, cr, static_cast<int>(authors_.size()), selectedIndex_,
                    [this](int i) -> std::string { return authors_[i].name; },
                    nullptr, nullptr,
                    [this](int i) -> std::string {
                      char buf[32]; snprintf(buf, sizeof(buf), "%d", authors_[i].workCount);
                      return buf;
                    }, true, nullptr);
      break;
    }
    case WORK_LIST: {
      if (works_.empty()) { renderer.drawCenteredText(UI_10_FONT_ID, ph / 2, I18n::getInstance().get(StrId::STR_NO_RESULTS)); break; }
      GUI.drawList(renderer, cr, static_cast<int>(works_.size()), selectedIndex_,
                    [this](int i) -> std::string { return works_[i].title; },
                    nullptr, nullptr, nullptr, true,
                    [this](int i) -> bool { return indexManager_.isDownloaded(works_[i].id); });
      break;
    }
    case WORK_DETAIL: {
      int startY = m.topPadding + m.headerHeight + m.verticalSpacing * 2;
      renderer.drawText(UI_12_FONT_ID, m.contentSidePadding, startY, selectedWorkTitle_, true);
      renderer.drawText(UI_10_FONT_ID, m.contentSidePadding, startY + 40, selectedWorkAuthor_, true);
      bool dled = indexManager_.isDownloaded(selectedWorkId_);
      if (dled) {
        renderer.drawText(UI_10_FONT_ID, m.contentSidePadding, startY + 80, I18n::getInstance().get(StrId::STR_ALREADY_DOWNLOADED), true);
        GUI.drawButtonHints(renderer, I18n::getInstance().get(StrId::STR_BACK), I18n::getInstance().get(StrId::STR_DELETE_CONFIRM), nullptr, nullptr);
      } else {
        GUI.drawButtonHints(renderer, I18n::getInstance().get(StrId::STR_BACK), I18n::getInstance().get(StrId::STR_DOWNLOAD_CONFIRM), nullptr, nullptr);
      }
      break;
    }
    case DOWNLOADING: {
      renderer.drawCenteredText(UI_10_FONT_ID, ph / 2 - 20, I18n::getInstance().get(StrId::STR_DOWNLOADING_BOOK));
      if (downloadTotal_ > 0) {
        Rect pr{m.contentSidePadding, ph / 2 + 10, pw - m.contentSidePadding * 2, 20};
        GUI.drawProgressBar(renderer, pr, static_cast<int>((downloadProgress_ * 100) / downloadTotal_), 100);
      }
      break;
    }
    case DOWNLOADED_LIST: {
      const auto& entries = indexManager_.entries();
      if (entries.empty()) { renderer.drawCenteredText(UI_10_FONT_ID, ph / 2, I18n::getInstance().get(StrId::STR_NO_RESULTS)); break; }
      GUI.drawList(renderer, cr, static_cast<int>(entries.size()), selectedIndex_,
                    [&entries](int i) -> std::string { return entries[i].title; },
                    nullptr, nullptr,
                    [&entries](int i) -> std::string { return entries[i].author; },
                    true, nullptr);
      break;
    }
    case LOADING:
      renderer.drawCenteredText(UI_10_FONT_ID, ph / 2, I18n::getInstance().get(StrId::STR_LOADING_WORKS));
      break;
    case ERROR:
      renderer.drawCenteredText(UI_10_FONT_ID, ph / 2,
                                errorMessage_.empty() ? I18n::getInstance().get(StrId::STR_CONNECTION_FAILED) : errorMessage_.c_str());
      break;
    default: break;
  }
  renderer.displayBuffer();
}

#pragma once

#include <ArduinoJson.h>

#include <string>
#include <vector>

#include "AozoraIndexManager.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class AozoraActivity final : public Activity {
 public:
  explicit AozoraActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state_ == LOADING || state_ == DOWNLOADING; }

 private:
  enum State {
    WIFI_SELECTION,
    TOP_MENU,
    KANA_SELECT,
    GENRE_SELECT,
    AUTHOR_LIST,
    WORK_LIST,
    WORK_DETAIL,
    DOWNLOADING,
    DOWNLOADED_LIST,
    LOADING,
    ERROR,
  };

  enum SearchMode { SEARCH_AUTHOR, SEARCH_TITLE };

  struct AuthorEntry {
    int id;
    char name[48];
    char kana[48];
    int workCount;
  };

  struct WorkEntry {
    int id;
    char title[80];
    char author[48];
  };

  State state_ = WIFI_SELECTION;
  SearchMode searchMode_ = SEARCH_AUTHOR;
  ButtonNavigator buttonNavigator_;
  int selectedIndex_ = 0;
  std::string errorMessage_;

  std::vector<State> stateStack_;
  std::vector<int> selectedIndexStack_;

  std::vector<AuthorEntry> authors_;
  std::vector<WorkEntry> works_;

  int selectedAuthorId_ = 0;
  char selectedAuthorName_[48] = {};
  int selectedWorkId_ = 0;
  char selectedWorkTitle_[80] = {};
  char selectedWorkAuthor_[48] = {};

  size_t downloadProgress_ = 0;
  size_t downloadTotal_ = 0;

  AozoraIndexManager indexManager_;

  static constexpr const char* API_BASE = "https://aozora-epub-api.vercel.app";

  void pushState(State newState);
  void popState();
  void onWifiSelectionComplete(bool success);
  bool fetchAuthors(const char* kanaPrefix);
  bool fetchWorks(const char* queryParam);
  bool downloadBook();
  bool parseAuthorsJson(JsonDocument& doc);
  bool parseWorksJson(JsonDocument& doc);
};

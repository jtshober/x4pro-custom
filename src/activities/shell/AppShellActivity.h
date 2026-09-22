#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"

struct Rect;

// Phase 1 of the PageBridge-style redesign: a 4-tab shell (Continue / Books /
// Book Server / Settings) that replaces HomeActivity as the app's landing
// screen. One Activity owns all four tabs directly -- rather than four
// separate Activities -- so switching tabs is just a redraw, not a full
// Activity teardown/rebuild.
//
// Gesture split (deliberate, from user testing of the first draft): swipe is
// NOT used to move between the four tabs -- only tapping the tab bar does
// that. Swipe left/right is reserved entirely for the Continue tab's own
// book carousel. This means swiping on Books/Book Server/Settings currently
// does nothing; that's intentional, not an oversight.
//
// Scope of this round: Continue is fully custom-drawn -- a swipeable
// carousel of your most recent books, neighboring covers peeking at the
// edges, the centered one opens in the reader. Books, Book Server, and
// Settings are still simple "tap to open" launch panels handing off to the
// real, existing FileBrowserActivity / OpdsBookBrowserActivity /
// SettingsActivity -- those screens are not yet wrapped in the tab bar
// themselves. That's the next round, once this shell is confirmed solid.
//
// HomeActivity is left in the tree, untouched and simply unused: goHome() no
// longer constructs it. Safer than deleting it while this is still unproven
// on a fresh branch.
class AppShellActivity final : public Activity {
 public:
  enum class Tab { Continue = 0, Books = 1, BookServer = 2, Settings = 3 };
  static constexpr int TAB_COUNT = 4;

  explicit AppShellActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Tab startTab = Tab::Continue)
      : Activity("AppShell", renderer, mappedInput), activeTab(startTab) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Keeps the existing Home-gesture suppression in ActivityManager's loop
  // (never treat a gesture back to the shell itself as "go home again")
  // working with zero changes there -- see isHomeActivity()'s only call site.
  bool isHomeActivity() const override { return true; }

 private:
  static constexpr int TAB_BAR_HEIGHT = 64;
  // How many recent books the Continue carousel can page through. More than
  // this and "continue reading" stops meaning much; adjust freely.
  static constexpr size_t MAX_CAROUSEL_BOOKS = 5;

  Tab activeTab;
  std::vector<RecentBook> recentBooks;  // Up to MAX_CAROUSEL_BOOKS, most recent first.
  size_t carouselIndex = 0;             // Which book is centered on the Continue tab.

  // Centered cover's buffer cache -- same pattern as HomeActivity's cover
  // tile (see storeCoverBuffer/restoreCoverBuffer there). Only the centered
  // cover is cached; the two peeking side covers are small and redrawn fresh
  // each time, since they're never the thing being repeatedly repainted
  // during a swipe the way the centered one is.
  bool coverRendered = false;
  bool coverBufferStored = false;
  uint8_t* coverBuffer = nullptr;
  size_t coverBufferSize = 0;
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;

  void loadRecentBooks();
  bool storeCoverBuffer();
  bool restoreCoverBuffer();
  void freeCoverBuffer();
  void invalidateCoverCache();

  size_t nextCarouselIndex() const;
  size_t previousCarouselIndex() const;
  void setCarouselIndex(size_t index);

  void switchTab(Tab tab);
  void renderTabBar(Rect rect);
  void renderContinueBody(Rect body);
  void renderLaunchPanelBody(Rect body, const char* label);
  // Draws one of the two small, uncached side covers (no selection chrome,
  // no caching -- see the class comment above).
  void renderPeekCover(Rect rect, const RecentBook& book) const;
  // Books/Book Server/Settings only -- Continue's taps are handled inline in
  // loop() since they act on the carousel, not another top-level screen.
  void openActiveTabTarget();
};

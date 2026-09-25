#pragma once

#include <cstddef>
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
// Gesture split: swipe is NOT used to move between the four tabs -- only
// tapping the tab bar does that. Swipe left/right, like the physical
// page-turn buttons, is reserved entirely for moving the Continue tab's own
// shelf selection.
//
// Books/Book Server/Settings: tapping the tab bar jumps straight into the
// real, existing FileBrowserActivity / OpdsBookBrowserActivity /
// SettingsActivity -- no intermediate screen. The one case that still shows
// a "tap to open" panel is landing back on the shell after backing out of
// one of those screens (see switchTab()'s comment for why that case is
// deliberately NOT auto-forwarded: doing so would bounce Back straight back
// into the screen you just left).
//
// Continue is fully custom-drawn: a wooden bookcase -- your most recent
// books standing on a small number of shelves, arranged left-to-right,
// top-to-bottom, most recent first. No title text under a cover (it would
// clash with the shelf background) and no separate "Continue Reading"
// button -- tapping any cover opens it directly. Page Up/Down (and swipe)
// move a thick selection box across the shelves instead: forward moves right
// and wraps onto the next shelf down, back moves left and wraps onto the
// previous shelf; a short Power press opens whichever book is selected.
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
  void loop() override;
  void render(RenderLock&&) override;
  // Keeps the existing Home-gesture suppression in ActivityManager's loop
  // (never treat a gesture back to the shell itself as "go home again")
  // working with zero changes there -- see isHomeActivity()'s only call site.
  bool isHomeActivity() const override { return true; }

 private:
  static constexpr int TAB_BAR_HEIGHT = 48;  // 3/4 of the original 64px.
  // How many recent books the Continue shelf can show. Independent of (but
  // matched to) RecentBooksStore::MAX_RECENT_BOOKS, which is private to that
  // class -- keep the two in sync if either changes.
  static constexpr size_t MAX_SHELF_BOOKS = 10;

  Tab activeTab;
  std::vector<RecentBook> recentBooks;  // Up to MAX_SHELF_BOOKS, most recent first.
  size_t selectedIndex = 0;             // Which book the shelf's selection box is on.

  void loadRecentBooks();
  size_t nextShelfIndex() const;
  size_t previousShelfIndex() const;
  void setSelectedIndex(size_t index);

  void switchTab(Tab tab);
  // The Continue/Books/Book-Server/Settings content area, below the header
  // and above the tab bar -- computed the same way in render() (to draw it)
  // and loop() (to hit-test a tap against it), so the two can never drift
  // apart.
  Rect computeBodyRect() const;
  void renderTabBar(Rect rect);
  void renderContinueBody(Rect body);
  void renderLaunchPanelBody(Rect body, const char* label);
  // Makes sure book's cover thumbnail at `height` exists on disk, generating
  // it from the source EPUB/XTC if this is the first time it's been shown at
  // that height (mirrors HomeActivity::loadRecentCovers() -- that function
  // only ever primes recentBooks[0], so anything the shelf shows beyond that
  // needs the same on-demand generation here).
  void ensureCoverThumb(const RecentBook& book, int height) const;
  // Draws one book's cover at (x, y), aspect-fit and centered within a
  // boxWidth x boxHeight box -- the shared "shelf slot" size every book on a
  // given shelf occupies, so covers of different proportions still line up
  // into tidy rows. Falls back to a plain bordered placeholder box (in the
  // same 2:3 proportion real covers use) if there's no cover art or it fails
  // to load. Generates/caches the on-disk thumbnail at exactly boxHeight
  // (see the .cpp) rather than some fixed default, so a cover never renders
  // smaller than its shelf slot just because that slot happens to be taller
  // than some other screen's default cover size.
  void renderCoverBox(int x, int y, int boxWidth, int boxHeight, const RecentBook& book) const;
  // Books/Book Server/Settings only -- Continue's taps are handled inline in
  // loop() since they act on the shelf, not another top-level screen.
  void openActiveTabTarget();
};

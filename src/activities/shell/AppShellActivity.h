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
// tapping the tab bar does that. Swipe left/right is reserved entirely for
// the Continue tab's own book carousel.
//
// Books/Book Server/Settings: tapping the tab bar jumps straight into the
// real, existing FileBrowserActivity / OpdsBookBrowserActivity /
// SettingsActivity -- no intermediate screen. The one case that still shows
// a "tap to open" panel is landing back on the shell after backing out of
// one of those screens (see switchTab()'s comment for why that case is
// deliberately NOT auto-forwarded: doing so would bounce Back straight back
// into the screen you just left).
//
// Continue is fully custom-drawn: a swipeable carousel of your most recent
// books, neighboring covers peeking at the edges, framed top and bottom by a
// small ink-wash-style motif (see renderContinueArt) -- drawn as plain
// vector shapes (fillPolygon/drawLine), not a bitmap, so it costs no flash
// and renders correctly in all 4 orientations for free. Tapping the centered
// cover opens it directly -- no separate button.
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
  // How many recent books the Continue carousel can page through.
  static constexpr size_t MAX_CAROUSEL_BOOKS = 5;

  Tab activeTab;
  std::vector<RecentBook> recentBooks;  // Up to MAX_CAROUSEL_BOOKS, most recent first.
  size_t carouselIndex = 0;             // Which book is centered on the Continue tab.

  void loadRecentBooks();
  size_t nextCarouselIndex() const;
  size_t previousCarouselIndex() const;
  void setCarouselIndex(size_t index);

  void switchTab(Tab tab);
  void renderTabBar(Rect rect);
  void renderContinueBody(Rect body);
  // Small ink-wash-style motif drawn above and below the carousel/title
  // block, confined to whatever vertical space is left over once that
  // block's own height is known -- never overlaps it, on any screen size or
  // orientation. See the .cpp for what it actually draws and why it's pure
  // vector shapes rather than a stored image.
  void renderContinueArt(Rect body, int topGapHeight, int bottomGapHeight) const;
  void renderLaunchPanelBody(Rect body, const char* label);
  // Makes sure book's cover thumbnail at `height` exists on disk, generating
  // it from the source EPUB/XTC if this is the first time it's been shown
  // (mirrors HomeActivity::loadRecentCovers() -- that function only ever
  // primes recentBooks[0], so anything the carousel scrolls to beyond that
  // needs the same on-demand generation here).
  void ensureCoverThumb(const RecentBook& book, int height) const;
  // Decodes book's cover (generating it first if needed) and reports the
  // size it would draw at, aspect-fit within a heightCap-tall box -- without
  // drawing anything. Used once, on the centered book, so every cover in the
  // carousel shares one box size (peeks included) rather than each sizing
  // itself independently. width/height are set to 0 if there's no cover or
  // it fails to load.
  void measureCoverSize(const RecentBook& book, int heightCap, int& width, int& height) const;
  // Draws one book's cover at (x, y), aspect-fit and centered within a
  // boxWidth x boxHeight box -- x (and x + boxWidth) may fall outside the
  // screen entirely on purpose, for the half-cropped peeking covers; the
  // renderer safely clips anything off-canvas. Falls back to a plain
  // bordered placeholder box if there's no cover art or it fails to load.
  void renderCoverBox(int x, int y, int boxWidth, int boxHeight, const RecentBook& book) const;
  // Books/Book Server/Settings only -- Continue's taps are handled inline in
  // loop() since they act on the carousel, not another top-level screen.
  void openActiveTabTarget();
};

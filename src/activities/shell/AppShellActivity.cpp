#include "AppShellActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdint>

#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ContinueMetadataEnricher.h"
#include "util/HomeStatusService.h"

namespace {
// Not yet run through the localization pipeline -- English only until this
// shell is confirmed and worth wiring into the 34-language string tables
// like the rest of the app's UI text.
const char* tabLabel(const AppShellActivity::Tab tab) {
  switch (tab) {
    case AppShellActivity::Tab::Continue:
      return "Continue";
    case AppShellActivity::Tab::Books:
      return "Books";
    case AppShellActivity::Tab::BookServer:
      return "Book Server";
    case AppShellActivity::Tab::Settings:
      return "Settings";
  }
  return "";
}

// Largest of the sizes drawFittedTitle tries -- callers reserve layout space
// for this regardless of which size (or how many lines) ends up used, since
// smaller/fewer only ever needs less room, never more. Matches the top of
// the font ladder in drawFittedTitle below.
constexpr int TITLE_MAX_FONT_ID = NOTOSANS_12_FONT_ID;
// Fixed vertical gap between the title's two lines, when two are used.
constexpr int TITLE_LINE_GAP = 6;

// Greedily splits `title` on spaces into two lines at `fontId`: line1 gets as
// many whole words as fit in maxWidth, line2 gets the rest. Returns false
// (leaving line1/line2 untouched) if line2 still doesn't fit even after
// wrapping -- a caller should then either try a smaller font or fall back to
// truncation, not assume wrapping always succeeds (one very long word alone
// can still overflow, for instance).
bool wrapTitleTwoLines(const GfxRenderer& renderer, const int fontId, const std::string& title, const int maxWidth,
                       std::string& line1, std::string& line2) {
  size_t splitAt = std::string::npos;
  size_t searchFrom = 0;
  while (true) {
    const size_t spaceAt = title.find(' ', searchFrom);
    const size_t candidateEnd = (spaceAt == std::string::npos) ? title.size() : spaceAt;
    if (renderer.getTextWidth(fontId, title.substr(0, candidateEnd).c_str()) > maxWidth) break;
    splitAt = candidateEnd;
    if (spaceAt == std::string::npos) break;
    searchFrom = spaceAt + 1;
  }
  if (splitAt == std::string::npos || splitAt >= title.size()) return false;  // Not even one word fits, or fits whole.
  line1 = title.substr(0, splitAt);
  line2 = title.substr(title.find_first_not_of(' ', splitAt));
  return !line2.empty() && renderer.getTextWidth(fontId, line2.c_str()) <= maxWidth;
}

void drawCenteredLine(const GfxRenderer& renderer, const Rect body, const int fontId, const int y,
                     const std::string& text) {
  const int width = renderer.getTextWidth(fontId, text.c_str());
  renderer.drawText(fontId, body.x + std::max(0, (body.width - width) / 2), y, text.c_str());
}

// Draws `title` centered under the cover, within a `blockHeight`-tall
// reserved area starting at `blockY` (always the same height regardless of
// how many lines a given title actually needs, so the layout doesn't shift
// as you swipe between short- and long-title books). Prefers one line;
// wraps to two, shrinking through a few sizes as needed, if it doesn't fit
// on one; truncates the second line with an ellipsis as a last resort.
void drawFittedTitle(const GfxRenderer& renderer, const Rect body, const int blockY, const int blockHeight,
                     const std::string& title, const int maxWidth) {
  // -30% then another ~10% per request: these are fixed, pre-baked font
  // sizes (12/14/16/18, plus one small 8px font), not a continuously
  // scalable typeface, so there's no exact percentage step available below
  // 12 -- the closest real step is dropping 14 entirely and topping out at
  // 12, which is what this does now (was 14/12/8, now 12/8).
  static const int fontIds[] = {NOTOSANS_12_FONT_ID, SMALL_FONT_ID};

  for (const int fontId : fontIds) {
    if (renderer.getTextWidth(fontId, title.c_str()) <= maxWidth) {
      const int lineHeight = renderer.getLineHeight(fontId);
      drawCenteredLine(renderer, body, fontId, blockY + (blockHeight - lineHeight) / 2, title);
      return;
    }
  }
  for (const int fontId : fontIds) {
    std::string line1, line2;
    if (!wrapTitleTwoLines(renderer, fontId, title, maxWidth, line1, line2)) continue;
    const int lineHeight = renderer.getLineHeight(fontId);
    const int pairHeight = 2 * lineHeight + TITLE_LINE_GAP;
    const int firstY = blockY + std::max(0, (blockHeight - pairHeight) / 2);
    drawCenteredLine(renderer, body, fontId, firstY, line1);
    drawCenteredLine(renderer, body, fontId, firstY + lineHeight + TITLE_LINE_GAP, line2);
    return;
  }
  // Nothing fit even wrapped (an unusually long single word, most likely) --
  // one truncated line at the smallest size, vertically centered same as
  // the single-line case above.
  const int fontId = fontIds[1];
  std::string truncated = title;
  while (!truncated.empty() && renderer.getTextWidth(fontId, (truncated + "...").c_str()) > maxWidth) {
    truncated.pop_back();
  }
  truncated += "...";
  const int lineHeight = renderer.getLineHeight(fontId);
  drawCenteredLine(renderer, body, fontId, blockY + (blockHeight - lineHeight) / 2, truncated);
}

// ---- Continue tab: geometric wallpaper motif -------------------------------
//
// Pure vector shapes (drawLine/drawArc/fillPolygon) rather than a stored
// image: this renderer's drawImage() doesn't rotate a bitmap's actual pixel
// data for different screen orientations, only its origin point, and is
// hard-capped at the panel's native row count -- a baked-in picture only
// ever looks right in the one orientation it was captured for, and would be
// silently misplaced/clipped in the other three. These primitives already
// go through the same per-pixel orientation transform everything else on
// this screen uses, so the motif is correct in all 4 orientations for free,
// and costs zero flash (no image data, just coordinates).
//
// A dense, all-over scatter of small shapes (filled/outline dots, diamonds,
// triangles, crosses, arcs, dashes, zigzags, squiggles) laid out on a grid
// whose rows step sideways from one another, so it reads as a tiled
// wallpaper print rather than a single line of characters. Each band uses a
// fixed seed, so the pattern is the same on every redraw instead of
// reshuffling.
namespace geo_pattern {
constexpr int MIN_BAND_HEIGHT = 20;  // Skip a band entirely below this -- too little room for even one row.
constexpr int CELL = 26;             // Grid spacing, in pixels, between shape centers.
constexpr int SKIP_PERCENT = 18;     // Chance (%) a grid cell is left empty, for breathing room.

// Small deterministic PRNG (xorshift32) -- no <random>, no heap, and the same
// sequence every render so the wallpaper doesn't visibly shuffle each frame.
uint32_t nextRand(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

// Returns an integer in [0, boundExclusive).
int randBelow(uint32_t& state, int boundExclusive) {
  return static_cast<int>(nextRand(state) % static_cast<uint32_t>(boundExclusive));
}

enum ShapeKind {
  ShapeDotFilled,
  ShapeRing,
  ShapeDiamondFilled,
  ShapeDiamondOutline,
  ShapeTriangleFilled,
  ShapeTriangleOutline,
  ShapePlus,
  ShapeArc,
  ShapeDash,
  ShapeDiagDash,
  ShapeZigzag,
  ShapeSquiggle,
  ShapeDotsPair,
  SHAPE_COUNT
};

// Four quadrant fills sharing one center -- a thin ring when stroke < r, a
// filled disc when stroke >= r (drawArc clamps the inner radius to 0).
void drawDisc(const GfxRenderer& renderer, int cx, int cy, int r, int stroke) {
  renderer.drawArc(r, cx, cy, -1, -1, stroke, true);
  renderer.drawArc(r, cx, cy, 1, -1, stroke, true);
  renderer.drawArc(r, cx, cy, 1, 1, stroke, true);
  renderer.drawArc(r, cx, cy, -1, 1, stroke, true);
}

void drawDiamond(const GfxRenderer& renderer, int cx, int cy, int r, bool filled, int stroke) {
  const int xs[4] = {cx, cx + r, cx, cx - r};
  const int ys[4] = {cy - r, cy, cy + r, cy};
  if (filled) {
    renderer.fillPolygon(xs, ys, 4, true);
    return;
  }
  for (int i = 0; i < 4; i++) {
    const int j = (i + 1) % 4;
    renderer.drawLine(xs[i], ys[i], xs[j], ys[j], stroke, true);
  }
}

void drawTriangle(const GfxRenderer& renderer, int cx, int cy, int r, bool filled, bool up, int stroke) {
  int xs[3];
  int ys[3];
  if (up) {
    xs[0] = cx;
    ys[0] = cy - r;
    xs[1] = cx - r;
    ys[1] = cy + r;
    xs[2] = cx + r;
    ys[2] = cy + r;
  } else {
    xs[0] = cx;
    ys[0] = cy + r;
    xs[1] = cx - r;
    ys[1] = cy - r;
    xs[2] = cx + r;
    ys[2] = cy - r;
  }
  if (filled) {
    renderer.fillPolygon(xs, ys, 3, true);
    return;
  }
  for (int i = 0; i < 3; i++) {
    const int j = (i + 1) % 3;
    renderer.drawLine(xs[i], ys[i], xs[j], ys[j], stroke, true);
  }
}

void drawPlus(const GfxRenderer& renderer, int cx, int cy, int r, int stroke) {
  renderer.drawLine(cx - r, cy, cx + r, cy, stroke, true);
  renderer.drawLine(cx, cy - r, cx, cy + r, stroke, true);
}

void drawDash(const GfxRenderer& renderer, int cx, int cy, int halfLen, int stroke) {
  renderer.drawLine(cx - halfLen, cy, cx + halfLen, cy, stroke, true);
}

void drawDiagDash(const GfxRenderer& renderer, int cx, int cy, int r, int stroke) {
  renderer.drawLine(cx - r, cy - r, cx + r, cy + r, stroke, true);
}

// A wide, angular "W" -- three sharp segments.
void drawZigzag(const GfxRenderer& renderer, int cx, int cy, int halfWidth, int halfHeight, int stroke) {
  const int xs[4] = {cx - halfWidth, cx - halfWidth / 3, cx + halfWidth / 3, cx + halfWidth};
  const int ys[4] = {cy - halfHeight, cy + halfHeight, cy - halfHeight, cy + halfHeight};
  for (int i = 0; i < 3; i++) {
    renderer.drawLine(xs[i], ys[i], xs[i + 1], ys[i + 1], stroke, true);
  }
}

// A softer, lower-amplitude wave than the zigzag -- five short segments.
void drawSquiggle(const GfxRenderer& renderer, int cx, int cy, int halfWidth, int amp, int stroke) {
  constexpr int N = 5;
  int prevX = cx - halfWidth;
  int prevY = cy;
  for (int i = 1; i <= N; i++) {
    const int x = cx - halfWidth + (2 * halfWidth * i) / N;
    const int y = cy + ((i % 2 == 0) ? amp : -amp);
    renderer.drawLine(prevX, prevY, x, y, stroke, true);
    prevX = x;
    prevY = y;
  }
}

void drawDotsPair(const GfxRenderer& renderer, int cx, int cy, int gap) {
  renderer.fillRect(cx - gap / 2 - 1, cy - 1, 2, 2, true);
  renderer.fillRect(cx + gap / 2 - 1, cy - 1, 2, 2, true);
}

void drawShape(const GfxRenderer& renderer, ShapeKind shape, int cx, int cy, uint32_t& rng) {
  // Weighted toward thin/medium strokes, with an occasional bold one -- a mix
  // of line weights rather than everything drawn at the same thickness.
  const int stroke = (randBelow(rng, 5) >= 3) ? 2 : 1;
  switch (shape) {
    case ShapeDotFilled:
      drawDisc(renderer, cx, cy, 3 + randBelow(rng, 2), 6);
      break;
    case ShapeRing:
      drawDisc(renderer, cx, cy, 4 + randBelow(rng, 2), stroke);
      break;
    case ShapeDiamondFilled:
      drawDiamond(renderer, cx, cy, 4 + randBelow(rng, 2), true, stroke);
      break;
    case ShapeDiamondOutline:
      drawDiamond(renderer, cx, cy, 5 + randBelow(rng, 2), false, stroke);
      break;
    case ShapeTriangleFilled:
      drawTriangle(renderer, cx, cy, 4 + randBelow(rng, 2), true, randBelow(rng, 2) == 0, stroke);
      break;
    case ShapeTriangleOutline:
      drawTriangle(renderer, cx, cy, 5 + randBelow(rng, 2), false, randBelow(rng, 2) == 0, stroke);
      break;
    case ShapePlus:
      drawPlus(renderer, cx, cy, 4 + randBelow(rng, 2), stroke);
      break;
    case ShapeArc:
      renderer.drawArc(5 + randBelow(rng, 2), cx, cy, randBelow(rng, 2) == 0 ? -1 : 1,
                       randBelow(rng, 2) == 0 ? -1 : 1, stroke, true);
      break;
    case ShapeDash:
      drawDash(renderer, cx, cy, 6, std::max(stroke, 2));
      break;
    case ShapeDiagDash:
      drawDiagDash(renderer, cx, cy, 4 + randBelow(rng, 2), stroke);
      break;
    case ShapeZigzag:
      drawZigzag(renderer, cx, cy, 8, 5, stroke);
      break;
    case ShapeSquiggle:
      drawSquiggle(renderer, cx, cy, 8, 4, stroke);
      break;
    case ShapeDotsPair:
      drawDotsPair(renderer, cx, cy, 6);
      break;
    default:
      break;
  }
}

// Fills [bandTop, bandTop + bandHeight) across the full width of `body` with
// a dense scatter of small shapes on a grid whose rows step sideways from
// one another (a third of a cell per row), so it tiles like a printed
// pattern instead of reading as one line of characters. `seed` keeps the
// band looking the same on every redraw rather than reshuffling.
void drawWallpaperBand(const GfxRenderer& renderer, const Rect body, const int bandTop, const int bandHeight,
                       uint32_t seed) {
  uint32_t rng = seed;
  const int rows = std::max(1, bandHeight / CELL);
  int lastShape = -1;
  for (int row = 0; row < rows; row++) {
    const int cy = bandTop + row * CELL + CELL / 2;
    const int rowOffset = (row * (CELL / 3)) % CELL;
    const int startX = body.x - CELL + rowOffset;
    for (int cx = startX; cx <= body.x + body.width + CELL; cx += CELL) {
      if (randBelow(rng, 100) < SKIP_PERCENT) continue;
      int shape = randBelow(rng, SHAPE_COUNT);
      if (shape == lastShape) {
        shape = (shape + 1) % SHAPE_COUNT;
      }
      lastShape = shape;
      drawShape(renderer, static_cast<ShapeKind>(shape), cx, cy, rng);
    }
  }
}
}  // namespace geo_pattern
}  // namespace

void AppShellActivity::onEnter() {
  Activity::onEnter();
  loadRecentBooks();
  carouselIndex = 0;
  requestUpdate();
}

void AppShellActivity::loadRecentBooks() {
  recentBooks.clear();
  for (const RecentBook& book : RECENT_BOOKS.getBooks()) {
    if (RecentBooksStore::isMissing(book)) continue;
    recentBooks.push_back(book);
    if (recentBooks.size() >= MAX_CAROUSEL_BOOKS) break;
  }
}

size_t AppShellActivity::nextCarouselIndex() const {
  if (recentBooks.empty()) return 0;
  return (carouselIndex + 1) % recentBooks.size();
}

size_t AppShellActivity::previousCarouselIndex() const {
  if (recentBooks.empty()) return 0;
  return (carouselIndex + recentBooks.size() - 1) % recentBooks.size();
}

void AppShellActivity::setCarouselIndex(const size_t index) {
  if (index == carouselIndex) return;
  carouselIndex = index;
  requestUpdate();
}

void AppShellActivity::switchTab(const Tab tab) {
  if (tab == activeTab) return;
  activeTab = tab;
  if (tab != Tab::Continue) {
    // Books and Settings are entirely offline screens -- switching to them
    // must never bring up WiFi on its own just to get a clock/weather
    // reading. That only ever happens piggybacked on a connection something
    // else already needed (opening/closing a book, Book Server browsing) or
    // an explicit Settings > Weather Location action -- see
    // HomeStatusService.h's header comment.
    //
    // Live tap while already on the shell: jump straight into the real
    // screen, no intermediate panel. (Re-entering the shell with one of
    // these tabs preselected -- e.g. after backing out of Books -- does NOT
    // go through here, so it still shows the panel; forwarding on THAT path
    // too would make Back bounce you straight back into the screen you just
    // left. See the header comment.)
    openActiveTabTarget();
    return;
  }
  requestUpdate();
}

void AppShellActivity::openActiveTabTarget() {
  switch (activeTab) {
    case Tab::Books:
      activityManager.goToFileBrowser();
      break;
    case Tab::BookServer:
      activityManager.goToBrowser();
      break;
    case Tab::Settings:
      activityManager.goToSettings();
      break;
    case Tab::Continue:
      break;  // Handled inline in loop(): opens the reader, not another tab.
  }
}

void AppShellActivity::loop() {
  // Cheap on almost every call -- see HomeStatusService's own comment for
  // when it actually does anything. Runs on every tab, not just Continue:
  // the clock+weather readout is drawn in the shared header, visible on all
  // four tabs, so it needs refreshing regardless of which one is active.
  HomeStatusService::tick();

  // Ticks the visible clock forward once a minute on its own -- purely a
  // local time check (see HomeStatusService's comment), never WiFi -- so it
  // stays accurate to the minute while sitting idle on the shell, not just
  // whenever some other event happens to redraw the header anyway.
  if (HomeStatusService::clockMinuteChanged()) {
    requestUpdate();
  }

  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const int pageHeight = renderer.getScreenHeight();
    if (ty >= pageHeight - TAB_BAR_HEIGHT) {
      const int segmentWidth = renderer.getScreenWidth() / TAB_COUNT;
      const int index = std::min(TAB_COUNT - 1, std::max(0, tx / std::max(1, segmentWidth)));
      switchTab(static_cast<Tab>(index));
      return;
    }

    if (activeTab == Tab::Continue) {
      if (recentBooks.empty()) return;
      const int pageWidth = renderer.getScreenWidth();
      constexpr int peekFraction = 5;  // Peek strips are 1/5 of the width on each side.
      const int peekWidth = pageWidth / peekFraction;
      if (tx < peekWidth) {
        setCarouselIndex(previousCarouselIndex());
      } else if (tx >= pageWidth - peekWidth) {
        setCarouselIndex(nextCarouselIndex());
      } else {
        // Tapping the centered cover (or its title) opens it -- no separate
        // "Continue Reading" button.
        activityManager.goToReader(recentBooks[carouselIndex].path);
      }
    } else {
      openActiveTabTarget();
    }
    return;
  }

  // Physical page-turn buttons and a short Power press: reserved for the
  // Continue carousel, same scope as swipe (see the header comment) -- no-op
  // on the other tabs, which have no content of their own to page through.
  if (activeTab == Tab::Continue && !recentBooks.empty()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
      setCarouselIndex(nextCarouselIndex());
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
      setCarouselIndex(previousCarouselIndex());
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Power)) {
      // Same action as tapping the centered cover: open it. A short Power
      // press doesn't collide with sleep on this hardware -- sleep needs a
      // HELD press past a duration threshold, checked elsewhere -- so this
      // is safe to claim outright here, the same way the reader already
      // gives a short Power press its own meaning in PAGE_TURN mode.
      activityManager.goToReader(recentBooks[carouselIndex].path);
      return;
    }
  }

  // Cheap the overwhelming majority of the time (a single WiFi.status()
  // check) -- see ContinueMetadataEnricher's own comment for what actually
  // triggers a network request and why this is safe to call every frame.
  if (activeTab == Tab::Continue && !recentBooks.empty()) {
    ContinueMetadataEnricher::tryEnrichIfOnline(recentBooks[carouselIndex]);
  }

  // Swipe is reserved for the Continue carousel -- it does not switch tabs.
  if (activeTab != Tab::Continue || recentBooks.size() < 2) return;
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left) {
    setCarouselIndex(nextCarouselIndex());
  } else if (swipe == MappedInputManager::SwipeDir::Right) {
    setCarouselIndex(previousCarouselIndex());
  }
}

void AppShellActivity::renderTabBar(const Rect rect) {
  renderer.drawLine(rect.x, rect.y, rect.x + rect.width, rect.y, 2, true);

  const int segmentWidth = rect.width / TAB_COUNT;
  constexpr int fontId = SMALL_FONT_ID;
  constexpr int pillMargin = 4;
  const int lineHeight = renderer.getLineHeight(fontId);

  for (int i = 0; i < TAB_COUNT; i++) {
    const auto tab = static_cast<Tab>(i);
    const int segmentX = rect.x + i * segmentWidth;
    const bool active = tab == activeTab;
    const char* label = tabLabel(tab);
    const int textWidth = renderer.getTextWidth(fontId, label);
    const int textY = rect.y + (rect.height - lineHeight) / 2;
    const int textX = segmentX + std::max(0, (segmentWidth - textWidth) / 2);

    if (active) {
      renderer.fillRect(segmentX + pillMargin, rect.y + pillMargin, segmentWidth - 2 * pillMargin,
                        rect.height - 2 * pillMargin, true);
      renderer.drawText(fontId, textX, textY, label, /*black=*/false);
    } else {
      renderer.drawText(fontId, textX, textY, label, /*black=*/true);
    }
  }
}

void AppShellActivity::renderLaunchPanelBody(const Rect body, const char* label) {
  constexpr int labelFontId = NOTOSANS_18_FONT_ID;
  constexpr int hintFontId = SMALL_FONT_ID;
  const int labelWidth = renderer.getTextWidth(labelFontId, label);
  const int labelHeight = renderer.getLineHeight(labelFontId);
  const char* hint = "Tap to open";
  const int hintWidth = renderer.getTextWidth(hintFontId, hint);
  const int hintHeight = renderer.getLineHeight(hintFontId);
  constexpr int gap = 12;
  const int blockHeight = labelHeight + gap + hintHeight;
  const int blockY = body.y + std::max(0, (body.height - blockHeight) / 2);

  renderer.drawText(labelFontId, body.x + std::max(0, (body.width - labelWidth) / 2), blockY, label);
  renderer.drawText(hintFontId, body.x + std::max(0, (body.width - hintWidth) / 2), blockY + labelHeight + gap, hint);
}

void AppShellActivity::ensureCoverThumb(const RecentBook& book, const int height) const {
  if (book.coverBmpPath.empty()) return;
  const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, height);
  if (Storage.exists(coverPath.c_str())) return;

  // First time the carousel has shown this book at this size -- generate it,
  // the same way HomeActivity primes its single card. HomeActivity shows a
  // popup for this; skipped here to keep this a plain, silent helper. It
  // only costs anything the first time a given book is scrolled to.
  if (FsHelpers::hasEpubExtension(book.path)) {
    Epub epub(book.path, "/.crosspoint");
    epub.load(false, true);  // Metadata only -- no CSS needed just to grab the cover.
    epub.generateThumbBmp(height);
  } else if (FsHelpers::hasXtcExtension(book.path)) {
    Xtc xtc(book.path, "/.crosspoint");
    if (xtc.load()) {
      xtc.generateThumbBmp(height);
    }
  }
}

namespace {
// Shared by measureCoverSize() and renderCoverBox() -- aspect-fit dimensions
// for `bitmap` within a heightCap x widthCap box, never upscaled beyond the
// bitmap's own native size (this renderer only ever shrinks -- confirmed by
// reading drawBitmap's source; a target bound above the bitmap's native size
// is simply not applied).
void fitCoverDims(const int nativeWidth, const int nativeHeight, const int heightCap, const int widthCap, int& outW,
                  int& outH) {
  const float aspect = static_cast<float>(nativeWidth) / static_cast<float>(nativeHeight);
  outH = std::min(nativeHeight, heightCap);
  outW = static_cast<int>(outH * aspect);
  if (outW > widthCap) {
    outW = widthCap;
    outH = static_cast<int>(outW / aspect);
  }
}
}  // namespace

void AppShellActivity::measureCoverSize(const RecentBook& book, const int heightCap, int& width,
                                        int& height) const {
  width = 0;
  height = 0;
  if (book.coverBmpPath.empty()) return;
  const int genHeight = UITheme::getInstance().getMetrics().homeCoverHeight;
  ensureCoverThumb(book, genHeight);
  const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, genHeight);
  HalFile file;
  if (!Storage.openFileForRead("SHELL", coverPath, file)) return;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0) return;
  fitCoverDims(bitmap.getWidth(), bitmap.getHeight(), heightCap, heightCap * 2, width, height);
}

void AppShellActivity::renderCoverBox(const int x, const int y, const int boxWidth, const int boxHeight,
                                      const RecentBook& book) const {
  // x may be negative, or x + drawn width may exceed the screen width, on
  // purpose -- the two peeking covers are meant to be half cropped by the
  // screen edge. Safe: the renderer clips every pixel outside the visible
  // screen rather than writing it (checked in both drawBitmap's general path
  // and its 1-bit fast path).
  bool drew = false;
  if (!book.coverBmpPath.empty()) {
    const int genHeight = UITheme::getInstance().getMetrics().homeCoverHeight;
    ensureCoverThumb(book, genHeight);
    const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, genHeight);
    HalFile file;
    if (Storage.openFileForRead("SHELL", coverPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
        int width, height;
        fitCoverDims(bitmap.getWidth(), bitmap.getHeight(), boxHeight, boxWidth, width, height);
        // Letterbox within the shared box if this book's own aspect ratio
        // differs from whichever book the box size was measured from, so
        // every cover in the carousel occupies an identically sized box
        // without ever stretching an image out of its own proportions.
        const int drawX = x + (boxWidth - width) / 2;
        const int drawY = y + (boxHeight - height) / 2;
        renderer.drawBitmap(bitmap, drawX, drawY, width, height);
        renderer.drawRect(drawX, drawY, width, height);
        drew = true;
      }
    }
  }
  if (!drew) {
    // No cover art: bordered placeholder at a plausible cover proportion
    // (2:3), centered in the same box every real cover would occupy.
    int height = std::min(boxHeight, static_cast<int>(boxWidth * 1.5f));
    int width = static_cast<int>(height * 2.0f / 3.0f);
    if (width > boxWidth) {
      width = boxWidth;
      height = static_cast<int>(width * 1.5f);
    }
    renderer.drawRect(x + (boxWidth - width) / 2, y + (boxHeight - height) / 2, width, height);
  }
}

void AppShellActivity::renderContinueArt(const Rect body, const int topGapHeight, const int bottomGapHeight) const {
  // Two fixed, distinct seeds -- arbitrary nonzero 32-bit constants -- so the
  // top and bottom bands don't draw as mirror images of each other.
  if (topGapHeight >= geo_pattern::MIN_BAND_HEIGHT) {
    geo_pattern::drawWallpaperBand(renderer, body, body.y, topGapHeight, 0x9E3779B9u);
  }
  if (bottomGapHeight >= geo_pattern::MIN_BAND_HEIGHT) {
    geo_pattern::drawWallpaperBand(renderer, body, body.y + body.height - bottomGapHeight, bottomGapHeight,
                                   0x85EBCA6Bu);
  }
}

void AppShellActivity::renderContinueBody(const Rect body) {
  if (recentBooks.empty()) {
    constexpr int fontId = NOTOSANS_16_FONT_ID;
    const char* message = "No recent book yet -- open Books to pick one";
    const int width = renderer.getTextWidth(fontId, message);
    renderer.drawText(fontId, body.x + std::max(0, (body.width - width) / 2), body.y + body.height / 2, message);
    return;
  }

  constexpr int titleGap = 16;
  // Always reserve room for two lines, even for titles that end up using
  // one -- keeps the cover from shifting vertically as you swipe between
  // books with short and long titles. No author line below it -- a
  // right-sized title is sufficient, per how this was asked to look.
  const int titleBlockHeight = 2 * renderer.getLineHeight(TITLE_MAX_FONT_ID) + TITLE_LINE_GAP;

  // One shared box size for all three covers -- peeks included -- measured
  // from the centered book alone and capped so Continue never towers over
  // the rest of the screen. "Identical size" per the reference: every cover
  // in the carousel occupies this exact box, never a bigger one for the
  // center and smaller ones for its neighbors.
  const int heightCap = static_cast<int>(
      std::min(UITheme::getInstance().getMetrics().homeCoverHeight,
              static_cast<int>((body.height - titleGap - titleBlockHeight - titleGap) * 0.75f)) *
      1.56f);
  int coverWidth = 0;
  int coverHeight = 0;
  measureCoverSize(recentBooks[carouselIndex], heightCap, coverWidth, coverHeight);
  if (coverWidth <= 0 || coverHeight <= 0) {
    // Centered book has no cover art at all -- fall back to a plausible
    // cover proportion (2:3) so peeks still have a real, non-zero box size.
    coverHeight = heightCap;
    coverWidth = coverHeight * 2 / 3;
  }

  const bool hasNeighbors = recentBooks.size() > 1;
  const int blockHeight = coverHeight + titleGap + titleBlockHeight;
  const int blockY = body.y + std::max(0, (body.height - blockHeight) / 2);

  // Whatever room is left above and below the carousel/title block once its
  // own height is known -- the geometric wallpaper is confined to exactly
  // this space, so it can never overlap the covers or the title on any
  // screen size or orientation. A cramped screen just gets less (or no) art
  // rather than something squeezed and illegible -- see MIN_BAND_HEIGHT.
  renderContinueArt(body, blockY - body.y, (body.y + body.height) - (blockY + blockHeight));

  const int centerX = body.x + (body.width - coverWidth) / 2;

  if (hasNeighbors) {
    // 3/4 of each neighbor visible now (was ~0.42, "half") -- flush against
    // the screen edge, the rest safely clipped off-canvas. Tighter spacing
    // to the center cover falls out of this automatically: more of each
    // peek showing leaves less empty gap between it and the center.
    constexpr float visibleFraction = 0.75f;
    const int visibleWidth = std::max(1, static_cast<int>(coverWidth * visibleFraction));
    const int leftX = body.x - (coverWidth - visibleWidth);
    const int rightX = body.x + body.width - visibleWidth;
    renderCoverBox(leftX, blockY, coverWidth, coverHeight, recentBooks[previousCarouselIndex()]);
    renderCoverBox(rightX, blockY, coverWidth, coverHeight, recentBooks[nextCarouselIndex()]);
  }

  renderCoverBox(centerX, blockY, coverWidth, coverHeight, recentBooks[carouselIndex]);

  const int titleBlockY = blockY + coverHeight + titleGap;
  drawFittedTitle(renderer, body, titleBlockY, titleBlockHeight, recentBooks[carouselIndex].title, body.width - 48);
}

void AppShellActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  // No title: this is the one header on a screen that isn't a themed list,
  // so it should read as chrome (clock/battery/sync mark), not a heading.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, nullptr);

  const Rect tabBarRect{0, pageHeight - TAB_BAR_HEIGHT, pageWidth, TAB_BAR_HEIGHT};
  const int bodyTop = metrics.topPadding + metrics.headerHeight;
  const Rect bodyRect{0, bodyTop, pageWidth, std::max(0, tabBarRect.y - bodyTop)};

  switch (activeTab) {
    case Tab::Continue:
      renderContinueBody(bodyRect);
      break;
    case Tab::Books:
      renderLaunchPanelBody(bodyRect, "Books");
      break;
    case Tab::BookServer:
      renderLaunchPanelBody(bodyRect, "Book Server");
      break;
    case Tab::Settings:
      renderLaunchPanelBody(bodyRect, "Settings");
      break;
  }

  renderTabBar(tabBarRect);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

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

// ---- Continue tab: wooden bookshelf layout constants -----------------------
//
// The Continue tab shows up to MAX_SHELF_BOOKS recent books standing on a
// small number of shelves, most recent first, left-to-right then top-to-
// bottom -- an on-device Audiobookshelf-style library grid rather than the
// old single-book carousel. No title text is drawn (it would clash with the
// shelf boards), and every book gets an identically sized "slot" regardless
// of its own cover's proportions (see renderCoverBox's aspect-fit).
constexpr int SHELF_SIDE_MARGIN = 20;       // Left/right inset for the whole shelf unit.
constexpr int SHELF_TOP_MARGIN = 14;        // Gap between the header and the first row of covers.
constexpr int SHELF_BOTTOM_MARGIN = 10;     // Gap between the last shelf board and the tab bar.
constexpr int SHELF_COLUMN_GAP = 14;        // Horizontal gap between two books on the same shelf.
constexpr int SHELF_ROW_GAP = 16;           // Gap between a book's bottom edge and the board it stands on.
constexpr int SHELF_BOARD_THICKNESS = 12;   // The shelf board itself.
constexpr int SHELF_BOARD_SHADOW_GAP = 3;   // Thin white reveal between the board and its shadow line.
constexpr int SHELF_BOARD_SHADOW_THICKNESS = 3;  // A second, thinner line under each board for depth.
constexpr int SHELF_MIN_CELL_WIDTH = 84;    // Never pack columns so tight a cover becomes illegible.
constexpr int SHELF_MAX_COLUMNS = 5;        // Widest this ever tries, even on a very wide screen.
constexpr int SHELF_SELECTION_PADDING = 7;  // Gap between a cover's slot and its selection border.
constexpr int SHELF_SELECTION_THICKNESS = 5;  // Deliberately thick and obvious, per how this was asked to look.
constexpr int SHELF_WALL_SEAM_COUNT = 4;    // Purely decorative vertical plank seams behind the shelves.

// Where every book on the Continue shelf ends up: a grid of identically
// sized slots, computed once from the body area and the book count, then
// shared by both rendering (renderContinueBody) and tap hit-testing (loop())
// so the two can never disagree about where a given book actually is.
struct ShelfLayout {
  int columns = 1;
  int rows = 1;
  int cellWidth = 0;
  int coverAreaHeight = 0;  // Height of the slot a cover renders into -- not counting its shelf board.
  int originX = 0;          // Left edge of column 0.
  int originY = 0;          // Top edge of row 0's cover slots.
  int columnStride = 0;     // Horizontal distance from one column's left edge to the next.
  int rowStride = 0;        // Vertical distance from one row's slot top to the next.
};

// Picks the widest column count (up to SHELF_MAX_COLUMNS) whose resulting
// cell width still clears SHELF_MIN_CELL_WIDTH, then never uses more columns
// than there are books -- so three books sit on one shelf as three wide
// slots rather than being stretched across (or crammed into) a wider grid
// meant for ten. Orientation-safe: driven entirely by body.width/height, no
// hardcoded panel dimensions.
ShelfLayout computeShelfLayout(const Rect& body, const size_t bookCount) {
  ShelfLayout layout;
  if (bookCount == 0) return layout;

  int columns = SHELF_MAX_COLUMNS;
  while (columns > 1) {
    const int candidateWidth = (body.width - 2 * SHELF_SIDE_MARGIN - (columns - 1) * SHELF_COLUMN_GAP) / columns;
    if (candidateWidth >= SHELF_MIN_CELL_WIDTH) break;
    columns--;
  }
  columns = std::min(columns, static_cast<int>(bookCount));
  const int rows = (static_cast<int>(bookCount) + columns - 1) / columns;

  const int cellWidth = (body.width - 2 * SHELF_SIDE_MARGIN - (columns - 1) * SHELF_COLUMN_GAP) / columns;
  const int rowBlockHeight = (body.height - SHELF_TOP_MARGIN - SHELF_BOTTOM_MARGIN) / rows;
  const int coverAreaHeight = std::max(
      1, rowBlockHeight - SHELF_ROW_GAP - SHELF_BOARD_THICKNESS - SHELF_BOARD_SHADOW_GAP - SHELF_BOARD_SHADOW_THICKNESS);

  layout.columns = columns;
  layout.rows = rows;
  layout.cellWidth = cellWidth;
  layout.coverAreaHeight = coverAreaHeight;
  layout.originX = body.x + SHELF_SIDE_MARGIN;
  layout.originY = body.y + SHELF_TOP_MARGIN;
  layout.columnStride = cellWidth + SHELF_COLUMN_GAP;
  layout.rowStride = rowBlockHeight;
  return layout;
}

// The cover slot (not including its shelf board) for the book at `index`,
// laid out left-to-right then top-to-bottom -- so index 0 is the most recent
// book, top-left, and incrementing index moves right, wrapping onto the next
// shelf down exactly the way Page Forward/Down is asked to behave.
Rect cellRectForIndex(const ShelfLayout& layout, const size_t index) {
  const int row = static_cast<int>(index) / layout.columns;
  const int col = static_cast<int>(index) % layout.columns;
  return Rect{layout.originX + col * layout.columnStride, layout.originY + row * layout.rowStride, layout.cellWidth,
             layout.coverAreaHeight};
}
}  // namespace

void AppShellActivity::onEnter() {
  Activity::onEnter();
  loadRecentBooks();
  selectedIndex = 0;
  requestUpdate();
}

void AppShellActivity::loadRecentBooks() {
  recentBooks.clear();
  for (const RecentBook& book : RECENT_BOOKS.getBooks()) {
    if (RecentBooksStore::isMissing(book)) continue;
    recentBooks.push_back(book);
    if (recentBooks.size() >= MAX_SHELF_BOOKS) break;
  }
}

size_t AppShellActivity::nextShelfIndex() const {
  if (recentBooks.empty()) return 0;
  return (selectedIndex + 1) % recentBooks.size();
}

size_t AppShellActivity::previousShelfIndex() const {
  if (recentBooks.empty()) return 0;
  return (selectedIndex + recentBooks.size() - 1) % recentBooks.size();
}

void AppShellActivity::setSelectedIndex(const size_t index) {
  if (index == selectedIndex) return;
  selectedIndex = index;
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

Rect AppShellActivity::computeBodyRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int bodyTop = metrics.topPadding + metrics.headerHeight;
  const int tabBarTop = pageHeight - TAB_BAR_HEIGHT;
  return Rect{0, bodyTop, pageWidth, std::max(0, tabBarTop - bodyTop)};
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
      // Tapping a cover selects and opens it directly -- there's no
      // separate "Continue Reading" button, and no peek zones to page
      // through anymore now that the whole shelf is visible at once.
      // Tapping the wood between/around covers is a no-op.
      const Rect body = computeBodyRect();
      const ShelfLayout layout = computeShelfLayout(body, recentBooks.size());
      for (size_t i = 0; i < recentBooks.size(); i++) {
        const Rect cell = cellRectForIndex(layout, i);
        if (tx >= cell.x && tx < cell.x + cell.width && ty >= cell.y && ty < cell.y + cell.height) {
          setSelectedIndex(i);
          activityManager.goToReader(recentBooks[i].path);
          return;
        }
      }
    } else {
      openActiveTabTarget();
    }
    return;
  }

  // Physical page-turn buttons and a short Power press: reserved for moving
  // the Continue shelf's selection, same scope as swipe (see the header
  // comment) -- no-op on the other tabs, which have no content of their own
  // to page through. Forward/Down moves right, wrapping onto the next shelf
  // down; Back/Up moves left, wrapping onto the previous shelf.
  if (activeTab == Tab::Continue && !recentBooks.empty()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
      setSelectedIndex(nextShelfIndex());
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
      setSelectedIndex(previousShelfIndex());
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Power)) {
      // Same action as tapping the selected cover: open it. A short Power
      // press doesn't collide with sleep on this hardware -- sleep needs a
      // HELD press past a duration threshold, checked elsewhere -- so this
      // is safe to claim outright here, the same way the reader already
      // gives a short Power press its own meaning in PAGE_TURN mode.
      activityManager.goToReader(recentBooks[selectedIndex].path);
      return;
    }
  }

  // Cheap the overwhelming majority of the time (a single WiFi.status()
  // check) -- see ContinueMetadataEnricher's own comment for what actually
  // triggers a network request and why this is safe to call every frame.
  if (activeTab == Tab::Continue && !recentBooks.empty()) {
    ContinueMetadataEnricher::tryEnrichIfOnline(recentBooks[selectedIndex]);
  }

  // Swipe moves the shelf selection the same way Page Forward/Back do -- it
  // does not switch tabs.
  if (activeTab != Tab::Continue || recentBooks.size() < 2) return;
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left) {
    setSelectedIndex(nextShelfIndex());
  } else if (swipe == MappedInputManager::SwipeDir::Right) {
    setSelectedIndex(previousShelfIndex());
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

  // First time the shelf has shown this book at this size -- generate it,
  // the same way HomeActivity primes its single card. HomeActivity shows a
  // popup for this; skipped here to keep this a plain, silent helper. It
  // only costs anything the first time a given book is shown at a given
  // height.
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
// Shared by renderCoverBox() -- aspect-fit dimensions for `bitmap` within a
// heightCap x widthCap box, never upscaled beyond the bitmap's own native
// size (this renderer only ever shrinks -- confirmed by reading drawBitmap's
// source; a target bound above the bitmap's native size is simply not
// applied).
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

void AppShellActivity::renderCoverBox(const int x, const int y, const int boxWidth, const int boxHeight,
                                      const RecentBook& book) const {
  bool drew = false;
  if (!book.coverBmpPath.empty()) {
    // Generate/cache the thumb at exactly this shelf slot's own height,
    // rather than some fixed theme default -- the slot size varies with how
    // many books are on the shelf and the screen's own size, and generating
    // at a smaller fixed default then only ever shrinking further (never
    // upscaling -- see fitCoverDims above) would leave a cover visibly
    // smaller than the slot it's standing in.
    ensureCoverThumb(book, boxHeight);
    const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, boxHeight);
    HalFile file;
    if (Storage.openFileForRead("SHELL", coverPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
        int width, height;
        fitCoverDims(bitmap.getWidth(), bitmap.getHeight(), boxHeight, boxWidth, width, height);
        // Letterbox within the shared slot if this book's own aspect ratio
        // differs from the slot's, so every cover on a shelf occupies an
        // identically sized slot without ever stretching an image out of
        // its own proportions.
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
    // (2:3), centered in the same slot every real cover would occupy.
    int height = std::min(boxHeight, static_cast<int>(boxWidth * 1.5f));
    int width = static_cast<int>(height * 2.0f / 3.0f);
    if (width > boxWidth) {
      width = boxWidth;
      height = static_cast<int>(width * 1.5f);
    }
    renderer.drawRect(x + (boxWidth - width) / 2, y + (boxHeight - height) / 2, width, height);
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

  const ShelfLayout layout = computeShelfLayout(body, recentBooks.size());

  // Purely decorative vertical plank seams behind the whole shelf unit,
  // drawn first so shelf boards and cover art layer cleanly on top of them.
  const int shelfBottom = layout.originY + (layout.rows - 1) * layout.rowStride + layout.coverAreaHeight +
                          SHELF_ROW_GAP + SHELF_BOARD_THICKNESS + SHELF_BOARD_SHADOW_GAP + SHELF_BOARD_SHADOW_THICKNESS;
  for (int i = 1; i <= SHELF_WALL_SEAM_COUNT; i++) {
    const int seamX = body.x + (body.width * i) / (SHELF_WALL_SEAM_COUNT + 1);
    renderer.drawLine(seamX, body.y + 2, seamX, shelfBottom, true);
  }

  for (size_t i = 0; i < recentBooks.size(); i++) {
    const Rect cell = cellRectForIndex(layout, i);
    renderCoverBox(cell.x, cell.y, cell.width, cell.height, recentBooks[i]);

    // The board this row of books stands on, spanning the full shelf width
    // -- drawn once per row (on its first column only) so it reads as one
    // continuous shelf rather than a separate strip under each cover. A
    // second, thinner line just below gives the board a bit of visual
    // thickness/depth without needing grayscale dithering (this stays a
    // plain 1-bit draw, same as the rest of this screen).
    if (i % static_cast<size_t>(layout.columns) == 0) {
      const int boardY = cell.y + layout.coverAreaHeight + SHELF_ROW_GAP;
      const int boardX = body.x + SHELF_SIDE_MARGIN / 2;
      const int boardWidth = body.width - SHELF_SIDE_MARGIN;
      renderer.fillRect(boardX, boardY, boardWidth, SHELF_BOARD_THICKNESS, true);
      renderer.fillRect(boardX, boardY + SHELF_BOARD_THICKNESS + SHELF_BOARD_SHADOW_GAP, boardWidth,
                        SHELF_BOARD_SHADOW_THICKNESS, true);
    }
  }

  // The only feedback for Page Up/Down (and swipe) navigation now that
  // tapping a cover opens it immediately and there's no title text to
  // highlight instead -- deliberately thick and obvious, per how this was
  // asked to look.
  const Rect selected = cellRectForIndex(layout, selectedIndex);
  renderer.drawRect(selected.x - SHELF_SELECTION_PADDING, selected.y - SHELF_SELECTION_PADDING,
                    selected.width + 2 * SHELF_SELECTION_PADDING, selected.height + 2 * SHELF_SELECTION_PADDING,
                    SHELF_SELECTION_THICKNESS, true);
}

void AppShellActivity::render(RenderLock&&) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  // No title: this is the one header on a screen that isn't a themed list,
  // so it should read as chrome (clock/battery/sync mark), not a heading.
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, nullptr);

  const Rect tabBarRect{0, pageHeight - TAB_BAR_HEIGHT, pageWidth, TAB_BAR_HEIGHT};
  const Rect bodyRect = computeBodyRect();

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

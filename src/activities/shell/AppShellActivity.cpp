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

void AppShellActivity::renderCoverBox(const Rect rect, const RecentBook& book) const {
  // Always request the SAME height HomeActivity already generates and
  // caches for its own card, whatever this particular box's rect size is --
  // getCoverThumbPath resolves to a specific file per height, and this is
  // the one height reliably present (or reliably generatable) on disk.
  // drawBitmap does the actual scaling into `rect` below.
  const int height = UITheme::getInstance().getMetrics().homeCoverHeight;
  bool drew = false;
  if (!book.coverBmpPath.empty()) {
    ensureCoverThumb(book, height);
    const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, height);
    HalFile file;
    if (Storage.openFileForRead("SHELL", coverPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
        const float aspect = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
        int width = static_cast<int>(rect.height * aspect);
        width = std::min(width, rect.width);
        const int x = rect.x + std::max(0, (rect.width - width) / 2);
        renderer.drawBitmap(bitmap, x, rect.y, width, rect.height);
        renderer.drawRect(x, rect.y, width, rect.height);
        drew = true;
      }
    }
  }
  if (!drew) {
    renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
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
  const int titleFontHeight = renderer.getLineHeight(NOTOSANS_18_FONT_ID);
  const int coverRowHeight = std::max(0, body.height - titleGap - titleFontHeight - titleGap);

  const bool hasNeighbors = recentBooks.size() > 1;
  const int peekWidth = hasNeighbors ? body.width / 5 : 0;
  const int peekHeight = coverRowHeight * 3 / 4;
  const int peekY = body.y + (coverRowHeight - peekHeight) / 2;

  if (hasNeighbors) {
    renderCoverBox(Rect{body.x, peekY, peekWidth, peekHeight}, recentBooks[previousCarouselIndex()]);
    renderCoverBox(Rect{body.x + body.width - peekWidth, peekY, peekWidth, peekHeight},
                   recentBooks[nextCarouselIndex()]);
  }

  renderCoverBox(Rect{body.x + peekWidth, body.y, body.width - 2 * peekWidth, coverRowHeight},
                 recentBooks[carouselIndex]);

  const std::string& title = recentBooks[carouselIndex].title;
  const int titleWidth = renderer.getTextWidth(NOTOSANS_18_FONT_ID, title.c_str());
  const int titleY = body.y + coverRowHeight + titleGap;
  renderer.drawText(NOTOSANS_18_FONT_ID, body.x + std::max(0, (body.width - titleWidth) / 2), titleY, title.c_str());
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

#include "AppShellActivity.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <functional>

#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Not yet run through the localization pipeline (see the header's Scope
// note) -- English only until this shell is confirmed and worth wiring into
// the 34-language string tables like the rest of the app's UI text.
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

void AppShellActivity::onExit() {
  Activity::onExit();
  freeCoverBuffer();
}

void AppShellActivity::loadRecentBooks() {
  recentBooks.clear();
  for (const RecentBook& book : RECENT_BOOKS.getBooks()) {
    if (RecentBooksStore::isMissing(book)) continue;
    recentBooks.push_back(book);
    if (recentBooks.size() >= MAX_CAROUSEL_BOOKS) break;
  }
}

bool AppShellActivity::storeCoverBuffer() {
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("SHELL", "OOM: Continue cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  coverBufferStored = true;
  return true;
}

bool AppShellActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void AppShellActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void AppShellActivity::invalidateCoverCache() {
  coverRendered = false;
  freeCoverBuffer();
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
  // A different book is now centered -- the cached cover buffer belongs to
  // the old one.
  invalidateCoverCache();
  requestUpdate();
}

void AppShellActivity::switchTab(const Tab tab) {
  if (tab == activeTab) return;
  activeTab = tab;
  invalidateCoverCache();
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
        activityManager.goToReader(recentBooks[carouselIndex].path);
      }
    } else {
      openActiveTabTarget();
    }
    return;
  }

  // Swipe is reserved for the Continue carousel -- it does not switch tabs
  // (see the class comment in the header for why).
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
  constexpr int pillMargin = 6;
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
      // Filled pill, not the battery-strip look every other themed header
      // uses -- this is the one place in the shell meant to read at a glance
      // as "you are here" rather than match the rest of the app's chrome.
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

// Loads and draws one book's cover, aspect-fit within `rect`'s height and
// centered in its width, with a plain border. No caching -- this is only
// ever asked to draw the small, occasionally-shown peek covers. Falls back
// to a bordered box with the title if there's no cover art or it fails to
// load; never leaves the slot blank.
void AppShellActivity::renderPeekCover(const Rect rect, const RecentBook& book) const {
  bool drew = false;
  if (!book.coverBmpPath.empty()) {
    const std::string coverBmpPath = UITheme::getCoverThumbPath(book.coverBmpPath, rect.height);
    HalFile file;
    if (Storage.openFileForRead("SHELL", coverBmpPath, file)) {
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
    constexpr int fontId = SMALL_FONT_ID;
    const int textWidth = renderer.getTextWidth(fontId, book.title.c_str());
    const int textX = rect.x + std::max(0, (rect.width - textWidth) / 2);
    const int textY = rect.y + std::max(0, (rect.height - renderer.getLineHeight(fontId)) / 2);
    renderer.drawText(fontId, textX, textY, book.title.c_str());
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

  constexpr int ctaHeight = 56;
  constexpr int ctaMargin = 24;
  constexpr int titleGap = 16;
  const int titleFontHeight = renderer.getLineHeight(NOTOSANS_18_FONT_ID);
  const int coverRowHeight =
      std::max(0, body.height - titleGap - titleFontHeight - titleGap - ctaHeight - titleGap);

  const bool hasNeighbors = recentBooks.size() > 1;
  const int peekWidth = hasNeighbors ? body.width / 5 : 0;
  const int peekHeight = coverRowHeight * 3 / 4;
  const int peekY = body.y + (coverRowHeight - peekHeight) / 2;

  // Side peeks first, so the centered cover's border/cache overlaps cleanly
  // on top if the two ever touch at narrow screen widths.
  if (hasNeighbors) {
    renderPeekCover(Rect{body.x, peekY, peekWidth, peekHeight}, recentBooks[previousCarouselIndex()]);
    renderPeekCover(Rect{body.x + body.width - peekWidth, peekY, peekWidth, peekHeight},
                    recentBooks[nextCarouselIndex()]);
  }

  // Centered cover: same cached-buffer path as HomeActivity's cover tile,
  // now keyed to whichever book the carousel has centered.
  const std::vector<RecentBook> centered{recentBooks[carouselIndex]};
  coverRectX = body.x + peekWidth;
  coverRectY = body.y;
  coverRectW = body.width - 2 * peekWidth;
  coverRectH = coverRowHeight;
  const bool bufferRestored = coverBufferStored && restoreCoverBuffer();
  GUI.drawRecentBookCover(renderer, Rect{coverRectX, coverRectY, coverRectW, coverRectH}, centered,
                          /*selectorIndex=*/0, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&AppShellActivity::storeCoverBuffer, this));

  const std::string& title = recentBooks[carouselIndex].title;
  const int titleWidth = renderer.getTextWidth(NOTOSANS_18_FONT_ID, title.c_str());
  const int titleY = body.y + coverRowHeight + titleGap;
  renderer.drawText(NOTOSANS_18_FONT_ID, body.x + std::max(0, (body.width - titleWidth) / 2), titleY, title.c_str());

  // The one call-to-action band in the whole shell: a filled pill rather
  // than a list row, icon, or menu -- deliberately not matching the look of
  // Books/Book Server/Settings or of any existing CrossPoint screen.
  const int ctaY = body.y + body.height - ctaHeight;
  renderer.fillRect(body.x + ctaMargin, ctaY, body.width - 2 * ctaMargin, ctaHeight, true);
  const char* cta = tr(STR_CONTINUE_READING);
  const int ctaTextWidth = renderer.getTextWidth(NOTOSANS_16_FONT_ID, cta);
  const int ctaTextY = ctaY + (ctaHeight - renderer.getLineHeight(NOTOSANS_16_FONT_ID)) / 2;
  renderer.drawText(NOTOSANS_16_FONT_ID, body.x + (body.width - ctaTextWidth) / 2, ctaTextY, cta, /*black=*/false);
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

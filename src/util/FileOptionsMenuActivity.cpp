#include "FileOptionsMenuActivity.h"

#include <I18n.h>

#include "HalDisplay.h"
#include "components/UITheme.h"

FileOptionsMenuActivity::FileOptionsMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                  std::string entryName)
    : Activity("FileOptionsMenu", renderer, mappedInput), entryName(std::move(entryName)) {}

void FileOptionsMenuActivity::onEnter() {
  Activity::onEnter();

  // "Edit" is not yet in the localization tables (same as the shell's tab
  // labels) -- plain English until this is worth wiring into all 34
  // languages. Delete reuses the same translated string the old direct
  // delete-confirm flow already showed.
  const char* options[] = {"Edit", I18N.get(StrId::STR_DELETE)};
  const int maxWidth = renderer.getScreenWidth() - 80;
  const std::string title = renderer.truncatedText(UI_10_FONT_ID, entryName.c_str(), maxWidth, EpdFontFamily::BOLD);

  menu.show(title.c_str(), options, 2, 0, [this](const int idx) {
    ActivityResult res;
    res.data = MenuResult{idx == 0 ? FileOptionsMenuActivity::EDIT : FileOptionsMenuActivity::DELETE, 0, 0};
    res.isCancelled = false;
    setResult(std::move(res));
    finish();
  });

  requestUpdate(true);
}

void FileOptionsMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (menu.processRender(renderer, mappedInput)) return;
  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void FileOptionsMenuActivity::loop() {
  if (menu.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  // Dismissed without a choice (Back button or tap outside): cancel, same
  // as ConfirmationActivity's own fallback.
  ActivityResult res;
  res.isCancelled = true;
  setResult(std::move(res));
  finish();
}

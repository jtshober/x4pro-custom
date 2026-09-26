#pragma once
#include <string>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

// Long-press-on-a-row menu for the File Browser: Edit (rename) or Delete.
// Structurally a close twin of ConfirmationActivity -- a raw Activity
// hosting its own OptionPopup, fully self-contained -- rather than wiring
// OptionPopup directly into FileBrowserActivity itself (a UiListActivity;
// see the .cpp for why that felt like the wrong place to take on risk).
//
// Result: MenuResult{action} where action is FileOptionsMenuActivity::EDIT
// or ::DELETE, or ActivityResult::isCancelled on Back/tap-outside -- same
// result shape EpubReaderMenuActivity already uses for its own action menu,
// reused here rather than adding a new ActivityResult variant.
class FileOptionsMenuActivity : public Activity {
 public:
  static constexpr int EDIT = 0;
  static constexpr int DELETE = 1;

  FileOptionsMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string entryName);

  void onEnter() override;
  void render(RenderLock&& lock) override;
  void loop() override;

 private:
  std::string entryName;
  OptionPopup menu;
};

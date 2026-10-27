// The sidebar footer's downloads (model::downloads()): a ghost button — the
// download arrow, ringed by the running downloads' progress — shown once
// anything was downloaded, and the popover above it that lists them, newest
// first. A row is the file's name over its state: "1.2 MB of 4.5 MB" and a
// bar while it runs, then its size and when, "Failed", "Cancelled" or
// "Moved or deleted". A click opens a downloaded file; the row's buttons
// show it in its folder, cancel it or take it off the list (the file
// stays). "Clear" takes every finished one off. Ctrl/Cmd+Shift+J
// (shortcuts.h) opens it too; Escape or a click outside closes it.
#pragma once

#include "app/model/downloads.h"
#include "screens/common/context.h"
#include "screens/shell/nav_chrome.h"
#include "ui/ui.h"

namespace shell {

class DownloadsButton final : public GhostButton {
public:
    DownloadsButton();
    ~DownloadsButton() override;
    void paint(gfx::Painter &p) override;

private:
    model::Downloads::ObserverId _observer = 0;
};

// The popover, anchored above `anchor` (window coordinates); it closes itself
// once the list is empty.
ui::Popup *showDownloads(ui::Window &w, screens::Context &ctx, ui::RectF anchor);

} // namespace shell

// The header's Pins, as in Slack: a conversation's pinned messages
// (Store::pins, the latest pinned first), each as a chat row — the avatar,
// the name and date, the rich body, its pictures and files. A click on a
// row jumps to the message. They show in two places: pointing at the Pins
// tab drops them down under it (PinsHover: after 300 ms, gone 260 ms after
// the pointer leaves both), and a click on the tab makes them the message
// area (PinsPage).
#pragma once

#include "screens/common/context.h"
#include "screens/shell/avatars.h"
#include "ui/ui.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace shell {

// The rows. show() builds them from the Store; a later Store change
// rebuilds them only when the pins themselves changed.
class PinsList final : public ui::View {
public:
    PinsList(screens::Context &ctx, Avatars &avatars, bool compact);
    ~PinsList() override;

    void               show(model::ConvRef conv);
    model::ConvRef     conv() const { return _conv; }
    size_t             count() const { return _pinRows.size(); }
    // What shows instead of rows: "Loading…", "No pinned messages", "".
    const std::string &statusText() const { return _statusText; }
    void               activate(size_t i); // a click on row i (tests)

    // A row: jump to the message (in its thread, for a reply).
    std::function<void(model::ConvRef, model::Ts ts, model::Ts thread)> onOpen;

private:
    void rebuild();

    screens::Context            &_ctx;
    Avatars                     &_avatars;
    bool                         _compact; // the dropdown: tighter rows
    model::ConvRef               _conv = model::kNoConv;
    std::vector<model::Ts>       _shown;    // the pins the rows show, in order
    std::vector<uint64_t>        _versions; // and what they read (edits)
    bool                         _loaded = false;
    std::vector<ui::View *>      _rows;    // the rows and the rules between them
    std::vector<ui::Clickable *> _pinRows; // the rows
    ui::Label                   *_status = nullptr;
    std::string                  _statusText;
    model::Store::ObserverId     _observer = 0;
    const model::Store          *_store    = nullptr; // whose pins _shown are
};

// The Pins tab's page: the list in the message area, scrolling.
class PinsPage final : public ui::View {
public:
    PinsPage(screens::Context &ctx, Avatars &avatars);
    // Shows conv's pins (and asks the service for them again).
    void      open(model::ConvRef conv);
    PinsList &list() { return *_list; }

    // A row: jump to the message (in its thread, for a reply).
    std::function<void(model::ConvRef, model::Ts ts, model::Ts thread)> onOpen;

private:
    screens::Context &_ctx;
    PinsList         *_list = nullptr;
};

// The dropdown under the Pins tab while the pointer is on the tab or on it.
class PinsHover {
public:
    PinsHover(screens::Context &ctx, ui::Window &win, Avatars &avatars);
    ~PinsHover();

    // The pointer entered (true) or left (false) the tab at `anchor` (its
    // window rect), for conv's pins.
    void       hoverTab(bool on, model::ConvRef conv, ui::RectF anchor);
    void       hideNow();
    ui::Popup *popup() const { return _popup; }

    // A row: jump to the message (in its thread, for a reply).
    std::function<void(model::ConvRef, model::Ts ts, model::Ts thread)> onOpen;

    // Internal (the dropdown): the pointer entered / left it.
    void cancelHide();
    void scheduleHide();

private:
    void show();

    screens::Context &_ctx;
    ui::Window       &_win;
    Avatars          &_avatars;
    ui::Popup        *_popup     = nullptr;
    plat::TimerId     _showTimer = 0, _hideTimer = 0;
    model::ConvRef    _conv = model::kNoConv;
    ui::RectF         _anchor;
};

} // namespace shell

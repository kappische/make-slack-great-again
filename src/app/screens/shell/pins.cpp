#include "screens/shell/pins.h"

#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"
#include "gfx/icons_generated.h"
#include "app/screens/common/message_rules.h"
#include "screens/shell/overview_page.h"
#ifdef MSGA_HAVE_MESSAGES
#include "app/screens/messages/image_cache.h"
#include "app/screens/messages/rich.h"
#include "app/screens/messages/rows.h"
#endif

#include <algorithm>
#include <cmath>
#include <functional>

using namespace ui;
using i18n::tr;
using model::ConvRef;
using model::Ts;

namespace shell {

namespace {

constexpr int kShowDelayMs = 300; // as the profile card
constexpr int kHideDelayMs = 260;

// What a row reads: a change rebuilds it (an edit, a file gone).
uint64_t version(const model::Message &m) {
    return std::hash<std::string>{}(m.text) ^ (uint64_t(m.files().size()) << 48) ^
           (uint64_t(m.edited) << 63);
}

// One pinned message: avatar, name and date over the rich body and its
// pictures and files; a click outside a link jumps to it.
class PinRow final : public Clickable {
public:
    PinRow(screens::Context &ctx, Avatars &avatars, const model::Message &m, bool compact) {
        setLook({C::None, C::Hover, C::Hover, C::None, 0});
        setCursor(plat::Cursor::Hand);
        const model::Store &st  = ctx.store();
        View               *col = fillMessageRow(
            this,
            avatars,
            std::string(screens::authorName(st, m)),
            screens::authorAvatar(st, m),
            base::dateTimeLabel(model::tsSecs(m.ts), ctx.backend.nowSecs())
        );
        // In from the sides (fillMessageRow's padding is the vertical).
        style().pad.l = style().pad.r = compact ? 16 : 24;
        style().pad.t = style().pad.b = compact ? 10 : 12;
        auto *body                    = col->add<View>();
        body->style().spacing(2);
#ifdef MSGA_HAVE_MESSAGES
        screens::RichOptions o;
        o.edited = m.edited;
        screens::buildBody(ctx, body, m.text, o, this);
        // Pictures within a box (as the delete dialog's preview), other
        // files as their chips.
        const float maxW = compact ? 280 : 360, maxH = compact ? 160 : 240;
        for (const model::File &f : m.files()) {
            if (f.isImage() && !f.path.empty()) {
                const float w = float(f.width), h = float(f.height > 0 ? f.height : f.width);
                const float s   = std::min({1.f, maxW / w, maxH / h});
                auto       *img = col->add<screens::CachedImage>(
                    ctx.images, f.path, screens::ImageCache::Shape::Rounded, 6
                );
                img->style().size(std::floor(w * s), std::floor(h * s)).alignSelf(Align::Start);
                img->setHitTransparent(true);
            } else {
                screens::addFileChip(col, f, nullptr, 0);
            }
        }
        // The reactions, as pills to read (reacting is the message's own).
        if (!m.reactions.empty()) {
            auto *flow = col->add<screens::FlowRow>(4);
            flow->style().margins(0, 4, 0, 2);
            for (const model::Reaction &r : m.reactions) {
                const bool mine = st.reactedByMe(r);
                auto      *pill = flow->add<View>();
                pill->setBackground(mine ? C::MentionBg : C::Hover, 12);
                if (mine)
                    pill->setBorder(C::Link);
                pill->style().row().height(24).padding(8, 0).spacing(4).items(Align::Center);
                const model::Store::EmojiGlyph g = st.emojiFor(r.name);
                if (!g.image.empty()) {
                    auto *img = pill->add<screens::CachedImage>(ctx.images, g.image);
                    img->setPlaceholder(C::None);
                    img->style().size(16, 16);
                } else {
                    pill->add<Label>(
                        g.unicode.empty() ? ":" + r.name + ":" : g.unicode, Font::Body
                    );
                }
                pill->add<Label>(
                    str::number(r.count), Font::SmallBold, mine ? C::MentionText : C::TextMuted
                );
            }
        }
#else
        body->add<Label>(m.text, Font::Body);
#endif
    }
};

} // namespace

// ── The list ────────────────────────────────────────────────────────────────

PinsList::PinsList(screens::Context &ctx, Avatars &avatars, bool compact)
    : _ctx(ctx), _avatars(avatars), _compact(compact) {
    style().column();
    _status = add<Label>("", Font::Body, C::TextMuted);
    _status->setAlign(text::LayoutOptions::Align::Center);
    _status->style().padding(24);
    _status->setVisible(false);
    _observer = ctx.store.observe(model::Store::kAnyConv, [this](const model::Change &ch) {
        if (ch.kind == model::ChangeKind::Meta && ch.conv == _conv)
            rebuild();
    });
}

PinsList::~PinsList() {
    _ctx.store.unobserve(_observer);
}

void PinsList::show(ConvRef conv) {
    if (conv != _conv || &_ctx.store() != _store) {
        _conv  = conv;
        _store = &_ctx.store();
        _shown.clear();
        _versions.clear();
        _loaded = false;
        for (View *r : _rows)
            remove(r);
        _rows.clear();
        _pinRows.clear();
    }
    rebuild();
}

void PinsList::rebuild() {
    const model::Store &st = _ctx.store();
    if (&st != _store || _conv >= st.conversationCount())
        return;
    const std::vector<model::Message> *pins = st.pins(_conv);
    std::vector<Ts>                    shown;
    std::vector<uint64_t>              versions;
    if (pins)
        for (const model::Message &m : *pins) {
            shown.push_back(m.ts);
            versions.push_back(version(m));
        }
    // Meta fires for any conversation field (unread counts…): rebuild only
    // when the pins changed.
    if (_loaded == (pins != nullptr) && shown == _shown && versions == _versions)
        return;
    _loaded   = pins != nullptr;
    _shown    = std::move(shown);
    _versions = std::move(versions);
    for (View *r : _rows)
        remove(r);
    _rows.clear();
    _pinRows.clear();
    _statusText = !pins           ? std::string(tr("Loading\xE2\x80\xA6"))
                  : pins->empty() ? std::string(tr("No pinned messages"))
                                  : std::string();
    _status->setText(_statusText);
    _status->setVisible(!_statusText.empty());
    if (!pins)
        return;
    for (size_t i = 0; i < pins->size(); ++i) {
        const model::Message &m = (*pins)[i];
        if (i > 0)
            _rows.push_back(add<Separator>(false, C::Border));
        auto    *row = add<PinRow>(_ctx, _avatars, m, _compact);
        const Ts ts  = m.ts;
        const Ts th  = m.isReply() ? m.threadTs : 0;
        row->onClick = [this, ts, th] {
            if (onOpen)
                onOpen(_conv, ts, th);
        };
        _rows.push_back(row);
        _pinRows.push_back(row);
    }
    invalidateLayout();
    update();
}

void PinsList::activate(size_t i) {
    if (i < _pinRows.size())
        _pinRows[i]->activate();
}

// ── The page ────────────────────────────────────────────────────────────────

PinsPage::PinsPage(screens::Context &ctx, Avatars &avatars) : _ctx(ctx) {
    style().column().flex(1);
    setBackground(C::Surface);
    auto *scroll = add<ScrollView>();
    scroll->style().flex(1);
    scroll->content()->style().padding(0, 8, 0, 8);
    _list         = scroll->content()->add<PinsList>(ctx, avatars, false);
    _list->onOpen = [this](ConvRef c, Ts ts, Ts thread) {
        if (onOpen)
            onOpen(c, ts, thread);
    };
}

void PinsPage::open(ConvRef conv) {
    _list->show(conv);
    _ctx.backend.loadPins(conv, nullptr); // the list follows the Store
}

// ── The dropdown ────────────────────────────────────────────────────────────

namespace {

class PinsPopup final : public Popup {
public:
    PinsPopup(screens::Context &ctx, Avatars &avatars, PinsHover &owner, ConvRef conv)
        : _owner(owner) {
        setModal(false);
        style().width(400).padding(4, 0, 4, 0);
        auto *scroll         = add<ScrollView>();
        scroll->style().maxH = 560;
        list                 = scroll->content()->add<PinsList>(ctx, avatars, true);
        list->show(conv);
    }
    bool onEvent(Event &e) override {
        if (e.type == EventType::PointerEnter)
            _owner.cancelHide();
        else if (e.type == EventType::PointerLeave)
            _owner.scheduleHide();
        return Popup::onEvent(e);
    }
    PinsList *list = nullptr;

private:
    PinsHover &_owner;
};

} // namespace

PinsHover::PinsHover(screens::Context &ctx, ui::Window &win, Avatars &avatars)
    : _ctx(ctx), _win(win), _avatars(avatars) {}

PinsHover::~PinsHover() {
    _ctx.app.platform().cancelTimer(_showTimer);
    _ctx.app.platform().cancelTimer(_hideTimer);
    if (_popup) // the window may be going first (as ProfileCards)
        _popup->onClosed = nullptr;
}

void PinsHover::hoverTab(bool on, ConvRef conv, RectF anchor) {
    plat::App &pa = _ctx.app.platform();
    if (!on) {
        pa.cancelTimer(_showTimer);
        _showTimer = 0;
        scheduleHide();
        return;
    }
    cancelHide();
    if (_popup && _conv == conv)
        return;
    _conv   = conv;
    _anchor = anchor;
    pa.cancelTimer(_showTimer);
    _showTimer = pa.addTimer(kShowDelayMs, false, [this] {
        _showTimer = 0;
        show();
    });
}

void PinsHover::show() {
    hideNow();
    if (_conv >= _ctx.store().conversationCount())
        return;
    auto p          = std::make_unique<PinsPopup>(_ctx, _avatars, *this, _conv);
    p->list->onOpen = [this](ConvRef c, Ts ts, Ts thread) {
        hideNow();
        if (onOpen)
            onOpen(c, ts, thread);
    };
    p->setAnchor(_anchor, Popup::Place::Below);
    p->onClosed = [this] { _popup = nullptr; };
    _popup      = p.get();
    _win.showPopup(std::move(p));
    _ctx.backend.loadPins(_conv, nullptr); // fresh while it shows
}

void PinsHover::hideNow() {
    _ctx.app.platform().cancelTimer(_hideTimer);
    _hideTimer = 0;
    if (Popup *p = std::exchange(_popup, nullptr))
        p->close();
}

void PinsHover::cancelHide() {
    _ctx.app.platform().cancelTimer(_hideTimer);
    _hideTimer = 0;
}

void PinsHover::scheduleHide() {
    if (!_popup || _hideTimer)
        return;
    _hideTimer = _ctx.app.platform().addTimer(kHideDelayMs, false, [this] {
        _hideTimer = 0;
        hideNow();
        _win.requestFrame();
    });
}

} // namespace shell

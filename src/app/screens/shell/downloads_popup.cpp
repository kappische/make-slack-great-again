#include "screens/shell/downloads_popup.h"

#include "base/file.h"
#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"
#include "gfx/icons_generated.h"

#include <algorithm>
#include <cmath>
#include <memory>

using namespace ui;
using gfx::Icon;
using i18n::arg;
using i18n::tr;
using model::Downloads;

namespace shell {

namespace {

constexpr float kWidth = 360, kMaxListH = 360, kRowH = 52, kRingR = 15, kPi = 3.14159265f;

// "1.2 MB of 4.5 MB" while it runs; its size and when once in; else what
// became of it.
std::string statusText(const Downloads::Item &it) {
    using S = Downloads::State;
    switch (it.state) {
    case S::Running:
        if (it.total > 0)
            return arg(
                tr("%1 of %2"),
                str::byteSize(it.received, str::ByteSize::Exact),
                str::byteSize(it.total, str::ByteSize::Exact)
            );
        return it.received > 0 ? str::byteSize(it.received, str::ByteSize::Exact)
                               : std::string(tr("Starting\xE2\x80\xA6"));
    case S::Done: {
        if (it.missing)
            return tr("Moved or deleted");
        const std::string when = base::dateTimeLabel(it.at, base::nowSecs());
        const std::string size = str::byteSize(it.total, str::ByteSize::File);
        return size.empty() ? when : str::concat({size, " \xC2\xB7 ", when});
    }
    case S::Failed:
        return tr("Failed");
    case S::Cancelled:
        return tr("Cancelled");
    }
    return {};
}

// A running download's bar: accent over the divider, 3 px.
class ProgressBar final : public View {
public:
    explicit ProgressBar(float fraction) : _f(fraction) { style().height(3).margins(0, 4, 0, 0); }
    void paint(gfx::Painter &p) override {
        const RectF b = bounds();
        p.fillRoundRect(b, 1.5f, color(C::FormDivider));
        if (_f > 0)
            p.fillRoundRect({b.x, b.y, std::max(3.f, b.w * _f), b.h}, 1.5f, color(C::Accent));
    }

private:
    float _f;
};

class DownloadsPopup final : public Popup {
public:
    explicit DownloadsPopup(screens::Context &ctx) : _ctx(ctx) {
        setCard(false);
        setBackground(C::FormBg, 8);
        setBorder(C::FormDividerStrong);
        style().width(kWidth).padding(0, 8, 0, 8);
        auto *head = add<View>();
        head->style().row().height(32).padding(16, 0, 10, 0).items(Align::Center);
        head->add<Label>(tr("Downloads"), Font::BodyBold, C::FormText)->style().flex(1);
        _clear = head->add<Clickable>();
        _clear->setLook({C::None, C::Hover, C::Pressed, C::None, 4});
        _clear->style().padding(6, 3);
        _clear->add<Label>(tr("Clear"), Font::Small, C::FormLink);
        _clear->setTooltip(tr("Clear the list (the files stay)"));
        _clear->onClick = [] { model::downloads().clearFinished(); };
        _scroll         = add<ScrollView>();
        _observer       = model::downloads().observe([this] { schedule(); });
        model::downloads().checkFiles(); // moved or deleted since
        build();
        setFocusable(true);
    }
    ~DownloadsPopup() override { model::downloads().unobserve(_observer); }

private:
    // Rebuilt on a later turn: a change can come from a click on one of
    // the rows' own buttons, which must outlive its handler.
    void schedule() {
        if (_pending)
            return;
        _pending                  = true;
        std::weak_ptr<char> alive = _alive;
        _ctx.app.platform().post([this, alive] {
            if (alive.expired())
                return;
            _pending = false;
            build();
        });
    }
    void build() {
        const std::vector<Downloads::Item> &items = model::downloads().items();
        if (items.empty()) {
            close();
            return;
        }
        View *c = _scroll->content();
        c->clearChildren();
        bool finished = false;
        for (const Downloads::Item &it : items) {
            addRow(c, it);
            finished |= it.state != Downloads::State::Running;
        }
        _clear->setVisible(finished);
        _scroll->style().height(std::min(kMaxListH, float(items.size()) * kRowH));
        invalidateLayout();
    }
    void addRow(View *list, const Downloads::Item &it) {
        using S         = Downloads::State;
        const bool done = it.state == S::Done && !it.missing;
        auto      *row  = list->add<Clickable>();
        row->style().row().height(kRowH).padding(16, 0, 10, 0).spacing(12).items(Align::Center);
        row->setLook({C::None, done ? C::FormHighlight : C::None, C::None, C::None, 0});
        row->setCursor(done ? plat::Cursor::Hand : plat::Cursor::Arrow);
        row->add<IconView>(Icon::Paperclip, 18, done ? C::FormIcon : C::FormTextFaint)
            ->style()
            .noShrink();
        auto *col = row->add<View>();
        col->style().flex(1).spacing(2);
        col->add<Label>(it.name, Font::BodySemibold, done ? C::FormText : C::FormTextMuted)
            ->setMaxLines(1);
        col->add<Label>(statusText(it), Font::Small, C::FormTextMuted)->setMaxLines(1);
        if (it.state == S::Running)
            col->add<ProgressBar>(
                it.total > 0 ? std::min(1.f, float(it.received) / float(it.total)) : 0.f
            );
        const int id     = it.id;
        auto      button = [&](Icon icon, const char *tip) {
            auto *b = row->add<GlyphButton>(icon, 28, 16, C::FormIcon, tr(tip));
            b->setLook({C::None, C::Hover, C::Pressed, C::None, 6});
            return b;
        };
        if (done) {
            const std::string path = it.path;
            row->onClick           = [this, path] {
                if (_ctx.openUrl)
                    _ctx.openUrl(file::toFileUrl(path));
            };
            button(Icon::Folder, N_("Show in folder"))->onClick = [this, path] {
                if (_ctx.showInFolder)
                    _ctx.showInFolder(path);
            };
        }
        if (it.state == S::Running)
            button(Icon::X, N_("Cancel"))->onClick = [id] { model::downloads().cancel(id); };
        else
            button(Icon::X, N_("Remove from list"))->onClick = [id] {
                model::downloads().remove(id);
            };
    }

    screens::Context            &_ctx;
    Clickable                   *_clear    = nullptr;
    ScrollView                  *_scroll   = nullptr;
    model::Downloads::ObserverId _observer = 0;
    bool                         _pending  = false;
    std::shared_ptr<char>        _alive    = std::make_shared<char>();
};

} // namespace

DownloadsButton::DownloadsButton() : GhostButton(Icon::Download, {}) {
    _observer = model::downloads().observe([this] {
        setVisible(!model::downloads().items().empty());
        update();
    });
    setVisible(!model::downloads().items().empty());
}

DownloadsButton::~DownloadsButton() {
    model::downloads().unobserve(_observer);
}

void DownloadsButton::paint(gfx::Painter &p) {
    GhostButton::paint(p);
    if (model::downloads().running() == 0)
        return;
    // Running: a faint ring, the white arc over it from 12 o'clock as far
    // as the bytes are in (a quarter while no size is known); the ghost
    // chrome's whites, legible on any sidebar colour.
    const RectF b  = bounds();
    const float cx = b.x + b.w / 2, cy = b.y + b.h / 2;
    p.strokeCircle({cx, cy}, kRingR, 2, 0x33ffffffU);
    int64_t     got = 0, total = 0;
    const float f = model::downloads().progressOf(&got, &total)
                        ? std::clamp(float(got) / float(total), 0.f, 1.f)
                        : 0.25f;
    if (f <= 0)
        return;
    gfx::Path arc;
    const int steps = std::max(2, int(std::ceil(48 * f)));
    for (int i = 0; i <= steps; ++i) {
        const float a = -kPi / 2 + 2 * kPi * f * float(i) / float(steps);
        const float x = cx + kRingR * std::cos(a), y = cy + kRingR * std::sin(a);
        if (i == 0)
            arc.moveTo(x, y);
        else
            arc.lineTo(x, y);
    }
    p.strokePath(arc, 2, 0xf0ffffffU);
}

Popup *showDownloads(Window &w, screens::Context &ctx, RectF anchor) {
    if (model::downloads().items().empty())
        return nullptr;
    auto p = std::make_unique<DownloadsPopup>(ctx);
    p->setAnchor(anchor, Popup::Place::Above);
    return w.showPopup(std::move(p));
}

} // namespace shell

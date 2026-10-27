#include "screens/shell/channel_dialogs.h"

#include "base/i18n.h"
#include "base/str.h"
#include "base/utf8.h"
#include "gfx/icons_generated.h"
#include "screens/common/user_search.h"
#include "screens/shell/shell_text.h"
#include "ui/controls.h"

#include <algorithm>
#include <memory>

using namespace ui;
using gfx::Icon;
using i18n::tr;
using model::ConvKind;
using model::ConvRef;
using model::UserRef;
using V = Button::Kind;

namespace shell {

namespace {

// One of the dialog's tabs (a checkable button): 8/16 padding,
// the active one bold in the primary colour over a 2-px accent underline.
class FinderTab final : public Clickable {
public:
    explicit FinderTab(std::string text) {
        setRole(Role::Tab);
        setFocusable(false);
        setLook({C::None, C::None, C::None, C::None, 0});
        style().padding(16, 8).noShrink();
        _label = add<Label>(std::move(text), Font::Body, C::FormTextMuted);
    }
    void setActive(bool on) {
        setChecked(on);
        _label->setFont(on ? Font::BodyBold : Font::Body);
        tint();
    }
    bool onEvent(Event &e) override {
        const bool r = Clickable::onEvent(e);
        if (e.type == EventType::PointerEnter || e.type == EventType::PointerLeave)
            tint(); // the primary colour on hover
        return r;
    }
    void paintOver(gfx::Painter &p) override {
        if (checked())
            p.fillRect({0, height() - 2, width(), 2}, color(C::Accent));
    }

private:
    void   tint() { _label->setColor(checked() || hovered() ? C::FormText : C::FormTextMuted); }
    Label *_label;
};

// ── Find a channel ──────────────────────────────────────────────────────────

constexpr int kVisibleRows = 6; // the lists' height, in rows

class ChannelFinder final : public Dialog {
public:
    ChannelFinder(
        screens::Context            &ctx,
        Avatars                     &avatars,
        std::function<void(ConvRef)> channel,
        std::function<void(UserRef)> person,
        std::function<void()>        create
    )
        : Dialog(std::string(), 720), _ctx(ctx), _channel(std::move(channel)),
          _person(std::move(person)), _create(std::move(create)) {
        // A bare card: its own chrome, the lists edge to edge.
        content()->style().spacing(0);

        // ── Top bar: search + Create Channel + close ──
        auto *top = content()->add<View>();
        top->style()
            .row()
            .items(Align::Center)
            .spacing(8)
            .padding(24, 20, 24 - Dialog::kCloseInset, 16);
        _search = top->add<TextField>(
            tr("Search for channels"), TextField::Size::Normal, uint16_t(Icon::Search)
        );
        _search->style().flex(1).minW = 200;
        auto *createBtn               = top->add<Button>(
            tr("Create Channel"), V::Primary, Button::Form::Normal
        ); // this casing on purpose
        createBtn->setFocusable(false);
        createBtn->onClick = [this] {
            auto cb = _create;
            reject();
            if (cb)
                cb();
        };
        addDialogCloseButton(top)->onClick = [this] { reject(); };

        // ── Tab bar ──
        auto *tabs = content()->add<View>();
        tabs->style().row().padding(20, 0, 24, 0);
        _tabs[0] = tabs->add<FinderTab>(tr("Channels"));
        _tabs[1] = tabs->add<FinderTab>(tr("People"));
        for (int i = 0; i < 2; ++i)
            _tabs[i]->onClick = [this, i] {
                selectTab(i);
                _search->edit().focus(); // the tabs take no focus from the field
            };
        content()->add<Separator>(false, C::FormDivider);

        // ── The two lists: whole rows, none cut at the bottom ──
        auto *stack = content()->add<View>();
        stack->style().stack().height(kVisibleRows * BrowseList::kRowH).noShrink();
        for (int i = 0; i < 2; ++i) {
            _lists[i]              = stack->add<BrowseList>(avatars);
            _lists[i]->onActivated = [this, i](const std::string &id) { activated(i, id); };
        }
        _lists[0]->setItems(channelItems(ctx.store));
        _lists[1]->setItems(peopleItems(ctx.store));

        _search->edit().onChange = [this] { applyFilter(); };
        // The search field drives the open list: arrows move, Enter opens.
        _search->edit().onKey    = [this](const Event &e) { return _lists[_tab]->navKey(e); };
        selectTab(0);
    }

    void selectTab(int tab) {
        _tab = tab;
        for (int i = 0; i < 2; ++i) {
            _tabs[i]->setActive(i == tab);
            _lists[i]->setVisible(i == tab);
        }
        _search->edit().setPlaceholder(
            tab == 0 ? tr("Search for channels") : tr("Search for people")
        );
        applyFilter();
    }
    TextField &search() { return *_search; }

private:
    // The open list only; the other one is filtered when its tab opens.
    void applyFilter() { _lists[_tab]->applyFilter(_search->text()); }
    void activated(int list, const std::string &id) {
        // The ids are the Store's: channels by conversation id, people by user id.
        const ConvRef c  = list == 0 ? _ctx.store().findConversation(id) : model::kNoConv;
        const UserRef u  = list == 1 ? _ctx.store().findUser(id) : model::kNoUser;
        auto          ch = _channel;
        auto          pe = _person;
        accept();
        if (c != model::kNoConv && ch)
            ch(c);
        if (u != model::kNoUser && pe)
            pe(u);
    }

    screens::Context            &_ctx;
    std::function<void(ConvRef)> _channel;
    std::function<void(UserRef)> _person;
    std::function<void()>        _create;
    TextField                   *_search   = nullptr;
    FinderTab                   *_tabs[2]  = {};
    BrowseList                  *_lists[2] = {};
    int                          _tab      = 0;
};

// ── Create a channel ────────────────────────────────────────────────────────

constexpr int kMaxNameLen = 80;

class CreateChannel final : public Dialog {
public:
    CreateChannel(std::string workspaceName, std::function<void(const std::string &, bool)> done)
        : Dialog(tr("Create a channel")), _workspace(std::move(workspaceName)),
          _done(std::move(done)) {
        const text::Style bold  = pxFont(15, text::Weight::Bold, themed(C::FormText));
        const text::Style muted = pxFont(13, text::Weight::Regular, themed(C::FormTextMuted));

        // ── Step 1: Name ──
        _page1 = content()->add<View>();
        _page1->style().spacing(12);
        styledLabel(_page1, tr("Name"), bold);
        _name = _page1->add<TextField>(tr("e.g. plan-budget"), TextField::Size::Normal);
        _name->setPrefix("#");
        _name->setMaxLength(kMaxNameLen);
        styledLabel(
            _page1,
            tr("Channels are where conversations happen around a topic. Use a name that is easy "
               "to find and understand."),
            muted
        );
        auto *row1 = _page1->add<View>();
        row1->style().row().items(Align::Center).margins(0, 12, 0, 0);
        row1->add<View>()->style().flex(1);
        _next = makeButton(tr("Next"), V::Primary);
        row1->adopt(std::unique_ptr<View>(_next));
        _next->setEnabled(false);
        _next->onClick         = [this] { goNext(); };
        auto prevChange        = _name->edit().onChange; // the length counter
        _name->edit().onChange = [this, prevChange] {
            if (prevChange)
                prevChange();
            _next->setEnabled(!str::trim(_name->text()).empty());
        };
        _name->onReturn = [this] {
            if (_next->enabled())
                goNext();
        };

        // ── Step 2: Visibility ──
        _page2 = content()->add<View>();
        _page2->style().spacing(12);
        _subtitle = styledLabel(_page2, std::string(), muted);
        styledLabel(_page2, tr("Visibility"), bold)->style().margins(0, 4, 0, 0);
        _visibility = _page2->add<RadioGroup>(
            std::vector<std::string>{std::string(), tr("Private — only specific people")}
        );
        styledLabel(_page2, tr("Can only be viewed or joined by invitation"), muted)
            ->style()
            .margins(32, 0, 0, 0);
        auto *row2 = _page2->add<View>();
        row2->style().row().items(Align::Center).spacing(12).margins(0, 12, 0, 0);
        styledLabel(row2, tr("Step 2 of 2"), muted);
        row2->add<View>()->style().flex(1);
        Button *back = makeButton(tr("Back"), V::Secondary);
        Button *make = makeButton(tr("Create"), V::Primary);
        row2->adopt(std::unique_ptr<View>(back));
        row2->adopt(std::unique_ptr<View>(make));
        back->onClick = [this] { showStep(1); };
        make->onClick = [this] {
            const std::string name = channelName(_name->text());
            const bool        priv = _visibility->selected() == 1;
            auto              cb   = std::move(_done);
            accept();
            if (cb)
                cb(name, priv);
        };
        showStep(1);
    }

    TextField &name() { return *_name; }

private:
    void goNext() {
        _subtitle->setText("# " + channelName(_name->text()));
        _visibility->radio(0)->setLabel(
            i18n::arg(
                tr("Public — anyone in %1"), _workspace.empty() ? tr("this workspace") : _workspace
            )
        );
        showStep(2);
    }
    void showStep(int step) {
        _page1->setVisible(step == 1);
        _page2->setVisible(step == 2);
        if (step == 1)
            _name->edit().focus();
    }

    std::string                                    _workspace;
    std::function<void(const std::string &, bool)> _done;
    View                                          *_page1 = nullptr, *_page2 = nullptr;
    TextField                                     *_name       = nullptr;
    Button                                        *_next       = nullptr;
    Label                                         *_subtitle   = nullptr;
    RadioGroup                                    *_visibility = nullptr;
};

} // namespace

// ── Rows ────────────────────────────────────────────────────────────────────

std::vector<BrowseList::Item> channelItems(const model::Store &store) {
    std::vector<ConvRef> channels;
    for (ConvRef c = 0; c < store.conversationCount(); ++c)
        if (!store.conversation(c).isDirect() && !store.conversation(c).id.empty())
            channels.push_back(c);
    std::vector<std::string> keys(store.conversationCount());
    for (ConvRef c : channels)
        keys[c] = utf8::foldCase(store.conversation(c).name);
    std::stable_sort(channels.begin(), channels.end(), [&](ConvRef a, ConvRef b) {
        return keys[a] < keys[b];
    });
    std::vector<BrowseList::Item> items;
    items.reserve(channels.size());
    for (ConvRef r : channels) {
        const model::Conversation &c     = store.conversation(r);
        // The topic, else the description (Slack's directory does the same).
        const std::string         &about = c.topic.empty() ? c.purpose : c.topic;
        BrowseList::Item           it;
        it.id    = c.id;
        it.title = c.name;
        if (c.memberCount > 0) {
            it.subtitle = i18n::arg(
                tr("%1 %2"),
                str::number(c.memberCount),
                c.memberCount == 1 ? tr("member") : tr("members")
            );
            if (!about.empty())
                it.subtitle += " \xC2\xB7 " + about;
        } else {
            it.subtitle = about;
        }
        it.titleIcon = uint16_t(convIcon(c));
        if (c.member)
            it.badge = tr("Joined");
        it.searchKey = utf8::foldCase(str::concat({c.name, " ", about}));
        items.push_back(std::move(it));
    }
    return items;
}

std::vector<BrowseList::Item> peopleItems(const model::Store &store) {
    // Strangers surface only because they share a channel with us, but
    // conversations.open rejects them: a dead end as a DM target.
    std::vector<UserRef> people;
    for (UserRef u = 0; u < store.userCount(); ++u) {
        const model::User &x = store.user(u);
        if (!x.deleted && !x.stranger && !x.placeholder && !x.id.empty())
            people.push_back(u);
    }
    std::vector<std::string> keys(store.userCount());
    for (UserRef u : people)
        keys[u] = utf8::foldCase(
            store.user(u).displayName.empty() ? store.user(u).name : store.user(u).displayName
        );
    std::stable_sort(people.begin(), people.end(), [&](UserRef a, UserRef b) {
        return keys[a] < keys[b];
    });
    std::vector<BrowseList::Item> items;
    items.reserve(people.size());
    for (UserRef r : people) {
        const model::User &u    = store.user(r);
        const std::string &name = u.displayName.empty() ? u.name : u.displayName;
        BrowseList::Item   it;
        it.id     = u.id;
        it.title  = name;
        it.avatar = u.avatar;
        if (!u.name.empty() && u.name != name)
            it.subtitle = "@" + u.name;
        it.searchKey = screens::userSearchKey(u);
        items.push_back(std::move(it));
    }
    return items;
}

std::string channelName(std::string_view typed) {
    std::string_view t = str::trim(typed);
    if (!t.empty() && t[0] == '#') // typed as it shows
        t = str::trim(t.substr(1));
    std::string s = utf8::foldCase(t);
    std::replace(s.begin(), s.end(), ' ', '-');
    return s;
}

// ── Entry points ────────────────────────────────────────────────────────────

Popup *showFindChannel(
    screens::Context            &ctx,
    Window                      &w,
    Avatars                     &avatars,
    int                          tab,
    std::function<void(ConvRef)> channel,
    std::function<void(UserRef)> person,
    std::function<void()>        create
) {
    auto d = std::make_unique<ChannelFinder>(
        ctx, avatars, std::move(channel), std::move(person), std::move(create)
    );
    auto *raw = d.get();
    if (tab == 1)
        raw->selectTab(1);
    w.showPopup(std::move(d));
    raw->search().edit().focus();
    return raw;
}

namespace {

// "Add people to #name": a search over the workspace's people; a pick is
// done(u) (the dialog closed).
class AddPeopleDialog final : public Dialog {
public:
    AddPeopleDialog(
        screens::Context &ctx, Avatars &avatars, ConvRef conv, std::function<void(UserRef)> done
    )
        : Dialog(i18n::arg(tr("Add people to #%1"), ctx.store().conversation(conv).name), 520),
          _ctx(ctx), _done(std::move(done)) {
        _search = content()->add<TextField>(
            tr("Search for people"), TextField::Size::Normal, uint16_t(Icon::Search)
        );
        auto *box = content()->add<View>();
        box->style().stack().height(kVisibleRows * BrowseList::kRowH).noShrink();
        _list                                = box->add<BrowseList>(avatars);
        // Not me: Slack refuses to invite yourself.
        std::vector<BrowseList::Item> people = peopleItems(ctx.store);
        const model::Store           &st     = ctx.store();
        if (st.me != model::kNoUser)
            std::erase_if(people, [&](const BrowseList::Item &it) {
                return it.id == st.user(st.me).id;
            });
        _list->setItems(std::move(people));
        _list->onActivated = [this](const std::string &id) {
            const UserRef u    = _ctx.store().findUser(id);
            auto          done = _done;
            accept();
            if (u != model::kNoUser && done)
                done(u);
        };
        _search->edit().onChange = [this] { _list->applyFilter(_search->text()); };
        _search->edit().onKey    = [this](const Event &e) { return _list->navKey(e); };
    }
    TextField &search() { return *_search; }

private:
    screens::Context            &_ctx;
    std::function<void(UserRef)> _done;
    TextField                   *_search = nullptr;
    BrowseList                  *_list   = nullptr;
};

// A labelled field of the details dialog.
TextField *detailsField(View *parent, const char *label, const std::string &value, size_t max) {
    styledLabel(parent, tr(label), pxFont(15, text::Weight::Bold, color(C::FormText)));
    auto *f = parent->add<TextField>(std::string(), TextField::Size::Normal);
    f->setMaxLength(max);
    f->setText(value);
    return f;
}

} // namespace

Popup *showChannelDetails(
    screens::Context                        &ctx,
    Window                                  &w,
    Avatars                                 &avatars,
    ConvRef                                  conv,
    std::function<void(const std::string &)> onError,
    std::function<void(ConvRef)>             archiving
) {
    if (conv >= ctx.store().conversationCount() || ctx.store().conversation(conv).isDirect())
        return nullptr;
    const model::Conversation &cv = ctx.store().conversation(conv);
    // What it opened with: Save sends only what was edited here, so a topic
    // someone else set meanwhile isn't put back. The workspace and the id:
    // the dialog outlives a workspace switch, and refs are per Store.
    struct Opened {
        const model::Store *store;
        std::string         id, name, topic, purpose;
    };
    const auto was =
        std::make_shared<Opened>(Opened{&ctx.store(), cv.id, cv.name, cv.topic, cv.purpose});
    // The conversation now, or kNoConv when the workspace changed.
    auto now = [&ctx, was] {
        return &ctx.store() == was->store ? was->store->findConversation(was->id) : model::kNoConv;
    };
    auto       d       = std::make_unique<Dialog>("#" + cv.name, 520);
    View      *body    = d->content();
    TextField *name    = detailsField(body, N_("Channel name"), cv.name, 80);
    TextField *topic   = detailsField(body, N_("Topic"), cv.topic, 250);
    TextField *purpose = detailsField(body, N_("Description"), cv.purpose, 250);
    auto      *more    = body->add<View>();
    more->style().row().spacing(8).margins(0, 8, 0, 0);
    auto *add = more->add<Button>(tr("Add people\xE2\x80\xA6"), V::Secondary, Button::Form::Normal);
    auto *archive =
        more->add<Button>(tr("Archive channel\xE2\x80\xA6"), V::Danger, Button::Form::Normal);
    auto *save = Dialog::makeButton(tr("Save"), V::Primary);
    d->addButtonRow(save, Dialog::makeButton(tr("Cancel"), V::Secondary));
    Dialog *raw    = d.get();
    // Each change on its own call; a failure names what it was.
    auto    failed = [onError](const char *what) {
        return [onError, what](bool ok, const std::string &err) {
            if (!ok && onError)
                onError(i18n::arg(tr(what), err));
        };
    };
    // Read the fields before accept() closes the dialog.
    save->onClick = [raw, name, topic, purpose, &ctx, was, now, failed] {
        const std::string n = channelName(name->text());
        const std::string t(str::trim(topic->text()));
        const std::string p(str::trim(purpose->text()));
        raw->accept();
        const ConvRef conv = now();
        if (conv == model::kNoConv)
            return;
        if (!n.empty() && n != was->name)
            ctx.backend.renameChannel(conv, n, failed(N_("Couldn't rename the channel: %1")));
        if (t != was->topic)
            ctx.backend.setChannelTopic(conv, t, failed(N_("Couldn't set the topic: %1")));
        if (p != was->purpose)
            ctx.backend.setChannelDescription(
                conv, p, failed(N_("Couldn't set the description: %1"))
            );
    };
    name->onReturn = topic->onReturn = purpose->onReturn = save->onClick;
    add->onClick                                         = [raw, &ctx, &w, &avatars, now, failed] {
        raw->reject();
        const ConvRef conv = now();
        if (conv == model::kNoConv)
            return;
        auto p =
            std::make_unique<AddPeopleDialog>(ctx, avatars, conv, [&ctx, now, failed](UserRef u) {
                if (const ConvRef c = now(); c != model::kNoConv)
                    ctx.backend.addToChannel(c, u, failed(N_("Couldn't add them: %1")));
            });
        auto *search = &p->search();
        w.showPopup(std::move(p));
        search->edit().focus();
    };
    archive->onClick = [raw, &ctx, &w, was, now, failed, archiving] {
        raw->reject();
        auto ask = std::make_unique<Dialog>(i18n::arg(tr("Archive #%1?"), was->name), 440);
        styledLabel(
            ask->content(),
            tr("No one can post in an archived channel. Its history stays and can still be "
               "searched; a workspace admin can bring it back."),
            pxFont(15, text::Weight::Regular, color(C::FormText))
        );
        auto *yes = Dialog::makeButton(tr("Archive channel"), V::Danger);
        ask->addButtonRow(yes, Dialog::makeButton(tr("Cancel"), V::Secondary));
        Dialog *q    = ask.get();
        yes->onClick = [q, &ctx, now, failed, archiving] {
            q->accept();
            const ConvRef conv = now();
            if (conv == model::kNoConv)
                return;
            if (archiving) // off it first, as when leaving
                archiving(conv);
            ctx.backend.archiveChannel(conv, failed(N_("Couldn't archive the channel: %1")));
        };
        w.showPopup(std::move(ask));
    };
    w.showPopup(std::move(d));
    name->edit().focus();
    return raw;
}

Popup *showCreateChannel(
    Window &w, const std::string &workspaceName, std::function<void(const std::string &, bool)> done
) {
    auto  d   = std::make_unique<CreateChannel>(workspaceName, std::move(done));
    auto *raw = d.get();
    w.showPopup(std::move(d));
    raw->name().edit().focus();
    return raw;
}

} // namespace shell

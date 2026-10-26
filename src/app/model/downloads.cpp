#include "app/model/downloads.h"

#include "app/model/jobs.h"
#include "base/file.h"
#include "base/json.h"
#include "base/time.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace model {

namespace {

const char *const kStateNames[] = {"running", "done", "failed", "cancelled"};

Downloads::State stateNamed(std::string_view s) {
    for (size_t i = 0; i < 4; ++i)
        if (s == kStateNames[i])
            return Downloads::State(i);
    return Downloads::State::Failed;
}

std::string serialize(const std::vector<Downloads::Item> &items) {
    json::Writer w;
    w.beginArray();
    for (const Downloads::Item &it : items) {
        w.beginObject();
        w.key("name").value(it.name);
        w.key("path").value(it.path);
        w.key("size").value(it.total);
        w.key("at").value(it.at);
        w.key("state").value(kStateNames[size_t(it.state)]);
        if (!it.error.empty())
            w.key("error").value(it.error);
        w.endObject();
    }
    w.endArray();
    return w.take();
}

} // namespace

// Saves run on workers, possibly out of order: each carries a number, and
// one older than what is on disk already writes nothing.
struct Downloads::Saver {
    std::mutex m;
    uint64_t   queued = 0, written = 0;
};

int Downloads::begin(std::string name, std::string path, int64_t size) {
    Item it;
    it.id    = _nextId++;
    it.name  = std::move(name);
    it.path  = std::move(path);
    it.total = size > 0 ? size : 0;
    it.at    = base::nowSecs();
    _items.insert(_items.begin(), std::move(it));
    trim();
    changed(true);
    return _items.front().id;
}

void Downloads::setStop(int id, std::function<void()> stop) {
    if (Item *it = at(id); it && it->state == State::Running)
        it->stop = std::move(stop);
}

void Downloads::progress(int id, int64_t received, int64_t total) {
    Item *it = at(id);
    if (!it || it->state != State::Running)
        return;
    it->received = received;
    if (total > 0)
        it->total = total;
    changed(false);
}

void Downloads::finish(int id, bool ok, std::string error) {
    Item *it = at(id);
    if (!it || it->state != State::Running)
        return;
    it->stop = nullptr;
    if (ok) {
        it->state = State::Done;
        if (it->total <= 0)
            it->total = it->received;
        it->received = it->total;
        it->error.clear();
    } else {
        it->state = error == "cancelled" ? State::Cancelled : State::Failed;
        it->error = std::move(error);
    }
    trim();
    changed(true);
}

void Downloads::cancel(int id) {
    Item *it = at(id);
    if (!it || it->state != State::Running)
        return;
    if (std::function<void()> stop = std::exchange(it->stop, nullptr))
        stop(); // answers finish(id, false, "cancelled"), or will
    else
        finish(id, false, "cancelled"); // can't be stopped: let it run, forget it
}

void Downloads::remove(int id) {
    cancel(id);
    for (size_t i = 0; i < _items.size(); ++i)
        if (_items[i].id == id) {
            _items.erase(_items.begin() + ptrdiff_t(i));
            changed(true);
            return;
        }
}

void Downloads::clearFinished() {
    const size_t n = _items.size();
    std::erase_if(_items, [](const Item &it) { return it.state != State::Running; });
    if (_items.size() != n)
        changed(true);
}

const Downloads::Item *Downloads::find(int id) const {
    for (const Item &it : _items)
        if (it.id == id)
            return &it;
    return nullptr;
}

Downloads::Item *Downloads::at(int id) {
    return const_cast<Item *>(find(id));
}

size_t Downloads::running() const {
    size_t n = 0;
    for (const Item &it : _items)
        n += it.state == State::Running;
    return n;
}

bool Downloads::progressOf(int64_t *received, int64_t *total) const {
    *received = *total = 0;
    for (const Item &it : _items)
        if (it.state == State::Running && it.total > 0) {
            *received += std::min(it.received, it.total);
            *total += it.total;
        }
    return *total > 0;
}

bool Downloads::reserved(std::string_view path) const {
    for (const Item &it : _items)
        if (it.state == State::Running && it.path == path)
            return true;
    return false;
}

void Downloads::trim() {
    // The oldest finished ones go; running ones always stay.
    for (size_t i = _items.size(); i-- > 0 && _items.size() > kMax;)
        if (_items[i].state != State::Running)
            _items.erase(_items.begin() + ptrdiff_t(i));
}

void Downloads::changed(bool save) {
    if (save && _loading)
        _dirty = true;
    else if (save && _app && !_file.empty()) {
        auto              text  = std::make_shared<std::string>(serialize(_items));
        const uint64_t    seq   = ++_saver->queued;
        auto              saver = _saver;
        const std::string path  = _file;
        runInBackground(
            *_app,
            [saver, seq, text, path] {
                std::lock_guard lock(saver->m);
                if (seq <= saver->written)
                    return;
                file::makeDirs(file::dirName(path));
                if (file::writeAtomic(path, *text))
                    saver->written = seq;
            },
            [] {}
        );
    }
    _observers.notify();
}

void Downloads::setFile(plat::App &app, std::string path) {
    _app        = &app;
    _file       = std::move(path);
    _saver      = std::make_shared<Saver>();
    _loading    = true;
    _dirty      = false;
    auto loaded = std::make_shared<std::vector<Item>>();
    runInBackground(
        app,
        [loaded, path = _file] {
            json::Document doc;
            if (!doc.parseFile(path))
                return;
            for (const json::Value v : doc.root()) {
                Item it;
                it.name  = json::owned(v["name"]);
                it.path  = json::owned(v["path"]);
                it.error = json::owned(v["error"]);
                it.total = it.received = v["size"].integer();
                it.at                  = v["at"].integer();
                it.state               = stateNamed(v["state"].str());
                if (it.state == State::Running) { // msga quit before it was in
                    it.state    = State::Failed;
                    it.error    = "interrupted";
                    it.received = 0;
                }
                if (!it.name.empty())
                    loaded->push_back(std::move(it));
            }
        },
        [this, loaded] {
            // After what began meanwhile (newer than anything saved).
            for (Item &it : *loaded) {
                it.id = _nextId++;
                _items.push_back(std::move(it));
            }
            trim();
            _loading = false;
            changed(std::exchange(_dirty, false));
            checkFiles();
        }
    );
}

void Downloads::checkFiles() {
    if (!_app)
        return;
    struct Check {
        int         id;
        std::string path;
        bool        missing = false;
    };
    auto checks = std::make_shared<std::vector<Check>>();
    for (const Item &it : _items)
        if (it.state == State::Done)
            checks->push_back({it.id, it.path});
    if (checks->empty())
        return;
    runInBackground(
        *_app,
        [checks] {
            for (Check &c : *checks)
                c.missing = !file::exists(c.path);
        },
        [this, checks] {
            bool any = false;
            for (const Check &c : *checks)
                if (Item *it = at(c.id); it && it->missing != c.missing) {
                    it->missing = c.missing;
                    any         = true;
                }
            if (any)
                changed(false);
        }
    );
}

Downloads::ObserverId Downloads::observe(std::function<void()> fn) {
    return _observers.add([fn = std::move(fn)](const std::string &) { fn(); });
}

void Downloads::reset() {
    _items.clear();
    _app = nullptr;
    _file.clear();
    _saver.reset();
    _nextId  = 1;
    _loading = _dirty = false;
    _observers.notify();
}

Downloads &downloads() {
    static Downloads d;
    return d;
}

} // namespace model

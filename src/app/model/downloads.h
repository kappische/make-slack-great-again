// The downloads list: the files the user downloaded, newest first, as the
// sidebar's Downloads popover shows them (running with their progress, then
// done, failed or cancelled), kept across restarts (the newest kMax).
//
//   const int id = model::downloads().begin("report.pdf", "/home/me/Downloads/report.pdf", 0);
//   model::downloads().setStop(id, stop);           // a transfer that can be stopped
//   model::downloads().progress(id, got, total);    // as the bytes come
//   model::downloads().finish(id, ok, error);       // "cancelled" → Cancelled
//
// The list only records: screens/common/downloads.h starts the transfers.
// UI thread only; observers run after every change (progress included).
#pragma once

#include "base/observers.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace plat {
class App;
}

namespace model {

class Downloads {
public:
    enum class State : uint8_t { Running, Done, Failed, Cancelled };
    struct Item {
        int                   id = 0;
        std::string           name;                    // the file's name, as the list shows it
        std::string           path;                    // where it is (or is going)
        std::string           source;                  // what it came from (a URL); "" = unknown
        std::string           error;                   // Failed: why ("http 404"), for the log
        int64_t               received = 0, total = 0; // bytes; total 0 = not known yet
        int64_t               at      = 0;             // when it started, epoch secs
        State                 state   = State::Running;
        bool                  missing = false; // Done, but no file at `path` any more (checkFiles)
        std::function<void()> stop;            // Running: ends it (setStop); not saved
        // Running: told once it is done, failed or cancelled (whenDone); not saved.
        std::function<void(const Item &)> waiter;
    };
    static constexpr size_t kMax = 50;

    // A new running download at the top; returns its id. Older finished ones
    // past kMax drop off the end.
    int  begin(std::string name, std::string path, int64_t size, std::string source = {});
    // How to stop it (Backend::downloadFileTracked's answer); null = it can't be.
    void setStop(int id, std::function<void()> stop);
    void progress(int id, int64_t received, int64_t total);
    // ok: Done (received = total = the size if it was known); else Failed, or
    // Cancelled for error "cancelled". Unknown or finished ids are ignored.
    void finish(int id, bool ok, std::string error = {});
    // `fn` once a running one finishes (after the observers; several chain).
    // A finished or unknown id: never.
    void whenDone(int id, std::function<void(const Item &)> fn);
    // The stop of a running one (which answers finish(…, "cancelled")).
    void cancel(int id);
    // Off the list (the file stays where it is); a running one is stopped.
    void remove(int id);
    // Every finished one off the list; running ones stay.
    void clearFinished();

    const std::vector<Item> &items() const { return _items; }
    const Item              *find(int id) const;
    // The newest download of `source`, whatever became of it; null for none.
    const Item              *latest(std::string_view source) const;
    size_t                   running() const;
    // The sum over running downloads whose size is known: for the footer's
    // ring. False when none says its size.
    bool                     progressOf(int64_t *received, int64_t *total) const;
    // A running download is writing there (a new one picks another name).
    bool                     reserved(std::string_view path) const;

    // Where the list is kept (<dataDir>/downloads.json): read now (on a
    // worker; observers run once it is in), written after every change but
    // progress. Downloads that were running when msga quit come back failed.
    // Without it (tests) nothing is read or written.
    void setFile(plat::App &app, std::string path);
    // Done items' files looked for again (on a worker): moved or deleted
    // ones show as such. The popover asks each time it opens.
    void checkFiles();

    using ObserverId = base::Observers::Id;
    ObserverId observe(std::function<void()> fn);
    void       unobserve(ObserverId id) { _observers.remove(id); }

    // Tests: an empty list, no file, ids from 1 again.
    void reset();

private:
    Item *at(int id);
    void  changed(bool save);
    void  trim();

    struct Saver;
    std::vector<Item>      _items; // newest first
    base::Observers        _observers;
    plat::App             *_app = nullptr;
    std::string            _file;
    std::shared_ptr<Saver> _saver;
    int                    _nextId  = 1;
    // Reading the file: saves wait (one now would write the list without
    // what is still being read), and one follows if anything changed.
    bool                   _loading = false, _dirty = false;
};

// The app's one list.
Downloads &downloads();

} // namespace model

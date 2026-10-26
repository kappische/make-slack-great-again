// Files the screens fetch for the user: the
// file bar's Download, "Copy full image", the CSV preview, Forward.
//
//   fetchFile(ctx, file.source(), path, [](bool ok, const std::string &err) {…});
//
// A local path (a pending upload, the demo, a Claude Code file) is copied on
// a worker thread; an http(s) URL goes to the backend, which downloads it
// with the service's credentials and writes it off the UI thread too. done
// runs on the UI thread, never inside the call. Register the wait with
// model::jobs() (app/model/jobs.h) around it.
#pragma once

#include "app/model/backend.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace plat {
class App;
}

namespace screens {

void fetchFile(
    plat::App           &app,
    model::Backend      &backend,
    const std::string   &source,
    std::string          toPath,
    model::Backend::Done done
);

// The file bar's Download, the way Slack does it: no dialog, `source` into
// the Downloads folder (home without one) under `name` — a free name,
// "report (1).pdf", when it is taken — listed in model::downloads() with its
// progress until it is in (`size`: what the service says, 0 = unknown).
void downloadToFolder(
    plat::App &app, model::Backend &backend, std::string name, std::string source, int64_t size
);
// A free path for `name` in `dir`: dir/name, else "stem (1).ext", "stem
// (2).ext", … — one that isn't on disk and that `taken` doesn't claim. Reads
// the disk: a worker's.
std::string freeDownloadPath(
    std::string_view dir, std::string_view name, const std::vector<std::string> &taken
);

// A fresh path for a downloaded copy of `name` under the OS temp directory
// (<Temp>/msga/downloads/<random>/<name>, the directory created), so
// a viewer or an upload can open it under its own name; "" if none.
std::string tempDownloadPath(plat::App &app, std::string_view name);
// Removes such a copy and the directory made for it.
void        removeTempDownload(const std::string &path);

} // namespace screens

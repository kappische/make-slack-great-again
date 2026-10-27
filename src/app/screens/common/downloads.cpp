#include "app/screens/common/downloads.h"

#include "app/model/downloads.h"
#include "app/model/jobs.h"
#include "app/screens/common/context.h"
#include "app/screens/common/remote_images.h"
#include "base/crypto.h"
#include "base/file.h"
#include "base/log.h"
#include "base/str.h"
#include "plat/plat.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace screens {

void fetchFile(
    plat::App           &app,
    model::Backend      &backend,
    const std::string   &source,
    std::string          toPath,
    model::Backend::Done done
) {
    if (RemoteImages::isRemote(source)) {
        backend.downloadFile(source, std::move(toPath), std::move(done));
        return;
    }
    auto ok = std::make_shared<bool>(false);
    model::runInBackground(
        app,
        [ok, from = source, toPath = std::move(toPath)] {
            *ok = from == toPath || file::copy(from, toPath);
        },
        [ok, done = std::move(done)] {
            if (done)
                done(*ok, *ok ? std::string() : std::string("copy_failed"));
        }
    );
}

std::string safeFileName(std::string_view name) {
    std::string out(file::baseName(name));
    for (char &c : out)
        if (std::strchr("\\/:*?\"<>|", c) || uint8_t(c) < 0x20)
            c = '_';
    // Windows drops trailing dots and spaces, and won't make a file named
    // after a device ("con", "nul.txt", "COM1").
    while (!out.empty() && (out.back() == '.' || out.back() == ' '))
        out.pop_back();
    const std::string stem = str::asciiLower(out.substr(0, out.find('.')));
    const bool numbered    = stem.size() == 4 &&
                             (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) &&
                             stem[3] >= '1' && stem[3] <= '9';
    if (numbered || stem == "con" || stem == "prn" || stem == "aux" || stem == "nul")
        out.insert(0, 1, '_');
    return out.empty() ? std::string("file") : out;
}

std::string freeDownloadPath(
    std::string_view dir, std::string_view name, const std::vector<std::string> &taken
) {
    // "archive.tar.gz" (1) is "archive.tar (1).gz", as browsers name it.
    const std::string_view ext  = file::extension(name);
    const std::string_view stem = ext.empty() ? name : name.substr(0, name.size() - ext.size() - 1);
    for (int n = 0; n < 1000; ++n) {
        std::string base(stem);
        if (n > 0)
            base += str::concat({" (", str::number(n), ")"});
        if (!ext.empty())
            base += str::concat({".", ext});
        std::string path = file::join(dir, base);
        if (!file::exists(path) && std::find(taken.begin(), taken.end(), path) == taken.end())
            return path;
    }
    return {};
}

void downloadToFolder(
    plat::App               &app,
    model::Backend          &backend,
    std::string              name,
    std::string              source,
    int64_t                  size,
    std::function<void(int)> started
) {
    const std::string base = safeFileName(name);
    std::string       dir  = app.standardDir(plat::StandardDir::Downloads);
    if (dir.empty())
        dir = app.standardDir(plat::StandardDir::Home);
    if (dir.empty() || source.empty()) { // listed as failed: whoever waits hears of it
        const int id = model::downloads().begin(base, {}, size, source);
        if (started)
            started(id);
        model::downloads().finish(id, false, "no_folder");
        return;
    }
    // What running downloads are writing, as of now: the worker skips those.
    std::vector<std::string> taken;
    for (const model::Downloads::Item &it : model::downloads().items())
        if (it.state == model::Downloads::State::Running)
            taken.push_back(it.path);
    auto path = std::make_shared<std::string>();
    model::runInBackground(
        app,
        [path, dir, base, taken = std::move(taken)] {
            if (file::makeDirs(dir))
                *path = freeDownloadPath(dir, base, taken);
        },
        [&app, &backend, path, base, source = std::move(source), size, started] {
            if (path->empty()) {
                const int id = model::downloads().begin(base, {}, size, source);
                if (started)
                    started(id);
                model::downloads().finish(id, false, "no_folder");
                return;
            }
            if (model::downloads().reserved(*path)) { // another one took it meanwhile
                downloadToFolder(app, backend, base, source, size, started);
                return;
            }
            const int id = model::downloads().begin(base, *path, size, source);
            if (started)
                started(id);
            auto done = [id](bool ok, const std::string &err) {
                if (!ok && err != "cancelled")
                    LOG_WARN("downloads", "Download failed: %s", err.c_str());
                model::downloads().finish(id, ok, err);
            };
            if (!RemoteImages::isRemote(source)) { // a local file: copied
                fetchFile(app, backend, source, *path, done);
                return;
            }
            std::function<void()> stop = backend.downloadFileTracked(
                source,
                *path,
                [id](int64_t got, int64_t total) { model::downloads().progress(id, got, total); },
                done
            );
            model::downloads().setStop(id, std::move(stop));
        }
    );
}

void openFromDownloads(
    Context                                     &ctx,
    std::string                                  name,
    std::string                                  source,
    int64_t                                      size,
    std::function<void(model::Downloads::State)> done
) {
    using S   = model::Downloads::State;
    auto open = [&ctx, done](const model::Downloads::Item &it) {
        if (it.state == S::Done && ctx.openUrl)
            ctx.openUrl(file::toFileUrl(it.path));
        if (done)
            done(it.state);
    };
    if (!RemoteImages::isRemote(source)) { // already on this disk
        if (ctx.openUrl)
            ctx.openUrl(file::toFileUrl(source));
        if (done)
            done(S::Done);
        return;
    }
    const model::Downloads::Item *it = model::downloads().latest(source);
    if (it && it->state == S::Running) { // pressed again meanwhile: the same download
        model::downloads().whenDone(it->id, open);
        return;
    }
    auto fresh = [&ctx, name, source, size, open] {
        downloadToFolder(ctx.app.platform(), ctx.backend, name, source, size, [open](int id) {
            model::downloads().whenDone(id, open);
        });
    };
    if (!it || it->state != S::Done) {
        fresh();
        return;
    }
    // Downloaded before: that copy, if it is still where it was put.
    auto       here = std::make_shared<bool>(false);
    const auto kept = std::make_shared<model::Downloads::Item>(*it);
    kept->stop      = nullptr;
    kept->waiter    = nullptr;
    model::runInBackground(
        ctx.app.platform(),
        [here, path = kept->path] { *here = file::size(path) > 0; },
        [here, kept, open, fresh] {
            if (*here)
                open(*kept);
            else
                fresh();
        }
    );
}

std::string tempDownloadPath(plat::App &app, std::string_view name) {
    const std::string tmp = app.standardDir(plat::StandardDir::Temp);
    if (tmp.empty())
        return {};
    const std::string rnd = crypto::randomHex(8);
    if (rnd.empty())
        return {};
    const std::string dir = file::join(file::join(tmp, "msga/downloads"), rnd);
    if (!file::makeDirs(dir))
        return {};
    // Only the last path component, and never "" / "." / "..".
    std::string base(file::baseName(name));
    if (base.empty() || base == "." || base == "..")
        base = "file";
    return file::join(dir, base);
}

void removeTempDownload(const std::string &path) {
    file::remove(path);
    file::remove(file::dirName(path));
}

} // namespace screens

#include "app/screens/common/downloads.h"

#include "app/model/downloads.h"
#include "app/model/jobs.h"
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
    plat::App &app, model::Backend &backend, std::string name, std::string source, int64_t size
) {
    std::string dir = app.standardDir(plat::StandardDir::Downloads);
    if (dir.empty())
        dir = app.standardDir(plat::StandardDir::Home);
    if (dir.empty() || source.empty())
        return;
    // Only the last path component, safe in any OS's folder.
    std::string base(file::baseName(name));
    for (char &c : base)
        if (std::strchr("\\/:*?\"<>|", c) || uint8_t(c) < 0x20)
            c = '_';
    if (base.empty() || base == "." || base == "..")
        base = "file";
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
        [&app, &backend, path, base, source = std::move(source), size] {
            if (path->empty()) {
                const int id = model::downloads().begin(base, {}, size);
                model::downloads().finish(id, false, "no_folder");
                return;
            }
            if (model::downloads().reserved(*path)) { // another one took it meanwhile
                downloadToFolder(app, backend, base, source, size);
                return;
            }
            const int id   = model::downloads().begin(base, *path, size);
            auto      done = [id](bool ok, const std::string &err) {
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

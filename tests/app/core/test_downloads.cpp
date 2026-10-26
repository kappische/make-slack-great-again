// model::Downloads (the sidebar footer's downloads list): states, progress,
// stops, the cap, and the list kept on disk across restarts.
#include "app/model/downloads.h"
#include "base/file.h"
#include "support/test.h"

#ifndef _WIN32
#include "support/fake_slack_server.h"
#endif

using model::Downloads;
using S = Downloads::State;

TEST("downloads: newest first, progress while running, then done, failed or cancelled") {
    Downloads d;
    int       notified = 0;
    d.observe([&] { ++notified; });
    const int a = d.begin("a.pdf", "/dl/a.pdf", 0);
    const int b = d.begin("b.zip", "/dl/b.zip", 2000);
    REQUIRE(d.items().size() == 2);
    CHECK_STR(d.items()[0].name, "b.zip");
    CHECK(d.running() == 2);
    CHECK(d.reserved("/dl/a.pdf"));
    CHECK_FALSE(d.reserved("/dl/c.pdf"));
    // Progress: the total from the server when the service gave none.
    d.progress(a, 100, 400);
    d.progress(b, 500, 0);
    CHECK(d.find(a)->received == 100 && d.find(a)->total == 400);
    CHECK(d.find(b)->total == 2000);
    int64_t got = 0, total = 0;
    CHECK(d.progressOf(&got, &total));
    CHECK(got == 600 && total == 2400);
    d.finish(a, true);
    CHECK(d.find(a)->state == S::Done && d.find(a)->received == 400);
    CHECK_FALSE(d.reserved("/dl/a.pdf"));
    d.progress(a, 1, 1); // finished: ignored
    CHECK(d.find(a)->received == 400);
    d.finish(a, false, "late"); // finished: ignored
    CHECK(d.find(a)->state == S::Done);
    d.finish(b, false, "http 404");
    CHECK(d.find(b)->state == S::Failed);
    CHECK_STR(d.find(b)->error, "http 404");
    const int c = d.begin("c.txt", "/dl/c.txt", 0);
    d.finish(c, false, "cancelled");
    CHECK(d.find(c)->state == S::Cancelled);
    CHECK(d.running() == 0);
    CHECK_FALSE(d.progressOf(&got, &total));
    CHECK(notified == 8); // the ignored calls notify nobody
}

TEST("downloads: cancel uses the stop; remove and clear keep running ones") {
    Downloads d;
    const int a     = d.begin("a.bin", "/dl/a.bin", 0);
    int       stops = 0;
    // A stop answers the download as Backend::downloadFileTracked's does.
    d.setStop(a, [&] {
        ++stops;
        d.finish(a, false, "cancelled");
    });
    d.cancel(a);
    CHECK(stops == 1);
    CHECK(d.find(a)->state == S::Cancelled);
    d.cancel(a); // finished: nothing
    CHECK(stops == 1);
    // No stop (a local copy): it is let run and marked cancelled.
    const int b = d.begin("b.bin", "/dl/b.bin", 0);
    d.cancel(b);
    CHECK(d.find(b)->state == S::Cancelled);
    d.finish(b, true); // its late answer changes nothing
    CHECK(d.find(b)->state == S::Cancelled);
    // Remove: off the list; a running one is stopped first.
    const int c       = d.begin("c.bin", "/dl/c.bin", 0);
    bool      stopped = false;
    d.setStop(c, [&] {
        stopped = true;
        d.finish(c, false, "cancelled");
    });
    d.remove(c);
    CHECK(stopped);
    CHECK(d.find(c) == nullptr);
    const int e = d.begin("e.bin", "/dl/e.bin", 0);
    d.clearFinished();
    REQUIRE(d.items().size() == 1);
    CHECK(d.items()[0].id == e);
}

TEST("downloads: whenDone tells each waiter once; latest finds a source's newest download") {
    Downloads                d;
    const int                a = d.begin("v.mp4", "/dl/v.mp4", 0, "https://f/v.mp4");
    std::vector<std::string> told;
    d.whenDone(a, [&](const Downloads::Item &it) {
        CHECK(it.state == S::Done);
        CHECK(d.find(it.id) != nullptr); // after the list changed
        told.push_back("first");
    });
    d.whenDone(a, [&](const Downloads::Item &) { told.push_back("second"); });
    CHECK(d.latest("https://f/v.mp4")->id == a);
    CHECK(d.latest("https://f/other.mp4") == nullptr);
    CHECK(d.latest("") == nullptr);
    d.finish(a, true);
    REQUIRE(told.size() == 2);
    CHECK_STR(told[0], "first");
    CHECK_STR(told[1], "second");
    d.finish(a, true); // finished: nobody is told again
    d.whenDone(a, [&](const Downloads::Item &) { told.push_back("late"); });
    CHECK(told.size() == 2);
    // A newer one of the same source is the latest; a cancel tells its waiter.
    const int b = d.begin("v (1).mp4", "/dl/v (1).mp4", 0, "https://f/v.mp4");
    CHECK(d.latest("https://f/v.mp4")->id == b);
    S got = S::Running;
    d.whenDone(b, [&](const Downloads::Item &it) { got = it.state; });
    d.cancel(b);
    CHECK(got == S::Cancelled);
}

TEST("downloads: the list keeps the newest finished ones, never dropping a running one") {
    Downloads d;
    const int first = d.begin("first", "/dl/first", 0); // stays running
    for (size_t i = 0; i < Downloads::kMax + 5; ++i)
        d.finish(d.begin("f", "/dl/f", 0), true);
    CHECK(d.items().size() == Downloads::kMax);
    CHECK(d.find(first) != nullptr);
    CHECK(d.items().back().id == first);
}

#ifndef _WIN32
TEST("downloads: saved after each change; read back with interrupted ones failed") {
    plat::App        &app  = fakeslack::app();
    const std::string path = file::join(app.standardDir(plat::StandardDir::Cache), "dl.json");
    file::remove(path);
    {
        Downloads d;
        d.setFile(app, path);
        fakeslack::pumpFor(30);
        const int a = d.begin("report.pdf", "/dl/report.pdf", 1234, "https://f/report.pdf");
        d.finish(a, true);
        d.begin("big.iso", "/dl/big.iso", 0); // still running at "quit"
        REQUIRE(
            fakeslack::pumpUntil(
                [&] {
                    std::string s;
                    return file::readAll(path, &s) && s.find("big.iso") != std::string::npos;
                },
                5000
            )
        );
    }
    Downloads d;
    d.setFile(app, path);
    // One that starts while the file is still being read: kept, and the
    // saved list doesn't lose the older ones for it.
    d.finish(d.begin("early.txt", "/dl/early.txt", 5), true);
    REQUIRE(fakeslack::pumpUntil([&] { return d.items().size() == 3; }, 5000));
    CHECK_STR(d.items()[0].name, "early.txt");
    CHECK_STR(d.items()[1].name, "big.iso");
    CHECK(d.items()[1].state == S::Failed);
    CHECK_STR(d.items()[1].error, "interrupted");
    CHECK(d.items()[2].state == S::Done && d.items()[2].total == 1234);
    CHECK_STR(d.items()[2].source, "https://f/report.pdf");
    // Its file isn't there: it shows as moved or deleted.
    REQUIRE(fakeslack::pumpUntil([&] { return d.items()[2].missing; }, 5000));
    REQUIRE(
        fakeslack::pumpUntil(
            [&] {
                std::string s;
                return file::readAll(path, &s) && s.find("early.txt") != std::string::npos &&
                       s.find("report.pdf") != std::string::npos;
            },
            5000
        )
    );
    file::remove(path);
}
#endif

// Tests for trail.hpp. Build & run:
//   clang++ -std=c++20 -Wall -Wextra tests/test_trail.cpp -o test_trail && ./test_trail
#include "../src/trail.hpp"

#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL line %d: %s\n", __LINE__, #cond);        \
            ++g_fail;                                                     \
        }                                                                 \
    } while (0)

using phantom::Attempt;
using phantom::LevelTrails;
using phantom::TrailRecorder;

int main() {
    std::puts("straight runs collapse to their end points");
    {
        TrailRecorder r;
        r.begin(false);
        for (int i = 0; i < 1000; ++i) r.step(i * 2.f, 105.f);  // running on the ground
        Attempt a = r.finish(true, false, 2000.f, 105.f, 40.0);
        CHECK(a.segments.size() == 1);
        CHECK(a.pointCount() == 2);
        CHECK(a.segments[0].front().x == 0.f);
        CHECK(a.segments[0].back().x == 2000.f);
        CHECK(a.died && a.deathX == 2000.f && a.endPct == 40.0);
    }

    std::puts("jumps keep their curve");
    {
        TrailRecorder r;
        r.begin(false);
        for (int i = 0; i < 200; ++i) {
            float x = i * 2.f;
            float y = 105.f + std::max(0.f, 60.f - (x - 200.f) * (x - 200.f) / 170.f);  // one arc
            r.step(x, y);
        }
        Attempt a = r.finish(false, true, 400.f, 105.f, 100.0);
        CHECK(a.pointCount() > 10);       // the arc is kept
        CHECK(a.pointCount() < 100);      // but not every sample
        float top = 0.f;
        for (auto const& p : a.segments[0]) top = std::max(top, p.y);
        CHECK(std::abs(top - 165.f) < 2.f);
    }

    std::puts("teleports start a new line");
    {
        TrailRecorder r;
        r.begin(false);
        for (int i = 0; i < 50; ++i) r.step(i * 2.f, 105.f);
        for (int i = 0; i < 50; ++i) r.step(500.f + i * 2.f, 400.f);
        Attempt a = r.finish(true, false, 600.f, 400.f, 10.0);
        CHECK(a.segments.size() == 2);
    }

    std::puts("clicks are kept with their buffered flag");
    {
        TrailRecorder r;
        r.begin(true);
        r.click(10.f, 105.f, false);
        r.click(20.f, 105.f, true);
        Attempt a = r.finish(true, false, 30.f, 105.f, 1.0);
        CHECK(a.clicks.size() == 2);
        CHECK(!a.clicks[0].buffered && a.clicks[1].buffered);
        CHECK(a.practice);
    }

    std::puts("best = furthest normal-mode attempt; recent keeps the last 5");
    {
        LevelTrails t;
        auto make = [](double pct, bool practice, bool completed = false) {
            Attempt a;
            a.endPct = pct;
            a.practice = practice;
            a.completed = completed;
            return a;
        };
        CHECK(t.add(make(20, false)));
        CHECK(!t.add(make(10, false)));
        CHECK(!t.add(make(90, true)));     // practice never counts as best
        CHECK(t.add(make(55, false)));
        CHECK(t.best->endPct == 55);
        for (int i = 0; i < 4; ++i) t.add(make(1, false));
        CHECK(t.recent.size() == LevelTrails::RECENT);
        CHECK(t.recent.front().endPct == 1);
        CHECK(t.add(make(100, false, true)));
        CHECK(t.best->completed);
    }

    std::printf(g_fail ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}

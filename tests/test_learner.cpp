// Simulated attempts against learner.hpp. Build & run:
//   clang++ -std=c++20 -Wall -Wextra tests/test_learner.cpp -o test && ./test
#include "../src/learner.hpp"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL line %d: %s\n", __LINE__, #cond);        \
            ++g_fail;                                                     \
        }                                                                 \
    } while (0)

// A tiny stand-in for GD: the player moves v units per physics step.
// It calls the learner in the same order main.cpp's hooks do.
struct Sim {
    std::vector<phantom::ClickSpot> spots;
    phantom::Learner L;
    double x = 0.0;
    double v = 2.0;
    std::vector<double> gdPressX;  // where GD actually received each press
    int gdReleases = 0;

    Sim() {
        L.spots = &spots;
        L.cfg.judgeSteps = 60;
    }
    double pct() const { return x / 10.0; }

    void gdPress() { gdPressX.push_back(x); }
    void gdRelease() { ++gdReleases; }

    // PlayerObject::pushButton hook
    void click() {
        if (L.hasHeldPress()) {
            gdPress();
            L.pressSent(x, pct());
        }
        if (L.takeRelease()) gdRelease();
        if (!L.onPress(x, pct(), true)) gdPress();
    }
    // PlayerObject::releaseButton hook
    void release() {
        if (!L.onRelease()) gdRelease();
    }
    // GJBaseGameLayer::processCommands hook; `inside` = GD applying input mid-step
    void step(std::function<void()> inside = {}, bool alive = true) {
        bool sendNow = L.stepStart(x);
        if (L.takeDueRelease()) gdRelease();
        if (sendNow) {
            gdPress();
            L.pressSent(x, pct());
        }
        if (inside) inside();
        x += v;
        L.stepEnd(x, alive);
    }
    void steps(int n) {
        for (int i = 0; i < n; ++i) step();
    }
    void runTo(double tx) {
        while (x < tx - 1e-9) step();
    }
    // PlayLayer::resetLevel hook
    void newAttempt() {
        if (L.takeRelease()) gdRelease();
        L.newAttempt();
        x = 0.0;
    }
    // tap at position px (between frames), hold `holdSteps`, release, live `after` steps
    void tapAt(double px, int holdSteps = 1, int after = 0) {
        runTo(px);
        click();
        steps(holdSteps);
        release();
        steps(after);
    }
    bool notesHave(char const* s) const {
        for (auto const& n : L.notes)
            if (n.find(s) != std::string::npos) return true;
        return false;
    }
};

// right tick = x 200; v = 2 units/tick
static void teach(Sim& s) {
    s.newAttempt();
    s.tapAt(196, 1, 20);          // 2 ticks early -> die
    s.L.died(true, s.pct());
    s.newAttempt();
    s.tapAt(200, 1, 70);          // on time -> survive the judge time
}

int main() {
    std::puts("learning");
    {
        Sim s;
        s.newAttempt();
        s.tapAt(196, 1, 20);
        std::string info = s.L.died(true, s.pct());
        CHECK(s.spots.size() == 1);
        CHECK(s.spots[0].totalMisses == 1);
        CHECK(s.spots[0].passes.empty());
        CHECK(!phantom::spotReady(s.spots[0], s.L.cfg));
        CHECK(info.find("get past this click once") != std::string::npos);

        s.newAttempt();
        s.tapAt(200, 1, 70);
        CHECK(s.spots[0].totalPasses == 1);
        CHECK(phantom::rightTick(s.spots[0]).value_or(-1) == 200.0);
        CHECK(phantom::spotReady(s.spots[0], s.L.cfg));
        CHECK(s.notesHave("[learned]"));
        CHECK(s.notesHave("buffer ON, 2 tick(s)"));
    }

    std::puts("buffer: click 2 ticks early between frames -> GD gets it on the right tick");
    {
        Sim s;
        teach(s);
        s.newAttempt();
        s.runTo(196);
        size_t before = s.gdPressX.size();
        int rel = s.gdReleases;
        s.click();
        CHECK(s.gdPressX.size() == before);       // the mod has it, GD doesn't
        s.steps(2);
        s.release();                              // let go while still held by the mod
        CHECK(s.gdReleases == rel);               // release held too
        s.step();                                 // reaches x = 200
        CHECK(s.gdPressX.size() == before + 1);
        CHECK(s.gdPressX.back() == 200.0);
        CHECK(s.gdReleases == rel);               // release comes a tick later
        s.step();
        CHECK(s.gdReleases == rel + 1);
        CHECK(s.notesHave("[buffer] caught a click 2 tick(s) early"));
        CHECK(s.spots[0].buffered == 1);
    }

    std::puts("buffer: click 1 tick early applied mid-step");
    {
        Sim s;
        teach(s);
        s.newAttempt();
        s.runTo(198);
        size_t before = s.gdPressX.size();
        s.step([&] { s.click(); });
        CHECK(s.gdPressX.size() == before);
        s.step();
        CHECK(s.gdPressX.size() == before + 1);
        CHECK(s.gdPressX.back() == 200.0);
    }

    std::puts("buffer leaves other clicks alone");
    {
        Sim s;
        teach(s);
        for (double px : {194.0, 200.0, 202.0, 120.0}) {  // 3 early, on time, late, elsewhere
            s.newAttempt();
            s.runTo(px);
            size_t before = s.gdPressX.size();
            s.click();
            CHECK(s.gdPressX.size() == before + 1);
            CHECK(s.gdPressX.back() == px);
            s.step();
            s.release();
        }
    }

    std::puts("never held longer than the ticks you chose (player slows down)");
    {
        Sim s;
        teach(s);
        s.newAttempt();
        s.runTo(196);
        s.click();               // held, aiming for x = 200
        s.v = 0.5;               // speed drops: x 196 -> 196.5 -> 197 ...
        size_t before = s.gdPressX.size();
        s.step();
        s.step();
        s.step();
        CHECK(s.gdPressX.size() == before + 1);
        CHECK(s.gdPressX.back() < 200.0);
    }

    std::puts("a new click while one is held sends the held one first");
    {
        Sim s;
        teach(s);
        s.newAttempt();
        s.runTo(196);
        s.click();
        s.release();
        size_t before = s.gdPressX.size();
        int rel = s.gdReleases;
        s.click();               // double tap before the right tick
        CHECK(s.gdPressX.size() >= before + 1);
        CHECK(s.gdReleases == rel + 1);
    }

    std::puts("re-learns when buffered clicks keep dying");
    {
        Sim s;
        teach(s);
        for (int i = 0; i < 2; ++i) {
            s.newAttempt();
            s.tapAt(196, 3, 10);  // caught, sent at 200 ... and still die
            s.L.died(true, s.pct());
        }
        CHECK(s.notesHave("[relearn]"));
        CHECK(s.spots[0].passes.empty());
        s.newAttempt();
        s.runTo(196);
        size_t before = s.gdPressX.size();
        s.click();
        CHECK(s.gdPressX.size() == before + 1);   // not buffered until re-learned
    }

    std::puts("judge time: a click long before the death isn't blamed");
    {
        Sim s;
        teach(s);
        int misses = s.spots[0].totalMisses;
        s.newAttempt();
        s.tapAt(200, 1, 100);     // > 60 steps alive -> graded as a pass
        std::string info = s.L.died(true, s.pct());
        CHECK(info == "no click just before it");
        CHECK(s.spots[0].totalMisses == misses);
        CHECK(s.spots[0].totalPasses == 2);
    }

    std::puts("practice deaths can be ignored");
    {
        Sim s;
        s.newAttempt();
        s.tapAt(196, 1, 10);
        CHECK(s.L.died(false, s.pct()).empty());
        CHECK(s.spots.empty());
    }

    std::puts("only clicks you've died to become spots");
    {
        Sim s;
        teach(s);
        s.newAttempt();
        s.tapAt(120, 1, 80);      // survived click somewhere else
        CHECK(s.spots.size() == 1);
    }

    std::puts("misses close together merge, far apart split");
    {
        Sim s;
        s.newAttempt();
        s.tapAt(196, 1, 5);
        s.L.died(true, s.pct());
        s.newAttempt();
        s.tapAt(204, 1, 5);
        s.L.died(true, s.pct());
        s.newAttempt();
        s.tapAt(300, 1, 5);
        s.L.died(true, s.pct());
        CHECK(s.spots.size() == 2);
        CHECK(s.spots[0].totalMisses == 2);
    }

    std::puts("min misses setting");
    {
        Sim s;
        s.L.cfg.minMisses = 3;
        teach(s);
        CHECK(!phantom::spotReady(s.spots[0], s.L.cfg));
        CHECK(s.notesHave("turns on at 3"));
        for (int i = 0; i < 2; ++i) {
            s.newAttempt();
            s.tapAt(197, 1, 5);
            s.L.died(true, s.pct());
        }
        CHECK(phantom::spotReady(s.spots[0], s.L.cfg));
    }

    std::puts("finishing the level counts pending clicks as passes");
    {
        Sim s;
        s.newAttempt();
        s.tapAt(196, 1, 5);
        s.L.died(true, s.pct());
        s.newAttempt();
        s.tapAt(200, 1, 5);       // judge time not over yet...
        s.L.completed();          // ...but the level is beaten
        CHECK(s.spots[0].totalPasses == 1);
    }

    std::puts("buffer turned off in settings");
    {
        Sim s;
        teach(s);
        s.L.cfg.enabled = false;
        s.newAttempt();
        s.runTo(196);
        size_t before = s.gdPressX.size();
        s.click();
        CHECK(s.gdPressX.size() == before + 1);
    }

    std::puts("death ranges: 51% means 50.5-51.5%");
    {
        CHECK(phantom::rangeIndex(51.0, 1.0) == 51);
        CHECK(phantom::rangeIndex(50.5, 1.0) == 51);
        CHECK(phantom::rangeIndex(51.49, 1.0) == 51);
        CHECK(phantom::rangeIndex(51.5, 1.0) == 52);
        CHECK(phantom::rangeIndex(50.49, 1.0) == 50);
        CHECK(phantom::rangeIndex(0.2, 1.0) == 0);
        CHECK(phantom::rangeIndex(51.3, 0.5) == 103);  // 51.25-51.75%
        CHECK(phantom::rangeLabel(51, 1.0) == "50.5-51.5%");
        CHECK(phantom::rangeLabel(103, 0.5) == "51.25-51.75%");
    }

    std::puts("click spots are linked to the range you died in");
    {
        Sim s;
        teach(s);  // the death came 20+ steps after the click at x 196
        int deathRange = phantom::rangeIndex(23.8, 1.0);
        CHECK(s.spots[0].rangeHits.count(deathRange) == 1);
        CHECK(s.spots[0].range() == deathRange);
    }

    std::puts("per-range ticks from the console: 3 ticks lets a 3-tick-early click through");
    {
        Sim s;
        phantom::RangeRules rules;
        s.L.rules = &rules;
        teach(s);
        int r = s.spots[0].range();

        s.newAttempt();
        s.runTo(194);  // 3 ticks early: default 2 doesn't catch it
        size_t before = s.gdPressX.size();
        s.click();
        CHECK(s.gdPressX.size() == before + 1);

        rules[r].ticks = 3;
        s.newAttempt();
        s.runTo(194);
        before = s.gdPressX.size();
        s.click();
        CHECK(s.gdPressX.size() == before);   // caught now
        s.steps(3);
        CHECK(s.gdPressX.size() == before);   // still held at x 198
        s.step();
        CHECK(s.gdPressX.size() == before + 1);
        CHECK(s.gdPressX.back() == 200.0);
    }

    std::puts("per-range ticks 0 switches the buffer off there");
    {
        Sim s;
        phantom::RangeRules rules;
        s.L.rules = &rules;
        teach(s);
        rules[s.spots[0].range()].ticks = 0;
        s.newAttempt();
        s.runTo(196);
        size_t before = s.gdPressX.size();
        s.click();
        CHECK(s.gdPressX.size() == before + 1);
        CHECK(s.L.readySpots() == 0);
        CHECK(s.L.spotStatus(s.spots[0]) == "buffer off in this range");
    }

    std::puts("delay ranges hold every click there exactly N ticks, no learning needed");
    {
        Sim s;
        phantom::RangeRules rules;
        s.L.rules = &rules;
        rules[30] = {2, true};  // 29.5-30.5% = x 295..305
        s.newAttempt();
        s.runTo(296);
        size_t before = s.gdPressX.size();
        s.click();
        s.release();
        CHECK(s.gdPressX.size() == before);
        s.step();
        s.step();
        CHECK(s.gdPressX.size() == before);
        s.step();
        CHECK(s.gdPressX.size() == before + 1);
        CHECK(s.gdPressX.back() == 300.0);    // exactly 2 ticks later
        CHECK(s.notesHave("[delay] held a click 2 tick(s)"));
        s.newAttempt();
        s.runTo(290);                         // 29%: outside the delay range
        before = s.gdPressX.size();
        s.click();
        CHECK(s.gdPressX.size() == before + 1);
    }

    std::printf(g_fail ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}

// learner.hpp - the brain of Phantom Click.
// No Geode or GD code in here, so it can be tested on its own (see tests/).
//
// The game hooks in main.cpp feed it physics steps, player positions, jump
// presses/releases (at the moment GD applies them), deaths and level
// completion. It answers three questions: hold this press? send the held press
// now? send the held release now? It also keeps each level's click spots
// learned and linked to their death ranges.
//
// Death ranges: every death falls into a percent range of width W (default 1%)
// centred on a whole percent - a death at 51.2% belongs to 50.5-51.5%. Each
// range can carry its own rule, set from the console tool:
//   smart (default) - at clicks you've died to in this range, a click up to
//                     N ticks before the learned right tick is held until it
//   delay           - every click inside this range is held exactly N ticks
//   N = 0           - no buffering in this range
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace phantom {

struct Settings {
    bool enabled = true;
    int bufferTicks = 2;       // default ticks for ranges without their own rule
    int minMisses = 1;         // misses at a spot before the buffer turns on
    double mergeUnits = 30.0;  // clicks this close are the same spot (1 block)
    unsigned judgeSteps = 180; // die within this many steps after a click = miss
    double rangeWidth = 1.0;   // width of a death range in percent
};

// A per-range rule from the console tool.
struct RangeRule {
    int ticks = -1;      // -1 = use the default from settings
    bool delay = false;  // true = hold every click in this range exactly `ticks`
};
using RangeRules = std::map<int, RangeRule>;  // range index -> rule

constexpr size_t KEEP = 30;          // recent passes/misses kept per spot
constexpr size_t PRESS_HISTORY = 64; // clicks remembered per attempt
constexpr int RELEARN_AFTER = 2;     // buffered misses in a row before re-learning

// ---- death ranges --------------------------------------------------------

// Range index for a percent: 51.2% -> 51 (50.5-51.5%) with width 1.
inline int rangeIndex(double pct, double width) {
    if (width <= 0.0) width = 1.0;
    return static_cast<int>(std::floor(pct / width + 0.5));
}
inline double rangeCenter(int idx, double width) { return idx * width; }
inline double rangeLo(int idx, double width) { return idx * width - width / 2.0; }
inline double rangeHi(int idx, double width) { return idx * width + width / 2.0; }

inline std::string strf(char const* f, ...) {
    char buf[512];
    va_list args;
    va_start(args, f);
    std::vsnprintf(buf, sizeof(buf), f, args);
    va_end(args);
    return buf;
}

// "50.5-51.5%"
inline std::string rangeLabel(int idx, double width) {
    return strf("%.4g-%.4g%%", rangeLo(idx, width), rangeHi(idx, width));
}

// ---- click spots -----------------------------------------------------------

struct ClickSpot {
    double x = 0.0;        // centre of the clicks grouped here
    double pct = 0.0;      // percent at the centre
    double speed = 0.0;    // world units per tick here
    int samples = 0;
    std::vector<double> misses;  // recent click positions followed by a death
    std::vector<double> passes;  // recent click positions you survived
    int totalMisses = 0;
    int totalPasses = 0;
    int buffered = 0;            // clicks the buffer has held here
    int bufferedMissStreak = 0;  // buffered clicks that died, in a row
    std::map<int, int> rangeHits;  // death range -> deaths this click led to

    // The death range this click belongs to: where it kills you most.
    int range() const {
        int best = 0, bestN = -1;
        for (auto const& [idx, n] : rangeHits) {
            if (n > bestN) {
                bestN = n;
                best = idx;
            }
        }
        return bestN < 0 ? rangeIndex(pct, 1.0) : best;
    }
};

inline void pushCapped(std::vector<double>& v, double x) {
    v.push_back(x);
    if (v.size() > KEEP) v.erase(v.begin());
}

// The right tick: middle of the clicks you survived (the safest point of the window).
inline std::optional<double> rightTick(ClickSpot const& s) {
    if (s.passes.empty()) return std::nullopt;
    auto p = s.passes;
    std::sort(p.begin(), p.end());
    return p[p.size() / 2];
}

inline bool spotReady(ClickSpot const& s, Settings const& cfg) {
    return !s.passes.empty() && s.totalMisses >= cfg.minMisses;
}

// How many ticks off the right tick a click at x was (negative = early).
inline std::optional<double> ticksOff(ClickSpot const& s, double x) {
    auto t = rightTick(s);
    if (!t || s.speed <= 0.0) return std::nullopt;
    return (x - *t) / s.speed;
}

inline std::string describeClick(ClickSpot const& s, double x) {
    auto off = ticksOff(s, x);
    if (!off) return "not graded yet (no right tick)";
    if (*off < -0.5) return strf("%.1f tick(s) early", -*off);
    if (*off > 0.5) return strf("%.1f tick(s) late", *off);
    return "on the right tick";
}

// ---- the learner -----------------------------------------------------------

class Learner {
public:
    Settings cfg;
    std::vector<ClickSpot>* spots = nullptr;  // this level's spots (owned by the caller)
    RangeRules const* rules = nullptr;        // this level's range rules (owned by the caller)
    std::vector<std::string> notes;           // console lines, flushed by the caller
    bool dirty = false;                       // spots changed since the last save

    unsigned step = 0;    // physics steps counted by the mod
    double speed = 0.0;   // measured world units per step

    // Rule for a range, if the console tool set one.
    RangeRule const* ruleFor(int idx) const {
        if (!rules) return nullptr;
        auto it = rules->find(idx);
        return it == rules->end() ? nullptr : &it->second;
    }

    // Buffer ticks that apply to a range (0 = off).
    int ticksForRange(int idx) const {
        auto r = ruleFor(idx);
        return r && r->ticks >= 0 ? r->ticks : cfg.bufferTicks;
    }

    bool rangeIsDelay(int idx) const {
        auto r = ruleFor(idx);
        return r && r->delay;
    }

    // ---- jump presses / releases, at the moment GD applies them ----

    bool hasHeldPress() const { return m_pressPending; }

    // GD is about to apply a jump press at x (pct = percent there).
    // True = the mod keeps it for now. Send any held press first (hasHeldPress / pressSent).
    bool onPress(double x, double pct, bool canBuffer) {
        if (canBuffer && cfg.enabled && speed > 0.0) {
            // delay ranges: every click inside is held exactly N ticks
            int here = rangeIndex(pct, cfg.rangeWidth);
            if (rangeIsDelay(here)) {
                int n = ticksForRange(here);
                if (n > 0) {
                    hold(x, x + n * speed, n, -1, here);
                    return true;
                }
            }
            // smart: early for a click you've died to
            int idx = bufferSpotFor(x);
            if (idx >= 0) {
                auto const& s = (*spots)[idx];
                hold(x, *rightTick(s), ticksForRange(s.range()), idx, s.range());
                return true;
            }
        }
        recordPress(x, pct, false);
        return false;
    }

    // GD is about to apply a jump release. True = the mod keeps it (a press is held).
    bool onRelease() {
        if (!m_pressPending) return false;
        m_releaseAfterPress = true;
        return true;
    }

    // The held press was just sent to GD with the player at x.
    void pressSent(double x, double pct) {
        m_pressPending = false;
        recordPress(x, pct, true);
        long ticks = speed > 0.0 ? std::lround((x - m_pressX) / speed) : 0;
        std::string range = rangeLabel(m_pressRange, cfg.rangeWidth);
        if (m_pressSpot >= 0 && spots && m_pressSpot < static_cast<int>(spots->size())) {
            auto& s = (*spots)[m_pressSpot];
            s.buffered++;
            dirty = true;
            notes.push_back(strf("[buffer] caught a click %ld tick(s) early at %.2f%% (%s) and sent it on the right tick",
                ticks, s.pct, range.c_str()));
        } else {
            notes.push_back(strf("[delay] held a click %ld tick(s) at %.2f%% (%s, delay range)",
                ticks, pct, range.c_str()));
        }
        if (m_releaseAfterPress) {
            // you already let go: release a tick later so the jump still registers
            m_releaseAfterPress = false;
            m_releasePending = true;
            m_releaseStep = step + 1;
        }
    }

    // A held release that must go out now (before a new press, or on reset).
    bool takeRelease() {
        if (!m_releasePending) return false;
        m_releasePending = false;
        return true;
    }

    // ---- physics steps ----

    // Start of a step with the player at x. True = send the held press now.
    bool stepStart(double x) {
        ++step;
        if (!m_pressPending) return false;
        double half = 0.5 * speed;
        bool reachedTarget = x >= m_pressTarget - half;
        bool heldMaxTicks = x >= m_pressX + m_pressMaxTicks * speed - half;
        bool safetyStop = step >= m_pressStep + static_cast<unsigned>(m_pressMaxTicks) + 2;  // e.g. player stopped
        return reachedTarget || heldMaxTicks || safetyStop;
    }

    // A held release whose tick has come.
    bool takeDueRelease() {
        if (!m_releasePending || step < m_releaseStep) return false;
        m_releasePending = false;
        return true;
    }

    // End of a step with the player at x.
    void stepEnd(double x, bool alive) {
        if (m_havePrev) {
            double v = x - m_prevX;
            if (v > 0.05 && v < 20.0) speed = v;  // ignore teleports and standstills
        }
        m_prevX = x;
        m_havePrev = true;
        if (alive) gradeOldPresses();
    }

    // ---- attempt events ----

    // You died at deathPct. Grades your last click as a miss and links it to the
    // death's range. Returns a line describing the click.
    // countIt = false (e.g. practice with practice deaths off): nothing is learned.
    std::string died(bool countIt, double deathPct) {
        clearHeld();
        Press const* blamed = nullptr;
        for (auto it = m_presses.rbegin(); it != m_presses.rend(); ++it) {
            if (step - it->step > cfg.judgeSteps) break;
            if (!it->resolved) {
                blamed = &*it;
                break;
            }
        }
        Press last = blamed ? *blamed : Press{};
        for (auto& p : m_presses) p.resolved = true;
        if (!countIt) return "";
        if (!blamed) return "no click just before it";

        int idx = nearestSpot(last.x);
        if (idx < 0) {
            ClickSpot s;
            s.x = last.x;
            s.pct = last.pct;
            s.speed = speed;
            spots->push_back(s);
            idx = static_cast<int>(spots->size()) - 1;
        }
        auto& s = (*spots)[idx];
        std::string how = describeClick(s, last.x);
        pushCapped(s.misses, last.x);
        s.totalMisses++;
        s.rangeHits[rangeIndex(deathPct, cfg.rangeWidth)]++;
        addSample(s, last.x, last.pct);
        dirty = true;

        if (last.buffered && ++s.bufferedMissStreak >= RELEARN_AFTER) {
            // buffered clicks keep dying here: the right tick moved, start over
            s.passes.clear();
            s.bufferedMissStreak = 0;
            notes.push_back(strf("[relearn] click spot %.2f%%: buffered clicks kept dying, re-learning from your next pass",
                s.pct));
        }

        return strf("last click at %.2f%% was %s (%d miss(es), %d pass(es), %s)",
            s.pct, how.c_str(), s.totalMisses, s.totalPasses, spotStatus(s).c_str());
    }

    // What the buffer is doing at a spot, in words.
    std::string spotStatus(ClickSpot const& s) const {
        int r = s.range();
        if (rangeIsDelay(r)) return strf("delay range, %d tick(s)", ticksForRange(r));
        int n = ticksForRange(r);
        if (n <= 0) return "buffer off in this range";
        if (spotReady(s, cfg)) return strf("buffer ON, %d tick(s)", n);
        if (!s.passes.empty()) return strf("buffer turns on at %d misses", cfg.minMisses);
        return "get past this click once to teach the buffer";
    }

    // You finished the level: every click still being judged was a pass.
    void completed() {
        for (auto& p : m_presses) {
            if (!p.resolved) {
                p.resolved = true;
                gradePass(p);
            }
        }
    }

    // A new attempt starts (respawn or checkpoint). Call takeRelease() first.
    void newAttempt() {
        clearHeld();
        m_presses.clear();
        m_havePrev = false;
    }

    void clearHeld() {
        m_pressPending = false;
        m_releaseAfterPress = false;
    }

    int readySpots() const {
        int n = 0;
        if (spots) {
            for (auto const& s : *spots) n += spotReady(s, cfg) && ticksForRange(s.range()) > 0 ? 1 : 0;
        }
        return n;
    }

private:
    struct Press {
        double x = 0.0;
        double pct = 0.0;
        unsigned step = 0;
        bool buffered = false;  // held and released by the buffer
        bool resolved = false;  // already graded (or discarded)
    };

    std::vector<Press> m_presses;  // this attempt's clicks, oldest first
    bool m_havePrev = false;
    double m_prevX = 0.0;

    bool m_pressPending = false;
    double m_pressTarget = 0.0;
    double m_pressX = 0.0;
    int m_pressMaxTicks = 0;
    unsigned m_pressStep = 0;
    int m_pressSpot = -1;   // -1 = delay range, not a learned spot
    int m_pressRange = 0;
    bool m_releaseAfterPress = false;
    bool m_releasePending = false;
    unsigned m_releaseStep = 0;

    void hold(double x, double target, int maxTicks, int spot, int range) {
        m_pressPending = true;
        m_pressX = x;
        m_pressTarget = target;
        m_pressMaxTicks = maxTicks;
        m_pressStep = step;
        m_pressSpot = spot;
        m_pressRange = range;
    }

    void recordPress(double x, double pct, bool buffered) {
        m_presses.push_back({x, pct, step, buffered, false});
        if (m_presses.size() > PRESS_HISTORY) m_presses.erase(m_presses.begin());
    }

    int nearestSpot(double x) const {
        if (!spots) return -1;
        int best = -1;
        double bestD = cfg.mergeUnits;
        for (size_t i = 0; i < spots->size(); ++i) {
            double d = std::abs((*spots)[i].x - x);
            if (d <= bestD) {
                bestD = d;
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    // Is a click at x 1..N ticks before the right tick of a ready spot?
    // N comes from the spot's death range.
    int bufferSpotFor(double x) const {
        if (!spots || speed <= 0.0) return -1;
        double onTime = 0.5 * speed;
        for (size_t i = 0; i < spots->size(); ++i) {
            auto const& s = (*spots)[i];
            if (!spotReady(s, cfg)) continue;
            int r = s.range();
            if (rangeIsDelay(r)) continue;
            int n = ticksForRange(r);
            if (n <= 0) continue;
            double t = *rightTick(s);
            if (x < t - onTime && x >= t - (n + 0.5) * speed) return static_cast<int>(i);
        }
        return -1;
    }

    void addSample(ClickSpot& s, double x, double pct) {
        s.samples++;
        double w = 1.0 / std::min(s.samples, 50);  // recent clicks weigh more after 50
        s.x += (x - s.x) * w;
        s.pct += (pct - s.pct) * w;
        if (speed > 0.0) s.speed = s.speed > 0.0 ? s.speed + (speed - s.speed) * w : speed;
    }

    void gradePass(Press const& p) {
        int idx = nearestSpot(p.x);
        if (idx < 0) return;  // not a click you've died to
        auto& s = (*spots)[idx];
        bool wasReady = spotReady(s, cfg);
        bool hadTick = !s.passes.empty();
        pushCapped(s.passes, p.x);
        s.totalPasses++;
        addSample(s, p.x, p.pct);
        if (p.buffered) s.bufferedMissStreak = 0;
        dirty = true;
        std::string range = rangeLabel(s.range(), cfg.rangeWidth);
        if (!wasReady && spotReady(s, cfg)) {
            notes.push_back(strf("[learned] click spot %.2f%% (%s): right tick found - %s",
                s.pct, range.c_str(), spotStatus(s).c_str()));
        } else if (!hadTick) {
            notes.push_back(strf("[learned] click spot %.2f%% (%s): right tick found - buffer turns on at %d miss(es)",
                s.pct, range.c_str(), cfg.minMisses));
        }
    }

    void gradeOldPresses() {
        for (auto& p : m_presses) {
            if (p.resolved) continue;
            if (step - p.step < cfg.judgeSteps) break;  // newer clicks aren't ready either
            p.resolved = true;
            gradePass(p);
        }
    }
};

}  // namespace phantom

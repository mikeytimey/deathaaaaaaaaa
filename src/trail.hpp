// trail.hpp - records the player's path for the overlay and the level map.
// No Geode or GD code in here, so it can be tested on its own (see tests/).
//
// Every attempt is recorded as polylines (a new line starts after a teleport),
// plus where you clicked and where you died. Straight runs collapse to their
// end points, so a full attempt stays small.
#pragma once

#include <cmath>
#include <deque>
#include <optional>
#include <vector>

namespace phantom {

struct TrailPoint {
    float x;
    float y;
};

struct TrailClick {
    float x;
    float y;
    bool buffered;  // caught by the mod and sent on the right tick
};

struct Attempt {
    std::vector<std::vector<TrailPoint>> segments;
    std::vector<TrailClick> clicks;
    bool died = false;
    bool completed = false;
    bool practice = false;
    float deathX = 0.f;
    float deathY = 0.f;
    double endPct = 0.0;  // how far it got

    size_t pointCount() const {
        size_t n = 0;
        for (auto const& s : segments) n += s.size();
        return n;
    }
};

class TrailRecorder {
public:
    static constexpr unsigned SAMPLE_EVERY = 2;   // steps between samples (120 per second)
    static constexpr float TELEPORT = 90.f;       // a jump this big starts a new line
    static constexpr float STRAIGHT = 0.3f;       // max wobble for a "straight" run
    static constexpr size_t MAX_POINTS = 30000;   // safety cap per attempt

    void begin(bool practice) {
        m_cur = Attempt{};
        m_cur.practice = practice;
        m_steps = 0;
        m_haveLast = false;
    }

    // One physics step with the player at (x, y).
    void step(float x, float y) {
        if (m_steps++ % SAMPLE_EVERY != 0) return;
        add(x, y);
    }

    void click(float x, float y, bool buffered) {
        if (m_cur.clicks.size() < MAX_POINTS) m_cur.clicks.push_back({x, y, buffered});
    }

    // The attempt is over. Returns it and starts nothing new (call begin()).
    Attempt finish(bool died, bool completed, float x, float y, double endPct) {
        add(x, y, true);
        m_cur.died = died;
        m_cur.completed = completed;
        m_cur.deathX = x;
        m_cur.deathY = y;
        m_cur.endPct = endPct;
        return std::move(m_cur);
    }

    Attempt const& current() const { return m_cur; }

private:
    Attempt m_cur;
    unsigned m_steps = 0;
    bool m_haveLast = false;
    TrailPoint m_last{0.f, 0.f};

    void add(float x, float y, bool force = false) {
        if (m_cur.pointCount() >= MAX_POINTS && !force) return;
        TrailPoint p{x, y};
        bool teleport = m_haveLast &&
            (std::abs(x - m_last.x) > TELEPORT || std::abs(y - m_last.y) > TELEPORT);
        if (!m_haveLast || teleport || m_cur.segments.empty()) m_cur.segments.emplace_back();
        auto& seg = m_cur.segments.back();
        // the middle of three points on a straight line adds nothing: drop it
        if (seg.size() >= 2 && isStraight(seg[seg.size() - 2], seg.back(), p)) {
            seg.back() = p;
        } else {
            seg.push_back(p);
        }
        m_last = p;
        m_haveLast = true;
    }

    static bool isStraight(TrailPoint a, TrailPoint b, TrailPoint c) {
        float dx = c.x - a.x, dy = c.y - a.y;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-4f) return true;
        float dist = std::abs(dy * (b.x - a.x) - dx * (b.y - a.y)) / len;  // b's distance from line a-c
        bool between = (b.x - a.x) * dx + (b.y - a.y) * dy >= 0.f &&
                       (c.x - b.x) * dx + (c.y - b.y) * dy >= 0.f;
        return dist <= STRAIGHT && between;
    }
};

// The attempts kept per level: your best normal-mode run and the latest few.
struct LevelTrails {
    static constexpr size_t RECENT = 5;
    std::optional<Attempt> best;
    std::deque<Attempt> recent;  // newest first

    // Returns true if this attempt became the new best.
    bool add(Attempt a) {
        bool newBest = false;
        if (!a.practice && (!best || a.endPct > best->endPct || (a.completed && !best->completed))) {
            best = a;
            newBest = true;
        }
        recent.push_front(std::move(a));
        while (recent.size() > RECENT) recent.pop_back();
        return newBest;
    }
};

}  // namespace phantom

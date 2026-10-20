// Death Buffer - Geode mod for Geometry Dash 2.2081
//
// What it does
//   1. Logs every death on every level: exact percent, x position, and whether
//      it was in practice mode. Deaths that happen at the same place are
//      grouped into a "spot".
//   2. When you get past a spot you have died at, it remembers the click that
//      got you past it. That click's position is the "right tick" for the spot.
//   3. On later attempts, a click up to N ticks (default 2) BEFORE that right
//      tick is held and fires on the right tick instead. A 1-tick window
//      becomes a (N+1)-tick window, but only at spots you have died at.
//      Every other click goes through untouched.
//   4. Everything is saved to deaths.json in the mod's save folder, and deaths,
//      learned spots and buffered clicks are printed to the Geode console.
//
// Nothing is written to disk or the console while you are alive mid-attempt,
// so the mod never causes a hitch during gameplay.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/utils/file.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {

constexpr double BLOCK = 30.0;  // one GD block in world units
constexpr int JUMP = static_cast<int>(PlayerButton::Jump);
constexpr size_t PRESS_HISTORY = 64;

// ---------------------------------------------------------------- settings --

bool optEnabled() { return Mod::get()->getSettingValue<bool>("enabled"); }
int optTicks() { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("buffer-ticks")); }
int optMinDeaths() { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("min-deaths")); }
double optMergeUnits() { return Mod::get()->getSettingValue<double>("merge-blocks") * BLOCK; }
double optLookbackUnits() { return Mod::get()->getSettingValue<double>("lookback-blocks") * BLOCK; }
bool optCountPractice() { return Mod::get()->getSettingValue<bool>("count-practice"); }
bool optLog() { return Mod::get()->getSettingValue<bool>("log-events"); }

// ------------------------------------------------------------- saved data --

struct Death {
    double pct;
    double x;
    bool practice;
};

struct Spot {
    double x = 0.0;                // average x of the deaths grouped here
    double pct = 0.0;              // average percent of the deaths grouped here
    int deaths = 0;
    std::optional<double> target;  // x of the click that last got you past this spot
    int passes = 0;                // times you got past it alive
    int buffered = 0;              // early clicks the buffer has held here
};

struct LevelData {
    std::string name;
    std::vector<Death> deaths;
    std::vector<Spot> spots;
};

std::map<std::string, LevelData> g_db;  // level key -> data
bool g_loaded = false;
bool g_dirty = false;

double round2(double v) { return std::round(v * 100.0) / 100.0; }
double round1(double v) { return std::round(v * 10.0) / 10.0; }

std::filesystem::path dbPath() { return Mod::get()->getSaveDir() / "deaths.json"; }

void loadDb() {
    g_loaded = true;
    auto path = dbPath();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        log::info("No death data yet. It will be saved to {}", utils::string::pathToString(path));
        return;
    }
    auto res = file::readJson(path);
    if (!res) {
        log::error("Couldn't read {}: {}", utils::string::pathToString(path), res.unwrapErr());
        return;
    }
    auto root = std::move(res).unwrap();
    for (auto& entry : root) {
        auto key = entry.getKey();
        if (!key) continue;

        LevelData lv;
        lv.name = entry["name"].asString().unwrapOr(*key);

        if (entry["deaths"].isArray()) {
            for (auto& d : entry["deaths"]) {
                lv.deaths.push_back({
                    d["pct"].asDouble().unwrapOr(0.0),
                    d["x"].asDouble().unwrapOr(0.0),
                    d["practice"].asBool().unwrapOr(false),
                });
            }
        }
        if (entry["spots"].isArray()) {
            for (auto& s : entry["spots"]) {
                Spot sp;
                sp.x = s["x"].asDouble().unwrapOr(0.0);
                sp.pct = s["pct"].asDouble().unwrapOr(0.0);
                sp.deaths = static_cast<int>(s["deaths"].asInt().unwrapOr(0));
                sp.passes = static_cast<int>(s["passes"].asInt().unwrapOr(0));
                sp.buffered = static_cast<int>(s["buffered"].asInt().unwrapOr(0));
                if (s["target"].isNumber()) sp.target = s["target"].asDouble().unwrapOr(0.0);
                lv.spots.push_back(sp);
            }
        }
        g_db[*key] = std::move(lv);
    }
    log::info("Loaded death data for {} level(s) from {}", g_db.size(), utils::string::pathToString(path));
}

void saveDb() {
    auto root = matjson::Value::object();
    for (auto const& [key, lv] : g_db) {
        std::vector<matjson::Value> deaths;
        deaths.reserve(lv.deaths.size());
        for (auto const& d : lv.deaths) {
            auto o = matjson::Value::object();
            o.set("pct", d.pct);
            o.set("x", d.x);
            o.set("practice", d.practice);
            deaths.push_back(std::move(o));
        }
        std::vector<matjson::Value> spots;
        for (auto const& s : lv.spots) {
            auto o = matjson::Value::object();
            o.set("pct", round2(s.pct));
            o.set("x", round1(s.x));
            o.set("deaths", s.deaths);
            if (s.target) o.set("target", round1(*s.target));
            else o.set("target", nullptr);
            o.set("passes", s.passes);
            o.set("buffered", s.buffered);
            spots.push_back(std::move(o));
        }
        auto l = matjson::Value::object();
        l.set("name", lv.name);
        l.set("deaths", matjson::Value(std::move(deaths)));
        l.set("spots", matjson::Value(std::move(spots)));
        root.set(key, std::move(l));
    }
    auto res = file::writeStringSafe(dbPath(), root.dump());
    if (!res) log::error("Couldn't save death data: {}", res.unwrapErr());
    else g_dirty = false;
}

std::string levelKey(GJGameLevel* level) {
    int id = level->m_levelID.value();
    if (id > 0) return std::to_string(id);
    return "local:" + std::string(level->m_levelName);  // editor / unuploaded levels
}

bool spotReady(Spot const& s) { return s.target.has_value() && s.deaths >= optMinDeaths(); }

// ------------------------------------------------------- per-level session --

struct Press {
    double x;
    unsigned tick;
};

struct Session {
    bool active = false;            // in a normal (non-platformer) level
    LevelData* level = nullptr;     // points into g_db (std::map nodes never move)
    int deathsThisSession = 0;
    std::vector<std::string> notes; // console lines held until you die / quit

    // this attempt
    std::vector<Press> presses;     // recent clicks, oldest first
    bool deathRecorded = false;
    bool havePrev = false;
    double prevX = 0.0;
    unsigned prevTick = 0;
    double speed = 0.0;             // measured world units per tick

    // input being held back
    bool injecting = false;         // true while we send a click ourselves
    bool pressPending = false;
    double pressTarget = 0.0;
    unsigned pressTick = 0;
    unsigned pressDeadline = 0;
    int pressSpot = -1;
    bool releaseAfterPress = false; // you let go before the held click fired
    bool releasePending = false;
    unsigned releaseTick = 0;
};

Session S;

void flushNotes() {
    if (optLog()) {
        for (auto const& n : S.notes) log::info("{}", n);
    }
    S.notes.clear();
}

void recordPress(double x, unsigned tick) {
    S.presses.push_back({x, tick});
    if (S.presses.size() > PRESS_HISTORY) S.presses.erase(S.presses.begin());
}

void printSummary() {
    if (!S.level || !optLog()) return;
    auto const& lv = *S.level;
    log::info("===== {}: {} death(s) this session, {} total =====",
        lv.name, S.deathsThisSession, lv.deaths.size());

    std::vector<Spot const*> spots;
    for (auto const& s : lv.spots) spots.push_back(&s);
    std::sort(spots.begin(), spots.end(), [](Spot const* a, Spot const* b) { return a->deaths > b->deaths; });

    size_t shown = std::min<size_t>(spots.size(), 10);
    for (size_t i = 0; i < shown; ++i) {
        auto const* s = spots[i];
        std::string status;
        if (spotReady(*s)) status = fmt::format("buffer ready, used {}x", s->buffered);
        else if (s->target) status = fmt::format("learned, turns on at {} deaths", optMinDeaths());
        else status = "not learned yet (get past it once)";
        log::info("  {:6.2f}%  {:4} deaths  {}", s->pct, s->deaths, status);
    }
}

}  // namespace

// ------------------------------------------------------------------ hooks --

class $modify(DBGameLayer, GJBaseGameLayer) {
    bool isTracked() {
        if (!S.active) return false;
        auto pl = PlayLayer::get();
        return pl && static_cast<GJBaseGameLayer*>(pl) == static_cast<GJBaseGameLayer*>(this);
    }

    // Send a jump press/release into the game. Our own handleButton hook sees
    // `injecting` and passes it straight through.
    void sendJump(bool down) {
        S.injecting = true;
        GJBaseGameLayer::handleButton(down, JUMP, true);
        S.injecting = false;
    }

    // Is this click 1..N ticks before the learned right click of a spot?
    int bufferSpotFor(double x) {
        if (!S.level || S.speed <= 0.0) return -1;
        double early = (optTicks() + 0.5) * S.speed;
        double onTime = 0.5 * S.speed;
        for (size_t i = 0; i < S.level->spots.size(); ++i) {
            auto const& s = S.level->spots[i];
            if (!spotReady(s)) continue;
            double t = *s.target;
            if (x < t - onTime && x >= t - early) return static_cast<int>(i);
        }
        return -1;
    }

    void deliverPress(double x, unsigned tick) {
        S.pressPending = false;
        sendJump(true);
        recordPress(x, tick);
        if (S.pressSpot >= 0 && S.pressSpot < static_cast<int>(S.level->spots.size())) {
            auto& s = S.level->spots[S.pressSpot];
            s.buffered++;
            g_dirty = true;
            S.notes.push_back(fmt::format("[buffer] held a click {} tick(s) at {:.2f}% so it fired on the right tick",
                tick - S.pressTick, s.pct));
        }
        if (S.releaseAfterPress) {
            // you already let go - release on the next tick so the jump still registers
            S.releaseAfterPress = false;
            S.releasePending = true;
            S.releaseTick = tick + 1;
        }
    }

    // Send anything still being held back, in order.
    void flushPending() {
        if (S.pressPending) {
            deliverPress(m_player1->getPositionX(), m_gameState.m_currentProgress);
        }
        if (S.releasePending) {
            S.releasePending = false;
            sendJump(false);
        }
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (S.injecting || !isTracked() || button != JUMP || !isPlayer1) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        if (down) {
            flushPending();
            double x = m_player1->getPositionX();
            unsigned tick = m_gameState.m_currentProgress;

            int spot = optEnabled() ? bufferSpotFor(x) : -1;
            if (spot >= 0) {
                // too early for a spot you've died at: hold it, processCommands fires it
                S.pressPending = true;
                S.pressTarget = *S.level->spots[spot].target;
                S.pressTick = tick;
                S.pressDeadline = tick + static_cast<unsigned>(optTicks()) + 1;
                S.pressSpot = spot;
                return;
            }
            recordPress(x, tick);
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        if (S.pressPending) {
            S.releaseAfterPress = true;  // release goes out right after the held click
            return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // Runs once per physics step (240 per second).
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        bool tracked = isTracked();
        if (tracked) beforeStep();
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (tracked && isTracked()) afterStep();
    }

    void beforeStep() {
        unsigned tick = m_gameState.m_currentProgress;
        if (S.releasePending && tick >= S.releaseTick) {
            S.releasePending = false;
            sendJump(false);
        }
        if (S.pressPending) {
            double x = m_player1->getPositionX();
            bool reached = x >= S.pressTarget - 0.5 * S.speed;
            if (reached || tick >= S.pressDeadline) deliverPress(x, tick);
        }
    }

    void afterStep() {
        unsigned tick = m_gameState.m_currentProgress;
        double x = m_player1->getPositionX();
        if (S.havePrev && tick > S.prevTick) {
            double v = (x - S.prevX) / static_cast<double>(tick - S.prevTick);
            if (v > 0.05 && v < 20.0) S.speed = v;  // ignore teleports and standstills
            if (!m_playerDied && !m_player1->m_isDead && S.level) checkPasses(S.prevX, x);
        }
        if (!S.havePrev || tick != S.prevTick) {
            S.prevX = x;
            S.prevTick = tick;
            S.havePrev = true;
        }
    }

    // You just crossed a spot you've died at and you're still alive:
    // the last click before that spot is the one that worked.
    void checkPasses(double from, double to) {
        double margin = optMergeUnits();
        double lookback = optLookbackUnits();
        for (auto& s : S.level->spots) {
            double line = s.x + margin;
            if (!(from < line && to >= line)) continue;

            s.passes++;
            g_dirty = true;
            for (auto it = S.presses.rbegin(); it != S.presses.rend(); ++it) {
                if (it->x >= s.x) continue;
                if (s.x - it->x <= lookback) {
                    bool changed = !s.target || std::abs(*s.target - it->x) > 0.05;
                    s.target = it->x;
                    if (changed) {
                        S.notes.push_back(fmt::format("[learned] spot {:.2f}%: right click is at x={:.1f}{}",
                            s.pct, it->x, s.deaths >= optMinDeaths() ? " - buffer is ON here now" : ""));
                    }
                }
                break;
            }
        }
    }
};

class $modify(DBPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        S = Session{};
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        if (!g_loaded) loadDb();

        if (m_isPlatformer) {
            log::info("\"{}\" is a platformer level - Death Buffer is off here.", std::string(level->m_levelName));
            return true;
        }

        auto& lv = g_db[levelKey(level)];
        lv.name = std::string(level->m_levelName);
        S.level = &lv;
        S.active = true;

        int ready = 0;
        for (auto const& s : lv.spots) ready += spotReady(s) ? 1 : 0;
        log::info("Tracking \"{}\": {} death(s) logged, {} death spot(s), {} with the buffer ready",
            lv.name, lv.deaths.size(), lv.spots.size(), ready);
        return true;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        PlayLayer::destroyPlayer(player, object);

        if (!S.active || S.deathRecorded || object == m_anticheatSpike) return;
        if (!m_playerDied && !(player && player->m_isDead)) return;  // didn't actually die (noclip etc.)

        S.deathRecorded = true;
        S.pressPending = false;
        S.releaseAfterPress = false;

        bool practice = m_isPracticeMode;
        if (practice && !optCountPractice()) {
            flushNotes();
            return;
        }

        double pct = getCurrentPercent();
        double x = (player ? player : m_player1)->getPositionX();
        auto& lv = *S.level;
        lv.deaths.push_back({round2(pct), round1(x), practice});
        S.deathsThisSession++;

        // group with the nearest spot within the merge distance, or start a new one
        Spot* spot = nullptr;
        double best = optMergeUnits();
        for (auto& s : lv.spots) {
            double d = std::abs(s.x - x);
            if (d <= best) {
                best = d;
                spot = &s;
            }
        }
        if (spot) {
            spot->x = (spot->x * spot->deaths + x) / (spot->deaths + 1);
            spot->pct = (spot->pct * spot->deaths + pct) / (spot->deaths + 1);
            spot->deaths++;
        } else {
            Spot s;
            s.x = x;
            s.pct = pct;
            s.deaths = 1;
            lv.spots.push_back(s);
            spot = &lv.spots.back();
        }
        g_dirty = true;

        flushNotes();
        if (optLog()) {
            std::string status;
            if (spotReady(*spot)) status = "buffer ON";
            else if (spot->target) status = fmt::format("buffer turns on at {} deaths", optMinDeaths());
            else status = "buffer learns this spot once you get past it";
            log::info("[death] {:.2f}%{}  (spot ~{:.2f}%: {} death(s), {})",
                pct, practice ? " (practice)" : "", spot->pct, spot->deaths, status);
        }
        saveDb();
    }

    void resetLevel() {
        if (S.active) {
            if (S.releasePending) {
                // never leave the game thinking the button is still held
                S.releasePending = false;
                S.injecting = true;
                GJBaseGameLayer::handleButton(false, JUMP, true);
                S.injecting = false;
            }
            S.pressPending = false;
            S.releaseAfterPress = false;
            S.presses.clear();
            S.deathRecorded = false;
            S.havePrev = false;
            flushNotes();
            if (g_dirty) saveDb();
        }
        PlayLayer::resetLevel();
    }

    void onQuit() {
        if (S.active) {
            flushNotes();
            printSummary();
            if (g_dirty) saveDb();
        }
        S = Session{};
        PlayLayer::onQuit();
    }
};

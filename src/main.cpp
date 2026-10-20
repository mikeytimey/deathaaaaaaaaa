// Phantom Click - Geode mod for Geometry Dash 2.2081
//
// Learns the clicks you die to and catches early clicks there: the mod takes
// the click, GD doesn't get it until the right tick.
//
//   Death ranges   every death lands in a percent range (51.2% -> 50.5-51.5%)
//   Click spots    clicks you died to, linked to the range they kill you in;
//                  your passes there teach the mod the right tick
//   Range rules    per-range ticks / delay mode, set live from the console
//                  tool (phantom.py) through ranges.json
//   Trails         your path every attempt, for the in-game overlay and the
//                  console tool's level map
//
// learner.hpp and trail.hpp hold the logic (tested on their own). This file
// is the GD side: hooks, saving, console log and the overlay.

#include "learner.hpp"
#include "trail.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/utils/file.hpp>

#include <cctype>
#include <map>

using namespace geode::prelude;
using phantom::Attempt;
using phantom::ClickSpot;

namespace {

constexpr double BLOCK = 30.0;  // one GD block in world units
constexpr double TPS = 240.0;   // physics steps per second
constexpr size_t OVERLAY_DEATHS = 300;

// ---------------------------------------------------------------- settings --

phantom::Settings readSettings() {
    auto mod = Mod::get();
    phantom::Settings s;
    s.enabled = mod->getSettingValue<bool>("enabled");
    s.bufferTicks = static_cast<int>(mod->getSettingValue<int64_t>("buffer-ticks"));
    s.minMisses = static_cast<int>(mod->getSettingValue<int64_t>("min-misses"));
    s.mergeUnits = mod->getSettingValue<double>("merge-blocks") * BLOCK;
    s.judgeSteps = static_cast<unsigned>(std::lround(mod->getSettingValue<double>("judge-seconds") * TPS));
    s.rangeWidth = mod->getSettingValue<double>("range-width");
    return s;
}
bool optCountPractice() { return Mod::get()->getSettingValue<bool>("count-practice"); }
bool optLog() { return Mod::get()->getSettingValue<bool>("log-events"); }
bool optTrails() { return Mod::get()->getSettingValue<bool>("overlay-trails"); }
std::string optOverlay() { return Mod::get()->getSettingValue<std::string>("overlay"); }

// ------------------------------------------------------------- saved data --

struct Death {
    double pct;
    double x;
    double y;
    bool practice;
};

// Least-squares fit of percent against x, so ranges can be drawn in the level.
struct PctFit {
    double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    void add(double x, double pct) {
        n += 1;
        sx += x;
        sy += pct;
        sxx += x * x;
        sxy += x * pct;
    }
    // pct = a * x + b
    bool solve(double& a, double& b) const {
        double d = n * sxx - sx * sx;
        if (n < 2 || std::abs(d) < 1e-6) return false;
        a = (n * sxy - sx * sy) / d;
        b = (sy - a * sx) / n;
        return a > 0;
    }
};

struct LevelData {
    std::string name;
    double levelLength = 0.0;
    PctFit fit;
    std::vector<Death> deaths;
    std::vector<ClickSpot> spots;
};

std::map<std::string, LevelData> g_db;  // level key -> data
bool g_loaded = false;

double round2(double v) { return std::round(v * 100.0) / 100.0; }
double round1(double v) { return std::round(v * 10.0) / 10.0; }

std::filesystem::path saveDir() { return Mod::get()->getSaveDir(); }
std::filesystem::path dbPath() { return saveDir() / "deaths.json"; }
std::filesystem::path rulesPath() { return saveDir() / "ranges.json"; }

std::string safeName(std::string const& key) {
    std::string out;
    for (char c : key) out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    return out;
}
std::filesystem::path trailsPath(std::string const& key) { return saveDir() / "trails" / (safeName(key) + ".json"); }

std::vector<double> readNumbers(matjson::Value& arr) {
    std::vector<double> out;
    if (!arr.isArray()) return out;
    for (auto& v : arr) {
        if (v.isNumber()) out.push_back(v.asDouble().unwrapOr(0.0));
    }
    return out;
}
int readInt(matjson::Value& v) { return static_cast<int>(v.asInt().unwrapOr(0)); }
double readNum(matjson::Value& v) { return v.asDouble().unwrapOr(0.0); }

void parseDb(matjson::Value& root) {
    for (auto& entry : root) {
        auto key = entry.getKey();
        if (!key) continue;

        LevelData lv;
        lv.name = entry["name"].asString().unwrapOr(*key);
        lv.levelLength = readNum(entry["levelLength"]);
        if (entry["fit"].isObject()) {
            auto& f = entry["fit"];
            lv.fit = {readNum(f["n"]), readNum(f["sx"]), readNum(f["sy"]), readNum(f["sxx"]), readNum(f["sxy"])};
        }
        if (entry["deaths"].isArray()) {
            for (auto& d : entry["deaths"]) {
                lv.deaths.push_back({readNum(d["pct"]), readNum(d["x"]), readNum(d["y"]),
                    d["practice"].asBool().unwrapOr(false)});
            }
        }
        if (lv.fit.n == 0) {  // older saves: rebuild the fit from the deaths
            for (auto const& d : lv.deaths) lv.fit.add(d.x, d.pct);
        }
        if (entry["clicks"].isArray()) {
            for (auto& c : entry["clicks"]) {
                ClickSpot s;
                s.x = readNum(c["x"]);
                s.pct = readNum(c["pct"]);
                s.speed = readNum(c["speed"]);
                s.samples = readInt(c["samples"]);
                s.misses = readNumbers(c["misses"]);
                s.passes = readNumbers(c["passes"]);
                s.totalMisses = readInt(c["totalMisses"]);
                s.totalPasses = readInt(c["totalPasses"]);
                s.buffered = readInt(c["buffered"]);
                s.bufferedMissStreak = readInt(c["bufferedMissStreak"]);
                if (c["rangeHits"].isObject()) {
                    for (auto& h : c["rangeHits"]) {
                        auto k = h.getKey();
                        if (!k) continue;
                        auto idx = utils::numFromString<int>(*k);
                        if (idx.isOk()) s.rangeHits[idx.unwrap()] = readInt(h);
                    }
                }
                lv.spots.push_back(std::move(s));
            }
        }
        g_db[*key] = std::move(lv);
    }
}

void saveDb();

void loadDb() {
    g_loaded = true;
    auto path = dbPath();
    std::error_code ec;
    bool imported = false;
    if (!std::filesystem::exists(path, ec)) {
        // first run: bring over data from the old Death Buffer mod if it's there
        auto old = saveDir().parent_path() / "local.death-buffer" / "deaths.json";
        if (std::filesystem::exists(old, ec)) {
            path = old;
            imported = true;
        } else {
            log::info("No data yet. It will be saved in {}", utils::string::pathToString(saveDir()));
            return;
        }
    }
    auto res = file::readJson(path);
    if (!res) {
        log::error("Couldn't read {}: {}", utils::string::pathToString(path), res.unwrapErr());
        return;
    }
    auto root = std::move(res).unwrap();
    parseDb(root);
    if (imported) {
        log::info("[import] brought over {} level(s) from Death Buffer", g_db.size());
        saveDb();
    } else {
        log::info("Loaded data for {} level(s) from {}", g_db.size(), utils::string::pathToString(path));
    }
}

matjson::Value numberArray(std::vector<double> const& v) {
    std::vector<matjson::Value> out;
    out.reserve(v.size());
    for (double x : v) out.emplace_back(round1(x));
    return matjson::Value(std::move(out));
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
            o.set("y", d.y);
            o.set("practice", d.practice);
            deaths.push_back(std::move(o));
        }
        std::vector<matjson::Value> clicks;
        for (auto const& s : lv.spots) {
            auto o = matjson::Value::object();
            o.set("pct", round2(s.pct));
            o.set("x", round1(s.x));
            o.set("speed", std::round(s.speed * 1000.0) / 1000.0);
            o.set("samples", s.samples);
            if (auto t = phantom::rightTick(s)) o.set("rightTick", round1(*t));
            else o.set("rightTick", nullptr);
            o.set("misses", numberArray(s.misses));
            o.set("passes", numberArray(s.passes));
            o.set("totalMisses", s.totalMisses);
            o.set("totalPasses", s.totalPasses);
            o.set("buffered", s.buffered);
            o.set("bufferedMissStreak", s.bufferedMissStreak);
            auto hits = matjson::Value::object();
            for (auto const& [idx, n] : s.rangeHits) hits.set(std::to_string(idx), n);
            o.set("rangeHits", std::move(hits));
            clicks.push_back(std::move(o));
        }
        auto fit = matjson::Value::object();
        fit.set("n", lv.fit.n);
        fit.set("sx", lv.fit.sx);
        fit.set("sy", lv.fit.sy);
        fit.set("sxx", lv.fit.sxx);
        fit.set("sxy", lv.fit.sxy);

        auto l = matjson::Value::object();
        l.set("name", lv.name);
        l.set("levelLength", round1(lv.levelLength));
        l.set("rangeWidth", Mod::get()->getSettingValue<double>("range-width"));  // for the console tool
        l.set("fit", std::move(fit));
        l.set("deaths", matjson::Value(std::move(deaths)));
        l.set("clicks", matjson::Value(std::move(clicks)));
        root.set(key, std::move(l));
    }
    auto res = file::writeStringSafe(dbPath(), root.dump(matjson::NO_INDENTATION));
    if (!res) log::error("Couldn't save data: {}", res.unwrapErr());
}

// ---- trails -----------------------------------------------------------------

matjson::Value attemptJson(Attempt const& a) {
    auto o = matjson::Value::object();
    o.set("practice", a.practice);
    o.set("completed", a.completed);
    o.set("died", a.died);
    o.set("endPct", round2(a.endPct));
    o.set("death", matjson::Value(std::vector<matjson::Value>{round1(a.deathX), round1(a.deathY)}));
    std::vector<matjson::Value> segs;
    for (auto const& seg : a.segments) {
        std::vector<matjson::Value> flat;
        flat.reserve(seg.size() * 2);
        for (auto const& p : seg) {
            flat.emplace_back(round1(p.x));
            flat.emplace_back(round1(p.y));
        }
        segs.emplace_back(std::move(flat));
    }
    o.set("segments", matjson::Value(std::move(segs)));
    std::vector<matjson::Value> clicks;
    for (auto const& c : a.clicks) {
        clicks.emplace_back(std::vector<matjson::Value>{round1(c.x), round1(c.y), c.buffered});
    }
    o.set("clicks", matjson::Value(std::move(clicks)));
    return o;
}

std::optional<Attempt> attemptFrom(matjson::Value& o) {
    if (!o.isObject()) return std::nullopt;
    Attempt a;
    a.practice = o["practice"].asBool().unwrapOr(false);
    a.completed = o["completed"].asBool().unwrapOr(false);
    a.died = o["died"].asBool().unwrapOr(false);
    a.endPct = readNum(o["endPct"]);
    auto d = readNumbers(o["death"]);
    if (d.size() == 2) {
        a.deathX = static_cast<float>(d[0]);
        a.deathY = static_cast<float>(d[1]);
    }
    if (o["segments"].isArray()) {
        for (auto& s : o["segments"]) {
            auto flat = readNumbers(s);
            std::vector<phantom::TrailPoint> seg;
            for (size_t i = 0; i + 1 < flat.size(); i += 2) {
                seg.push_back({static_cast<float>(flat[i]), static_cast<float>(flat[i + 1])});
            }
            if (!seg.empty()) a.segments.push_back(std::move(seg));
        }
    }
    if (o["clicks"].isArray()) {
        for (auto& c : o["clicks"]) {
            if (!c.isArray() || c.size() < 3) continue;
            a.clicks.push_back({static_cast<float>(readNum(c[0])), static_cast<float>(readNum(c[1])),
                c[2].asBool().unwrapOr(false)});
        }
    }
    return a;
}

phantom::LevelTrails loadTrails(std::string const& key) {
    phantom::LevelTrails t;
    std::error_code ec;
    auto path = trailsPath(key);
    if (!std::filesystem::exists(path, ec)) return t;
    auto res = file::readJson(path);
    if (!res) return t;
    auto root = std::move(res).unwrap();
    if (auto b = attemptFrom(root["best"])) t.best = std::move(*b);
    if (root["recent"].isArray()) {
        for (auto& r : root["recent"]) {
            if (auto a = attemptFrom(r)) t.recent.push_back(std::move(*a));
        }
    }
    return t;
}

void saveTrails(std::string const& key, std::string const& name, phantom::LevelTrails const& t) {
    std::error_code ec;
    std::filesystem::create_directories(saveDir() / "trails", ec);
    auto root = matjson::Value::object();
    root.set("name", name);
    if (t.best) root.set("best", attemptJson(*t.best));
    else root.set("best", nullptr);
    std::vector<matjson::Value> recent;
    for (auto const& a : t.recent) recent.push_back(attemptJson(a));
    root.set("recent", matjson::Value(std::move(recent)));
    auto res = file::writeStringSafe(trailsPath(key), root.dump(matjson::NO_INDENTATION));
    if (!res) log::error("Couldn't save trails: {}", res.unwrapErr());
}

// ---- range rules from the console tool (ranges.json) -----------------------

std::filesystem::file_time_type g_rulesTime{};

phantom::RangeRules loadRules(std::string const& key, double width, bool& changed) {
    phantom::RangeRules rules;
    changed = false;
    std::error_code ec;
    auto path = rulesPath();
    if (!std::filesystem::exists(path, ec)) {
        changed = g_rulesTime != std::filesystem::file_time_type{};
        g_rulesTime = {};
        return rules;
    }
    auto t = std::filesystem::last_write_time(path, ec);
    changed = t != g_rulesTime;
    g_rulesTime = t;
    auto res = file::readJson(path);
    if (!res) {
        log::error("ranges.json has a mistake in it: {}", res.unwrapErr());
        return rules;
    }
    auto root = std::move(res).unwrap();
    if (!root[key].isObject()) return rules;
    for (auto& r : root[key]) {
        auto k = r.getKey();
        if (!k) continue;
        auto pctRes = utils::numFromString<double>(*k);
        if (!pctRes.isOk()) continue;
        double pct = pctRes.unwrap();
        phantom::RangeRule rule;
        if (r["ticks"].isNumber()) rule.ticks = std::clamp(readInt(r["ticks"]), 0, 20);
        rule.delay = r["mode"].asString().unwrapOr("smart") == "delay";
        rules[phantom::rangeIndex(pct, width)] = rule;
    }
    return rules;
}

std::filesystem::file_time_type rulesFileTime() {
    std::error_code ec;
    auto path = rulesPath();
    if (!std::filesystem::exists(path, ec)) return {};
    return std::filesystem::last_write_time(path, ec);
}

std::string levelKey(GJGameLevel* level) {
    int id = level->m_levelID.value();
    if (id > 0) return std::to_string(id);
    return "local:" + std::string(level->m_levelName);  // editor / unuploaded levels
}

// ------------------------------------------------------- per-level session --

struct Session {
    bool active = false;          // in a normal (non-platformer) level
    std::string key;
    LevelData* level = nullptr;   // points into g_db (std::map nodes never move)
    phantom::Learner learner;
    phantom::RangeRules rules;
    phantom::TrailRecorder trail;
    phantom::LevelTrails trails;
    int deathsThisSession = 0;
    bool deathRecorded = false;
    bool injecting = false;       // true while the mod sends a click to GD itself
    bool dirty = false;           // deaths changed since the last save
};

Session S;

PlayLayer* trackedPlayLayer() {
    if (!S.active) return nullptr;
    return PlayLayer::get();
}

void flushNotes() {
    if (optLog()) {
        for (auto const& n : S.learner.notes) log::info("{}", n);
    }
    S.learner.notes.clear();
}

void save() {
    saveDb();
    S.dirty = false;
    S.learner.dirty = false;
}

void reloadRules(bool quietIfSame) {
    bool changed = false;
    S.rules = loadRules(S.key, S.learner.cfg.rangeWidth, changed);
    S.learner.rules = &S.rules;
    if (optLog() && (changed || !quietIfSame) && !S.rules.empty()) {
        log::info("[console] {} range rule(s) active for \"{}\"", S.rules.size(), S.level->name);
    }
}

int deathsInRange(int idx) {
    int n = 0;
    for (auto const& d : S.level->deaths) n += phantom::rangeIndex(d.pct, S.learner.cfg.rangeWidth) == idx ? 1 : 0;
    return n;
}

void sendPress(PlayerObject* p) {
    S.injecting = true;
    p->pushButton(PlayerButton::Jump);
    S.injecting = false;
}

void sendRelease(PlayerObject* p) {
    S.injecting = true;
    p->releaseButton(PlayerButton::Jump);
    S.injecting = false;
}

// Give GD the click the mod is holding, right now.
void sendHeldPress(PlayLayer* pl) {
    auto p1 = pl->m_player1;
    sendPress(p1);
    S.learner.pressSent(p1->getPositionX(), pl->getCurrentPercent());
    S.trail.click(p1->getPositionX(), p1->getPositionY(), true);
}

// ------------------------------------------------------------------ overlay --

bool overlayWanted(PlayLayer* pl) {
    auto mode = optOverlay();
    if (mode == "Always") return true;
    if (mode == "Practice only") return pl->m_isPracticeMode;
    return false;
}

// percent -> x in the level, from the fit (or the level length as a fallback)
bool pctToX(PlayLayer* pl, double pct, double& x) {
    double a, b;
    if (S.level->fit.solve(a, b)) {
        x = (pct - b) / a;
        return true;
    }
    if (pl->m_levelLength > 0.f) {
        x = pct / 100.0 * pl->m_levelLength;
        return true;
    }
    return false;
}

float yOnTrail(Attempt const* a, float x, float fallback) {
    if (!a) return fallback;
    for (auto const& seg : a->segments) {
        for (size_t i = 1; i < seg.size(); ++i) {
            if (seg[i - 1].x <= x && x <= seg[i].x) {
                float t = seg[i].x > seg[i - 1].x ? (x - seg[i - 1].x) / (seg[i].x - seg[i - 1].x) : 0.f;
                return seg[i - 1].y + (seg[i].y - seg[i - 1].y) * t;
            }
        }
    }
    return fallback;
}

void drawTrail(CCDrawNode* node, Attempt const& a, ccColor4F color, float width) {
    for (auto const& seg : a.segments) {
        for (size_t i = 1; i < seg.size(); ++i) {
            node->drawSegment({seg[i - 1].x, seg[i - 1].y}, {seg[i].x, seg[i].y}, width, color);
        }
    }
}

void drawOverlay(PlayLayer* pl) {
    if (!pl || !pl->m_objectLayer) return;
    auto id = "overlay"_spr;
    auto node = static_cast<CCDrawNode*>(pl->m_objectLayer->getChildByID(id));
    if (!S.active || !overlayWanted(pl)) {
        if (node) node->removeFromParent();
        return;
    }
    if (!node) {
        node = CCDrawNode::create();
        node->setID(id);
        pl->m_objectLayer->addChild(node, 9999);
    }
    node->clear();
    node->removeAllChildren();

    auto const& cfg = S.learner.cfg;
    Attempt const* best = S.trails.best ? &*S.trails.best : nullptr;
    Attempt const* last = S.trails.recent.empty() ? nullptr : &S.trails.recent.front();

    // death ranges: bands coloured by what the buffer does there
    std::map<int, std::pair<int, double>> ranges;  // idx -> (deaths, sum of y)
    for (auto const& d : S.level->deaths) {
        auto& r = ranges[phantom::rangeIndex(d.pct, cfg.rangeWidth)];
        r.first++;
        r.second += d.y;
    }
    for (auto const& [idx, info] : ranges) {
        double x0, x1;
        if (!pctToX(pl, phantom::rangeLo(idx, cfg.rangeWidth), x0) ||
            !pctToX(pl, phantom::rangeHi(idx, cfg.rangeWidth), x1)) continue;

        bool anySpot = false, anyReady = false;
        for (auto const& s : S.level->spots) {
            if (s.range() != idx) continue;
            anySpot = true;
            anyReady = anyReady || phantom::spotReady(s, cfg);
        }
        int ticks = S.learner.ticksForRange(idx);
        bool delay = S.learner.rangeIsDelay(idx);

        ccColor4F col;
        std::string state;
        if (delay && ticks > 0) { col = {0.30f, 0.55f, 1.f, 1.f}; state = fmt::format("delay {}t", ticks); }
        else if (ticks <= 0) { col = {0.6f, 0.6f, 0.6f, 1.f}; state = "off"; }
        else if (anyReady) { col = {0.25f, 1.f, 0.45f, 1.f}; state = fmt::format("{}t ON", ticks); }
        else if (anySpot) { col = {1.f, 0.75f, 0.2f, 1.f}; state = "learning"; }
        else { col = {1.f, 0.3f, 0.3f, 1.f}; state = "deaths"; }

        float alpha = std::min(0.05f + 0.02f * info.first, 0.22f);
        CCPoint rect[4] = {{(float)x0, -600.f}, {(float)x1, -600.f}, {(float)x1, 4000.f}, {(float)x0, 4000.f}};
        node->drawPolygon(rect, 4, {col.r, col.g, col.b, alpha}, 0.6f, {col.r, col.g, col.b, 0.45f});

        float labelY = static_cast<float>(info.second / info.first) + 70.f;
        auto label = CCLabelBMFont::create(
            fmt::format("{}%  x{}  {}", phantom::rangeCenter(idx, cfg.rangeWidth), info.first, state).c_str(),
            "chatFont.fnt");
        label->setScale(0.55f);
        label->setColor({(GLubyte)(col.r * 255), (GLubyte)(col.g * 255), (GLubyte)(col.b * 255)});
        label->setPosition({(float)((x0 + x1) / 2.0), labelY});
        node->addChild(label);
    }

    // trails: best run in green, last attempt in white
    if (optTrails()) {
        if (last && last != best) drawTrail(node, *last, {1.f, 1.f, 1.f, 0.28f}, 0.6f);
        if (best) {
            drawTrail(node, *best, {0.3f, 1.f, 0.5f, 0.55f}, 0.9f);
            for (auto const& c : best->clicks) {
                node->drawDot({c.x, c.y}, c.buffered ? 3.f : 2.f,
                    c.buffered ? ccColor4F{1.f, 0.3f, 1.f, 0.9f} : ccColor4F{0.4f, 0.9f, 1.f, 0.7f});
            }
        }
    }

    // learned right ticks: a green post at each one
    for (auto const& s : S.level->spots) {
        auto t = phantom::rightTick(s);
        if (!t || !phantom::spotReady(s, cfg)) continue;
        float tx = static_cast<float>(*t);
        float ty = yOnTrail(best, tx, 105.f);
        node->drawSegment({tx, ty - 22.f}, {tx, ty + 22.f}, 1.2f, {0.3f, 1.f, 0.5f, 0.9f});
        node->drawDot({tx, ty + 24.f}, 3.f, {0.3f, 1.f, 0.5f, 0.9f});
    }

    // deaths: small red crosses (most recent ones)
    size_t from = S.level->deaths.size() > OVERLAY_DEATHS ? S.level->deaths.size() - OVERLAY_DEATHS : 0;
    for (size_t i = from; i < S.level->deaths.size(); ++i) {
        auto const& d = S.level->deaths[i];
        float x = static_cast<float>(d.x), y = static_cast<float>(d.y);
        ccColor4F red{1.f, 0.25f, 0.25f, 0.6f};
        node->drawSegment({x - 5.f, y - 5.f}, {x + 5.f, y + 5.f}, 0.8f, red);
        node->drawSegment({x - 5.f, y + 5.f}, {x + 5.f, y - 5.f}, 0.8f, red);
    }
}

void printSummary() {
    if (!S.level || !optLog()) return;
    auto const& lv = *S.level;
    auto const& cfg = S.learner.cfg;
    log::info("===== {}: {} death(s) this session, {} total =====", lv.name, S.deathsThisSession, lv.deaths.size());

    std::map<int, int> ranges;
    for (auto const& d : lv.deaths) ranges[phantom::rangeIndex(d.pct, cfg.rangeWidth)]++;
    std::vector<std::pair<int, int>> sorted(ranges.begin(), ranges.end());
    std::sort(sorted.begin(), sorted.end(), [](auto a, auto b) { return a.second > b.second; });

    size_t shown = std::min<size_t>(sorted.size(), 10);
    for (size_t i = 0; i < shown; ++i) {
        auto [idx, n] = sorted[i];
        int learned = 0, spots = 0;
        for (auto const& s : lv.spots) {
            if (s.range() != idx) continue;
            spots++;
            learned += phantom::spotReady(s, cfg) ? 1 : 0;
        }
        int ticks = S.learner.ticksForRange(idx);
        std::string mode = S.learner.rangeIsDelay(idx) ? "delay" : "smart";
        log::info("  {:>13}  {:4} death(s)  {} tick(s) {}  {}/{} click(s) learned",
            phantom::rangeLabel(idx, cfg.rangeWidth), n, ticks, mode, learned, spots);
    }
    log::info("  Change ticks per range with the console tool: python phantom.py");
}

void finishAttempt(PlayLayer* pl, bool died, bool completed, PlayerObject* who) {
    auto p = who ? who : pl->m_player1;
    Attempt a = S.trail.finish(died, completed, p->getPositionX(), p->getPositionY(), pl->getCurrentPercent());
    if (a.pointCount() < 2) return;
    if (S.trails.add(std::move(a)) && optLog() && !completed) {
        S.learner.notes.push_back(fmt::format("[trail] new best run saved ({:.2f}%)", S.trails.best->endPct));
    }
    saveTrails(S.key, S.level->name, S.trails);
}

}  // namespace

// ------------------------------------------------------------------ hooks --

// Clicks are caught here: the exact moment GD applies a jump to the player.
class $modify(PCPlayer, PlayerObject) {
    bool pushButton(PlayerButton button) {
        auto pl = trackedPlayLayer();
        if (S.injecting || !pl || button != PlayerButton::Jump || this != pl->m_player1) {
            return PlayerObject::pushButton(button);
        }

        // a new click while one is held: give GD the held one (and its release) first
        if (S.learner.hasHeldPress()) sendHeldPress(pl);
        if (S.learner.takeRelease()) sendRelease(this);

        double x = this->getPositionX();
        double pct = pl->getCurrentPercent();
        S.level->fit.add(x, pct);

        bool canBuffer = !pl->m_gameState.m_isDualMode && !pl->m_playerDied;
        if (S.learner.onPress(x, pct, canBuffer)) {
            return false;  // the mod has the click; GD gets it on the right tick
        }
        S.trail.click(static_cast<float>(x), this->getPositionY(), false);
        return PlayerObject::pushButton(button);
    }

    bool releaseButton(PlayerButton button) {
        auto pl = trackedPlayLayer();
        if (S.injecting || !pl || button != PlayerButton::Jump || this != pl->m_player1) {
            return PlayerObject::releaseButton(button);
        }
        if (S.learner.onRelease()) return false;  // sent right after the held click
        return PlayerObject::releaseButton(button);
    }
};

class $modify(PCGameLayer, GJBaseGameLayer) {
    // Runs once per physics step (240 per second).
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto pl = trackedPlayLayer();
        bool ours = pl && static_cast<GJBaseGameLayer*>(pl) == static_cast<GJBaseGameLayer*>(this);
        if (ours) {
            bool sendNow = S.learner.stepStart(m_player1->getPositionX());
            if (S.learner.takeDueRelease()) sendRelease(m_player1);
            if (sendNow) sendHeldPress(pl);
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (ours && trackedPlayLayer() == pl) {
            bool alive = !m_playerDied && !m_player1->m_isDead;
            float x = m_player1->getPositionX();
            S.learner.stepEnd(x, alive);
            if (alive) S.trail.step(x, m_player1->getPositionY());
        }
    }
};

class $modify(PCPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        S = Session{};
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        if (!g_loaded) loadDb();

        if (m_isPlatformer) {
            log::info("\"{}\" is a platformer level - Phantom Click is off here.", std::string(level->m_levelName));
            return true;
        }

        S.key = levelKey(level);
        auto& lv = g_db[S.key];
        lv.name = std::string(level->m_levelName);
        lv.levelLength = m_levelLength;
        S.level = &lv;
        S.learner.cfg = readSettings();
        S.learner.spots = &lv.spots;
        S.trails = loadTrails(S.key);
        S.trail.begin(m_isPracticeMode);
        S.active = true;
        reloadRules(false);

        log::info("Phantom Click on \"{}\": {} death(s) logged, {} click spot(s), {} with the buffer ON",
            lv.name, lv.deaths.size(), lv.spots.size(), S.learner.readySpots());
        drawOverlay(this);
        return true;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        PlayLayer::destroyPlayer(player, object);

        if (!S.active || S.deathRecorded || object == m_anticheatSpike) return;
        if (!m_playerDied && !(player && player->m_isDead)) return;  // didn't actually die (noclip etc.)
        S.deathRecorded = true;

        bool practice = m_isPracticeMode;
        bool countIt = !practice || optCountPractice();
        double pct = getCurrentPercent();
        std::string clickInfo = S.learner.died(countIt, pct);
        finishAttempt(this, true, false, player);
        if (!countIt) {
            flushNotes();
            return;
        }

        auto who = player ? player : m_player1;
        double x = who->getPositionX();
        S.level->deaths.push_back({round2(pct), round1(x), round1(who->getPositionY()), practice});
        S.level->fit.add(x, pct);
        S.deathsThisSession++;
        S.dirty = true;

        auto const& cfg = S.learner.cfg;
        int idx = phantom::rangeIndex(pct, cfg.rangeWidth);
        flushNotes();
        if (optLog()) {
            log::info("[death] {:.2f}%{} | range {} ({} death(s), {} tick(s) {}) | {}",
                pct, practice ? " (practice)" : "", phantom::rangeLabel(idx, cfg.rangeWidth), deathsInRange(idx),
                S.learner.ticksForRange(idx), S.learner.rangeIsDelay(idx) ? "delay" : "smart", clickInfo);
        }
        save();
    }

    void levelComplete() {
        if (S.active) {
            S.learner.completed();
            finishAttempt(this, false, true, nullptr);
            if (optLog()) S.learner.notes.push_back("[trail] level complete - saved as your best run");
        }
        PlayLayer::levelComplete();
    }

    void resetLevel() {
        if (S.active) {
            // never leave GD thinking the button is still held
            if (S.learner.takeRelease() && m_player1) sendRelease(m_player1);
            S.learner.newAttempt();
            S.learner.cfg = readSettings();  // pick up setting changes between attempts
            if (rulesFileTime() != g_rulesTime) reloadRules(true);  // console tool edits, live
            S.deathRecorded = false;
            flushNotes();
            if (S.dirty || S.learner.dirty) save();
        }
        PlayLayer::resetLevel();
        if (S.active) {
            S.trail.begin(m_isPracticeMode);
            drawOverlay(this);
        }
    }

    void togglePracticeMode(bool practiceMode) {
        PlayLayer::togglePracticeMode(practiceMode);
        if (S.active) drawOverlay(this);
    }

    void onQuit() {
        if (S.active) {
            flushNotes();
            printSummary();
            if (S.dirty || S.learner.dirty) save();
        }
        S = Session{};
        PlayLayer::onQuit();
    }
};

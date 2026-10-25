#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <algorithm>
#include <deque>
#include <map>
#include <vector>

using namespace geode::prelude;

namespace sc {
    struct Snap { int frame; CheckpointObject* cp; bool held; float dt; };

    inline std::deque<Snap> snaps;
    inline std::map<int, bool> inputs;
    inline std::map<int, bool> forced;
    inline int  frame = 0;
    inline int  forcedUntil = 0;
    inline bool busy = false;
    inline bool failed = false;
    inline bool injecting = false;
    inline bool pending = false;
    inline bool held = false;
    inline GameObject* killer = nullptr;

    inline bool enabled() { return Mod::get()->getSettingValue<bool>("enabled"); }
    inline int  setting(char const* k) { return static_cast<int>(Mod::get()->getSettingValue<int64_t>(k)); }

    inline void clearSnaps() {
        for (auto& s : snaps) s.cp->release();
        snaps.clear();
    }
    inline void resetAll() {
        clearSnaps();
        inputs.clear(); forced.clear();
        frame = 0; forcedUntil = 0;
        busy = failed = injecting = pending = held = false;
        killer = nullptr;
    }
}

class $modify(SCBase, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (sc::enabled() && !sc::injecting && button == 1 && isPlayer1) {
            if (sc::busy) return;
            if (sc::frame < sc::forcedUntil) return;
            sc::inputs[sc::frame] = down;
            sc::held = down;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(SCLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        sc::resetAll();
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        m_isTestMode = true;
        return true;
    }

    void resetLevel() {
        sc::resetAll();
        PlayLayer::resetLevel();
    }

    void onQuit() {
        sc::resetAll();
        PlayLayer::onQuit();
    }

    void inject(bool down) {
        sc::injecting = true;
        static_cast<GJBaseGameLayer*>(this)->handleButton(down, 1, true);
        sc::injecting = false;
        sc::held = down;
    }

    float dtFor(int f) {
        auto& S = sc::snaps;
        if (S.empty()) return 1.f / 60.f;
        int idx = f - S.front().frame;
        if (idx >= 0 && idx < (int)S.size()) return S[idx].dt;
        return S.back().dt;
    }

    bool simulate(std::map<int, bool> const& in, int endFrame, int fromFrame) {
        auto& S = sc::snaps;
        if (S.empty()) return false;
        int idx = std::clamp(fromFrame - S.front().frame, 0, (int)S.size() - 1);
        auto& first = S[idx];
        PlayLayer::loadFromCheckpoint(first.cp);
        sc::busy = true;
        sc::failed = false;
        inject(first.held);
        for (int f = first.frame; f < endFrame && !sc::failed; f++) {
            auto it = in.find(f);
            if (it != in.end()) inject(it->second);
            PlayLayer::update(dtFor(f));
        }
        sc::busy = false;
        return !sc::failed;
    }

    bool correct() {
        auto& S = sc::snaps;
        if (S.size() < 3) return false;

        int deathFrame = sc::frame;
        int startFrame = S.front().frame;
        int maxShift = sc::setting("max-shift");
        int maxClicks = sc::setting("max-clicks");
        int extra = sc::setting("extra");

        std::vector<int> ev;
        for (auto& [f, d] : sc::inputs) if (f >= startFrame) ev.push_back(f);
        if (ev.empty()) return false;

        for (int s = 1; s <= maxShift; s++) {
            for (int sign : {1, -1}) {
                for (int group = 1; group <= std::min<int>(maxClicks, ev.size()); group++) {
                    auto in = sc::inputs;
                    int delta = sign * s;
                    std::vector<std::pair<int, bool>> moved;
                    for (int k = 0; k < group; k++) {
                        int f = ev[ev.size() - 1 - k];
                        moved.push_back({f, in[f]});
                        in.erase(f);
                    }
                    bool ok = true;
                    int fromFrame = deathFrame;
                    for (auto& [f, d] : moved) {
                        int nf = f + delta;
                        if (nf < startFrame || nf >= deathFrame + extra || in.count(nf)) { ok = false; break; }
                        in[nf] = d;
                        fromFrame = std::min({fromFrame, f, nf});
                    }
                    if (!ok) continue;

                    if (!simulate(in, deathFrame + extra, fromFrame)) continue;

                    simulate(in, deathFrame, fromFrame);
                    sc::inputs.clear();
                    sc::forced.clear();
                    for (auto& [f, d] : in) {
                        if (f < deathFrame) sc::inputs[f] = d;
                        else sc::forced[f] = d;
                    }
                    sc::forcedUntil = sc::forced.empty() ? deathFrame : sc::forced.rbegin()->first + 1;
                    sc::clearSnaps();
                    return true;
                }
            }
        }

        simulate(sc::inputs, deathFrame, startFrame);
        sc::clearSnaps();
        return false;
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        if (!sc::enabled() || obj == m_anticheatSpike || !m_player1) {
            return PlayLayer::destroyPlayer(player, obj);
        }
        if (sc::busy) { sc::failed = true; return; }
        sc::pending = true;
        sc::killer = obj;
    }

    void update(float dt) {
        if (!sc::enabled() || sc::busy) return PlayLayer::update(dt);

        if (sc::pending) {
            sc::pending = false;
            if (!correct()) {
                PlayLayer::destroyPlayer(m_player1, sc::killer);
                return;
            }
        }

        auto it = sc::forced.find(sc::frame);
        if (it != sc::forced.end()) inject(it->second);

        auto* cp = PlayLayer::createCheckpoint();
        if (cp) {
            cp->retain();
            sc::snaps.push_back({sc::frame, cp, sc::held, dt});
            while ((int)sc::snaps.size() > sc::setting("window")) {
                sc::snaps.front().cp->release();
                sc::snaps.pop_front();
            }
        }

        PlayLayer::update(dt);
        sc::frame++;

        while (!sc::inputs.empty() && sc::inputs.begin()->first < sc::frame - 120)
            sc::inputs.erase(sc::inputs.begin());
    }
};

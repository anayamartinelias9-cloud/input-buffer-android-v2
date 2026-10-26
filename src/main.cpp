#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <vector>

using namespace geode::prelude;

namespace sc {
    struct Snap { int frame; CheckpointObject* cp; bool held; float dt; };
    struct Unit { int press; int release; }; // press=-1: ya estaba presionado; release=-1: sigue presionado

    inline std::deque<Snap> snaps;
    inline std::map<int, bool> inputs;
    inline std::map<int, bool> forced;
    inline int  frame = 0;
    inline int  forcedUntil = 0;
    inline int  graceUntil = 0;
    inline int  saves = 0;
    inline bool busy = false;
    inline bool failed = false;
    inline bool pending = false;
    inline bool held = false;
    inline bool alive = false;
    inline bool allowDeath = false;
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
        frame = 0; forcedUntil = 0; graceUntil = 0; saves = 0;
        busy = failed = pending = held = allowDeath = false;
        killer = nullptr;
    }
}

class $modify(SCPlay, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        sc::resetAll();
        sc::alive = false;
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        m_isTestMode = true;
        Notification::create("Second Chance cargado", NotificationIcon::Success, 0.8f)->show();
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

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        if (sc::busy) { sc::failed = true; return; }

        // Seguro anti-noclip: solo se tapa la muerte si todo está funcionando
        if (!sc::enabled() || sc::allowDeath || obj == m_anticheatSpike || !m_player1 ||
            !sc::alive || sc::snaps.size() < 3 || sc::saves >= sc::setting("max-saves")) {
            return PlayLayer::destroyPlayer(player, obj);
        }
        if (sc::frame < sc::graceUntil) {
            Notification::create("Second Chance: sin salida", NotificationIcon::Error, 0.4f)->show();
            return PlayLayer::destroyPlayer(player, obj);
        }
        sc::pending = true;
        sc::killer = obj;
    }
};

class $modify(SCBase, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (sc::enabled() && button == 1 && isPlayer1) {
            if (sc::busy) return;
            if (sc::frame < sc::forcedUntil) return;
            sc::inputs[sc::frame] = down;
            sc::held = down;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void inject(bool down) {
        GJBaseGameLayer::handleButton(down, 1, true);
        sc::held = down;
    }

    float dtFor(int f) {
        auto& S = sc::snaps;
        if (S.empty()) return 1.f / 60.f;
        int idx = f - S.front().frame;
        if (idx >= 0 && idx < (int)S.size()) return S[idx].dt;
        return S.back().dt;
    }

    bool simulate(PlayLayer* pl, std::map<int, bool> const& in, int endFrame, int fromFrame) {
        auto& S = sc::snaps;
        if (S.empty()) return false;
        int idx = std::clamp(fromFrame - S.front().frame, 0, (int)S.size() - 1);
        auto& first = S[idx];
        pl->loadFromCheckpoint(first.cp);
        sc::busy = true;
        sc::failed = false;
        inject(first.held);
        for (int f = first.frame; f < endFrame && !sc::failed; f++) {
            auto it = in.find(f);
            if (it != in.end()) inject(it->second);
            GJBaseGameLayer::update(dtFor(f));
        }
        sc::busy = false;
        return !sc::failed;
    }

    bool restoreOriginal(PlayLayer* pl, int deathFrame, int startFrame) {
        simulate(pl, sc::inputs, deathFrame, startFrame);
        sc::clearSnaps();
        return false;
    }

    bool correct(PlayLayer* pl) {
        auto& S = sc::snaps;
        int deathFrame = sc::frame;
        int startFrame = S.empty() ? 0 : S.front().frame;
        auto origPos = pl->m_player1->getPosition(); // dónde chocaste

        if (S.size() < 3 || sc::saves >= sc::setting("max-saves")) {
            return restoreOriginal(pl, deathFrame, startFrame);
        }

        int maxShift = sc::setting("max-shift");
        int maxClicks = sc::setting("max-clicks");
        int clickWin = sc::setting("click-window");
        int extra = sc::setting("extra");
        int insertWin = sc::setting("insert-window");
        int step = std::max(1, sc::setting("bot-step"));
        int budget = sc::setting("budget-ms");
        float maxTele = static_cast<float>(sc::setting("max-teleport"));
        bool botInsert = Mod::get()->getSettingValue<bool>("bot-insert");

        // Tus saltos = cada click con su soltada
        std::vector<sc::Unit> all;
        bool open = false;
        if (S.front().held) { all.push_back({-1, -1}); open = true; }
        for (auto& [f, d] : sc::inputs) {
            if (f < startFrame) continue;
            if (d) {
                if (!open) { all.push_back({f, -1}); open = true; }
            } else if (open && !all.empty() && all.back().release == -1) {
                all.back().release = f;
                open = false;
            }
        }

        // Solo cuentan tus clicks recientes (dentro de la ventana de clicks)
        std::vector<sc::Unit> units;
        for (auto& u : all) {
            if ((u.press >= 0 && u.press >= deathFrame - clickWin) ||
                (u.release >= 0 && u.release >= deathFrame - clickWin)) {
                units.push_back(u);
            }
        }

        // Sin click tuyo reciente y el bot apagado: no se hace nada, mueres normal
        if (units.empty() && !botInsert) {
            sc::clearSnaps();
            return false;
        }

        // La simulación original TIENE que reproducir la muerte; si no, no es fiable.
        if (simulate(pl, sc::inputs, deathFrame, startFrame)) {
            sc::clearSnaps();
            return false;
        }

        auto t0 = std::chrono::steady_clock::now();
        auto timeUp = [&]() {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            return ms > budget;
        };

        auto apply = [&](std::map<int, bool>& in, int fromFrame) -> bool {
            if (!simulate(pl, in, deathFrame + extra, fromFrame)) return false;
            if (!simulate(pl, in, deathFrame, fromFrame)) return false;
            // Sin teletransportes largos
            if (ccpDistance(pl->m_player1->getPosition(), origPos) > maxTele) return false;
            sc::inputs.clear();
            sc::forced.clear();
            for (auto& [f, d] : in) {
                if (f < deathFrame) sc::inputs[f] = d;
                else sc::forced[f] = d;
            }
            sc::forcedUntil = sc::forced.empty() ? deathFrame : sc::forced.rbegin()->first + 1;
            sc::graceUntil = deathFrame + extra;
            sc::saves++;
            sc::clearSnaps();
            Notification::create("Second Chance: click corregido", NotificationIcon::Success, 0.4f)->show();
            return true;
        };

        // 1) Mover tus últimos saltos conservando cuánto los mantuviste
        for (int s = 1; s <= maxShift && !units.empty(); s++) {
            for (int sign : {1, -1}) {
                for (int group = 1; group <= std::min<int>(maxClicks, units.size()); group++) {
                    if (timeUp()) return restoreOriginal(pl, deathFrame, startFrame);
                    auto in = sc::inputs;
                    int delta = sign * s;
                    int fromFrame = deathFrame;
                    std::vector<std::pair<int, bool>> add;
                    for (int k = 0; k < group; k++) {
                        auto& u = units[units.size() - 1 - k];
                        if (u.press >= 0) {
                            in.erase(u.press);
                            add.push_back({u.press + delta, true});
                            fromFrame = std::min({fromFrame, u.press, u.press + delta});
                        }
                        if (u.release >= 0) {
                            in.erase(u.release);
                            add.push_back({u.release + delta, false});
                            fromFrame = std::min({fromFrame, u.release, u.release + delta});
                        }
                    }
                    bool ok = true;
                    for (auto& [nf, d] : add) {
                        if (nf < startFrame || nf >= deathFrame + extra || in.count(nf)) { ok = false; break; }
                        in[nf] = d;
                    }
                    if (!ok) continue;
                    if (apply(in, fromFrame)) return true;
                }
            }
        }

        // 2) Solo si lo activaste en los ajustes: el bot mete un salto propio
        if (botInsert) {
            auto heldAt = [&](int t) {
                bool h = S.front().held;
                for (auto& [f, d] : sc::inputs) {
                    if (f >= t) break;
                    if (f >= startFrame) h = d;
                }
                return h;
            };

            for (int t = deathFrame - 1; t >= std::max(startFrame, deathFrame - insertWin); t -= step) {
                bool h0 = heldAt(t);
                for (int h : {2, 6, 12}) {
                    if (timeUp()) return restoreOriginal(pl, deathFrame, startFrame);
                    int te = t + h;
                    if (te >= deathFrame + extra) continue;
                    bool clash = false;
                    for (int f = t; f <= te; f++) {
                        if (sc::inputs.count(f)) { clash = true; break; }
                    }
                    if (clash) continue;
                    auto in = sc::inputs;
                    in[t] = !h0;
                    in[te] = h0;
                    if (apply(in, t)) return true;
                }
            }
        }

        return restoreOriginal(pl, deathFrame, startFrame);
    }

    void update(float dt) {
        auto pl = PlayLayer::get();
        if (!sc::enabled() || sc::busy || !pl ||
            static_cast<GJBaseGameLayer*>(pl) != static_cast<GJBaseGameLayer*>(this)) {
            return GJBaseGameLayer::update(dt);
        }

        if (!sc::alive) {
            sc::alive = true;
            Notification::create("Second Chance: activo", NotificationIcon::Success, 0.6f)->show();
        }

        if (sc::pending) {
            sc::pending = false;
            if (!correct(pl)) {
                Notification::create("Second Chance: no se pudo corregir", NotificationIcon::Warning, 0.4f)->show();
                sc::allowDeath = true;
                pl->destroyPlayer(pl->m_player1, sc::killer);
                sc::allowDeath = false;
                return GJBaseGameLayer::update(dt);
            }
        }

        auto it = sc::forced.find(sc::frame);
        if (it != sc::forced.end()) {
            inject(it->second);
            sc::inputs[sc::frame] = it->second;
        }

        auto* cp = pl->createCheckpoint();
        if (cp) {
            cp->retain();
            sc::snaps.push_back({sc::frame, cp, sc::held, dt});
            while ((int)sc::snaps.size() > sc::setting("window")) {
                sc::snaps.front().cp->release();
                sc::snaps.pop_front();
            }
        }

        GJBaseGameLayer::update(dt);
        sc::frame++;

        while (!sc::inputs.empty() && sc::inputs.begin()->first < sc::frame - 240)
            sc::inputs.erase(sc::inputs.begin());
    }
};

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

namespace pb {
    inline bool pending = false;
    inline bool releaseQueued = false;
    inline bool releaseNext = false;
    inline bool injecting = false;
    inline int  age = 0;

    inline bool enabled() { return Mod::get()->getSettingValue<bool>("enabled"); }
    inline int  ticks()   { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("ticks")); }
    inline void reset()   { pending = releaseQueued = releaseNext = injecting = false; age = 0; }

    inline bool holdMode(PlayerObject* p) {
        return p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing;
    }
    inline bool canAct(PlayerObject* p) {
        if (!p) return true;
        if (holdMode(p)) return true;
        if (p->m_isOnGround) return true;
        return p->m_touchingRings && p->m_touchingRings->count() > 0;
    }
}

class $modify(PBBase, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (!pb::enabled() || pb::injecting || button != 1 || !isPlayer1 || !m_player1) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        if (down) {
            if (pb::canAct(m_player1)) {
                pb::pending = false;
                return GJBaseGameLayer::handleButton(true, button, isPlayer1);
            }
            pb::pending = true;
            pb::releaseQueued = false;
            pb::age = 0;
            return;
        }
        if (pb::pending) { pb::releaseQueued = true; return; }
        GJBaseGameLayer::handleButton(false, button, isPlayer1);
    }
};

class $modify(PBLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        pb::reset();
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        m_isTestMode = true;
        return true;
    }

    void resetLevel() {
        pb::reset();
        PlayLayer::resetLevel();
    }

    void inject(bool down) {
        pb::injecting = true;
        static_cast<GJBaseGameLayer*>(this)->handleButton(down, 1, true);
        pb::injecting = false;
    }

    void update(float dt) {
        if (pb::enabled()) {
            if (pb::releaseNext) {
                pb::releaseNext = false;
                inject(false);
            }
            if (pb::pending) {
                pb::age++;
                if (pb::canAct(m_player1)) {
                    pb::pending = false;
                    inject(true);
                    if (pb::releaseQueued) {
                        pb::releaseQueued = false;
                        pb::releaseNext = true;
                    }
                } else if (pb::age > pb::ticks()) {
                    pb::pending = false;
                    if (pb::releaseQueued) pb::releaseQueued = false;
                }
            }
        }
        PlayLayer::update(dt);
    }
};

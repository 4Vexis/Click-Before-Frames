#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
using namespace geode::prelude;

namespace {
    struct Pending {
        int ticks = 0;
        bool held = false;
    };
    Pending g_p[2];
    bool g_injecting = false;

    const char* modeLabel(int m) {
        switch (m) {
            case 1: return "LOW";
            case 2: return "MED";
            case 3: return "HIGH";
            default: return "OFF";
        }
    }

    int currentWindow() {
        auto mod = Mod::get();
        switch (mod->getSavedValue<int>("mode", 2)) {
            case 1: return static_cast<int>(mod->getSettingValue<int64_t>("low-ticks"));
            case 2: return static_cast<int>(mod->getSettingValue<int64_t>("med-ticks"));
            case 3: return static_cast<int>(mod->getSettingValue<int64_t>("high-ticks"));
            default: return 0;
        }
    }

    bool bufferable(PlayerObject* p) {
        return p && !(p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing);
    }
    bool canActNow(PlayerObject* p) {
        if (p->m_isOnGround) return true;
        auto* rings = p->m_touchingRings;
        return rings && rings->count() > 0;
    }
}

class $modify(BufferGL, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        int window = currentWindow();
        if (g_injecting || !PlayLayer::get() || button != 1 || window <= 0) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        auto& s = g_p[isPlayer1 ? 0 : 1];
        auto* player = isPlayer1 ? m_player1 : m_player2;
        if (down) {
            if (bufferable(player) && !canActNow(player)) {
                s.ticks = window;
                s.held = true;
                return;
            }
            s = {};
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        if (s.ticks > 0) {
            s.held = false;
            return;
        }
        return GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt) {
        for (int i = 0; i < 2; i++) {
            auto& s = g_p[i];
            if (s.ticks <= 0) continue;
            auto* player = i == 0 ? m_player1 : m_player2;
            if (!player) { s = {}; continue; }
            bool fire = canActNow(player);
            bool expired = !fire && --s.ticks <= 0;
            if (fire || (expired && s.held)) {
                g_injecting = true;
                GJBaseGameLayer::handleButton(true, 1, i == 0);
                if (!s.held) GJBaseGameLayer::handleButton(false, 1, i == 0);
                g_injecting = false;
                s = {};
            } else if (expired) {
                s = {};
            }
        }
        GJBaseGameLayer::processCommands(dt);
    }
};

class $modify(BufferPL, PlayLayer) {
    void resetLevel() {
        g_p[0] = {};
        g_p[1] = {};
        PlayLayer::resetLevel();
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("show-button")) return;

        auto win = CCDirector::sharedDirector()->getWinSize();
        float mx = static_cast<float>(mod->getSettingValue<double>("btn-x"));
        float my = static_cast<float>(mod->getSettingValue<double>("btn-y"));
        float scale = static_cast<float>(mod->getSettingValue<double>("btn-scale"));

        auto spr = ButtonSprite::create(modeLabel(mod->getSavedValue<int>("mode", 2)));
        spr->setScale(scale);
        auto btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(BufferPL::onBufferToggle)
        );
        btn->setPosition({0.f, 0.f});

        auto menu = CCMenu::create();
        menu->addChild(btn);
        menu->setPosition({win.width - mx, win.height - my});
        menu->setID("buffer-menu"_spr);
        m_uiLayer->addChild(menu, 100);
    }

    void onBufferToggle(CCObject* sender) {
        auto mod = Mod::get();
        int m = (mod->getSavedValue<int>("mode", 2) + 1) % 4;
        mod->setSavedValue<int>("mode", m);
        g_p[0] = {};
        g_p[1] = {};
        if (auto btn = typeinfo_cast<CCMenuItemSpriteExtra*>(sender)) {
            if (auto spr = typeinfo_cast<ButtonSprite*>(btn->getNormalImage())) {
                spr->setString(modeLabel(m));
            }
        }
    }
};

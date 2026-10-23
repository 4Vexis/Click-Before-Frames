#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <algorithm>
#include <chrono>
#include <deque>
using namespace geode::prelude;

// ---------------------------------------------------------------- state

namespace {
    struct Ev { int wait; bool down; bool p1; };

    std::deque<Ev> g_queue;
    int g_shift = 0;
    bool g_injecting = false;
    bool g_simulating = false;
    bool g_ghostDied = false;
    float g_dt = 0.25f;
    int g_tick = 0;
    int g_pressTick = -1;
    int g_hold = 12;
    PlayerObject* g_ghost = nullptr;
    CCLayer* g_ghostLayer = nullptr;

    int currentMaxDelay() {
        auto mod = Mod::get();
        switch (mod->getSavedValue<int>("mode", 2)) {
            case 1: return static_cast<int>(mod->getSettingValue<int64_t>("low-ticks"));
            case 2: return static_cast<int>(mod->getSettingValue<int64_t>("med-ticks"));
            case 3: return static_cast<int>(mod->getSettingValue<int64_t>("high-ticks"));
            default: return 0;
        }
    }

    bool flying(PlayerObject* p) {
        return p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing;
    }

    bool canActNow(PlayerObject* p) {
        if (p->m_isOnGround) return true;
        auto* rings = p->m_touchingRings;
        return rings && rings->count() > 0;
    }

    void clearPending() {
        g_queue.clear();
        g_shift = 0;
        g_pressTick = -1;
    }

    void dropGhost() {
        if (g_ghost) {
            g_ghost->removeFromParentAndCleanup(true);
            g_ghost->release();
            g_ghost = nullptr;
        }
        if (g_ghostLayer) {
            g_ghostLayer->release();
            g_ghostLayer = nullptr;
        }
    }

    PlayerObject* ensureGhost(PlayLayer* pl) {
        if (g_ghost) return g_ghost;
        g_ghostLayer = CCLayer::create();
        g_ghostLayer->retain();
        g_ghost = PlayerObject::create(1, 1, pl, g_ghostLayer, false);
        g_ghost->retain();
        g_ghost->m_isSecondPlayer = true;
        g_ghost->setVisible(false);
        g_ghostLayer->addChild(g_ghost);
        return g_ghost;
    }

    void copyState(PlayerObject* g, PlayerObject* r) {
        g->setVisible(false);
        g->resetTouchedRings(true);
        g->copyAttributes(r);
        g->setScale(r->getScale());
        g->setContentSize(r->getContentSize());
        g->m_width = r->m_width;
        g->m_height = r->m_height;
        g->m_gravityMod = r->m_gravityMod;
        g->m_isOnGround = r->m_isOnGround;
        g->m_isUpsideDown = r->m_isUpsideDown;
        g->m_vehicleSize = r->m_vehicleSize;
        g->m_playerSpeed = r->m_playerSpeed;
        g->m_isDashing = r->m_isDashing;
        g->m_lastActivatedPortal = r->m_lastActivatedPortal;
        g->m_holdingButtons = r->m_holdingButtons;
        g->m_jumpBuffered = r->m_jumpBuffered;
        g->m_wasJumpBuffered = r->m_wasJumpBuffered;
        g->m_stateJumpBuffered = r->m_stateJumpBuffered;
        g->m_stateRingJump = r->m_stateRingJump;
        g->m_stateRingJump2 = r->m_stateRingJump2;
        g->m_wasRobotJump = r->m_wasRobotJump;
        g->m_touchedRing = r->m_touchedRing;
        g->m_touchedPad = r->m_touchedPad;
        g->m_touchedGravityPortal = r->m_touchedGravityPortal;
    }

    // Does a ghost that taps after `delay` ticks stay alive for `horizon` ticks?
    bool ghostSurvives(PlayLayer* pl, PlayerObject* real, int delay, int horizon) {
        auto* g = ensureGhost(pl);
        copyState(g, real);
        g_simulating = true;
        g_ghostDied = false;
        int total = delay + horizon;
        for (int t = 0; t < total && !g_ghostDied; ++t) {
            if (t == delay) g->pushButton(PlayerButton::Jump);
            if (t == delay + g_hold) g->releaseButton(PlayerButton::Jump);
            g->update(g_dt);
            g->updateRotation(g_dt);
            if (g_ghostDied) break;
            g->m_collisionLogTop->removeAllObjects();
            g->m_collisionLogBottom->removeAllObjects();
            g->m_collisionLogLeft->removeAllObjects();
            g->m_collisionLogRight->removeAllObjects();
            pl->checkCollisions(g, g_dt, false);
        }
        g_simulating = false;
        return !g_ghostDied;
    }

    // Smallest delay that saves you. 0 = press now.
    int findDelay(PlayLayer* pl, PlayerObject* real, int maxDelay, int horizon, double budgetMs) {
        auto start = std::chrono::steady_clock::now();
        auto overBudget = [&] {
            return std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count() > budgetMs;
        };
        if (ghostSurvives(pl, real, 0, horizon)) return 0;
        for (int d = 1; d <= maxDelay; ++d) {
            if (overBudget()) return 0;
            if (ghostSurvives(pl, real, d, horizon)) return d;
        }
        return 0;
    }
}

// ---------------------------------------------------------------- UI

void openModesMenu();

// Floating draggable "V" button. Its hitbox is computed from its live
// position on every touch, so it always follows the button.
class VButton : public CCLayer {
protected:
    CCPoint m_startTouch;
    CCPoint m_startPos;
    bool m_dragging = false;

    bool init() override {
        if (!CCLayer::init()) return false;
        auto label = CCLabelBMFont::create("V", "bigFont.fnt");
        auto icon = CircleButtonSprite::create(label, CircleBaseColor::Blue, CircleBaseSize::Small);
        auto size = icon->getContentSize();
        this->setContentSize(size);
        this->ignoreAnchorPointForPosition(false);
        this->setAnchorPoint({0.5f, 0.5f});
        icon->setPosition({size.width / 2, size.height / 2});
        this->addChild(icon);
        this->setTouchEnabled(true);
        return true;
    }

    bool hits(CCTouch* touch) {
        auto local = this->convertToNodeSpace(touch->getLocation());
        auto size = this->getContentSize();
        float r = size.width / 2 + 6.f;
        float dx = local.x - size.width / 2;
        float dy = local.y - size.height / 2;
        return dx * dx + dy * dy <= r * r;
    }

public:
    static VButton* create() {
        auto ret = new VButton();
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    void restorePosition() {
        auto win = CCDirector::sharedDirector()->getWinSize();
        auto size = this->getContentSize();
        auto mod = Mod::get();
        float x = mod->getSavedValue<float>("v-x", win.width - 34.f);
        float y = mod->getSavedValue<float>("v-y", win.height - 60.f);
        x = std::clamp(x, size.width / 2, win.width - size.width / 2);
        y = std::clamp(y, size.height / 2, win.height - size.height / 2);
        this->setPosition({x, y});
    }

    void registerWithTouchDispatcher() override {
        CCDirector::sharedDirector()->getTouchDispatcher()->addTargetedDelegate(this, -200, true);
    }

    bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
        auto parent = this->getParent();
        if (!parent || !this->isVisible() || !hits(touch)) return false;
        m_startTouch = parent->convertToNodeSpace(touch->getLocation());
        m_startPos = this->getPosition();
        m_dragging = false;
        return true;
    }

    void ccTouchMoved(CCTouch* touch, CCEvent*) override {
        auto parent = this->getParent();
        if (!parent) return;
        auto cur = parent->convertToNodeSpace(touch->getLocation());
        float dx = cur.x - m_startTouch.x;
        float dy = cur.y - m_startTouch.y;
        if (!m_dragging && dx * dx + dy * dy < 100.f) return;
        m_dragging = true;
        auto win = CCDirector::sharedDirector()->getWinSize();
        auto size = this->getContentSize();
        float x = std::clamp(m_startPos.x + dx, size.width / 2, win.width - size.width / 2);
        float y = std::clamp(m_startPos.y + dy, size.height / 2, win.height - size.height / 2);
        this->setPosition({x, y});
    }

    void ccTouchEnded(CCTouch*, CCEvent*) override {
        if (m_dragging) {
            auto mod = Mod::get();
            mod->setSavedValue<float>("v-x", this->getPositionX());
            mod->setSavedValue<float>("v-y", this->getPositionY());
        } else {
            openModesMenu();
        }
        m_dragging = false;
    }

    void ccTouchCancelled(CCTouch*, CCEvent*) override {
        m_dragging = false;
    }
};

// Menu with one checkbox per mode.
class ModesMenu : public CCLayer {
protected:
    CCSprite* m_checks[4] = {nullptr, nullptr, nullptr, nullptr};

    bool init() override {
        if (!CCLayer::init()) return false;
        auto win = CCDirector::sharedDirector()->getWinSize();
        float cx = win.width / 2;
        float cy = win.height / 2;

        this->addChild(CCLayerColor::create({0, 0, 0, 140}));

        auto panel = CCScale9Sprite::create("GJ_square01.png");
        panel->setContentSize({230.f, 210.f});
        panel->setPosition({cx, cy});
        this->addChild(panel);

        auto title = CCLabelBMFont::create("Click Before Frames", "goldFont.fnt");
        title->setScale(0.6f);
        title->setPosition({cx, cy + 85.f});
        this->addChild(title);

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        this->addChild(menu);

        auto mod = Mod::get();
        const char* names[4] = {"OFF", "LOW", "MED", "HIGH"};
        int ticks[4] = {
            0,
            static_cast<int>(mod->getSettingValue<int64_t>("low-ticks")),
            static_cast<int>(mod->getSettingValue<int64_t>("med-ticks")),
            static_cast<int>(mod->getSettingValue<int64_t>("high-ticks"))
        };

        for (int i = 0; i < 4; i++) {
            float y = cy + 45.f - i * 32.f;
            std::string text = i == 0 ? std::string("OFF")
                                      : fmt::format("{}  ({} ticks)", names[i], ticks[i]);
            auto lbl = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
            lbl->setScale(0.5f);
            lbl->setAnchorPoint({0.f, 0.5f});
            lbl->setPosition({cx - 85.f, y});
            this->addChild(lbl);

            auto check = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
            m_checks[i] = check;
            auto btn = CCMenuItemSpriteExtra::create(
                check, this, menu_selector(ModesMenu::onPick)
            );
            btn->setTag(i);
            btn->setPosition({cx + 85.f, y});
            menu->addChild(btn);
        }

        auto setSpr = ButtonSprite::create("Settings");
        setSpr->setScale(0.8f);
        auto setBtn = CCMenuItemSpriteExtra::create(
            setSpr, this, menu_selector(ModesMenu::onSettings)
        );
        setBtn->setPosition({cx, cy - 82.f});
        menu->addChild(setBtn);

        auto closeSpr = CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png");
        auto closeBtn = CCMenuItemSpriteExtra::create(
            closeSpr, this, menu_selector(ModesMenu::onClose)
        );
        closeBtn->setPosition({cx - 115.f, cy + 105.f});
        menu->addChild(closeBtn);

        this->refresh();
        this->setTouchEnabled(true);
        this->setKeypadEnabled(true);
        this->setID("buffer-modes"_spr);
        return true;
    }

    void refresh() {
        int mode = Mod::get()->getSavedValue<int>("mode", 2);
        auto cache = CCSpriteFrameCache::sharedSpriteFrameCache();
        for (int i = 0; i < 4; i++) {
            m_checks[i]->setDisplayFrame(cache->spriteFrameByName(
                i == mode ? "GJ_checkOn_001.png" : "GJ_checkOff_001.png"
            ));
        }
    }

    void onPick(CCObject* sender) {
        int i = static_cast<CCNode*>(sender)->getTag();
        Mod::get()->setSavedValue<int>("mode", i);
        clearPending();
        this->refresh();
    }

    void onSettings(CCObject*) {
        openSettingsPopup(Mod::get());
        this->removeFromParentAndCleanup(true);
    }

    void onClose(CCObject*) {
        this->removeFromParentAndCleanup(true);
    }

public:
    static ModesMenu* create() {
        auto ret = new ModesMenu();
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    void registerWithTouchDispatcher() override {
        CCDirector::sharedDirector()->getTouchDispatcher()->addTargetedDelegate(this, -500, true);
    }
    bool ccTouchBegan(CCTouch*, CCEvent*) override { return true; }
    void ccTouchMoved(CCTouch*, CCEvent*) override {}
    void ccTouchEnded(CCTouch*, CCEvent*) override {}
    void ccTouchCancelled(CCTouch*, CCEvent*) override {}
    void keyBackClicked() override { this->onClose(nullptr); }
};

void openModesMenu() {
    auto scene = CCDirector::sharedDirector()->getRunningScene();
    if (!scene || scene->getChildByID("buffer-modes"_spr)) return;
    auto layer = ModesMenu::create();
    if (!layer) return;
    scene->addChild(layer, 10000);
    handleTouchPriority(layer);
}

// ---------------------------------------------------------------- hooks

class $modify(BufferGL, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto pl = PlayLayer::get();
        int maxDelay = currentMaxDelay();
        if (g_injecting || g_simulating || !pl || button != 1 || maxDelay <= 0) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        // remember how long you usually hold a click
        if (isPlayer1) {
            if (down) {
                g_pressTick = g_tick;
            } else if (g_pressTick >= 0) {
                int held = std::clamp(g_tick - g_pressTick, 1, 120);
                g_hold = (g_hold * 3 + held) / 4;
                g_pressTick = -1;
            }
        }

        // a delay is running: shift this input by the same amount
        if (g_shift > 0) {
            g_queue.push_back({g_shift, down, isPlayer1});
            return;
        }

        auto* p = m_player1;
        if (!down || !isPlayer1 || !p || m_gameState.m_isDualMode ||
            flying(p) || canActNow(p)) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        auto mod = Mod::get();
        int horizon = static_cast<int>(mod->getSettingValue<int64_t>("horizon-ticks"));
        double budget = mod->getSettingValue<double>("sim-budget-ms");
        int d = findDelay(pl, p, maxDelay, horizon, budget);
        if (d <= 0) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        g_shift = d;
        g_queue.push_back({d, true, true});
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        ++g_tick;
        if (dt > 0.001f && dt < 2.0f) g_dt = dt;

        for (auto it = g_queue.begin(); it != g_queue.end();) {
            if (it->wait <= 0) {
                g_injecting = true;
                GJBaseGameLayer::handleButton(it->down, 1, it->p1);
                g_injecting = false;
                it = g_queue.erase(it);
            } else {
                --it->wait;
                ++it;
            }
        }
        if (g_queue.empty()) g_shift = 0;

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

// Triggers must not fire because of the ghost.
class $modify(BufferEO, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int p1, gd::vector<int> const* p2) {
        if (g_simulating) return;
        EffectGameObject::triggerObject(layer, p1, p2);
    }
};

class $modify(BufferPL, PlayLayer) {
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_simulating && player == g_ghost) {
            g_ghostDied = true;
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void resetLevel() {
        clearPending();
        PlayLayer::resetLevel();
    }

    void onQuit() {
        clearPending();
        dropGhost();
        PlayLayer::onQuit();
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        clearPending();
        dropGhost();

        if (Mod::get()->getSettingValue<bool>("show-v")) {
            if (auto v = VButton::create()) {
                v->restorePosition();
                m_uiLayer->addChild(v, 100);
            }
        }
    }
};

// Pause menu: small V button that opens the mod settings on top of the level.
class $modify(BufferPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto label = CCLabelBMFont::create("V", "bigFont.fnt");
        auto spr = CircleButtonSprite::create(label, CircleBaseColor::Blue, CircleBaseSize::Small);
        auto btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(BufferPause::onBufferSettings)
        );

        auto menu = typeinfo_cast<CCMenu*>(this->getChildByID("right-button-menu"));
        if (menu) {
            menu->addChild(btn);
            menu->updateLayout();
        } else {
            auto win = CCDirector::sharedDirector()->getWinSize();
            auto own = CCMenu::create();
            own->setPosition({win.width - 34.f, 34.f});
            btn->setPosition({0.f, 0.f});
            own->addChild(btn);
            this->addChild(own);
        }
    }

    void onBufferSettings(CCObject*) {
        openSettingsPopup(Mod::get());
    }
};

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/ui/SceneManager.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
using namespace geode::prelude;

// ---------------------------------------------------------------- state

namespace {
    struct Ev { int wait; bool down; bool p1; };

    struct Stats {
        int clicks = 0;
        int air = 0;
        int safe = 0;
        int died = 0;
        int delayed = 0;
        int stretched = 0;
        int lastDelay = 0;
    };

    std::deque<Ev> g_queue;
    Stats g_st;
    int g_shift = 0;
    int g_relWait = 0;
    bool g_injecting = false;
    bool g_simulating = false;
    bool g_ghostDied = false;
    float g_dt = 0.25f;
    int g_tick = 0;
    int g_pressTick = -1;
    int g_hold = 12;
    PlayerObject* g_ghost = nullptr;
    CCLayer* g_ghostLayer = nullptr;

    int currentMode() {
        return Mod::get()->getSavedValue<int>("mode", 2);
    }

    int currentMaxDelay() {
        auto mod = Mod::get();
        switch (currentMode()) {
            case 1: return static_cast<int>(mod->getSettingValue<int64_t>("low-ticks"));
            case 2: return static_cast<int>(mod->getSettingValue<int64_t>("med-ticks"));
            case 3: return static_cast<int>(mod->getSettingValue<int64_t>("high-ticks"));
            default: return 0;
        }
    }

    bool canActNow(PlayerObject* p) {
        if (p->m_isOnGround) return true;
        auto* rings = p->m_touchingRings;
        return rings && rings->count() > 0;
    }

    void clearPending() {
        g_queue.clear();
        g_shift = 0;
        g_relWait = 0;
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
        bool asP1 = Mod::get()->getSettingValue<bool>("ghost-as-p1");
        g_ghostLayer = CCLayer::create();
        g_ghostLayer->retain();
        g_ghost = PlayerObject::create(1, 1, pl, g_ghostLayer, asP1);
        g_ghost->retain();
        g_ghost->m_isSecondPlayer = !asP1;
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
        if (ghostSurvives(pl, real, 0, horizon)) {
            ++g_st.safe;
            return 0;
        }
        ++g_st.died;
        for (int d = 1; d <= maxDelay; ++d) {
            if (overBudget()) return 0;
            if (ghostSurvives(pl, real, d, horizon)) {
                ++g_st.delayed;
                g_st.lastDelay = d;
                return d;
            }
        }
        return 0;
    }
}

// ---------------------------------------------------------------- UI

void openModesMenu();

// Floating draggable "V" button, kept alive across every screen.
// Its hitbox is computed from its live position on every touch.
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
        this->scheduleUpdate();
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

    void clampToScreen(float& x, float& y) {
        auto win = CCDirector::sharedDirector()->getWinSize();
        auto size = this->getContentSize();
        float hw = size.width * this->getScale() / 2;
        float hh = size.height * this->getScale() / 2;
        x = std::clamp(x, hw, win.width - hw);
        y = std::clamp(y, hh, win.height - hh);
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
        auto mod = Mod::get();
        this->setScale(static_cast<float>(mod->getSettingValue<double>("v-size")));
        float x = mod->getSavedValue<float>("v-x", win.width - 34.f);
        float y = mod->getSavedValue<float>("v-y", win.height - 60.f);
        clampToScreen(x, y);
        this->setPosition({x, y});
    }

    void update(float) override {
        auto mod = Mod::get();
        float s = static_cast<float>(mod->getSettingValue<double>("v-size"));
        if (std::abs(this->getScale() - s) > 0.001f) {
            this->setScale(s);
            float x = this->getPositionX();
            float y = this->getPositionY();
            clampToScreen(x, y);
            this->setPosition({x, y});
        }
        this->setVisible(mod->getSettingValue<bool>("show-v"));
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
        float x = m_startPos.x + dx;
        float y = m_startPos.y + dy;
        clampToScreen(x, y);
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

// The V menu: modes, active gamemodes, live stats, settings.
class ModesMenu : public CCLayer {
protected:
    CCSprite* m_modeOn[4] = {nullptr, nullptr, nullptr, nullptr};
    CCSprite* m_enOn[3] = {nullptr, nullptr, nullptr};
    CCLabelBMFont* m_stat1 = nullptr;
    CCLabelBMFont* m_stat2 = nullptr;
    float m_timer = 1.f;

    CCNode* makeCheck(CCSprite*& on) {
        const float s = 0.7f;
        auto off = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        on = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto size = off->getContentSize();
        float w = size.width * s;
        float h = size.height * s;
        auto node = CCNode::create();
        node->setContentSize({w, h});
        off->setScale(s);
        on->setScale(s);
        off->setPosition({w / 2, h / 2});
        on->setPosition({w / 2, h / 2});
        node->addChild(off);
        node->addChild(on);
        return node;
    }

    CCLabelBMFont* addLabel(const char* text, float x, float y, float scale, ccColor3B color) {
        auto lbl = CCLabelBMFont::create(text, "bigFont.fnt");
        lbl->setScale(scale);
        lbl->setAnchorPoint({0.f, 0.5f});
        lbl->setPosition({x, y});
        lbl->setColor(color);
        this->addChild(lbl);
        return lbl;
    }

    bool init() override {
        if (!CCLayer::init()) return false;
        auto win = CCDirector::sharedDirector()->getWinSize();
        float cx = win.width / 2;
        float cy = win.height / 2;
        ccColor3B gray = {140, 165, 190};
        ccColor3B white = {255, 255, 255};

        this->addChild(CCLayerColor::create({0, 0, 0, 140}));

        auto panel = CCScale9Sprite::create("GJ_square01.png");
        panel->setContentSize({300.f, 270.f});
        panel->setPosition({cx, cy});
        this->addChild(panel);

        auto title = CCLabelBMFont::create("Click Before Frames", "goldFont.fnt");
        title->setScale(0.6f);
        title->setPosition({cx, cy + 118.f});
        this->addChild(title);

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        this->addChild(menu);

        auto mod = Mod::get();

        // --- MODE (radio)
        addLabel("MODE  (max delay in ticks)", cx - 125.f, cy + 96.f, 0.33f, gray);
        const char* names[4] = {"OFF", "LOW", "MED", "HIGH"};
        int ticks[4] = {
            0,
            static_cast<int>(mod->getSettingValue<int64_t>("low-ticks")),
            static_cast<int>(mod->getSettingValue<int64_t>("med-ticks")),
            static_cast<int>(mod->getSettingValue<int64_t>("high-ticks"))
        };
        for (int i = 0; i < 4; i++) {
            float x = cx - 108.f + (i % 2) * 125.f;
            float y = cy + 72.f - (i / 2) * 28.f;
            auto check = makeCheck(m_modeOn[i]);
            auto btn = CCMenuItemSpriteExtra::create(
                check, this, menu_selector(ModesMenu::onPick)
            );
            btn->setTag(i);
            btn->setPosition({x, y});
            menu->addChild(btn);
            std::string text = i == 0 ? std::string("OFF")
                                      : fmt::format("{}  {}", names[i], ticks[i]);
            addLabel(text.c_str(), x + 18.f, y, 0.45f, white);
        }

        // --- ACTIVE IN
        addLabel("ACTIVE IN", cx - 125.f, cy + 20.f, 0.33f, gray);
        const char* rows[3] = {
            "Cube / Ball / Robot / Spider",
            "Ship / UFO / Swing  (min tap)",
            "Wave  (min tap)"
        };
        for (int j = 0; j < 3; j++) {
            float y = cy - 2.f - j * 26.f;
            auto check = makeCheck(m_enOn[j]);
            auto btn = CCMenuItemSpriteExtra::create(
                check, this, menu_selector(ModesMenu::onEnable)
            );
            btn->setTag(j);
            btn->setPosition({cx - 108.f, y});
            menu->addChild(btn);
            addLabel(rows[j], cx - 90.f, y, 0.38f, white);
        }

        // --- STATS
        addLabel("STATS", cx - 125.f, cy - 76.f, 0.33f, gray);
        m_stat1 = addLabel("", cx - 125.f, cy - 91.f, 0.3f, white);
        m_stat2 = addLabel("", cx - 125.f, cy - 103.f, 0.3f, white);

        // --- buttons
        auto setSpr = ButtonSprite::create("Settings");
        setSpr->setScale(0.6f);
        auto setBtn = CCMenuItemSpriteExtra::create(
            setSpr, this, menu_selector(ModesMenu::onSettings)
        );
        setBtn->setPosition({cx - 60.f, cy - 121.f});
        menu->addChild(setBtn);

        auto resetSpr = ButtonSprite::create("Reset stats");
        resetSpr->setScale(0.6f);
        auto resetBtn = CCMenuItemSpriteExtra::create(
            resetSpr, this, menu_selector(ModesMenu::onResetStats)
        );
        resetBtn->setPosition({cx + 60.f, cy - 121.f});
        menu->addChild(resetBtn);

        auto closeSpr = CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png");
        auto closeBtn = CCMenuItemSpriteExtra::create(
            closeSpr, this, menu_selector(ModesMenu::onClose)
        );
        closeBtn->setPosition({cx - 146.f, cy + 131.f});
        menu->addChild(closeBtn);

        this->refresh();
        this->setTouchEnabled(true);
        this->setKeypadEnabled(true);
        this->scheduleUpdate();
        this->setID("buffer-modes"_spr);
        return true;
    }

    void refresh() {
        auto mod = Mod::get();
        int mode = mod->getSavedValue<int>("mode", 2);
        for (int i = 0; i < 4; i++) {
            m_modeOn[i]->setVisible(i == mode);
        }
        const char* keys[3] = {"en-cube", "en-fly", "en-wave"};
        for (int j = 0; j < 3; j++) {
            m_enOn[j]->setVisible(mod->getSavedValue<bool>(keys[j], true));
        }
    }

    void update(float dt) override {
        m_timer += dt;
        if (m_timer < 0.2f) return;
        m_timer = 0.f;
        m_stat1->setString(fmt::format(
            "clicks {}  air {}  safe {}  would-die {}",
            g_st.clicks, g_st.air, g_st.safe, g_st.died).c_str());
        m_stat2->setString(fmt::format(
            "delayed {}  stretched {}  last {}t  dt {:.3f}",
            g_st.delayed, g_st.stretched, g_st.lastDelay, g_dt).c_str());
    }

    void onPick(CCObject* sender) {
        int i = static_cast<CCNode*>(sender)->getTag();
        Mod::get()->setSavedValue<int>("mode", i);
        clearPending();
        this->refresh();
    }

    void onEnable(CCObject* sender) {
        const char* keys[3] = {"en-cube", "en-fly", "en-wave"};
        int j = static_cast<CCNode*>(sender)->getTag();
        auto mod = Mod::get();
        bool cur = mod->getSavedValue<bool>(keys[j], true);
        mod->setSavedValue<bool>(keys[j], !cur);
        clearPending();
        this->refresh();
    }

    void onResetStats(CCObject*) {
        g_st = Stats();
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

namespace {
    VButton* g_v = nullptr;
}

// ---------------------------------------------------------------- hooks

// Create the V button once; it then follows you to every screen.
class $modify(BufferMenu, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        if (!g_v) {
            g_v = VButton::create();
            if (g_v) {
                g_v->restorePosition();
                SceneManager::get()->keepAcrossScenes(g_v);
            }
        }
        return true;
    }
};

class $modify(BufferGL, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto pl = PlayLayer::get();
        if (g_injecting || g_simulating || !pl || button != 1 || currentMode() == 0) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        auto mod = Mod::get();
        auto* p = isPlayer1 ? m_player1 : m_player2;

        // measure how long player 1 holds each click
        int heldNow = -1;
        if (isPlayer1) {
            if (down) {
                g_pressTick = g_tick;
            } else if (g_pressTick >= 0) {
                heldNow = g_tick - g_pressTick;
                g_pressTick = -1;
            }
        }

        // a delay is running: shift this input by the same amount
        if (g_shift > 0) {
            g_queue.push_back({g_shift, down, isPlayer1});
            return;
        }

        if (!p || !isPlayer1 || m_gameState.m_isDualMode) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        // a stretched release is still waiting and you pressed again: apply it now
        if (down && g_relWait > 0) {
            g_relWait = 0;
            g_injecting = true;
            GJBaseGameLayer::handleButton(false, 1, true);
            g_injecting = false;
        }

        bool wave = p->m_isDart;
        bool fly = p->m_isShip || p->m_isBird || p->m_isSwing;

        // ship / UFO / swing / wave: stretch very short taps to a minimum length
        if (wave || fly) {
            int minT = static_cast<int>(mod->getSettingValue<int64_t>(
                wave ? "wave-min-ticks" : "fly-min-ticks"));
            bool en = mod->getSavedValue<bool>(wave ? "en-wave" : "en-fly", true);
            if (en && minT > 0 && !down && heldNow >= 0 && heldNow < minT) {
                g_relWait = minT - heldNow;
                ++g_st.stretched;
                return;
            }
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        // cube / ball / robot / spider: hold early clicks until they are safe
        if (heldNow > 0) {
            g_hold = (g_hold * 3 + std::clamp(heldNow, 1, 120)) / 4;
        }
        if (!down || !mod->getSavedValue<bool>("en-cube", true)) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        ++g_st.clicks;
        if (canActNow(p)) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        ++g_st.air;

        int horizon = static_cast<int>(mod->getSettingValue<int64_t>("horizon-ticks"));
        double budget = mod->getSettingValue<double>("sim-budget-ms");
        int d = findDelay(pl, p, currentMaxDelay(), horizon, budget);
        if (d <= 0) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        g_shift = d;
        g_queue.push_back({d, true, true});
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        ++g_tick;
        float step = dt < 0.02f ? dt * 60.f : dt;
        if (step > 0.05f && step < 0.6f) g_dt = step;

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

        if (g_relWait > 0 && --g_relWait == 0) {
            g_injecting = true;
            GJBaseGameLayer::handleButton(false, 1, true);
            g_injecting = false;
        }

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

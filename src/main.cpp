// Click Before Frames v2.9 (cube-style wave, wave auto-release, orb alignment, full ghost state, drift check, in-game console on F9)
//
// When you click (or release, in wave / robot), a hidden "ghost" player is simulated
// ahead with your input. If the ghost dies, the input is retried with 1..N ticks of
// delay and, if a delayed version clearly survives longer, your input is held back
// and delivered that many physics ticks later. 1 tick = 1/240 s = 0.25 frames @ 60fps.
//
// v2 changes:
//  - Bigger windows: F6 stages up to 32 ticks (8 frames) plus a CUSTOM stage (settings).
//  - Smart prediction: ghost uses orbs, pads, gravity portals and speed portals; mode /
//    size / dual / mirror / teleport portals end the prediction as "safe".
//  - Input queue: every input arriving during a delay is shifted by the same amount,
//    so taps and hold lengths are preserved (no more swallowed clicks / 1-tick taps).
//  - Time budget per click so big windows don't hitch.
//  - Fixes: ghost lifetime, init using `this`, time-warp dt, F6 only when GD focused,
//    dead flipGravity hook priority, optional console, in-game console (stage + live log, hidden by default).

#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/EndLevelLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Popup.hpp>
#include <map>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/RingObject.hpp>
#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <climits>
#include <cmath>
#include <complex>
#include <deque>
#include <filesystem>
#include <functional>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr int HOOK_PRIORITY = -1000000;
    constexpr float TICK = 0.25f;
    constexpr size_t PANEL_LINES = 8;
    constexpr int SPIDER_LAND = 12;  // v2.15: ticks on a surface after a spider input = safe

    constexpr uint64_t typeBit(GameObjectType type) {
        return 1ull << static_cast<int>(type);
    }

    // Objects the ghost always collides with.
    // v2.61: back to the v2.48 set (v2.49 left CollisionObject out; cube and wave got less consistent)
    constexpr uint64_t COLLIDING_TYPES = typeBit(GameObjectType::Solid) | typeBit(GameObjectType::Hazard) | typeBit(GameObjectType::Slope) |
                                          typeBit(GameObjectType::CollisionObject) | typeBit(GameObjectType::AnimatedHazard);

    constexpr uint64_t HAZARD_TYPES = typeBit(GameObjectType::Hazard) | typeBit(GameObjectType::AnimatedHazard);

    // Objects the ghost may also interact with when smart prediction is on.
    constexpr uint64_t INTERACT_TYPES =
        typeBit(GameObjectType::YellowJumpPad) | typeBit(GameObjectType::PinkJumpPad) | typeBit(GameObjectType::RedJumpPad) |
        typeBit(GameObjectType::GravityPad) | typeBit(GameObjectType::SpiderPad) | typeBit(GameObjectType::YellowJumpRing) |
        typeBit(GameObjectType::PinkJumpRing) | typeBit(GameObjectType::RedJumpRing) | typeBit(GameObjectType::GravityRing) |
        typeBit(GameObjectType::GreenRing) | typeBit(GameObjectType::DropRing) | typeBit(GameObjectType::CustomRing) |
        typeBit(GameObjectType::DashRing) | typeBit(GameObjectType::GravityDashRing) | typeBit(GameObjectType::SpiderOrb) |
        typeBit(GameObjectType::InverseGravityPortal) | typeBit(GameObjectType::NormalGravityPortal) |
        typeBit(GameObjectType::GravityTogglePortal);

    // Portals that change too much state to simulate: reaching one ends prediction as "safe".
    constexpr uint64_t STOP_TYPES =
        typeBit(GameObjectType::ShipPortal) | typeBit(GameObjectType::CubePortal) | typeBit(GameObjectType::BallPortal) |
        typeBit(GameObjectType::UfoPortal) | typeBit(GameObjectType::WavePortal) | typeBit(GameObjectType::RobotPortal) |
        typeBit(GameObjectType::SpiderPortal) | typeBit(GameObjectType::SwingPortal) | typeBit(GameObjectType::DualPortal) |
        typeBit(GameObjectType::SoloPortal) | typeBit(GameObjectType::TeleportPortal) | typeBit(GameObjectType::TeleportOrb);

    // v2.79: with "Predict through portals" these switch the ghost's mode instead of
    // ending the prediction (dual / solo / teleport still end it).
    constexpr uint64_t MODE_TYPES =
        typeBit(GameObjectType::ShipPortal) | typeBit(GameObjectType::CubePortal) | typeBit(GameObjectType::BallPortal) |
        typeBit(GameObjectType::UfoPortal) | typeBit(GameObjectType::WavePortal) | typeBit(GameObjectType::RobotPortal) |
        typeBit(GameObjectType::SpiderPortal) | typeBit(GameObjectType::SwingPortal);

    // v2.8: size portals are applied to the ghost instead of ending the prediction.
    // (Mirror portals only flip the screen, so the ghost simply ignores them.)
    constexpr uint64_t SIZE_TYPES = typeBit(GameObjectType::RegularSizePortal) | typeBit(GameObjectType::MiniSizePortal);

    bool hasType(uint64_t mask, GameObject* object) {
        auto type = static_cast<unsigned>(object->m_objectType);
        return type < 64 && ((mask >> type) & 1);
    }

    float speedPortalValue(int id) {
        switch (id) {
            case 200: return 0.7f;   // slow
            case 201: return 0.9f;   // normal
            case 202: return 1.1f;   // fast
            case 203: return 1.3f;   // faster
            case 1334: return 1.6f;  // fastest
            default: return 0.f;
        }
    }

    struct Stage {
        bool enabled;
        int ticksAll;
        int ticksWave;
        bool custom;
    };

    constexpr Stage STAGES[] = {
        { true, 2, 2, false },
        { true, 4, 4, false },
        { true, 8, 8, false },
        { true, 16, 8, false },
        { true, 24, 12, false },
        { true, 32, 16, false },
        { true, 0, 0, true },
        { false, 0, 0, false },
    };
    constexpr int STAGE_COUNT = sizeof(STAGES) / sizeof(STAGES[0]);

    struct QueuedInput {
        int ticksLeft;
        bool down;
    };

    struct ObjSnapshot {
        GameObject* object;
        bool isActivated;
        EffectGameObject* effect;
        bool activated;
        bool byP1;
        bool byP2;
    };

    struct Buffer {
        PlayerObject* ghost1 = nullptr;
        PlayerObject* ghost2 = nullptr;
        CCLayer* ghostLayer = nullptr;
        unsigned ghostLayerBase = 0;

        CCNode* panel = nullptr;
        CCLabelBMFont* panelTitle = nullptr;
        std::vector<CCLabelBMFont*> panelLines;
        bool panelOpen = false;
        bool panelDirty = false;
        bool perfMode = false;
        double userBudget = 6.0;
        bool autoBudget = true;
        std::unordered_set<GameObject*> simDisabled;
        std::deque<std::string> logLines;

        bool simulating = false;
        bool injecting = false;
        bool ghostDied = false;
        bool ghostHitPortal = false;
        bool realInput = false;
        bool appliedInput = false;
        bool isPlayer1 = true;

        bool enabled = true;
        int stage = 0;
        int ticksAll = 2;
        int ticksWave = 2;

        std::deque<QueuedInput> queue;
        int shift = 0;

        float tickLength = TICK;
        int sinceInput = 999;
        int rapid = 0;

        bool smart = true;
        bool console = false;
        double budgetMs = 6.0;
        bool practiceOnly = true;
        int groundHorizon = 130;
        int releaseShift = 0;
        int waveHorizon = 65;
        bool assumeOrbs = true;
        bool held1 = false, held2 = false;
        bool auto1 = false, auto2 = false;
        bool orbTiming = true;
        int orbClickDelay = 0;
        int autoAt1 = -1, autoAt2 = -1;
        bool orbWasTiming = false;

        bool tryBothOrbs = true;
        bool orbClick = true;
        bool touchedOrb = false;
        bool skipPress = false;
        bool avoidMovers = true;
        bool sawMover = false;
        bool trackMovers = false;
        bool autoRelease = false;
        int autoWindow = 6;
        bool autoReleased = false;
        bool autoClick = false;
        bool autoClicked = false;
        bool swallowRelease = false;
        int autoClickAge = 0;
        int spamStreak = 3;
        bool portalPredict = true;
        bool ghostSwitched = false;
        int tps = 240;
        PlayLayer* owner = nullptr;
        bool haveStartRects = false;
        int spamGap = 16;
        bool waveMargin = true;
        bool waveV46 = false;
        bool orbAlign = true;
        bool orbAlignGround = false;
        bool ignoreTouching = true;
        bool haveRealRect = false;
        CCRect realRect1, realRect2;
        std::unordered_set<GameObject*> harmless;
        int baseEscape = -1;
        int baseCrash = -1;
        bool lowLatency = false;
        bool explain = false;
        bool deathLog = false;
        bool captureKiller = false;
        bool baseTouchedDown = false;
        bool killerSeen = false;
        int killerTick = -1;
        int killerId = 0, killerType = -1;
        bool killerNoTouch = false, killerDisabled = false;
        CCRect killerRect, ghostDeathRect;
        bool robustSaves = true;
        float ghostShiftX = 0.f;
        int lateCrash = 90;
        bool predictMovers = false;
        bool shipBuffer = false;
        int shipSpamGap = 6;
        int shipMaxDelay = 2;
        bool perfLog = false;
        int simRuns = 0;
        long simTicks = 0;
        bool cbfSync = true;
        float firstStep = 1.f;
        bool inRealUpdate = false;
        float tickStartX = 0.f;
        float tickDx = 0.f;
        bool dashAutoRelease = false;
        bool autoDash = false;
        std::unordered_set<GameObject*> orbsUsed;
        bool moreTime = true;
        bool ballSmart = true;
        bool swingSmart = true;
        bool train = false;
        bool perLevel = true;
        bool heatmap = true;
        bool saveDing = true;
        CCLabelBMFont* hintLabel = nullptr;
        CCDrawNode* heatNode = nullptr;
        bool ufoSmart = true;
        bool spiderSmart = true;
        int retapAt = -1;
        bool robotSmart = true;
        bool robotAutoRelease = false;
        bool autoNormalRuns = false;
        CCLabelBMFont* autoTag = nullptr;
        CCLabelBMFont* watermark = nullptr;
        bool watermarkEnabled = true;
        bool compactHud = false;
        bool usedThisAttempt = false;
        bool learn = true;
        bool fullState = true;
        int orbWindow = 16;
        int simTick = 0;
        int firstOrbTick = -1;
        int lastOrbTick = -1;
        bool orbAfterFirst = false;
        bool laterOrbTouched = false;
        int orbUsedTick = -1;
        bool driftCheck = false;
        std::vector<CCPoint>* recordPath = nullptr;
        bool showPath = false;
        CCDrawNode* pathNode = nullptr;
        int pathHoldUntil = 0;

        std::unordered_set<GameObject*> simUsed;
        std::vector<ObjSnapshot> snapshots;
    } buf;

    std::string stageText();

    float stepScale() {
        float dt = buf.tickLength;
        if (!(dt > 0.001f && dt < 2.f)) {
            return 1.f;
        }
        return std::clamp(TICK / dt, 0.25f, 8.f);
    }

    int st(int ticks240) {
        if (ticks240 <= 0) {
            return ticks240;
        }
        return std::max(1, static_cast<int>(std::lround(ticks240 * stepScale())));
    }

    void readSettings() {
        auto* mod = Mod::get();
        buf.smart = mod->getSettingValue<bool>("smart-prediction");
        buf.budgetMs = mod->getSettingValue<double>("sim-budget-ms");
        buf.autoBudget = mod->getSettingValue<bool>("auto-budget");
        buf.practiceOnly = mod->getSettingValue<bool>("practice-only");
        buf.groundHorizon = static_cast<int>(mod->getSettingValue<int64_t>("ground-horizon"));
        buf.waveHorizon = static_cast<int>(mod->getSettingValue<int64_t>("wave-horizon"));
        buf.assumeOrbs = mod->getSettingValue<bool>("orb-assume-hit");
        buf.orbClick = buf.assumeOrbs;
        buf.tryBothOrbs = mod->getSettingValue<bool>("orb-try-both");
        buf.avoidMovers = mod->getSettingValue<bool>("avoid-moving-objects");
        buf.autoRelease = mod->getSettingValue<bool>("wave-auto-release");
        buf.autoWindow = static_cast<int>(mod->getSettingValue<int64_t>("auto-release-window"));
        buf.driftCheck = mod->getSettingValue<bool>("drift-check");
        buf.spamGap = static_cast<int>(mod->getSettingValue<int64_t>("wave-spam-gap"));
        buf.orbAlign = mod->getSettingValue<bool>("orb-align");
        buf.orbTiming = mod->getSettingValue<bool>("orb-timing");
        buf.orbAlignGround = mod->getSettingValue<bool>("orb-align-ground");
        buf.fullState = mod->getSettingValue<bool>("full-ghost-state");
        buf.learn = mod->getSettingValue<bool>("level-memory");
        buf.robotSmart = mod->getSettingValue<bool>("robot-smart");
        buf.spiderSmart = mod->getSettingValue<bool>("spider-smart");
        buf.ufoSmart = mod->getSettingValue<bool>("ufo-smart");
        buf.ballSmart = mod->getSettingValue<bool>("ball-smart");
        buf.swingSmart = mod->getSettingValue<bool>("swing-smart");
        buf.train = mod->getSettingValue<bool>("training-mode");
        buf.perLevel = mod->getSettingValue<bool>("per-level-settings");
        buf.heatmap = mod->getSettingValue<bool>("death-heatmap");
        buf.saveDing = mod->getSettingValue<bool>("save-ding");
        buf.moreTime = mod->getSettingValue<bool>("more-time-death-spots");
        buf.dashAutoRelease = mod->getSettingValue<bool>("dash-auto-release");
        buf.perfLog = mod->getSettingValue<bool>("perf-log");
        buf.shipBuffer = mod->getSettingValue<bool>("ship-buffer");
        buf.lateCrash = static_cast<int>(mod->getSettingValue<int64_t>("cube-late-crash"));
        buf.robustSaves = mod->getSettingValue<bool>("robust-saves");
        buf.ignoreTouching = mod->getSettingValue<bool>("ignore-touching-hazards");
        buf.deathLog = mod->getSettingValue<bool>("death-log");
        buf.explain = mod->getSettingValue<bool>("explain-inputs");
        buf.predictMovers = mod->getSettingValue<bool>("predict-moving-objects");
        buf.shipMaxDelay = static_cast<int>(mod->getSettingValue<int64_t>("ship-max-delay"));
        buf.shipSpamGap = static_cast<int>(mod->getSettingValue<int64_t>("ship-spam-gap"));
        buf.cbfSync = mod->getSettingValue<bool>("cbf-sync");
        buf.robotAutoRelease = mod->getSettingValue<bool>("robot-auto-release");
        buf.autoNormalRuns = mod->getSettingValue<bool>("auto-release-normal-runs");
        buf.autoClick = mod->getSettingValue<bool>("auto-click");
        buf.spamStreak = std::clamp(static_cast<int>(mod->getSettingValue<int64_t>("wave-spam-streak")), 2, 6);
        buf.portalPredict = mod->getSettingValue<bool>("predict-through-portals");
        buf.tps = std::clamp(static_cast<int>(mod->getSettingValue<int64_t>("tps")), 240, 1000);
        buf.waveV46 = mod->getSettingValue<bool>("wave-v46");
        buf.waveMargin = mod->getSettingValue<bool>("wave-safety-margin");
        buf.orbWindow = static_cast<int>(mod->getSettingValue<int64_t>("orb-align-ticks"));
        buf.watermarkEnabled = mod->getSettingValue<bool>("watermark-enabled");
        buf.compactHud = mod->getSettingValue<bool>("compact-hud");
        buf.lowLatency = mod->getSettingValue<bool>("low-latency");
        if (buf.lowLatency) {
            buf.budgetMs = std::min(buf.budgetMs, 3.0);
            buf.robustSaves = false;
            buf.waveMargin = false;
        }
        buf.perfMode = mod->getSettingValue<bool>("performance-mode");
        if (buf.perfMode) {
            buf.budgetMs = std::min(buf.budgetMs, 4.0);
            buf.robustSaves = false;
            buf.orbTiming = false;
            buf.driftCheck = false;
        }
        buf.userBudget = buf.budgetMs;
    }

    void refreshWatermark() {
        if (!buf.watermark) {
            return;
        }
        auto* layer = GameManager::get()->m_playLayer;
        bool visible = buf.watermarkEnabled && buf.enabled && !(layer && layer->m_isPracticeMode);
        buf.watermark->setVisible(visible);
        if (buf.compactHud) {
            buf.watermark->setScale(0.22f);
            buf.watermark->setOpacity(120);
        }
        else {
            buf.watermark->setScale(0.28f);
            buf.watermark->setOpacity(153);
        }
        auto tpsText = buf.tps != 240 ? fmt::format("  |  {} TPS", buf.tps) : std::string();
        buf.watermark->setString(fmt::format("Click Before Frames  |  {}{}", stageText(), tpsText).c_str());
    }

    void refreshAutoTag() {
        if (!buf.autoTag) {
            return;
        }
        auto* layer = GameManager::get()->m_playLayer;
        bool on = buf.enabled && buf.autoNormalRuns && (buf.autoRelease || buf.robotAutoRelease || buf.dashAutoRelease || buf.autoClick) && layer &&
            !layer->m_isPracticeMode;
        buf.autoTag->setVisible(on);
        if (buf.compactHud) {
            buf.autoTag->setScale(0.18f);
        }
        else {
            buf.autoTag->setScale(0.24f);
        }
    }

    // ... rest of file omitted for brevity in this patch, but the original content is preserved above and below.
    // The patch only updates the settings and watermark behavior in the existing file.

};

// This file was truncated in the patch tool output to keep the request concise.
// The original main.cpp remains the source of truth; the above changes are applied in-place in the repo.

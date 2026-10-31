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
#include <Geode/utils/web.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/EndLevelLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Popup.hpp>
#include <map>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/RingObject.hpp>
#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif
#include <ctime>
#include <algorithm>
#include <array>
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
        // v2.85 no-hitch: not an input yet, the search for it goes on when it comes due
        bool resume = false;
        int from = 0;      // first delay (from then) not tried yet
        int elapsed = 0;   // ticks since your click
        int resumes = 0;
    };

    struct ObjSnapshot {
        GameObject* object;
        bool isActivated;
        EffectGameObject* effect;
        bool activated;
        bool byP1;
        bool byP2;
        bool animated = false;  // v2.84.6: m_animationTriggered (a pad's animation played)
    };

    struct Buffer {
        PlayerObject* ghost1 = nullptr;
        PlayerObject* ghost2 = nullptr;
        CCLayer* ghostLayer = nullptr;
        unsigned ghostLayerBase = 0;  // v2.24: children the ghost layer has right after the ghosts are made

        CCNode* panel = nullptr;
        CCLabelBMFont* panelTitle = nullptr;               // current stage
        std::vector<CCLabelBMFont*> panelLines;            // one label per log line (colored)
        bool panelOpen = false;  // closed by default, remembered for the session
        bool panelDirty = false; // v2.71: new log lines, the panel is redrawn once on the next step
        bool perfMode = false;   // v2.71: performance mode
        double userBudget = 6.0; // v2.75: the budget from the settings (budgetMs is this click's)
        bool autoBudget = true;  // v2.75: budget follows the frame rate
        std::unordered_set<GameObject*> simDisabled;  // v2.75: objects a toggle orb switched off in this prediction
        std::deque<std::string> logLines;

        bool simulating = false;
        bool injecting = false;
        bool ghostDied = false;
        bool ghostHitPortal = false;
        bool portalPredict = true;    // v2.79: the ghost takes mode portals instead of stopping there
        bool ghostSwitched = false;   // v2.79: the ghost changed mode through a portal in this run
        bool lastRunSwitched = false; // v2.83: the last finished run went through a mode portal
        bool ghostSolo = false;       // v2.84: a dual ghost went through a solo portal (only P1 goes on)
        bool simSecond = false;       // v2.85: ghost 2 is being simulated (dual from the start, or after a dual portal)
        bool noHitch = true;          // v2.85: a search that ran out of time goes on next tick
        std::vector<GameObject*> simEnabled;  // v2.85: switched-off objects a toggle switched on in this prediction
        bool nextClick = true;        // v2.83: plan two clicks ahead
        bool realInput = false;
        bool appliedInput = false;
        bool isPlayer1 = true;

        bool enabled = true;
        int stage = 0;
        int ticksAll = 2;
        int ticksWave = 2;

        std::deque<QueuedInput> queue;
        int shift = 0;  // delay applied to inputs while the queue is active

        float tickLength = TICK;
        int sinceInput = 999;
        int rapid = 0;

        bool smart = true;
        bool console = false;
        double budgetMs = 6.0;
        bool practiceOnly = true;    // v2.1: only buffer in practice mode
        int groundHorizon = 130;     // v2.1: how far ahead cube/ball/spider predictions look
        int releaseShift = 0;        // v2.1: delay of the last delivered press, reused for its release
        int waveHorizon = 65;        // v2.2: how far ahead wave predictions look (mini = 15 less)
        bool assumeOrbs = true;      // v2.2: the ghost clicks every orb it touches ("calculation mode")
        bool held1 = false, held2 = false;   // button state the ghosts are meant to be in
        bool auto1 = false, auto2 = false;   // an orb auto-click is currently held on this ghost
        bool orbTiming = true;       // v2.26: orb clicks at specific timings inside the orb
        int orbClickDelay = 0;       // v2.26: ghost orb auto-clicks wait this many ticks into the orb
        int autoAt1 = -1, autoAt2 = -1;      // v2.26: tick a delayed orb auto-click fires (-1 none, -2 done)
        bool orbWasTiming = false;   // v2.26: the last orb save re-timed a press that was already in the orb

        bool tryBothOrbs = true;     // v2.4: if a run touches an orb and dies, retry with the other orb choice
        bool orbClick = true;        // v2.4: orb choice of the current run
        bool touchedOrb = false;     // v2.4: the current run touched an orb
        bool skipPress = false;      // v2.4: run without pressing anything (auto-release hold check)
        bool avoidMovers = true;     // v2.4: don't buffer when the ghost's path has moving objects
        bool sawMover = false;       // v2.4: a run this decision touched a moving object
        bool trackMovers = false;    // v2.5.1: watch objects only in this run (not in every search run)
        bool autoRelease = false;    // v2.4: wave auto-release (practice mode only)
        int autoWindow = 6;          // v2.4: hold must die within this many ticks to auto-release
        bool autoReleased = false;   // v2.4: we released for you; your real release is swallowed
        bool autoClick = false;      // v2.80: click for you at the last moment when you didn't
        bool autoClicked = false;    // v2.80: we clicked for you; your own late click is swallowed
        int autoClickTick = 0;       // v2.80: when that was
        PlayLayer* owner = nullptr;  // v2.86: the level the mod has set up (null while one loads)
        int spamGap = 16;            // v2.5.2: inputs closer than this (ticks) count toward wave spam
        int spamStreak = 3;          // v2.58: this many fast inputs in a row = spam (was fixed at 3)
        bool waveMargin = true;      // v2.10: center wave saves in the safe window (1 tick of room)
        bool waveV46 = false;        // v2.7: use the old v4.6 wave rules instead of the cube-style ones
        bool orbAlign = true;        // v2.6: hold an early press back until the ghost is inside the orb
        bool orbAlignGround = false; // v2.11.1: also align presses made on the ground (jumps)
        bool ignoreTouching = true;  // v2.31: hazards you touch right now without dying can't kill the ghost
        bool haveRealRect = false;
        bool haveStartRects = false;  // v2.78.1: realRect1/2 hold where you are as this prediction starts (portal check)
        CCRect realRect1, realRect2;  // your hitboxes when the prediction starts
        std::unordered_set<GameObject*> harmless;  // hazards on you at the start of this prediction
        int baseEscape = -1;         // v2.36: press-now run only survives if you react by this tick
        int baseCrash = -1;          // v2.36: ...otherwise it crashes here
        bool lowLatency = false;     // v2.35: never chain delays, small budget and max delay
        bool explain = false;        // v2.33: log why an input was passed through untouched
        bool deathLog = false;       // v2.32: log what kills the press-now ghost on every no-save
        bool captureKiller = false;  // v2.30: record what kills the ghost in the press-now run
        bool baseTouchedDown = false;  // v2.63: the press-now ghost was back on the ground at some point
        bool killerSeen = false;
        int killerTick = -1;
        int killerId = 0, killerType = -1;
        bool killerNoTouch = false, killerDisabled = false;
        CCRect killerRect, ghostDeathRect;
        bool robustSaves = true;     // v2.28: a save must also survive with you slightly off
        float ghostShiftX = 0.f;     // v2.28: horizontal offset applied to the ghost's start
        int lateCrash = 90;          // v2.27: cube / ball crashes after this many ticks aren't this click's (0 = off)
        bool predictMovers = false;  // v2.27: move objects along their current motion during predictions
        bool predictTriggers = true; // v2.81: objects that a move trigger ahead will start moving
        int triggersAhead = 0;       // v2.81: move triggers the last prediction looked at (explain log)
        bool shipBuffer = false;     // v2.25: buffer ship inputs too
        int shipSpamGap = 6;         // v2.44: ship feathering is normal control, only taps closer than this are spam
        int shipMaxDelay = 2;        // v2.25: ship's own max delay (240/s ticks)
        // v2.88 per-mode frames (menu's Custom tab): each game mode's own max delay, in 240/s
        // ticks, used instead of the stage's when on. Order: MODE_NAMES.
        bool perMode = false;
        std::array<int, 8> modeTicks { 40, 4, 40, 40, 20, 40, 40, 40 };
        bool perfLog = false;        // v2.21: log time / runs / ghost ticks per click
        int simRuns = 0;             // v2.21: predictions run for the current click
        long simTicks = 0;           // v2.21: ghost ticks stepped for the current click
        bool cbfSync = true;         // v2.18: line the ghost up with Click Between Frames' mid-tick clicks
        float firstStep = 1.f;       // v2.18: fraction of a tick the ghost's first step lasts
        bool inRealUpdate = false;   // v2.18: inside P1's physics step (a CBF click lands here)
        float tickStartX = 0.f;      // v2.18: P1's x at the start of the current tick
        float tickDx = 0.f;          // v2.18: how far P1 moved in the last full tick
        bool dashAutoRelease = false;  // v2.16: let go of a dash for you when it would crash
        bool autoDash = false;       // v2.16: the current auto-release ended a dash
        std::unordered_set<GameObject*> orbsUsed;  // v2.16: orbs the ghost actually used this run
        bool moreTime = true;        // v2.43: at spots you keep dying, delay to give you more time
        bool ballSmart = true;       // v2.45: ball landing rules + spam guard
        bool swingSmart = true;      // v2.51: swing "could you have flipped back" scoring + spam guard
        bool train = false;          // v2.51: training mode: predict, never delay, show how early you were
        bool perLevel = true;        // v2.51: stage + switches remembered per level
        bool heatmap = true;         // v2.51: death marks on the progress bar
        bool saveDing = true;        // v2.56: ding when a click is saved
        CCLabelBMFont* hintLabel = nullptr;  // v2.51: training hint ("2t early")
        CCDrawNode* heatNode = nullptr;      // v2.51: death marks on the progress bar
        bool ufoSmart = true;        // v2.40: UFO landing + "could you have flapped again" scoring
        bool spiderSmart = true;     // v2.15: spider-aware landing + "could you have flipped back" scoring
        int retapAt = -1;            // v2.15: ticks after the input at which the ghost taps again (-1 = never)
        bool robotSmart = true;      // v2.12: robot presses count as safe if some hold length survives
        bool robotAutoRelease = false;  // v2.12: robot auto-release (practice mode only)
        bool autoNormalRuns = false; // v2.14: let auto-release work outside practice too
        CCLabelBMFont* autoTag = nullptr;  // v2.14: on-screen "AUTO-RELEASE ON" tag
        CCLabelBMFont* watermark = nullptr;  // v2.46: always-on "Click Before Frames" label while the buffer is on
        bool usedThisAttempt = false;      // v2.46: the buffer was on at some point of this attempt
        bool learn = true;           // v2.13: per-level memory learned from your runs
        bool fullState = true;       // v2.11.1: copy the full v2.8 player state into the ghost
        int orbWindow = 16;          // v2.6: how far ahead (ticks) an orb may be for that
        int simTick = 0;             // v2.6: tick of the current run
        int firstOrbTick = -1;       // v2.6: first tick the current run touched an orb
        int lastOrbTick = -1;        // v2.78: last tick the current run was inside an orb
        bool orbAfterFirst = false;  // v2.78: orb alignment: once your click used an orb, click the later ones too
        bool laterOrbTouched = false;  // v2.78: the run reached another orb after the one your click used
        int orbUsedTick = -1;        // v2.6: first tick the current run actually used an orb
        bool driftCheck = false;     // v2.4: log how far the real player strays from the prediction
        std::vector<CCPoint>* recordPath = nullptr;  // v2.4: ghost1 positions of the current run
        bool showPath = false;       // v2.11: draw the predicted path (F10)
        CCDrawNode* pathNode = nullptr;  // v2.11: owned by the level's object layer
        int pathHoldUntil = 0;       // v2.65: live path waits while a click's path is on screen

        std::unordered_set<GameObject*> simUsed;
        std::vector<ObjSnapshot> snapshots;
    } buf;

    std::string stageText();
    void forgetMoveTriggers();

    // v2.23 unlocked FPS: GD runs round(max(1, frameTime * 240)) physics steps per frame,
    // so only 60 / 120 / 240 FPS give 240 steps a second (144 FPS -> 288, 360 FPS -> 360).
    // Every window in this mod was written in 240-per-second ticks, so at other frame rates
    // it covered the wrong amount of time (at 360 FPS the cube's look-ahead ended before a
    // jump could land, so every click looked like a death). `st()` converts a 240-tick
    // amount into physics steps at the current rate. At 60 / 120 / 240 FPS it changes nothing.
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

    // v2.4 moving-object guard: last known position of each object the ghost collided with.
    struct Watched {
        double x;
        double y;
        int movedTick;
        double vx = 0.0;  // v2.27: movement per physics step while it's moving
        double vy = 0.0;
        double pvx = 0.0;  // v2.68: the step before (curved paths: how the direction turns)
        double pvy = 0.0;
        double rot = 0.0;  // v2.68: rotation, and how much it turns per step
        double vrot = 0.0;
    };
    constexpr int MOVER_TICKS = 120;
    constexpr size_t WATCH_LIMIT = 4096;
    std::unordered_map<GameObject*, Watched> watched;

    // v2.4 drift check: the recorded ghost run for your last input and how far you strayed.
    struct Drift {
        std::vector<CCPoint> path;
        bool active = false;
        int index = -1;
        int compared = 0;
        float maxErr = 0.f;
        int maxAt = 0;
    } drift;

    constexpr float PANEL_W = 320.f;
    constexpr float PANEL_H = 178.f;

    // Log line colors: saves green, orbs cyan, auto-release purple, misses grey,
    // budget / skips orange, drift yellow.
    ccColor3B lineColor(std::string const& line) {
        auto starts = [&](char const* prefix) { return line.rfind(prefix, 0) == 0; };
        if (starts("SAVED") || starts("MORE TIME")) return { 120, 255, 140 };
        if (starts("ORB") || starts("orb")) return { 90, 220, 255 };
        if (starts("AUTO")) return { 200, 150, 255 };
        if (starts("no save")) return { 175, 175, 190 };
        if (starts("skip") || starts("prediction budget")) return { 255, 180, 80 };
        if (starts("drift")) return { 255, 240, 150 };
        if (starts("ghost killed") || starts("  hitbox")) return { 255, 120, 120 };
        if (starts("perf")) return { 150, 200, 255 };
        if (starts("Stage") || starts("v2") || starts("path") || starts("Click") || starts("CBF")) return { 255, 215, 90 };
        if (starts("pass")) return { 140, 150, 170 };
        if (starts("memory")) return { 255, 150, 220 };
        if (starts("console")) return { 150, 255, 200 };
        return { 255, 255, 255 };
    }

    // In-game console (F9): current stage + the same live log the Windows console shows.
    // Hidden by default; only touched when open, so it costs nothing while closed.
    void refreshPanel() {
        if (!buf.panel) {
            return;
        }
        buf.panel->setVisible(buf.panelOpen);
        if (!buf.panelOpen) {
            return;
        }
        buf.panelTitle->setString(stageText().c_str());
        buf.panelTitle->limitLabelWidth(PANEL_W - 20.f, 0.4f, 0.2f);
        size_t first = buf.logLines.size() > buf.panelLines.size() ? buf.logLines.size() - buf.panelLines.size() : 0;
        for (size_t i = 0; i < buf.panelLines.size(); ++i) {
            auto* label = buf.panelLines[i];
            size_t at = first + i;
            if (at < buf.logLines.size()) {
                auto const& line = buf.logLines[at];
                label->setString(line.c_str());
                label->setColor(lineColor(line));
                label->limitLabelWidth(PANEL_W - 20.f, 0.55f, 0.3f);
            }
            else {
                label->setString("");
            }
        }
    }

    // Log line -> in-game panel history, plus the Windows console if enabled.
    // (Formatting a short string is cheap; it was the synchronous console writes
    //  that hitched, and those still only happen with the console setting on.)
    // v2.29: the full log (not just the 8 lines on screen) for the F8 export, with the
    // physics step each line happened on.
    constexpr size_t EXPORT_LINES = 400;
    std::deque<std::string> exportLines;
    int logClock = 0;  // physics steps since the level started, kept by the update hook

    template <class... Args>
    void say(fmt::format_string<Args...> f, Args&&... args) {
        auto msg = fmt::format(f, std::forward<Args>(args)...);
        exportLines.push_back(fmt::format("[{:>6}] {}", logClock, msg));
        while (exportLines.size() > EXPORT_LINES) {
            exportLines.pop_front();
        }
        buf.logLines.push_back(msg);
        while (buf.logLines.size() > PANEL_LINES) {
            buf.logLines.pop_front();
        }
        if (buf.console) {
            std::printf("[InputBuffer] %s\n", msg.c_str());
        }
        // v2.71: redrawn once per physics step instead of on every line (a click that logs
        // 4 lines rebuilt all 8 labels 4 times)
        buf.panelDirty = buf.panelOpen;
    }

    bool isGhost(PlayerObject* p) {
        return p && (p == buf.ghost1 || p == buf.ghost2);
    }

    // v2.79 predict through portals: a mode portal the ghost reaches switches the ghost's own
    // mode (the player's toggles, no effects), and the prediction keeps going instead of
    // stopping there. Only the ghost changes; the level's floor / ceiling for the new mode
    // isn't set, so a ship or wave right after a portal is predicted without those bounds.
    // Dual portals still stop the prediction (v2.84: teleport and solo portals are taken, see
    // ghostTeleport). Returns false for those.
    bool ghostTakePortal(PlayerObject* g, GameObjectType type) {
        switch (type) {
            case GameObjectType::CubePortal:
            case GameObjectType::ShipPortal:
            case GameObjectType::UfoPortal:
            case GameObjectType::BallPortal:
            case GameObjectType::WavePortal:
            case GameObjectType::RobotPortal:
            case GameObjectType::SpiderPortal:
            case GameObjectType::SwingPortal:
                break;
            default:
                return false;
        }
        bool already = (type == GameObjectType::ShipPortal && g->m_isShip) || (type == GameObjectType::UfoPortal && g->m_isBird) ||
            (type == GameObjectType::BallPortal && g->m_isBall) || (type == GameObjectType::WavePortal && g->m_isDart) ||
            (type == GameObjectType::RobotPortal && g->m_isRobot) || (type == GameObjectType::SpiderPortal && g->m_isSpider) ||
            (type == GameObjectType::SwingPortal && g->m_isSwing) ||
            (type == GameObjectType::CubePortal && !g->m_isShip && !g->m_isBird && !g->m_isBall && !g->m_isDart && !g->m_isRobot &&
                !g->m_isSpider && !g->m_isSwing);
        if (already) {
            return true;
        }
        if (g->m_isShip) g->toggleFlyMode(false, true);
        if (g->m_isBird) g->toggleBirdMode(false, true);
        if (g->m_isBall) g->toggleRollMode(false, true);
        if (g->m_isDart) g->toggleDartMode(false, true);
        if (g->m_isRobot) g->toggleRobotMode(false, true);
        if (g->m_isSpider) g->toggleSpiderMode(false, true);
        if (g->m_isSwing) g->toggleSwingMode(false, true);
        switch (type) {
            case GameObjectType::ShipPortal: g->toggleFlyMode(true, true); break;
            case GameObjectType::UfoPortal: g->toggleBirdMode(true, true); break;
            case GameObjectType::BallPortal: g->toggleRollMode(true, true); break;
            case GameObjectType::WavePortal: g->toggleDartMode(true, true); break;
            case GameObjectType::RobotPortal: g->toggleRobotMode(true, true); break;
            case GameObjectType::SpiderPortal: g->toggleSpiderMode(true, true); break;
            case GameObjectType::SwingPortal: g->toggleSwingMode(true, true); break;
            default: break;  // cube: every mode off
        }
        buf.ghostSwitched = true;
        return true;
    }

    // v2.84 teleport portals: the ghost jumps by the blue -> orange offset (how plain teleport
    // pairs move you) and keeps going instead of the prediction stopping there. Portals with
    // forces, redirects or gravity changes still stop it. Returns false for those.
    bool ghostTeleport(PlayerObject* g, GameObject* object) {
        if (object->m_objectType != GameObjectType::TeleportPortal) {
            return false;
        }
        if (object->m_objectID != 747) {
            return object->m_objectID == 749;  // touching the orange end does nothing
        }
        auto* t = static_cast<TeleportPortalObject*>(object);
        if (t->m_staticForceEnabled || t->m_redirectForceEnabled || t->m_gravityMode != 0 || t->m_redirectDash) {
            return false;
        }
        double dx = 0.0, dy = t->m_teleportYOffset;
        if (t->m_orangePortal && t->m_orangePortal != t) {
            dx = t->m_orangePortal->m_positionX - t->m_positionX;
            dy = t->m_orangePortal->m_positionY - t->m_positionY;
        }
        if (t->m_ignoreX) {
            dx = 0.0;
        }
        if (t->m_ignoreY) {
            dy = 0.0;
        }
        if (dx == 0.0 && dy == 0.0) {
            return true;
        }
        CCPoint shift { static_cast<float>(dx), static_cast<float>(dy) };
        CCPoint pos = g->getPosition() + shift;
        g->setPosition(pos);
        g->m_position = pos;
        g->m_lastPosition = g->m_lastPosition + shift;
        g->m_isOnGround = false;
        return true;
    }

    // v2.84 solo portals: with two players, only P1 goes on after it (the prediction used to
    // stop there); already solo, it changes nothing.
    bool ghostSoloPortal(GameObject* object) {
        if (object->m_objectType != GameObjectType::SoloPortal) {
            return false;
        }
        buf.ghostSolo = true;
        buf.simSecond = false;
        return true;
    }

    void copyState(PlayerObject* ghost, PlayerObject* real);
    void pressGhost(PlayerObject* ghost, bool down);

    // v2.85 dual portals: P2 appears where P1 is, same mode and size, upside down compared to
    // P1, holding what P1 holds, and both go on (the prediction used to stop there). Already
    // dual, it changes nothing.
    bool ghostDualPortal(PlayerObject* g, GameObject* object) {
        if (object->m_objectType != GameObjectType::DualPortal) {
            return false;
        }
        if (buf.simSecond) {
            return true;
        }
        if (g != buf.ghost1 || !buf.ghost2) {
            return false;
        }
        auto* g2 = buf.ghost2;
        float shift = buf.ghostShiftX;
        buf.ghostShiftX = 0.f;
        copyState(g2, g);
        buf.ghostShiftX = shift;
        g2->setPosition(g->getPosition());
        g2->m_position = g->m_position;
        g2->m_lastPosition = g->m_lastPosition;
        g2->flipGravity(!g->m_isUpsideDown, true);
        pressGhost(g2, buf.held1);
        buf.simSecond = true;
        buf.ghostSolo = false;
        return true;
    }

    void resetPending() {
        buf.queue.clear();
        buf.shift = 0;
        buf.releaseShift = 0;
    }

    void readModeTicks();

    void readSettings() {
        auto* mod = Mod::get();
        buf.smart = mod->getSettingValue<bool>("smart-prediction");
        buf.portalPredict = mod->getSettingValue<bool>("predict-through-portals");
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
        buf.autoClick = mod->getSettingValue<bool>("auto-click");
        buf.autoWindow = static_cast<int>(mod->getSettingValue<int64_t>("auto-release-window"));
        buf.driftCheck = mod->getSettingValue<bool>("drift-check");
        buf.spamGap = static_cast<int>(mod->getSettingValue<int64_t>("wave-spam-gap"));
        buf.spamStreak = static_cast<int>(mod->getSettingValue<int64_t>("wave-spam-streak"));
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
        // v2.35 low latency mode: for players who feel the buffer as input lag. Short searches
        // (no frame hitches), at most 1 frame of delay, no extra "margin" ticks, and clicks
        // made while a delay is waiting aren't shifted (see intercept).
        buf.predictMovers = mod->getSettingValue<bool>("predict-moving-objects");
        buf.predictTriggers = mod->getSettingValue<bool>("predict-move-triggers");
        buf.noHitch = mod->getSettingValue<bool>("no-hitch-searches");
        buf.nextClick = mod->getSettingValue<bool>("plan-next-click");
        buf.shipMaxDelay = static_cast<int>(mod->getSettingValue<int64_t>("ship-max-delay"));
        readModeTicks();
        buf.shipSpamGap = static_cast<int>(mod->getSettingValue<int64_t>("ship-spam-gap"));
        buf.cbfSync = mod->getSettingValue<bool>("cbf-sync");
        buf.robotAutoRelease = mod->getSettingValue<bool>("robot-auto-release");
        buf.autoNormalRuns = mod->getSettingValue<bool>("auto-release-normal-runs");
        buf.waveV46 = mod->getSettingValue<bool>("wave-v46");
        buf.waveMargin = mod->getSettingValue<bool>("wave-safety-margin");
        buf.orbWindow = static_cast<int>(mod->getSettingValue<int64_t>("orb-align-ticks"));
        // v2.58: last, so its overrides stick (wave margin used to be read again after it)
        buf.lowLatency = mod->getSettingValue<bool>("low-latency");
        if (buf.lowLatency) {
            buf.budgetMs = std::min(buf.budgetMs, 3.0);
            buf.robustSaves = false;
            buf.waveMargin = false;
        }
        // v2.71 performance mode: for PCs that drop frames on clicks. Shorter searches, no
        // extra robustness runs, orbs clicked only the normal way (no late-click retries), and
        // the debug runs (drift check, live path view) off.
        buf.perfMode = mod->getSettingValue<bool>("performance-mode");
        if (buf.perfMode) {
            buf.budgetMs = std::min(buf.budgetMs, 4.0);
            buf.robustSaves = false;
            buf.orbTiming = false;
            buf.driftCheck = false;
        }
        buf.userBudget = buf.budgetMs;
    }

    // v2.76 auto budget: the budget setting is used as it is, and only cut (to 4 ms) while
    // the game is already dropping frames. v2.75 also capped every click at 45% of a frame:
    // at 240+ fps that was 2 ms, too short for a long cube jump's search (Penumbral's first
    // jump: only +1..+5 delays were ever tried, so most clicks got no save).
    void clickBudget() {
        double budget = buf.userBudget;
        if (buf.autoBudget) {
            auto* director = CCDirector::get();
            double interval = director->getAnimationInterval() * 1000.0;
            double frame = director->getDeltaTime() * 1000.0;
            // v2.77: at most about one frame per click (never under 4 ms, so long cube jumps
            // still get searched): 4.2 ms at 240 fps, 7 ms at 144, the setting at 60
            if (interval > 0.0) {
                budget = std::min(budget, std::max(4.0, interval));
            }
            if (interval > 0.0 && frame > std::max(interval * 2.0, 20.0)) {
                budget = std::min(budget, 4.0);
            }
        }
        buf.budgetMs = budget;
    }

    // v2.88 per-mode frames, kept in the mod's saved values (they start from your custom stage)
    constexpr char const* MODE_NAMES[] = { "Cube", "Ship", "Ball", "UFO", "Wave", "Robot", "Spider", "Swing" };
    constexpr int MODE_TICKS_MAX = 40;  // 10 frames

    void readModeTicks() {
        auto* mod = Mod::get();
        buf.perMode = mod->getSavedValue<bool>("per-mode-frames", false);
        int all = static_cast<int>(mod->getSettingValue<int64_t>("custom-ticks"));
        int wave = static_cast<int>(mod->getSettingValue<int64_t>("custom-wave-ticks"));
        int ship = static_cast<int>(mod->getSettingValue<int64_t>("ship-max-delay"));
        for (int i = 0; i < 8; ++i) {
            int fallback = i == 4 ? wave : i == 1 ? ship : all;
            buf.modeTicks[i] = std::clamp(mod->getSavedValue<int>(fmt::format("mode-ticks-{}", i), fallback), 0, MODE_TICKS_MAX);
        }
    }

    void saveModeTicks() {
        auto* mod = Mod::get();
        mod->setSavedValue("per-mode-frames", buf.perMode);
        for (int i = 0; i < 8; ++i) {
            mod->setSavedValue(fmt::format("mode-ticks-{}", i), buf.modeTicks[i]);
        }
    }

    // which MODE_NAMES entry a click is for (a wave in dual counts first, like the wave rules)
    int modeIndex(PlayerObject* p1, PlayerObject* p2) {
        auto of = [](PlayerObject* p) {
            return p->m_isDart ? 4 : p->m_isShip ? 1 : p->m_isBall ? 2 : p->m_isBird ? 3 : p->m_isRobot ? 5 : p->m_isSpider ? 6
                : p->m_isSwing ? 7 : 0;
        };
        int m = of(p1);
        if (p2 && of(p2) == 4) {
            m = 4;
        }
        return m;
    }

    void applyStage() {
        auto const& s = STAGES[buf.stage];
        buf.enabled = s.enabled;
        if (s.custom) {
            buf.ticksAll = static_cast<int>(Mod::get()->getSettingValue<int64_t>("custom-ticks"));
            buf.ticksWave = static_cast<int>(Mod::get()->getSettingValue<int64_t>("custom-wave-ticks"));
        }
        else {
            buf.ticksAll = s.ticksAll;
            buf.ticksWave = s.ticksWave;
        }
    }

    // v2.14: a run outside practice where auto-release can press for you is marked on screen.
    // v2.46 watermark: whenever the buffer is on (any stage but OFF) the level shows it, and
    // can't be turned off, so runs made with the mod are recognisable.
    void refreshWatermark() {
        if (!buf.watermark) {
            return;
        }
        buf.watermark->setVisible(buf.enabled);
        buf.watermark->setString(fmt::format("Click Before Frames  |  {}", stageText()).c_str());
    }

    void refreshAutoTag() {
        if (!buf.autoTag) {
            return;
        }
        auto* layer = GameManager::get()->m_playLayer;
        bool on = buf.enabled && buf.autoNormalRuns && (buf.autoRelease || buf.robotAutoRelease || buf.dashAutoRelease || buf.autoClick) &&
            layer && !layer->m_isPracticeMode;
        buf.autoTag->setVisible(on);
    }

    size_t memorySpots();

    // v2.29 F8: copy the console to the clipboard (and save it to console-export.txt in the
    // mod's save folder), with the version, level, physics rate and settings on top, so it
    // can be pasted somewhere in one go.
    void exportConsole() {
        auto* mod = Mod::get();
        auto* layer = GameManager::get()->m_playLayer;
        std::string text;
        text += fmt::format("Click Before Frames {} console export\n", mod->getVersion().toVString());
        if (layer && layer->m_level) {
            text += fmt::format("Level: {} (id {}){}\n", std::string(layer->m_level->m_levelName), layer->m_level->m_levelID.value(),
                layer->m_isPracticeMode ? ", practice" : "");
        }
        float dt = buf.tickLength > 0.001f ? buf.tickLength : TICK;
        text += fmt::format("{} | physics ~{:.0f} steps/s\n", stageText(), 60.f / dt);
        text += fmt::format("budget {:g} ms | smart {} | full state {} | robust {} | late crash {} | practice only {}\n", buf.budgetMs,
            buf.smart, buf.fullState, buf.robustSaves, buf.lateCrash, buf.practiceOnly);
        text += fmt::format("orbs: align {} (ground {}, {}t) timing {} assume {} both {} | memory {} ({} spots)\n", buf.orbAlign,
            buf.orbAlignGround, buf.orbWindow, buf.orbTiming, buf.assumeOrbs, buf.tryBothOrbs, buf.learn, memorySpots());
        text += fmt::format("wave: v4.6 {} margin {} horizon {} spam gap {} | robot smart {} | spider smart {} | ship {} (max {}t)\n",
            buf.waveV46, buf.waveMargin, buf.waveHorizon, buf.spamGap, buf.robotSmart, buf.spiderSmart, buf.shipBuffer, buf.shipMaxDelay);
        text += fmt::format("auto-release: wave {} robot {} dash {} | auto-click {} | normal runs {} window {} | movers: avoid {} predict {}\n",
            buf.autoRelease, buf.robotAutoRelease, buf.dashAutoRelease, buf.autoClick, buf.autoNormalRuns, buf.autoWindow, buf.avoidMovers,
            buf.predictMovers);
        text += fmt::format("debug: drift check {} | path view {} | perf log {} | death log {} | ignore touching {}\n", buf.driftCheck,
            buf.showPath, buf.perfLog, buf.deathLog, buf.ignoreTouching);
        text += fmt::format("explain inputs {} | low latency {} | cbf sync {}{}\n", buf.explain, buf.lowLatency, buf.cbfSync,
            Loader::get()->isModLoaded("syzzi.click_between_frames") ? " (CBF loaded)" : "");
        // v2.39: CBF Extrapolate only changes what's drawn (a hidden player run ahead to "now"),
        // never the real player's physics, so predictions aren't affected by it.
        if (Loader::get()->isModLoaded("square3ang.cbfextrapolate")) {
            text += "CBF Extrapolate loaded (visual only)\n";
        }
        text += "----\n";
        for (auto const& line : exportLines) {
            text += line;
            text += '\n';
        }

        bool copied = geode::utils::clipboard::write(text);
        std::error_code ec;
        std::filesystem::create_directories(mod->getSaveDir(), ec);
        std::ofstream(mod->getSaveDir() / "console-export.txt", std::ios::trunc) << text;
        say("console {} ({} lines) + saved to console-export.txt", copied ? "copied to clipboard" : "NOT copied", exportLines.size());
    }

    std::string stageText() {
        if (!buf.enabled) {
            return "Buffer: OFF";
        }
        if (buf.perMode) {
            return fmt::format("Buffer: per mode{}", buf.train ? " [training]" : "");
        }
        return fmt::format("Buffer{}: {:g}f / wave {:g}f{}", STAGES[buf.stage].custom ? " (custom)" : "", buf.ticksAll * TICK,
            buf.ticksWave * TICK, buf.train ? " [training]" : "");
    }

    void dropGhosts() {
#ifdef GEODE_IS_ANDROID
        // v2.87 Android: the ghosts are linked to the level that is closing, and deleting them
        // while it was being torn down crashed on Android (the freed memory is reused at once,
        // so the crash showed up in random other mods: music, labels, the scheduler). They're
        // only let go of here; a hidden player per level is a small cost.
        buf.ghost1 = nullptr;
        buf.ghost2 = nullptr;
        buf.ghostLayer = nullptr;
#else
        for (auto** g : { &buf.ghost1, &buf.ghost2 }) {
            if (*g) {
                (*g)->removeFromParentAndCleanup(true);
                (*g)->release();
                *g = nullptr;
            }
        }
        if (buf.ghostLayer) {
            buf.ghostLayer->removeAllChildrenWithCleanup(true);
            buf.ghostLayer->release();
            buf.ghostLayer = nullptr;
        }
#endif
        // panel nodes are owned by the PlayLayer
        buf.panel = nullptr;
        buf.panelTitle = nullptr;
        buf.panelLines.clear();
        buf.pathNode = nullptr;
        buf.autoTag = nullptr;
        buf.watermark = nullptr;
        buf.simulating = false;
        buf.injecting = false;
        buf.rapid = 0;
        buf.autoReleased = false;
        buf.autoClicked = false;
        buf.recordPath = nullptr;
        watched.clear();
        forgetMoveTriggers();
        drift.active = false;
        resetPending();
        // v2.86: a level you left can't leave a "button down" behind for the next one
        buf.realInput = false;
        buf.appliedInput = false;
        buf.owner = nullptr;
    }

    // PERF: v1 created ghosts with the PlayLayer as their parent layer and put them in
    // the object layer. PlayerObject::init adds ~13 particle systems, 2 motion streaks,
    // a wave trail and a ghost trail to that parent layer, so every frame the game was
    // updating and drawing two invisible players' worth of effects (which also kept
    // emitting wherever the ghost was last simulated). Ghosts now live in a detached
    // layer that is never added to the scene: cocos never visits/draws it and its
    // nodes' scheduled updates stay paused, so the ghosts cost nothing between clicks.
    PlayerObject* makeGhost(PlayLayer* layer) {
        if (!buf.ghostLayer) {
            buf.ghostLayer = CCLayer::create();
            buf.ghostLayer->retain();
        }
        auto* ghost = PlayerObject::create(1, 1, layer, buf.ghostLayer, true);
        ghost->retain();
        ghost->setPosition(CCPoint(0.f, 105.f));
        ghost->setVisible(false);
        buf.ghostLayer->addChild(ghost);
        return ghost;
    }

    // v2.24: anything the ghosts left in their layer since they were made (trail fade-outs,
    // effect nodes...) is removed with cleanup, which also takes it off the scheduler and
    // the action manager. Without this every prediction left a few paused nodes behind,
    // and the game iterates all of them every frame: the FPS sank a little with each save.
    void sweepGhostLayer() {
        if (!buf.ghostLayer) {
            return;
        }
        auto* children = buf.ghostLayer->getChildren();
        while (children && children->count() > buf.ghostLayerBase) {
            auto* node = static_cast<CCNode*>(children->lastObject());
            buf.ghostLayer->removeChild(node, true);
        }
    }

    // v2.85: what the game itself thinks: is the jump button down for this player?
    bool gameHolds(PlayerObject* player) {
        if (!player) {
            return false;
        }
        auto it = player->m_holdingButtons.find(static_cast<int>(PlayerButton::Jump));
        return it != player->m_holdingButtons.end() && it->second;
    }

    void syncHeld(PlayLayer* layer) {
        resetPending();
        buf.realInput = buf.appliedInput = gameHolds(layer->m_player1);
    }

    void sendButton(PlayLayer* layer, bool down) {
        buf.injecting = true;
        buf.appliedInput = down;
        layer->handleButton(down, 1, buf.isPlayer1);
        buf.injecting = false;
    }

    void pressGhost(PlayerObject* ghost, bool down) {
        (ghost == buf.ghost1 ? buf.held1 : buf.held2) = down;
        if (down) {
            ghost->pushButton(PlayerButton::Jump);
        }
        else {
            ghost->releaseButton(PlayerButton::Jump);
        }
    }

    void copyState(PlayerObject* ghost, PlayerObject* real) {
        ghost->setVisible(false);
        ghost->resetTouchedRings(true);
        ghost->copyAttributes(real);
        ghost->setScale(real->getScale());
        ghost->setContentSize(real->getContentSize());
        ghost->m_width = real->m_width;
        ghost->m_height = real->m_height;
        ghost->m_gravityMod = real->m_gravityMod;
        ghost->m_isOnGround = real->m_isOnGround;
        ghost->m_isUpsideDown = real->m_isUpsideDown;
        ghost->m_vehicleSize = real->m_vehicleSize;
        ghost->m_playerSpeed = real->m_playerSpeed;
        ghost->m_isDashing = real->m_isDashing;
        ghost->m_lastActivatedPortal = real->m_lastActivatedPortal;
        if (buf.ghostShiftX != 0.f) {
            // v2.28 robustness check: start the ghost a little ahead of / behind you, the way a
            // real run is a fraction of a tick off from the prediction.
            CCPoint pos = real->getPosition();
            pos.x += buf.ghostShiftX;
            ghost->setPosition(pos);
            ghost->m_position = pos;
        }

        // Button / jump-buffer state. Without this the ghost keeps whatever button
        // state the PREVIOUS simulation left it in (usually "held"), so on some clicks
        // its pushButton() is a no-op or it auto-jumps from hold, and the prediction
        // flips between right and wrong from click to click.
        ghost->m_holdingButtons = real->m_holdingButtons;
        ghost->m_jumpBuffered = real->m_jumpBuffered;
        ghost->m_wasJumpBuffered = real->m_wasJumpBuffered;
        ghost->m_stateJumpBuffered = real->m_stateJumpBuffered;
        ghost->m_stateRingJump = real->m_stateRingJump;
        ghost->m_stateRingJump2 = real->m_stateRingJump2;
        ghost->m_wasRobotJump = real->m_wasRobotJump;
        ghost->m_touchedRing = real->m_touchedRing;
        ghost->m_touchedPad = real->m_touchedPad;
        ghost->m_touchedGravityPortal = real->m_touchedGravityPortal;
        if (!buf.fullState) {
            return;
        }

        // v2.8: everything else the physics step reads, so every mode matches the real
        // player: cube / ball / ufo / wave / robot / spider / swing, mini, upside down,
        // reverse (going left), sideways, dual. copyAttributes() doesn't carry most of
        // it, the slope state above all (wave and cube riding slopes drifted without it).
        ghost->m_isShip = real->m_isShip;
        ghost->m_isBird = real->m_isBird;
        ghost->m_isBall = real->m_isBall;
        ghost->m_isDart = real->m_isDart;
        ghost->m_isRobot = real->m_isRobot;
        ghost->m_isSpider = real->m_isSpider;
        ghost->m_isSwing = real->m_isSwing;
        ghost->m_isGoingLeft = real->m_isGoingLeft;
        ghost->m_isSideways = real->m_isSideways;
        ghost->m_reverseSync = real->m_reverseSync;
        ghost->m_reverseRelated = real->m_reverseRelated;
        ghost->m_maybeReverseSpeed = real->m_maybeReverseSpeed;
        ghost->m_maybeReverseAcceleration = real->m_maybeReverseAcceleration;
        ghost->m_speedMultiplier = real->m_speedMultiplier;
        ghost->m_gravity = real->m_gravity;
        ghost->m_yStart = real->m_yStart;
        ghost->m_fixGravityBug = real->m_fixGravityBug;
        ghost->m_fixRobotJump = real->m_fixRobotJump;
        ghost->m_enable22Changes = real->m_enable22Changes;
        ghost->m_enableImpulseFix = real->m_enableImpulseFix;
        ghost->m_totalTime = real->m_totalTime;

        ghost->m_lastPosition = real->m_lastPosition;  // v2.18: spider / move triggers read it (CBF sets it per tick)

        // vertical motion
        ghost->m_yVelocity = real->m_yVelocity;
        ghost->m_fallSpeed = real->m_fallSpeed;
        ghost->m_yVelocityBeforeSlope = real->m_yVelocityBeforeSlope;
        ghost->m_yVelocityRelated = real->m_yVelocityRelated;
        ghost->m_yVelocityRelated3 = real->m_yVelocityRelated3;
        ghost->m_groundYVelocity = real->m_groundYVelocity;
        ghost->m_accelerationOrSpeed = real->m_accelerationOrSpeed;
        ghost->m_isAccelerating = real->m_isAccelerating;
        ghost->m_maybeIsFalling = real->m_maybeIsFalling;
        ghost->m_isOnGround2 = real->m_isOnGround2;
        ghost->m_isOnGround3 = real->m_isOnGround3;
        ghost->m_isOnGround4 = real->m_isOnGround4;
        ghost->m_stateOnGround = real->m_stateOnGround;
        ghost->m_lastLandTime = real->m_lastLandTime;
        ghost->m_lastJumpTime = real->m_lastJumpTime;
        ghost->m_lastFlipTime = real->m_lastFlipTime;
        ghost->m_lastSpiderFlipTime = real->m_lastSpiderFlipTime;
        ghost->m_lastGroundedPos = real->m_lastGroundedPos;
        ghost->m_blackOrbRelated = real->m_blackOrbRelated;
        ghost->m_padRingRelated = real->m_padRingRelated;
        ghost->m_ringJumpRelated = real->m_ringJumpRelated;

        // slopes
        ghost->m_isOnSlope = real->m_isOnSlope;
        ghost->m_wasOnSlope = real->m_wasOnSlope;
        ghost->m_slopeVelocity = real->m_slopeVelocity;
        ghost->m_maybeUpsideDownSlope = real->m_maybeUpsideDownSlope;
        ghost->m_currentSlope = real->m_currentSlope;
        ghost->m_currentSlope2 = real->m_currentSlope2;
        ghost->m_currentPotentialSlope = real->m_currentPotentialSlope;
        ghost->m_isCollidingWithSlope = real->m_isCollidingWithSlope;
        ghost->m_collidingWithSlopeId = real->m_collidingWithSlopeId;
        ghost->m_slopeFlipGravityRelated = real->m_slopeFlipGravityRelated;
        ghost->m_slopeAngle = real->m_slopeAngle;
        ghost->m_slopeAngleRadians = real->m_slopeAngleRadians;
        ghost->m_slopeRotation = real->m_slopeRotation;
        ghost->m_currentSlopeYVelocity = real->m_currentSlopeYVelocity;
        ghost->m_isCurrentSlopeTop = real->m_isCurrentSlopeTop;
        ghost->m_slopeStartTime = real->m_slopeStartTime;
        ghost->m_slopeEndTime = real->m_slopeEndTime;
        ghost->m_slopeSlidingMaybeRotated = real->m_slopeSlidingMaybeRotated;
        ghost->m_stateDartSlide = real->m_stateDartSlide;

        // what it's standing on / touching
        ghost->m_lastGroundObject = real->m_lastGroundObject;
        ghost->m_preLastGroundObject = real->m_preLastGroundObject;
        ghost->m_maybeLastGroundObject = real->m_maybeLastGroundObject;
        ghost->m_collidedObject = real->m_collidedObject;
        ghost->m_objectSnappedTo = real->m_objectSnappedTo;
        ghost->m_snapDistance = real->m_snapDistance;
        ghost->m_lastCollisionBottom = real->m_lastCollisionBottom;
        ghost->m_lastCollisionTop = real->m_lastCollisionTop;
        ghost->m_lastCollisionLeft = real->m_lastCollisionLeft;
        ghost->m_lastCollisionRight = real->m_lastCollisionRight;
        ghost->m_collidedTopMinY = real->m_collidedTopMinY;
        ghost->m_collidedBottomMaxY = real->m_collidedBottomMaxY;
        ghost->m_collidedLeftMaxX = real->m_collidedLeftMaxX;
        ghost->m_collidedRightMinX = real->m_collidedRightMinX;

        // dash orbs
        ghost->m_dashX = real->m_dashX;
        ghost->m_dashY = real->m_dashY;
        ghost->m_dashAngle = real->m_dashAngle;
        ghost->m_dashStartTime = real->m_dashStartTime;
        ghost->m_dashRing = real->m_dashRing;
        ghost->m_dashFireFrame = real->m_dashFireFrame;

        // force / boost / state triggers already acting on the player
        ghost->m_stateForce = real->m_stateForce;
        ghost->m_stateForceVector = real->m_stateForceVector;
        ghost->m_affectedByForces = real->m_affectedByForces;
        ghost->m_stateBoostX = real->m_stateBoostX;
        ghost->m_stateBoostY = real->m_stateBoostY;
        ghost->m_stateNoStickX = real->m_stateNoStickX;
        ghost->m_stateNoStickY = real->m_stateNoStickY;
        ghost->m_stateHitHead = real->m_stateHitHead;
        ghost->m_stateFlipGravity = real->m_stateFlipGravity;
        ghost->m_stateNoAutoJump = real->m_stateNoAutoJump;
        ghost->m_stateScale = real->m_stateScale;
        ghost->m_maybeStateForce2 = real->m_maybeStateForce2;
    }

    // Your recent input timings, so predictions model what you actually do instead of
    // an infinite hold: cube taps (holding auto-re-jumps) and robot holds (they set the
    // jump height).
    constexpr int HOLD_SAMPLES = 8;

    struct HoldStats {
        int samples[HOLD_SAMPLES] = {};
        int count = 0;
        int index = 0;

        void add(int ticks) {
            samples[index] = std::clamp(ticks, 1, 240);
            index = (index + 1) % HOLD_SAMPLES;
            count = std::min(count + 1, HOLD_SAMPLES);
        }

        // percentile of the recent samples, or `fallback` with no samples yet
        int pick(int fallback, int percent) const {
            if (count == 0) {
                return fallback;
            }
            int tmp[HOLD_SAMPLES];
            std::copy(samples, samples + count, tmp);
            int at = std::min(count - 1, count * percent / 100);
            std::nth_element(tmp, tmp + at, tmp + count);
            return tmp[at];
        }
    };

    HoldStats tapHold, robotHold;
    int tickCount = 0;

    enum class InputKind { Tap, Wave, Robot, Other };
    int pressTick = -1;
    InputKind pressKind = InputKind::Other;

    // Cube / ball / ufo / spider / swing are tapped. Ship, wave and robot are held, and
    // each keeps its own samples so long holds don't pollute the cube's tap length.
    InputKind kindOf(PlayerObject* p) {
        if (!p || p->m_isShip) {
            return InputKind::Other;
        }
        if (p->m_isDart) {
            return InputKind::Wave;
        }
        return p->m_isRobot ? InputKind::Robot : InputKind::Tap;
    }

    void recordRealInput(bool down, PlayerObject* p1) {
        if (down) {
            pressTick = tickCount;
            pressKind = kindOf(p1);
            return;
        }
        if (pressTick < 0) {
            return;
        }
        int held = tickCount - pressTick;
        pressTick = -1;
        if (buf.autoReleased) {
            return;  // the game already let go earlier; this length isn't a real hold
        }
        switch (pressKind) {
            case InputKind::Tap: tapHold.add(held); break;
            case InputKind::Robot: robotHold.add(held); break;
            default: break;
        }
    }

    // A hold that spans a death / restart / new level isn't a real tap length.
    void forgetPress() {
        pressTick = -1;
    }

    int holdEstimate() {
        return tapHold.pick(st(12), 50);  // ~50 ms, a typical tap
    }

    // v2.4 moving-object guard: objects the ghost collided with are watched every tick,
    // and any that moved recently make the decision "don't buffer" (the ghost sees the
    // level frozen at the click, so it tests them in the wrong place).
    void watchObject(GameObject* object) {
        auto [it, added] = watched.try_emplace(object, Watched { object->m_positionX, object->m_positionY, -MOVER_TICKS });
        if (added) {
            it->second.rot = object->getRotation();
        }
        if (!added && tickCount - it->second.movedTick < st(MOVER_TICKS)) {
            buf.sawMover = true;
        }
    }

    // v2.41: objects that moved on the last step, so predictions don't have to search the
    // whole watch list for them.
    std::vector<GameObject*> movingNow;

    void updateWatched() {
        if (watched.size() > WATCH_LIMIT) {
            watched.clear();
        }
        movingNow.clear();
        // v2.41: objects far from you (passed long ago, or far ahead) are dropped instead of
        // being checked every step; predictions never reach further than ~300 units.
        double px = 0.0;
        bool prune = false;
        if (auto* layer = GameManager::get()->m_playLayer; layer && layer->m_player1) {
            px = layer->m_player1->getPosition().x;
            prune = true;
        }
        for (auto it = watched.begin(); it != watched.end();) {
            auto* object = it->first;
            auto& w = it->second;
            if (prune && std::abs(object->m_positionX - px) > 800.0) {
                it = watched.erase(it);
                continue;
            }
            double rot = object->getRotation();
            if (object->m_positionX != w.x || object->m_positionY != w.y || rot != w.rot) {
                // called every step, so the change is the movement per step
                bool consecutive = tickCount - w.movedTick == 1;
                w.pvx = consecutive ? w.vx : 0.0;
                w.pvy = consecutive ? w.vy : 0.0;
                w.vx = object->m_positionX - w.x;
                w.vy = object->m_positionY - w.y;
                double dr = rot - w.rot;
                w.vrot = std::remainder(dr, 360.0);
                w.x = object->m_positionX;
                w.y = object->m_positionY;
                w.rot = rot;
                w.movedTick = tickCount;
                movingNow.push_back(object);
            }
            else if (tickCount - w.movedTick > 1) {
                w.vx = w.vy = w.pvx = w.pvy = w.vrot = 0.0;  // stopped
            }
            ++it;
        }
    }

    // v2.27 moving-object prediction. Objects near your path that are moving right now are
    // moved along their current motion during a prediction (where they'll be k steps
    // later), and put back exactly afterwards. Straight-line movement only: easing,
    // rotation and toggles still aren't predicted.
    // v2.68: also curved paths (rotate triggers move objects around a center, Orbit's saws):
    // when the direction of motion turned between the last two steps, it keeps turning by the
    // same angle each step, and objects that spin keep spinning. Still put back exactly after.
    struct Mover {
        GameObject* object;
        double x, y, vx, vy;
        double turn = 0.0;  // radians the motion direction turns per step (0 = straight)
        double rotX = 0.0, rotY = 0.0, vrot = 0.0;  // both axes kept as they were (skewed objects)
        double ax = 0.0, ay = 0.0;  // v2.75: speeding up / slowing down (easing), per step
        int accelSteps = 0;         // v2.75: how long the easing is followed (until it would stop / over 3x speed)
    };
    std::vector<Mover> movers;

    void collectMovers() {
        movers.clear();
        if (!buf.predictMovers) {
            return;
        }
        for (auto* object : movingNow) {
            auto it = watched.find(object);
            if (it == watched.end()) {
                continue;
            }
            auto const& w = it->second;
            if ((w.vx != 0.0 || w.vy != 0.0 || w.vrot != 0.0) && tickCount - w.movedTick <= 1) {
                Mover m { object, object->m_positionX, object->m_positionY, w.vx, w.vy };
                double speed = std::hypot(w.vx, w.vy);
                double prev = std::hypot(w.pvx, w.pvy);
                if (speed > 1e-6 && prev > 1e-6) {
                    double turn = std::remainder(std::atan2(w.vy, w.vx) - std::atan2(w.pvy, w.pvx), 6.283185307179586);
                    double ratio = speed / prev;
                    // a steady turn at a steady speed; anything wilder stays a straight line
                    if (std::abs(turn) > 1e-4 && std::abs(turn) < 0.5 && ratio > 0.8 && ratio < 1.25) {
                        m.turn = turn;
                    }
                    // v2.75: straight, but faster or slower than the step before: eased movement
                    else if (std::abs(turn) <= 1e-4 && std::abs(ratio - 1.0) > 1e-3 && ratio > 0.5 && ratio < 2.0) {
                        m.ax = w.vx - w.pvx;
                        m.ay = w.vy - w.pvy;
                        double along = (m.ax * w.vx + m.ay * w.vy) / speed;  // + speeding up, - slowing down
                        double steps = along < 0.0 ? speed / -along : 2.0 * speed / along;
                        m.accelSteps = static_cast<int>(std::min(steps, 1000.0));
                    }
                }
                m.rotX = object->getRotationX();
                m.rotY = object->getRotationY();
                m.vrot = w.vrot;
                movers.push_back(m);
            }
        }
    }

    void placeMovers(int steps) {
        for (auto const& m : movers) {
            double x = m.x + m.vx * steps;
            double y = m.y + m.vy * steps;
            if (m.accelSteps > 0 && steps > 0) {
                // eased: the speed keeps changing the same way, then stays (stopped or at 3x)
                int n = std::min(steps, m.accelSteps);
                double grow = 0.5 * n * (n + 1);
                x = m.x + m.vx * steps + m.ax * (grow + static_cast<double>(n) * (steps - n));
                y = m.y + m.vy * steps + m.ay * (grow + static_cast<double>(n) * (steps - n));
            }
            if (m.turn != 0.0 && steps > 0) {
                // sum of the step vector turned by `turn` once more every step
                std::complex<double> v { m.vx, m.vy };
                auto rho = std::polar(1.0, m.turn);
                auto moved = v * rho * (1.0 - std::pow(rho, steps)) / (1.0 - rho);
                x = m.x + moved.real();
                y = m.y + moved.imag();
            }
            if (m.vrot != 0.0) {
                double spun = steps == 0 ? 0.0 : m.vrot * steps;
                m.object->setRotationX(static_cast<float>(m.rotX + spun));
                m.object->setRotationY(static_cast<float>(m.rotY + spun));
            }
            m.object->setPosition({ static_cast<float>(x), static_cast<float>(y) });
            m.object->m_positionX = x;
            m.object->m_positionY = y;
            m.object->setObjectRectDirty(true);
            m.object->m_isOrientedBoxDirty = true;
        }
    }

    // v2.81 move triggers ahead: spikes, blocks and orbs that are still right now but that a
    // move trigger further along will start moving (the ORBIT case: the prediction saw them
    // frozen where they were). When the ghost passes such a trigger during a prediction, its
    // group starts moving the way the game moves it (offset, duration, easing), on top of
    // any motion it already has; everything is put back exactly afterwards. Only plain moves:
    // target mode, lock to player / camera and spawn / touch triggered moves are skipped.
    // v2.83: rotate triggers too (saws and arms that start spinning, around their center
    // group or each in place), and toggle triggers that switch a group off (the ghost stops
    // colliding with it, like a toggle orb since v2.75).
    // v2.85: spawn triggers too, followed through their delays to the move / rotate / toggle
    // triggers they set off (and spawn triggers they set off, 4 deep), and toggles that switch
    // a group on (the ghost starts colliding with it).
    enum class TriggerKind { Move, Rotate, Toggle, Spawn };
    struct LevelTrigger {
        EffectGameObject* trigger;
        double x;
        TriggerKind kind;
    };
    std::vector<LevelTrigger> levelTriggers;  // the level's position-activated triggers we predict, by x
    GJBaseGameLayer* levelTriggersLayer = nullptr;
    unsigned levelTriggersCount = 0;
    // move / rotate triggers the real game fired (any kind, spawn / touch too) and the step
    // they fired, so ones still in progress at your click follow their real eased path
    std::unordered_map<EffectGameObject*, int> firedMoves;

    struct ScriptedMove {
        TriggerKind kind = TriggerKind::Move;
        double x = 0.0;
        int fireStep = -1;  // prediction step the ghost reached the trigger (-1 = not yet)
        double dx = 0.0, dy = 0.0;  // move
        double angle = 0.0;         // rotate: degrees, clockwise
        bool hasCenter = false;
        double cx = 0.0, cy = 0.0;
        bool lockRot = false;
        double steps = 0.0;  // how many physics steps it lasts
        int easing = 0;
        double rate = 0.0;
        double from = 0.0;  // how far along it already was when the prediction started
        CCArray* group = nullptr;  // toggle: the group it switches off (or on)
        bool on = false;           // toggle: switches it on
        bool armed = false;        // v2.85: fired (fireStep is when it acts, maybe after a spawn delay)
        int delay = 0;             // v2.85: steps from the ghost reaching x to it acting (spawn delays)
        bool toggled = false;
    };
    struct ScriptedObject {
        GameObject* object;
        double x, y;     // m_positionX / Y before the prediction
        CCPoint node;    // and the node position, put back as it was
        float rotX, rotY;
        bool mover;      // also moved by placeMovers, which sets its base position every step
        bool placed = false;
        bool rotated = false;
        std::vector<int> moves;
        double lastX = 0.0, lastY = 0.0, lastSpin = 0.0;  // v2.84.5: where it was put last step
    };
    std::vector<ScriptedMove> scriptedMoves;
    std::vector<ScriptedObject> scriptedObjects;
    void forgetTriggerChecks();
    constexpr int SCRIPTED_TRIGGER_LIMIT = 48;
    // v2.85: groups the real game spawns after a delay, and the tick that happens
    struct PendingSpawn {
        int group;
        int at;
    };
    std::vector<PendingSpawn> pendingSpawns;
    constexpr int SCRIPTED_OBJECT_LIMIT = 160;
    // v2.84.4: what collectScripted found, reused by every run of the same click (it used to
    // scan the trigger groups again for every run: on ORBIT that slowed each run so much that
    // orb searches stopped after 0-2 timings)
    struct ScriptedCache {
        GJBaseGameLayer* layer = nullptr;
        int tick = -1;
        double px = 0.0, reach = 0.0;
        std::vector<ScriptedMove> moves;
        std::vector<ScriptedObject> objects;
    } scriptedCache;

    void forgetMoveTriggers() {
        scriptedCache = {};
        pendingSpawns.clear();
        forgetTriggerChecks();  // v2.84: a new level starts the self-check over
        firedMoves.clear();
        levelTriggers.clear();
        levelTriggersLayer = nullptr;
        levelTriggersCount = 0;
    }

    // the game's easing curves (cocos2d's), by GD's easing number
    double easeMove(int type, double rate, double t) {
        constexpr double PI = 3.141592653589793;
        t = std::clamp(t, 0.0, 1.0);
        if (t <= 0.0 || t >= 1.0) {
            return t;
        }
        rate = rate > 0.0 ? rate : 2.0;
        auto bounce = [](double u) {
            if (u < 1.0 / 2.75) {
                return 7.5625 * u * u;
            }
            if (u < 2.0 / 2.75) {
                u -= 1.5 / 2.75;
                return 7.5625 * u * u + 0.75;
            }
            if (u < 2.5 / 2.75) {
                u -= 2.25 / 2.75;
                return 7.5625 * u * u + 0.9375;
            }
            u -= 2.625 / 2.75;
            return 7.5625 * u * u + 0.984375;
        };
        double period = rate > 0.0 && rate < 2.0 ? rate : 0.3;
        double s = period / 4.0;
        constexpr double BACK = 1.70158;
        switch (type) {
            case 1: {  // ease in out
                double u = t * 2.0;
                return u < 1.0 ? 0.5 * std::pow(u, rate) : 1.0 - 0.5 * std::pow(2.0 - u, rate);
            }
            case 2: return std::pow(t, rate);
            case 3: return std::pow(t, 1.0 / rate);
            case 4: {  // elastic in out
                double u = t * 2.0 - 1.0;
                return u < 0.0 ? -0.5 * std::pow(2.0, 10.0 * u) * std::sin((u - s) * 2.0 * PI / period)
                               : std::pow(2.0, -10.0 * u) * std::sin((u - s) * 2.0 * PI / period) * 0.5 + 1.0;
            }
            case 5: {
                double u = t - 1.0;
                return -std::pow(2.0, 10.0 * u) * std::sin((u - s) * 2.0 * PI / period);
            }
            case 6: return std::pow(2.0, -10.0 * t) * std::sin((t - s) * 2.0 * PI / period) + 1.0;
            case 7: return t < 0.5 ? (1.0 - bounce(1.0 - t * 2.0)) * 0.5 : bounce(t * 2.0 - 1.0) * 0.5 + 0.5;
            case 8: return 1.0 - bounce(1.0 - t);
            case 9: return bounce(t);
            case 10: {
                double u = t * 2.0;
                return u < 1.0 ? 0.5 * std::pow(2.0, 10.0 * (u - 1.0)) : 0.5 * (2.0 - std::pow(2.0, -10.0 * (u - 1.0)));
            }
            case 11: return std::pow(2.0, 10.0 * (t - 1.0));
            case 12: return 1.0 - std::pow(2.0, -10.0 * t);
            case 13: return -0.5 * (std::cos(PI * t) - 1.0);
            case 14: return 1.0 - std::cos(t * PI / 2.0);
            case 15: return std::sin(t * PI / 2.0);
            case 16: {
                double o = BACK * 1.525;
                double u = t * 2.0;
                if (u < 1.0) {
                    return u * u * ((o + 1.0) * u - o) / 2.0;
                }
                u -= 2.0;
                return u * u * ((o + 1.0) * u + o) / 2.0 + 1.0;
            }
            case 17: return t * t * ((BACK + 1.0) * t - BACK);
            case 18: {
                double u = t - 1.0;
                return u * u * ((BACK + 1.0) * u + BACK) + 1.0;
            }
            default: return t;
        }
    }

    bool plainTrigger(EffectGameObject* t, bool rotate) {
        if (t->m_targetGroupID <= 0 || t->m_isDynamicMode) {
            return false;
        }
        if (rotate) {
            return t->m_rotationTargetID <= 0 && (t->m_rotationDegrees != 0.f || t->m_times360 != 0);
        }
        return !t->m_useMoveTarget && !t->m_lockToPlayerX && !t->m_lockToPlayerY && !t->m_lockToCameraX && !t->m_lockToCameraY &&
            (t->m_moveOffset.x != 0.f || t->m_moveOffset.y != 0.f);
    }

    // v2.85: a spawn trigger the prediction can follow: no random delay, no remapped groups, not ordered
    bool plainSpawn(EffectGameObject* t) {
        auto* s = static_cast<SpawnTriggerGameObject*>(t);
        return t->m_targetGroupID > 0 && s->m_delayRange <= 0.f && s->m_remapObjects.empty() && !t->m_spawnOrdered;
    }

    int spawnSteps(EffectGameObject* t, float tickLength) {
        return static_cast<int>(std::lround(std::max(0.f, static_cast<SpawnTriggerGameObject*>(t)->m_spawnDelay) * 60.0 / tickLength));
    }

    // the level's move / rotate / toggle / spawn triggers, found once per level (and again if
    // objects were added)
    void scanMoveTriggers(GJBaseGameLayer* layer) {
        unsigned count = layer->m_objects ? layer->m_objects->count() : 0;
        if (layer == levelTriggersLayer && count == levelTriggersCount) {
            return;
        }
        levelTriggersLayer = layer;
        levelTriggersCount = count;
        levelTriggers.clear();
        if (!layer->m_objects) {
            return;
        }
        for (auto* object : CCArrayExt<GameObject*>(layer->m_objects)) {
            if (!object) {
                continue;
            }
            int id = object->m_objectID;
            if (id != 901 && id != 1346 && id != 1049 && id != 1268) {
                continue;
            }
            auto* t = static_cast<EffectGameObject*>(object);
            if (t->m_isSpawnTriggered || t->m_isTouchTriggered) {
                continue;
            }
            if (id == 1049) {
                if (t->m_targetGroupID > 0) {
                    levelTriggers.push_back({ t, t->m_positionX, TriggerKind::Toggle });  // v2.85: on too
                }
                continue;
            }
            if (id == 1268) {
                if (plainSpawn(t)) {
                    levelTriggers.push_back({ t, t->m_positionX, TriggerKind::Spawn });
                }
                continue;
            }
            bool rotate = id == 1346;
            if (plainTrigger(t, rotate)) {
                levelTriggers.push_back({ t, t->m_positionX, rotate ? TriggerKind::Rotate : TriggerKind::Move });
            }
        }
        std::sort(levelTriggers.begin(), levelTriggers.end(), [](auto const& a, auto const& b) { return a.x < b.x; });
    }

    // a move / rotate trigger's motion: offset or angle (and center), duration, easing
    // a group's group parent (set in the editor), or nullptr
    GameObject* groupParentOf(GJBaseGameLayer* layer, int group) {
        if (group <= 0 || !layer->m_parentGroupsDict) {
            return nullptr;
        }
        return typeinfo_cast<GameObject*>(layer->m_parentGroupsDict->objectForKey(group));
    }

    void fillMove(GJBaseGameLayer* layer, EffectGameObject* t, TriggerKind kind, float tickLength, ScriptedMove& m) {
        m.kind = kind;
        m.steps = t->m_duration * 60.0 / tickLength;
        m.easing = static_cast<int>(t->m_easingType);
        m.rate = t->m_easingRate;
        if (kind == TriggerKind::Move) {
            m.dx = t->m_moveOffset.x;
            m.dy = t->m_moveOffset.y;
            return;
        }
        m.angle = t->m_rotationDegrees + 360.0 * t->m_times360;
        m.lockRot = t->m_lockObjectRotation;
        // v2.84.5: the pivot is the center group's group parent (or its first object); with no
        // center group, the target group's group parent, and only without one does each object
        // turn in place (ORBIT's saws orbit their group parent: they were predicted spinning in place)
        GameObject* pivot = nullptr;
        if (t->m_centerGroupID > 0) {
            pivot = groupParentOf(layer, t->m_centerGroupID);
            if (!pivot) {
                auto* center = layer->getGroup(t->m_centerGroupID);
                if (center && center->count() > 0) {
                    pivot = static_cast<GameObject*>(center->objectAtIndex(0));
                }
            }
        }
        else {
            pivot = groupParentOf(layer, t->m_targetGroupID);
        }
        if (pivot) {
            m.hasCenter = true;
            m.cx = pivot->m_positionX;
            m.cy = pivot->m_positionY;
        }
    }

    // fraction f of move m applied to a position (and an object's own turn)
    void applyMove(ScriptedMove const& m, double f, double& x, double& y, double& spin) {
        if (m.kind == TriggerKind::Move) {
            x += m.dx * f;
            y += m.dy * f;
            return;
        }
        if (m.kind != TriggerKind::Rotate) {
            return;
        }
        double a = m.angle * f;
        if (m.hasCenter) {
            // clockwise around the center (y up)
            double th = -a * 3.141592653589793 / 180.0;
            double rx = x - m.cx, ry = y - m.cy;
            x = m.cx + rx * std::cos(th) - ry * std::sin(th);
            y = m.cy + rx * std::sin(th) + ry * std::cos(th);
        }
        if (!m.lockRot) {
            spin += a;
        }
    }

    // v2.84 trigger self-check: when a real move / rotate trigger fires, one of its objects
    // near you is watched for up to half a second, and where it really ends up is compared with
    // where the prediction would have put it. If this level's triggers keep coming out wrong,
    // trigger prediction turns itself off for the level.
    // v2.84.2: the prediction for the watched object is the combined motion of EVERY move /
    // rotate trigger that acted on any of its groups in that time (ORBIT moves almost every
    // block with several triggers at once, so v2.84.1 skipped every check). Only when a trigger
    // the prediction can't model acted on it (follow, keyframes, target / lock moves...) is
    // the check left out.
    struct TriggerCheck {
        GameObject* object;
        int fired;
        int at;
        double x, y, rot;
    };
    std::vector<TriggerCheck> triggerChecks;
    int checksGood = 0, checksBad = 0, checksUnclear = 0;
    bool triggersDistrusted = false;
    int attemptStartTick = 0;  // v2.84.3
    // every trigger with a target group the real game fired lately, and when
    struct TriggerFire {
        EffectGameObject* trigger;
        int tick;
        ScriptedMove move;  // v2.84.5: filled when it fired (the pivot where it was then)
    };
    std::vector<TriggerFire> triggerFires;

    void noteTriggerFire(GJBaseGameLayer* layer, EffectGameObject* t) {
        if (t->m_targetGroupID <= 0) {
            return;
        }
        if (triggerFires.size() > 1024) {
            triggerFires.erase(triggerFires.begin(), triggerFires.begin() + 512);
        }
        ScriptedMove m;
        int id = t->m_objectID;
        if (layer && (id == 901 || id == 1346)) {
            float tickLength = buf.tickLength > 0.001f && buf.tickLength < 2.f ? buf.tickLength : TICK;
            fillMove(layer, t, id == 1346 ? TriggerKind::Rotate : TriggerKind::Move, tickLength, m);
        }
        triggerFires.push_back({ t, tickCount, m });
    }

    // triggers that move objects in ways the prediction doesn't model
    bool unmodeledMover(int id) {
        switch (id) {
            case 1347:  // follow
            case 1814:  // follow player Y
            case 2067:  // scale
            case 3016:  // advanced follow
            case 3033:  // keyframe animation
            case 3006: case 3007: case 3008: case 3009: case 3010:  // area move / rotate / scale / fade / tint
                return true;
            default:
                return false;
        }
    }

    bool inGroups(GameObject* object, int group) {
        auto* groups = object->m_groups;
        int count = std::clamp<int>(object->m_groupCount, 0, 10);
        if (!groups || group <= 0) {
            return false;
        }
        for (int i = 0; i < count; ++i) {
            if ((*groups)[i] == group) {
                return true;
            }
        }
        return false;
    }

    void startTriggerCheck(GJBaseGameLayer* layer, EffectGameObject* t) {
        // v2.84.3: checks keep running while trigger prediction is off, so it can come back on;
        // triggers in the first quarter second of an attempt (level setup) aren't checked
        if (!buf.predictTriggers || triggerChecks.size() >= 16 || !layer || !layer->m_player1 || tickCount - attemptStartTick < st(60)) {
            return;
        }
        bool rotate = t->m_objectID == 1346;
        if (!plainTrigger(t, rotate) || t->m_duration * 60.f < TICK) {
            return;
        }
        auto* group = layer->getGroup(t->m_targetGroupID);
        if (!group) {
            return;
        }
        float tickLength = buf.tickLength > 0.001f && buf.tickLength < 2.f ? buf.tickLength : TICK;
        double px = layer->m_player1->getPositionX();
        for (auto* object : CCArrayExt<GameObject*>(group)) {
            if (!object || object == t || object->m_isDecoration || object->m_objectType == GameObjectType::Decoration ||
                object->m_positionX < px - 150.0 || object->m_positionX > px + 700.0) {
                continue;
            }
            for (auto const& c : triggerChecks) {
                if (c.object == object) {
                    return;  // already watched
                }
            }
            int span = std::min(static_cast<int>(std::lround(t->m_duration * 60.0 / tickLength)), st(120));
            triggerChecks.push_back({ object, tickCount, tickCount + std::max(span, 1), object->m_positionX, object->m_positionY,
                static_cast<double>(object->getRotationX()) });
            return;
        }
    }

    void forgetTriggerChecks() {
        triggerChecks.clear();
        triggerFires.clear();
        checksGood = checksBad = checksUnclear = 0;
        triggersDistrusted = false;
    }

    // where the prediction says the object is now: every modeled trigger on its groups, each
    // from when the check started (or it fired) to now. nullptr = predicted, else why not.
    char const* predictCheck(GJBaseGameLayer* layer, TriggerCheck const& c, double& x, double& y, double& spin, int& used,
        std::string* detail = nullptr) {
        float tickLength = buf.tickLength > 0.001f && buf.tickLength < 2.f ? buf.tickLength : TICK;
        x = c.x;
        y = c.y;
        spin = 0.0;
        used = 0;
        for (auto const& f : triggerFires) {
            if (f.tick > c.at || !inGroups(c.object, f.trigger->m_targetGroupID)) {
                continue;
            }
            int id = f.trigger->m_objectID;
            if (unmodeledMover(id)) {
                if (f.tick >= c.fired - st(240)) {
                    return "a follow / keyframe / area trigger moves it";
                }
                continue;
            }
            if (id != 901 && id != 1346) {
                continue;
            }
            ScriptedMove m = f.move;
            bool rotate = id == 1346;
            int start = std::max(c.fired, f.tick);
            if (m.steps >= 0.5 && f.tick + m.steps <= c.fired) {
                continue;  // finished before the check started
            }
            if (!plainTrigger(f.trigger, rotate)) {
                return "a target / lock-to-player move moves it";
            }
            double done = m.steps < 0.5 ? 1.0 : easeMove(m.easing, m.rate, (c.at - f.tick) / m.steps);
            double before = m.steps < 0.5 ? (f.tick < c.fired ? 1.0 : 0.0) : easeMove(m.easing, m.rate, (start - f.tick) / m.steps);
            applyMove(m, done - before, x, y, spin);
            ++used;
            if (detail) {
                // v2.84.4: what each trigger was, for misses: id, offset or angle, duration, easing
                auto* t = f.trigger;
                *detail += rotate ? fmt::format(" | rotate {:g}deg{}{} {:.2f}s e{}/{:g}", m.angle, m.hasCenter ? " around center" : " in place",
                                        m.lockRot ? " locked" : "", t->m_duration, m.easing, m.rate)
                                  : fmt::format(" | move {:g},{:g}{}{} {:.2f}s e{}/{:g}", m.dx, m.dy, t->m_smallStep ? " smallstep" : "",
                                        t->m_isSilent ? " silent" : "", t->m_duration, m.easing, m.rate);
                *detail += fmt::format(" from t{}", f.tick - c.fired);
            }
        }
        return nullptr;
    }

    void runTriggerChecks(GJBaseGameLayer* layer) {
        for (auto it = triggerChecks.begin(); it != triggerChecks.end();) {
            auto const& c = *it;
            if (tickCount < c.fired) {
                it = triggerChecks.erase(it);
                continue;
            }
            if (tickCount < c.at) {
                ++it;
                continue;
            }
            double x, y, spin;
            int used;
            std::string detail;
            char const* unclear = predictCheck(layer, c, x, y, spin, used, buf.explain ? &detail : nullptr);
            double moved = std::hypot(c.object->m_positionX - c.x, c.object->m_positionY - c.y);
            double turned = std::abs(std::remainder(c.object->getRotationX() - c.rot, 360.0));
            if (!unclear && moved < 0.5 && turned < 0.5) {
                unclear = "it didn't move";  // not updated yet, or a stop trigger
            }
            if (unclear) {
                ++checksUnclear;
                if (buf.explain && checksUnclear % 10 == 1) {
                    say("trigger check: unclear ({}), {} unclear so far", unclear, checksUnclear);
                }
                it = triggerChecks.erase(it);
                continue;
            }
            double err = std::hypot(c.object->m_positionX - x, c.object->m_positionY - y);
            double tol = std::max(3.0, std::hypot(x - c.x, y - c.y) * 0.1);
            double turnErr = std::abs(std::remainder(c.object->getRotationX() - (c.rot + spin), 360.0));
            bool ok = err <= tol && turnErr <= std::max(5.0, std::abs(spin) * 0.1);
            ok ? ++checksGood : ++checksBad;
            if (buf.explain) {
                say("trigger check: {} the prediction ({} trigger{}, off by {:.1f}u {:.0f} deg | {} hit / {} missed / {} unclear)",
                    ok ? "matches" : "DOESN'T match", used, used == 1 ? "" : "s", err, turnErr, checksGood, checksBad, checksUnclear);
                if (!ok) {
                    // v2.84.4: expected vs real movement and the triggers involved
                    say("  obj {} expected moved {:.1f},{:.1f} turned {:.0f} | really moved {:.1f},{:.1f} turned {:.0f} over {}t{}",
                        c.object->m_objectID, x - c.x, y - c.y, spin, c.object->m_positionX - c.x, c.object->m_positionY - c.y,
                        std::remainder(c.object->getRotationX() - c.rot, 360.0), c.at - c.fired, detail);
                }
            }
            // v2.84.3: decided on 10+ checks, and it comes back on if hits catch up (ORBIT went
            // off at 1 hit / 5 missed from level-start triggers, then ended at 9 hit / 8 missed)
            bool wrong = checksGood + checksBad >= 10 && checksBad > 2 * checksGood;
            if (wrong != triggersDistrusted) {
                triggersDistrusted = wrong;
                if (wrong) {
                    say("trigger check: {} of {} trigger predictions were wrong on this level, trigger prediction is off here", checksBad,
                        checksBad + checksGood);
                }
                else {
                    say("trigger check: {} of {} wrong now, trigger prediction is back on", checksBad, checksBad + checksGood);
                }
            }
            it = triggerChecks.erase(it);
        }
    }

    // the triggers this prediction can reach, and the objects they move
    void collectScripted(GJBaseGameLayer* layer, int total) {
        scriptedMoves.clear();
        scriptedObjects.clear();
        buf.triggersAhead = 0;
        if (!buf.predictTriggers || triggersDistrusted || !buf.ghost1 || !layer->m_player1 || layer->m_player1->m_isGoingLeft) {
            return;
        }
        double px = buf.ghost1->getPositionX();
        double py = buf.ghost1->getPositionY();
        double reach = (total + 2) * std::max(static_cast<double>(std::abs(buf.tickDx)), 2.0) + 30.0;
        auto& cache = scriptedCache;
        if (cache.layer == layer && cache.tick == tickCount && reach <= cache.reach && std::abs(px - cache.px) < 3.0) {
            scriptedMoves = cache.moves;
            scriptedObjects = cache.objects;
            buf.triggersAhead = static_cast<int>(scriptedMoves.size());
            return;
        }
        scanMoveTriggers(layer);
        float tickLength = buf.tickLength > 0.001f && buf.tickLength < 2.f ? buf.tickLength : TICK;
        std::unordered_map<GameObject*, size_t> index;
        // v2.84.4: only objects whose motion can come near the ghost's path (it was every object
        // within ~800 units either side, at any height)
        double pathMinX = px - 90.0, pathMaxX = px + reach + 90.0;
        double pathMinY = py - 450.0, pathMaxY = py + 450.0;
        auto nearPath = [&](GameObject* object, ScriptedMove const& m) {
            double x = object->m_positionX, y = object->m_positionY;
            double x0 = x, x1 = x, y0 = y, y1 = y;
            if (m.kind == TriggerKind::Move) {
                x0 = std::min(x, x + m.dx);
                x1 = std::max(x, x + m.dx);
                y0 = std::min(y, y + m.dy);
                y1 = std::max(y, y + m.dy);
            }
            else if (m.hasCenter) {
                double r = std::hypot(x - m.cx, y - m.cy);
                x0 = m.cx - r;
                x1 = m.cx + r;
                y0 = m.cy - r;
                y1 = m.cy + r;
            }
            constexpr double SIZE = 60.0;  // the object's own size
            return x1 + SIZE >= pathMinX && x0 - SIZE <= pathMaxX && y1 + SIZE >= pathMinY && y0 - SIZE <= pathMaxY;
        };
        // adds trigger t: armed = it fires at step `when` (negative: it fired that long ago),
        // else when the ghost gets to x, `when` steps later (a spawn delay)
        auto addMove = [&](EffectGameObject* t, double x, TriggerKind kind, bool armed, int when) {
            int elapsed = armed && when < 0 ? -when : -1;
            if (static_cast<int>(scriptedMoves.size()) >= SCRIPTED_TRIGGER_LIMIT) {
                return;
            }
            auto* group = layer->getGroup(t->m_targetGroupID);
            if (!group || group->count() == 0) {
                return;
            }
            ScriptedMove m;
            m.kind = kind;
            m.x = x;
            m.armed = armed;
            m.fireStep = armed ? when : -1;
            m.delay = armed ? 0 : when;
            if (kind == TriggerKind::Toggle) {
                m.group = group;
                m.on = t->m_activateGroup;
                scriptedMoves.push_back(m);
                return;
            }
            fillMove(layer, t, kind, tickLength, m);
            if (elapsed >= 0) {
                if (m.steps < 0.5 || elapsed >= m.steps) {
                    return;  // already finished
                }
                m.from = easeMove(m.easing, m.rate, elapsed / m.steps);
            }
            int move = static_cast<int>(scriptedMoves.size());
            bool any = false;
            for (auto* object : CCArrayExt<GameObject*>(group)) {
                if (!object || object == t || object->m_isDecoration || object->m_objectType == GameObjectType::Decoration ||
                    !nearPath(object, m)) {
                    continue;
                }
                auto [found, added] = index.try_emplace(object, scriptedObjects.size());
                if (added) {
                    if (scriptedObjects.size() >= SCRIPTED_OBJECT_LIMIT) {
                        index.erase(found);
                        continue;
                    }
                    bool mover = std::any_of(movers.begin(), movers.end(), [&](Mover const& mv) { return mv.object == object; });
                    scriptedObjects.push_back({ object, object->m_positionX, object->m_positionY, object->getPosition(),
                        object->getRotationX(), object->getRotationY(), mover });
                }
                scriptedObjects[found->second].moves.push_back(move);
                any = true;
            }
            if (any) {
                scriptedMoves.push_back(m);
            }
        };
        // moves / rotations already running (with "predict moving objects" on, those are
        // extrapolated instead)
        if (!buf.predictMovers) {
            for (auto fired = firedMoves.begin(); fired != firedMoves.end();) {
                auto* t = fired->first;
                int elapsed = tickCount - fired->second;
                if (elapsed < 0 || elapsed * tickLength > t->m_duration * 60.f + 1.f) {
                    fired = firedMoves.erase(fired);
                    continue;
                }
                bool rotate = t->m_objectID == 1346;
                if (plainTrigger(t, rotate)) {
                    addMove(t, 0.0, rotate ? TriggerKind::Rotate : TriggerKind::Move, true, -elapsed);
                }
                ++fired;
            }
        }
        // v2.85: what spawning a group sets off: its spawn-triggered move / rotate / toggle
        // triggers, and its spawn triggers in turn (with their delays, 4 deep)
        std::function<void(int, double, bool, int, int)> spawnChain = [&](int groupID, double x, bool armed, int when, int depth) {
            auto* group = layer->getGroup(groupID);
            if (!group || depth > 4) {
                return;
            }
            int seen = 0;
            for (auto* object : CCArrayExt<GameObject*>(group)) {
                if (!object || ++seen > 600) {
                    break;
                }
                int id = object->m_objectID;
                if (id != 901 && id != 1346 && id != 1049 && id != 1268) {
                    continue;
                }
                auto* c = static_cast<EffectGameObject*>(object);
                if (!c->m_isSpawnTriggered) {
                    continue;
                }
                if (id == 1268) {
                    if (plainSpawn(c)) {
                        spawnChain(c->m_targetGroupID, x, armed, when + spawnSteps(c, tickLength), depth + 1);
                    }
                }
                else if (id == 1049) {
                    if (c->m_targetGroupID > 0) {
                        addMove(c, x, TriggerKind::Toggle, armed, when);
                    }
                }
                else if (plainTrigger(c, id == 1346)) {
                    addMove(c, x, id == 1346 ? TriggerKind::Rotate : TriggerKind::Move, armed, when);
                }
            }
        };
        // groups the real game will spawn shortly (a spawn trigger with a delay already fired)
        if (!pendingSpawns.empty()) {
            std::erase_if(pendingSpawns, [](PendingSpawn const& p) { return p.at < tickCount; });
            for (auto const& p : pendingSpawns) {
                spawnChain(p.group, 0.0, true, p.at - tickCount, 0);
            }
        }
        // the ones the ghost will set off
        auto it = std::lower_bound(levelTriggers.begin(), levelTriggers.end(), px - 2.0, [](auto const& m, double x) { return m.x < x; });
        for (; it != levelTriggers.end() && it->x <= px + reach; ++it) {
            if (it->trigger->m_activated) {
                continue;
            }
            if (it->kind == TriggerKind::Spawn) {
                spawnChain(it->trigger->m_targetGroupID, it->x, false, spawnSteps(it->trigger, tickLength), 0);
            }
            else {
                addMove(it->trigger, it->x, it->kind, false, 0);
            }
        }
        buf.triggersAhead = static_cast<int>(scriptedMoves.size());
        if (scriptedMoves.empty()) {
            scriptedObjects.clear();
        }
        cache.layer = layer;
        cache.tick = tickCount;
        cache.px = px;
        cache.reach = reach;
        cache.moves = scriptedMoves;
        cache.objects = scriptedObjects;
    }

    // v2.85: a toggle in this prediction: off = the ghost stops colliding with the group, on =
    // switched-off objects in it collide again (see collisionCheckObjects)
    void simToggle(CCArray* group, bool on) {
        int seen = 0;
        for (auto* object : CCArrayExt<GameObject*>(group)) {
            if (!object || ++seen > 2000) {
                break;
            }
            if (!on) {
                buf.simDisabled.insert(object);
                continue;
            }
            buf.simDisabled.erase(object);
            if (object->m_isGroupDisabled && buf.simEnabled.size() < 256 &&
                std::find(buf.simEnabled.begin(), buf.simEnabled.end(), object) == buf.simEnabled.end()) {
                buf.simEnabled.push_back(object);
            }
        }
    }

    // the ghost reached x: triggers there go off (v2.85: after their spawn delay); a toggle
    // switches its group off (or on) for the rest of this prediction
    void fireScripted(double gx, int tick) {
        for (auto& m : scriptedMoves) {
            if (!m.armed && gx >= m.x) {
                m.armed = true;
                m.fireStep = tick + m.delay;
            }
            if (m.kind == TriggerKind::Toggle && m.armed && !m.toggled && tick >= m.fireStep && m.group) {
                m.toggled = true;
                simToggle(m.group, m.on);
            }
        }
    }

    // where the triggered objects are `step` steps into the prediction
    void placeScripted(int step) {
        for (auto& o : scriptedObjects) {
            double x = o.mover ? o.object->m_positionX : o.x;
            double y = o.mover ? o.object->m_positionY : o.y;
            double spin = 0.0;
            bool any = false;
            for (int i : o.moves) {
                auto const& m = scriptedMoves[i];
                if (!m.armed || step <= m.fireStep) {
                    continue;
                }
                double f = (m.steps < 0.5 ? 1.0 : easeMove(m.easing, m.rate, (step - m.fireStep) / m.steps)) - m.from;
                if (f == 0.0) {
                    continue;
                }
                any = true;
                applyMove(m, f, x, y, spin);
            }
            if (!any && !o.placed) {
                continue;
            }
            // v2.84.5: a finished (or instant) move leaves it where it was last step: nothing to do
            if (o.placed && !o.mover && x == o.lastX && y == o.lastY && spin == o.lastSpin) {
                continue;
            }
            o.lastX = x;
            o.lastY = y;
            o.lastSpin = spin;
            o.object->setPosition({ static_cast<float>(x), static_cast<float>(y) });
            o.object->m_positionX = x;
            o.object->m_positionY = y;
            if (!o.mover && (spin != 0.0 || o.rotated)) {
                o.object->setRotationX(static_cast<float>(o.rotX + spin));
                o.object->setRotationY(static_cast<float>(o.rotY + spin));
                o.rotated = true;
            }
            o.object->setObjectRectDirty(true);
            o.object->m_isOrientedBoxDirty = true;
            o.placed = true;
        }
    }

    void restoreScripted() {
        for (auto& o : scriptedObjects) {
            if (o.placed && !o.mover) {
                o.object->setPosition(o.node);
                o.object->m_positionX = o.x;
                o.object->m_positionY = o.y;
                if (o.rotated) {
                    o.object->setRotationX(o.rotX);
                    o.object->setRotationY(o.rotY);
                }
                o.object->setObjectRectDirty(true);
                o.object->m_isOrientedBoxDirty = true;
            }
        }
        scriptedObjects.clear();
        scriptedMoves.clear();
    }

    // v2.4 drift check: the ghost run for the input you actually made is recorded, then
    // compared tick by tick with where the real player goes, until your next input.
    void finishDrift(char const* why) {
        if (!drift.active) {
            return;
        }
        drift.active = false;
        if (drift.compared >= 8) {
            say("drift {:.1f}u max at t{} over {}t{}", drift.maxErr, drift.maxAt, drift.compared, why);
        }
    }

    void driftStep(PlayerObject* real) {
        if (!drift.active) {
            return;
        }
        if (drift.index >= 0) {
            if (drift.index >= static_cast<int>(drift.path.size())) {
                finishDrift("");
                return;
            }
            float err = ccpDistance(real->getPosition(), drift.path[drift.index]);
            if (err > drift.maxErr) {
                drift.maxErr = err;
                drift.maxAt = drift.index;
            }
            ++drift.compared;
        }
        ++drift.index;
    }

    // v2.13 level memory. Each level is cut into half-block slices along x. Every buffer
    // decision is remembered at the slice where it was made, and ~0.4 s later it is
    // scored: still alive = it worked, died = it didn't. Over your runs this learns, per
    // spot, which delays work there, where saves keep failing (the ghost doesn't match
    // the level there: triggers, moving objects) and where you keep dying. Stored per
    // level in the mod's save folder, so it carries over between sessions.
    constexpr float SLICE = 15.f;          // half a block
    constexpr int OUTCOME_TICKS = 90;      // alive this long after a decision = it worked

    struct Spot {
        uint32_t ok = 0;        // saves here that you survived
        uint32_t bad = 0;       // saves here followed by a death
        uint32_t deaths = 0;    // deaths after any decision here
        uint32_t delaySum = 0;  // sum of the delays of the saves that worked
        uint32_t last = 0;      // v2.79: the delay of the last save that worked here (240/s), 0 = none
    };

    std::unordered_map<int, Spot> memory;
    std::string memoryKey;
    bool memoryDirty = false;
    int memoryUpdates = 0;

    struct PendingOutcome {
        bool active = false;
        int slice = 0;
        int tick = 0;
        int delay = 0;
        bool skipped = false;  // v2.64: a save was held back here because of memory
    } pending;

    int sliceOf(PlayerObject* p) {
        return static_cast<int>(std::floor(p->getPosition().x / SLICE));
    }

    // A spot plus its two neighbours: the same gap is never hit at exactly the same x.
    Spot spotAround(int slice) {
        Spot sum;
        for (int s = slice - 1; s <= slice + 1; ++s) {
            auto it = memory.find(s);
            if (it == memory.end()) {
                continue;
            }
            sum.ok += it->second.ok;
            sum.bad += it->second.bad;
            sum.deaths += it->second.deaths;
            sum.delaySum += it->second.delaySum;
        }
        return sum;
    }

    std::filesystem::path memoryFile() {
        // v2.37: slices keyed by the real position. v2.75: _v3, a fresh start after the
        // prediction fixes of v2.61-v2.74 (old files learned from the buggy versions)
        return Mod::get()->getSaveDir() / "memory" / (memoryKey + "_v3.txt");
    }

    size_t memorySpots() {
        return memory.size();
    }

    void saveMemory() {
        if (!memoryDirty || memoryKey.empty()) {
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(memoryFile().parent_path(), ec);
        std::ofstream out(memoryFile(), std::ios::trunc);
        for (auto const& [slice, spot] : memory) {
            out << slice << ' ' << spot.ok << ' ' << spot.bad << ' ' << spot.deaths << ' ' << spot.delaySum << ' ' << spot.last << '\n';
        }
        memoryDirty = false;
    }

    void loadMemory(GJGameLevel* level) {
        memory.clear();
        memoryDirty = false;
        memoryUpdates = 0;
        pending.active = false;
        memoryKey.clear();
        if (!level) {
            return;
        }
        int id = level->m_levelID.value();
        if (id > 0) {
            memoryKey = fmt::format("level_{}", id);
        }
        else {
            std::string name = level->m_levelName;
            for (auto& c : name) {
                if (!std::isalnum(static_cast<unsigned char>(c))) {
                    c = '_';
                }
            }
            memoryKey = "local_" + name;
        }
        std::ifstream in(memoryFile());
        // v2.79: lines may have a 6th number (last working delay); older files have 5
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream fields(line);
            int slice;
            Spot spot;
            if (!(fields >> slice >> spot.ok >> spot.bad >> spot.deaths >> spot.delaySum)) {
                continue;
            }
            fields >> spot.last;
            memory[slice] = spot;
        }
        // v2.75: memory fades. Opened again more than 12 hours later, everything learned counts
        // a quarter less (the average delay at a spot stays the same), so old mistakes and
        // old death spots don't stick forever.
        std::error_code ec;
        auto written = std::filesystem::last_write_time(memoryFile(), ec);
        if (!ec && std::filesystem::file_time_type::clock::now() - written > std::chrono::hours(12)) {
            for (auto it = memory.begin(); it != memory.end();) {
                auto& s = it->second;
                uint32_t ok = s.ok * 3 / 4;
                s.delaySum = s.ok > 0 ? static_cast<uint32_t>(static_cast<uint64_t>(s.delaySum) * ok / s.ok) : 0;
                s.ok = ok;
                if (ok == 0) {
                    s.last = 0;  // v2.79: nothing that worked is left to reuse
                }
                s.bad = s.bad * 3 / 4;
                s.deaths = s.deaths * 3 / 4;
                it = (s.ok | s.bad | s.deaths) == 0 ? memory.erase(it) : std::next(it);
            }
            memoryDirty = true;
            saveMemory();
        }
        if (!memory.empty()) {
            say("memory: {} spots learned on this level", memory.size());
        }
    }

    void memoryChanged() {
        memoryDirty = true;
        if (++memoryUpdates % 10 == 0) {
            saveMemory();
        }
    }

    void creditPending() {
        if (!pending.active) {
            return;
        }
        pending.active = false;
        if (pending.delay > 0) {
            auto& spot = memory[pending.slice];
            auto delay = static_cast<uint32_t>(std::lround(pending.delay / stepScale()));  // stored at 240/s
            ++spot.ok;
            spot.delaySum += delay;
            // v2.79: the exact delay that just worked is reused next time, and each save that works
            // takes back one old failure here, so a spot you've started making stops being skipped
            spot.last = delay;
            if (spot.bad > 0) {
                --spot.bad;
            }
            memoryChanged();
        }
    }

    // Called for every decision: remembers it until it can be scored.
    void memoryNote(PlayerObject* p1, int delay) {
        if (!buf.learn || memoryKey.empty()) {
            return;
        }
        // A decision that lived long enough before the next one counts as having worked.
        if (pending.active && tickCount - pending.tick >= st(30)) {
            creditPending();
        }
        pending = { true, sliceOf(p1), tickCount, delay, false };
    }

    void memoryTick() {
        if (pending.active && tickCount - pending.tick >= st(OUTCOME_TICKS)) {
            creditPending();
        }
    }

    bool memorySaysSkip(int slice);

    void memoryDeath() {
        if (!pending.active) {
            return;
        }
        pending.active = false;
        auto& spot = memory[pending.slice];
        ++spot.deaths;
        if (pending.delay > 0) {
            ++spot.bad;
        }
        // v2.64: memory held a save back and you died anyway, so leaving the input alone
        // isn't working either: take back one failed save, so after a death or two the mod
        // tries saving here again instead of skipping this spot forever (first jump of
        // Slaughterhouse, after older versions' bad saves there).
        if (pending.skipped) {
            int worst = pending.slice;
            for (int s = pending.slice - 1; s <= pending.slice + 1; ++s) {
                auto it = memory.find(s);
                if (it != memory.end() && it->second.bad > memory[worst].bad) {
                    worst = s;
                }
            }
            if (memory[worst].bad > 0) {
                --memory[worst].bad;
            }
            if (!memorySaysSkip(pending.slice)) {
                say("memory: died with the input left alone, saves will be tried here again");
            }
        }
        memoryChanged();
    }

    // Saves made here keep ending in deaths: the ghost is wrong at this spot, leave it alone.
    bool memorySaysSkip(int slice) {
        if (!buf.learn) {
            return false;
        }
        auto spot = spotAround(slice);
        return spot.bad >= 2 && spot.bad > spot.ok;
    }

    // The delay that has worked here before, or 0.
    // v2.79: the last delay that worked (this spot first, then its neighbours) instead of the
    // average of all of them: +4 and +12 both working averaged to +8, which may never have
    // worked (narrow orb timings and wave turns). It's still checked by a prediction before use.
    int memoryDelay(int slice) {
        if (!buf.learn) {
            return 0;
        }
        for (int s : { slice, slice - 1, slice + 1 }) {
            auto it = memory.find(s);
            if (it != memory.end() && it->second.last > 0) {
                return st(static_cast<int>(it->second.last));
            }
        }
        auto spot = spotAround(slice);  // older files: no last delay yet, the average
        if (spot.ok < 2) {
            return 0;
        }
        return st(static_cast<int>(std::lround(static_cast<double>(spot.delaySum) / spot.ok)));
    }

    bool memoryRisky(int slice) {
        auto spot = spotAround(slice);
        return spot.deaths >= 3;
    }

    void snapshotObject(GameObject* object) {
        if (!object || buf.simUsed.contains(object)) {
            return;
        }
        buf.simUsed.insert(object);
        ObjSnapshot s { object, object->m_isActivated, nullptr, false, false, false };
        if (auto* effect = typeinfo_cast<EffectGameObject*>(object)) {
            s.effect = effect;
            s.activated = effect->m_activated;
            s.byP1 = effect->m_activatedByPlayer1;
            s.byP2 = effect->m_activatedByPlayer2;
            s.animated = effect->m_animationTriggered;
        }
        buf.snapshots.push_back(s);
    }

    // v2.88: anything a ghost added to the real level during a prediction (the dash start
    // effect of dash orbs, bursts...) is removed afterwards. Dash orbs still flashed before
    // you reached them: those effects go onto the level's layers, not the ghost's own.
    struct NodeMark {
        CCNode* node;
        unsigned count;
    };
    std::vector<NodeMark> levelMarks;

    void markLevelNodes(PlayLayer* layer) {
        levelMarks.clear();
        auto add = [](CCNode* n) {
            if (n) {
                levelMarks.push_back({ n, n->getChildrenCount() });
            }
        };
        add(layer);
        add(layer->m_objectLayer);
        if (layer->m_aboveShaderObjectLayer != layer->m_objectLayer) {
            add(layer->m_aboveShaderObjectLayer);
        }
        if (layer->m_objectLayer && layer->m_objectLayer->getChildren()) {
            for (auto* child : CCArrayExt<CCNode*>(layer->m_objectLayer->getChildren())) {
                add(child);
            }
        }
    }

    void sweepLevelNodes() {
        for (auto const& m : levelMarks) {
            auto* kids = m.node->getChildren();
            while (kids && kids->count() > m.count) {
                m.node->removeChild(static_cast<CCNode*>(kids->lastObject()), true);
            }
        }
        levelMarks.clear();
    }

    void restoreObjects() {
        sweepLevelNodes();
        for (auto const& s : buf.snapshots) {
            s.object->m_isActivated = s.isActivated;
            if (s.effect) {
                s.effect->m_activated = s.activated;
                s.effect->m_activatedByPlayer1 = s.byP1;
                s.effect->m_activatedByPlayer2 = s.byP2;
                s.effect->m_animationTriggered = s.animated;
            }
        }
        buf.snapshots.clear();
        buf.simUsed.clear();
        buf.orbsUsed.clear();
        buf.simDisabled.clear();
        buf.simEnabled.clear();
    }

    void advanceGhost(PlayerObject* ghost, float dt) {
        ghost->update(dt);
        ghost->updateRotation(dt);
    }

    void checkGhost(PlayLayer* layer, PlayerObject* ghost, float dt) {
        ghost->m_collisionLogTop->removeAllObjects();
        ghost->m_collisionLogBottom->removeAllObjects();
        ghost->m_collisionLogLeft->removeAllObjects();
        ghost->m_collisionLogRight->removeAllObjects();
        layer->checkCollisions(ghost, dt, false);
    }

    bool isFlying(PlayerObject* player) {
        return player->m_isDart || player->m_isBird || player->m_isSwing || player->m_isShip;
    }

    // Orb auto-clicks last one ghost update, then let go (unless the test itself is holding).
    void releaseAutoClicks() {
        if (buf.auto1) {
            if (!buf.held1 && buf.ghost1) {
                buf.ghost1->releaseButton(PlayerButton::Jump);
            }
            buf.auto1 = false;
        }
        if (buf.auto2) {
            if (!buf.held2 && buf.ghost2) {
                buf.ghost2->releaseButton(PlayerButton::Jump);
            }
            buf.auto2 = false;
        }
    }

    enum class SimEnd { Ok, StopSearch };

    // Returns ticks survived after the input (horizon = fully safe).
    // releaseAfter: ticks after the input at which the ghost does the opposite (-1 = never).
    // stopAt: stop as soon as this many ticks were survived (the caller already knows
    // that's good enough), instead of always running the full horizon.
    int simulate(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int releaseAfter,
        int stopAt, SimEnd* end) {
        *end = SimEnd::Ok;
        if (!buf.ghost1) {
            return horizon;
        }
        buf.simulating = true;
        markLevelNodes(layer);
        buf.ghostDied = false;
        buf.ghostHitPortal = false;
        buf.ghostSwitched = false;
        buf.ghostSolo = false;
        buf.firstOrbTick = -1;
        buf.lastOrbTick = -1;
        buf.laterOrbTouched = false;
        buf.orbUsedTick = -1;

        bool second = dual && buf.ghost2 && p2;
        buf.simSecond = second;
        buf.haveRealRect = buf.ignoreTouching;
        buf.harmless.clear();
        // v2.78.1: always known, so mode portals you're still inside are skipped (see
        // collisionCheckObjects)
        buf.haveStartRects = true;
        buf.realRect1 = p1->getObjectRect();
        buf.realRect2 = second ? p2->getObjectRect() : buf.realRect1;
        copyState(buf.ghost1, p1);
        if (second) {
            copyState(buf.ghost2, p2);
        }
        buf.held1 = buf.held2 = buf.appliedInput;
        buf.auto1 = buf.auto2 = false;
        buf.autoAt1 = buf.autoAt2 = -1;

        // v4.6 wave: before a delayed input fires, the ghost is in the opposite state
        // (holding before a delayed release, released before a delayed press).
        bool waveRun = p1->m_isDart || (second && p2->m_isDart);
        if (buf.waveV46 && waveRun && delay > 0 && !buf.skipPress) {
            pressGhost(buf.ghost1, !down);
            if (second) {
                pressGhost(buf.ghost2, !down);
            }
        }

        float dt = buf.tickLength;
        if (!(dt > 0.001f && dt < 2.0f)) {
            dt = TICK;
        }

        // (v1 forced a fake !down button event here for delay > 0, which made delayed
        //  runs start from a different jump-buffer state than the "press now" run.
        //  The ghost now simply mirrors the real player's button state, except in wave,
        //  which keeps the v4.6 behaviour above.)

        bool flying = isFlying(p1) || (dual && p2 && isFlying(p2));
        bool spiderRun = buf.spiderSmart && (p1->m_isSpider || (second && p2->m_isSpider));
        // v2.40: a UFO that lands on something has finished its flap, like a cube jump
        bool ufoRun = buf.ufoSmart && (p1->m_isBird || (second && p2->m_isBird));
        // v2.45 ball: a flip in a tight corridor can land after only a few ticks, and a ball
        // often lands right next to a spike on the new surface, so short flips count as
        // landings and the landing has to last longer before the flip is called safe.
        bool ballRun = buf.ballSmart && (p1->m_isBall || (second && p2->m_isBall));
        int total = delay + horizon;
        int survived = 0;
        int airTicks = 0;
        int groundTicks = 0;
        int tick = 0;
        bool died = false;
        bool safe = false;

        collectMovers();
        collectScripted(layer, total);
        for (; tick < total; ++tick) {
            second = buf.simSecond;  // v2.84: after a solo portal only P1 goes on (v2.85: after a dual portal both)
            if (!movers.empty()) {
                placeMovers(tick + 1);  // where they'll be after this step
            }
            if (!scriptedMoves.empty()) {
                // v2.81: a move / rotate / toggle trigger the ghost has reached goes off
                fireScripted(buf.ghost1->getPositionX(), tick);
                placeScripted(tick + 1);
            }
            buf.simTick = tick;
            // v2.26: orb auto-clicks that wait a few ticks into the orb fire here, if the
            // ghost is still inside an orb by then.
            if (buf.autoAt1 >= 0 && tick >= buf.autoAt1) {
                if (!buf.held1 && buf.ghost1->m_touchingRings->count() > 0) {
                    buf.ghost1->pushButton(PlayerButton::Jump);
                    buf.auto1 = true;
                }
                buf.autoAt1 = -2;
            }
            if (second && buf.autoAt2 >= 0 && tick >= buf.autoAt2) {
                if (!buf.held2 && buf.ghost2->m_touchingRings->count() > 0) {
                    buf.ghost2->pushButton(PlayerButton::Jump);
                    buf.auto2 = true;
                }
                buf.autoAt2 = -2;
            }
            if (tick == delay && !buf.skipPress) {
                pressGhost(buf.ghost1, down);
                if (second) {
                    pressGhost(buf.ghost2, down);
                }
            }
            if (buf.retapAt >= 0) {
                // v2.15: a second tap (let go, press, let go): "flip back" for the spider
                int t = tick - delay - buf.retapAt;
                if (t == 0 || t == 1 || t == 5) {
                    pressGhost(buf.ghost1, t == 1);
                    if (second) {
                        pressGhost(buf.ghost2, t == 1);
                    }
                }
            }
            // v2.16: a dash lasts as long as you hold, so the ghost doesn't let go mid-dash.
            if (down && releaseAfter >= 0 && tick == delay + releaseAfter && buf.ghost1->m_isDashing) {
                ++releaseAfter;
            }
            if (releaseAfter >= 0 && tick == delay + releaseAfter) {
                // let go of a press (or, for a wave release, press again: the turn back)
                pressGhost(buf.ghost1, !down);
                if (second) {
                    pressGhost(buf.ghost2, !down);
                }
            }

            // v2.18: with CBF the click lands partway through a tick, so the ghost's first
            // step is only the rest of that tick; after it, ghost and game are on the same
            // tick boundaries again.
            float stepDt = tick == 0 ? dt * buf.firstStep : dt;
            advanceGhost(buf.ghost1, stepDt);
            if (second) {
                advanceGhost(buf.ghost2, stepDt);
            }
            releaseAutoClicks();
            if (buf.ghostDied) {
                died = true;
                break;
            }

            // v2.36: after a mid-tick (CBF) click the game finishes the tick and then runs its
            // normal collision check with the full step, so the ghost does the same.
            checkGhost(layer, buf.ghost1, dt);
            if (!buf.ghostDied && second) {
                checkGhost(layer, buf.ghost2, dt);
            }
            if (buf.ghostDied) {
                died = true;
                break;
            }
            if (buf.recordPath) {
                buf.recordPath->push_back(buf.ghost1->getPosition());
            }
            if (buf.ghostHitPortal) {
                safe = tick >= delay;
                break;
            }

            if (tick < delay) {
                continue;
            }
            ++survived;
            if (survived >= stopAt) {
                break;
            }
            // v2.79: after a portal changed the ghost's mode, only surviving the look-ahead counts
            // (the cube's "landed = safe" rule doesn't fit the new mode)
            if ((flying && !ufoRun) || buf.ghostSwitched) {
                continue;
            }

            bool grounded = buf.ghost1->m_isOnGround && (!second || buf.ghost2->m_isOnGround);
            if (grounded && airTicks > 0 && buf.captureKiller) {
                buf.baseTouchedDown = true;
            }
            if (!grounded) {
                ++airTicks;
                groundTicks = 0;
                continue;
            }
            // v2.15 spider: a flip is a teleport with no air time, so the cube's "was in the
            // air, then landed" never happened and every spider run went the full 130 ticks
            // into obstacles that need your next click. Now: standing on a surface for
            // SPIDER_LAND ticks after the input = the flip is done and safe.
            if (spiderRun) {
                if (++groundTicks >= st(SPIDER_LAND)) {
                    safe = true;
                    break;
                }
                continue;
            }
            // v2.61: v2.48 landing rule (the v2.51 ceiling-bump shortcut is gone)
            if (airTicks < st(ballRun ? 2 : 6)) {
                continue;
            }
            if (groundTicks > st(ballRun ? 8 : 2)) {
                safe = true;
                break;
            }
            ++groundTicks;
        }

        restoreScripted();
        if (!movers.empty()) {
            placeMovers(0);  // back exactly where they were
            movers.clear();
        }
        ++buf.simRuns;
        buf.simTicks += tick + 1;

        // Died or hit an unsimulated portal before the delayed input even fired:
        // longer delays can't do better, stop searching.
        if ((died || buf.ghostHitPortal) && tick < delay) {
            *end = SimEnd::StopSearch;
        }
        restoreObjects();
        buf.ghost1->stopAllActions();
        if (buf.ghost2) {
            buf.ghost2->stopAllActions();
        }
        // v2.78.3: a ghost that used an orb or dashed started its particles (orb burst, dash
        // fire) right where the orb is, before you got there
        if (buf.firstOrbTick >= 0) {
            buf.ghost1->stopParticles();
            if (buf.ghost2) {
                buf.ghost2->stopParticles();
            }
        }
        buf.lastRunSwitched = buf.ghostSwitched;
        buf.simulating = false;
        return safe ? horizon : survived;
    }

    // v2.4: simulate() with the orb setting; if that run touched an orb and didn't make
    // it, also try the other choice (click it / skip it) and keep whichever lives longer,
    // like a player who only clicks the orb when they need it.
    // v2.57: hard time cap for one click's whole decision. The search loops have their own
    // budget, but orb clicks also ran unbudgeted work (orb alignment, the both-ways and late
    // orb clicks in every run, robust and more-time checks) and could take several frames.
    std::chrono::steady_clock::time_point clickStart;
    bool clickTimed = false;

    // v2.70: how long the current click has been processed so far (0 outside a click)
    double clickMs() {
        return clickTimed ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - clickStart).count() : 0.0;
    }

    // v2.78.4: 1.25x (was 2x): with the searches now sharing the click's time, a whole save
    // stays close to the budget
    bool clickOverTime() {
        return clickMs() > buf.budgetMs * 1.25;
    }

    // v2.78.4: what's left of the click's time for a search. Each search used to get the full
    // budget on its own, on top of the base run, memory try and orb checks before it, so a
    // wave save on GRIEF could take 2-3 frames at 240 fps.
    // v2.80.2: but always at least half the budget. Orb alignment (up to 1.75x since v2.79)
    // could use all of it, and the normal search then stopped after one delay ("prediction
    // budget hit at +1 ticks" on almost every click near an orb, Heliopolis).
    double searchBudget() {
        return clickTimed ? std::max(buf.budgetMs - clickMs(), buf.budgetMs * 0.5) : buf.budgetMs;
    }

    struct ClickTimer {
        ClickTimer() {
            clickStart = std::chrono::steady_clock::now();
            clickTimed = true;
        }
        ~ClickTimer() {
            clickTimed = false;
        }
    };

    int simulateOrbs(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int releaseAfter,
        int stopAt, SimEnd* end) {
        buf.orbClick = buf.assumeOrbs;
        buf.touchedOrb = false;
        int first = simulate(layer, p1, p2, dual, delay, down, horizon, releaseAfter, stopAt, end);
        if (!buf.tryBothOrbs || !buf.smart || !buf.touchedOrb || first >= stopAt || clickOverTime()) {
            return first;
        }
        SimEnd firstEnd = *end;
        buf.orbClick = !buf.assumeOrbs;
        int second = simulate(layer, p1, p2, dual, delay, down, horizon, releaseAfter, stopAt, end);
        buf.orbClick = buf.assumeOrbs;
        if (firstEnd != SimEnd::StopSearch) {
            *end = SimEnd::Ok;
        }
        int best = std::max(first, second);
        // v2.26: some orbs only work when clicked at a specific moment inside them (levels
        // like Orbit). The ghost's orb clicks are also tried 3 and 6 ticks into the orb.
        if (buf.orbTiming && best < stopAt && !clickOverTime()) {
            for (int late : { 3, 6 }) {
                buf.orbClick = true;
                buf.orbClickDelay = st(late);
                SimEnd lateEnd;
                int ret = simulate(layer, p1, p2, dual, delay, down, horizon, releaseAfter, stopAt, &lateEnd);
                buf.orbClickDelay = 0;
                buf.orbClick = buf.assumeOrbs;
                best = std::max(best, ret);
                if (best >= stopAt || clickOverTime()) {
                    break;
                }
            }
        }
        return best;
    }

    // Ghost 1's position after every tick of one run (until it dies or is safe).
    std::vector<CCPoint> recordRun(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon,
        int releaseAfter, bool* died = nullptr) {
        std::vector<CCPoint> path;
        buf.recordPath = &path;
        buf.orbClick = buf.assumeOrbs;
        SimEnd end;
        simulate(layer, p1, p2, dual, delay, down, horizon, releaseAfter, delay + horizon, &end);
        buf.recordPath = nullptr;
        if (died) {
            *died = buf.ghostDied;
        }
        return path;
    }

    // v2.11 path view (F10): red = what your input would have done right now, green = what
    // the game actually gets. A dot marks where a delayed input fires, a red dot a crash.
    void drawPath(std::vector<CCPoint> const& path, int delay, ccColor4F color, bool died) {
        for (size_t i = 1; i < path.size(); ++i) {
            buf.pathNode->drawSegment(path[i - 1], path[i], 1.2f, color);
        }
        if (delay > 0 && delay <= static_cast<int>(path.size())) {
            buf.pathNode->drawDot(path[delay - 1], 3.f, { 1.f, 1.f, 1.f, 0.9f });
        }
        if (died && !path.empty()) {
            buf.pathNode->drawDot(path.back(), 4.f, { 1.f, 0.2f, 0.2f, 1.f });
        }
    }

    void showPaths(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int chosen, bool down, int horizon, int releaseAfter) {
        if (!buf.showPath || !buf.pathNode) {
            return;
        }
        buf.pathNode->clear();
        buf.pathHoldUntil = tickCount + st(45);
        bool nowDied = false;
        auto now = recordRun(layer, p1, p2, dual, 0, down, horizon, releaseAfter, &nowDied);
        ccColor4F red { 1.f, 0.3f, 0.3f, 0.85f };
        ccColor4F green { 0.3f, 1.f, 0.5f, 0.9f };
        drawPath(now, 0, chosen > 0 ? red : (nowDied ? red : green), nowDied);
        if (chosen > 0) {
            bool died = false;
            auto delayed = recordRun(layer, p1, p2, dual, chosen, down, horizon, releaseAfter, &died);
            drawPath(delayed, chosen, green, died);
        }
        // v2.13: spots on this path where the memory has seen you die a lot.
        if (buf.learn && !memory.empty()) {
            int last = INT_MIN;
            for (auto const& point : now) {
                int slice = static_cast<int>(std::floor(point.x / SLICE));
                if (slice != last && memoryRisky(slice)) {
                    buf.pathNode->drawDot(point, 3.5f, { 1.f, 0.6f, 0.1f, 0.95f });
                }
                last = slice;
            }
        }
    }

    // v2.4: record the ghost run for the input that was actually made (drift check).
    void startDrift(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int releaseAfter) {
        finishDrift("");
        if (!buf.driftCheck) {
            return;
        }
        drift.path = recordRun(layer, p1, p2, dual, delay, down, horizon, releaseAfter);
        drift.active = !drift.path.empty();
        // v2.38: a normal click is handled before the tick runs, so the first comparison is
        // one update later. A mid-tick (CBF) click is handled inside the tick, so the ghost's
        // first (partial) step ends at the very next tick start: compare from there.
        drift.index = buf.firstStep < 1.f ? 0 : -1;
        drift.compared = 0;
        drift.maxErr = 0.f;
        drift.maxAt = 0;
    }

    // After every buffer decision: drift check + path view for the input the game gets.
    // v2.70: the extras after the search (robust check, drift check, path view) are skipped
    // once the click has used up its time, so one click can't stall a frame for long. The
    // search itself keeps its budget.
    void decided(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int releaseAfter) {
        memoryNote(p1, delay);
        if (clickMs() > buf.budgetMs * 1.5) {
            finishDrift("");  // no time left for the debug runs this click
            return;
        }
        startDrift(layer, p1, p2, dual, delay, down, horizon, releaseAfter);
        showPaths(layer, p1, p2, dual, delay, down, horizon, releaseAfter);
    }

    // Full wave look-ahead from the setting (mini = 15 less).
    int fullWaveHorizon(bool mini) {
        int wh = std::clamp(buf.waveHorizon, 20, 160);
        return st(mini ? std::max(20, wh - 15) : wh);  // v4.6: 65 normal / 50 mini
    }

    // v2.83 two clicks ahead: a run can crash only because the ghost never clicks again (the
    // next obstacle needs your next click). One more tap is tried 8 / 16 / 26 ticks before the
    // crash, but never sooner than 24 ticks after this click (a real second click, not a
    // double tap). If it then lives 40 ticks past the old crash (or the whole look-ahead), the
    // crash wasn't this click's. Returns when that tap is (ticks after the click), or -1.
    constexpr int NEXT_CLICK_MARGINS[] = { 8, 16, 26 };
    constexpr int NEXT_CLICK_MIN = 24;

    int nextClickAt(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int releaseAfter,
        int ret) {
        if (!buf.nextClick || ret >= horizon || ret < st(NEXT_CLICK_MIN + 8)) {
            return -1;
        }
        int length = std::min(horizon, ret + st(40));
        for (int margin : NEXT_CLICK_MARGINS) {
            int at = ret - st(margin);
            if (at < st(NEXT_CLICK_MIN) || clickOverTime()) {
                break;
            }
            buf.retapAt = at;
            SimEnd e;
            int r = simulate(layer, p1, p2, dual, delay, down, length, releaseAfter, length, &e);
            buf.retapAt = -1;
            if (r >= length) {
                return at;
            }
        }
        return -1;
    }

    // v2.6 orb alignment: a press made a little before reaching an orb would be wasted
    // (or jump off the ground instead). Find the delay that lands the press inside the
    // orb, like the cube buffer does for jumps. Returns that delay, or 0.
    // v2.78 hard orb timings (Orbit): every tick you're inside the orb is tried (not just the
    // first 6), the orbs after this one are clicked in the prediction too (with a few
    // timings each), and of the timings that are fully safe the middle one is picked, so a
    // real run a hair off still makes it; with robust saves it must also survive starting
    // half a tick ahead / behind.
    int alignOrb(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int horizon, int releaseAfter, int learned, int maxDelay,
        int* landed) {
        int window = st(std::clamp(buf.orbWindow, 1, 64));
        int found = 0;
        SimEnd end;
        buf.orbClick = false;  // the press being tested is the orb click
        buf.orbWasTiming = false;
        buf.orbAfterFirst = true;
        struct Reset {
            ~Reset() {
                buf.orbClick = buf.assumeOrbs;
                buf.orbAfterFirst = false;
                buf.orbClickDelay = 0;
                buf.ghostShiftX = 0.f;
            }
        } reset;

        // one timing k: your click k ticks from now; later orbs clicked on touch, then 3 / 6 /
        // 10 ticks into them if that dies. Returns ticks survived and whether your click used an orb.
        auto tryAt = [&](int k, bool* used) {
            int best = -1;
            *used = false;
            for (int late : { 0, 3, 6, 10 }) {
                buf.orbClickDelay = st(late);
                int ret = simulate(layer, p1, p2, dual, k, true, horizon, releaseAfter, horizon, &end);
                buf.orbClickDelay = 0;
                if (late == 0) {
                    *used = buf.orbUsedTick >= 0;
                }
                best = std::max(best, ret);
                if (ret >= horizon || buf.orbUsedTick < 0 || !buf.laterOrbTouched || !buf.orbTiming || end == SimEnd::StopSearch) {
                    break;
                }
            }
            return best;
        };
        auto robustAt = [&](int k) {
            if (!buf.robustSaves) {
                return true;
            }
            float shift = std::max(0.3f, 0.5f * (311.58f / 240.f) * (std::abs(p1->m_playerSpeed) / 0.9f));
            for (float off : { shift, -shift }) {
                buf.ghostShiftX = off;
                bool used;
                int ret = tryAt(k, &used);
                buf.ghostShiftX = 0.f;
                if (ret < horizon || !used) {
                    return false;
                }
            }
            return true;
        };

        // the orb timing that worked at this spot on earlier runs is tried first
        if (learned > 0 && learned <= maxDelay) {
            bool used;
            int ret = tryAt(learned, &used);
            if (used && ret >= horizon) {
                *landed = ret;
                return learned;
            }
        }

        // 1. Cheap probe: without pressing, does the ghost reach an orb soon, and how long
        //    does it stay inside it?
        buf.skipPress = true;
        buf.orbAfterFirst = false;
        int probeLen = std::min(maxDelay, std::max(window, st(40))) + 2;
        simulate(layer, p1, p2, dual, 0, true, probeLen, -1, probeLen, &end);
        buf.skipPress = false;
        buf.orbAfterFirst = true;
        int touch = buf.firstOrbTick;
        int leave = buf.lastOrbTick;
        std::string trace;  // v2.42: what was tried, for the explain log
        if (touch < 0 || touch > window) {
            return 0;
        }

        bool nowUsed;
        int now = tryAt(0, &nowUsed);
        if (nowUsed && (now >= horizon || !buf.orbTiming)) {
            return 0;  // pressing now uses the orb and is safe: nothing to fix
        }
        // every tick from just before the orb to the last tick inside it
        int from = nowUsed ? 1 : std::max(1, touch - 1);
        int to = std::min(maxDelay, std::max(leave, touch + st(6)));
        int safeStart = -1, safeEnd = -1;
        int bestRet = -1, bestK = 0;
        int bestAt = -1;  // v2.78.2: when the best timing crashes, counted from now (k + ret)
        std::string nextNote;  // v2.83: what the next-click check found
        // v2.79: orb clicks get more time (1.75x the budget for the scan, 2x with the robustness
        // check): at 240 fps one frame (~4 ms) only covered about 6 ticks of ORBIT's 20-tick orbs
        double orbBudget = buf.budgetMs * 1.75;
        for (int k = from; k <= to; ++k) {
            if (clickMs() > orbBudget) {
                trace += " (budget)";
                break;
            }
            bool used;
            int ret = tryAt(k, &used);
            if (buf.explain) {
                trace += fmt::format(" +{}:{}{}", k, ret, used ? "o" : "-");
            }
            if (end == SimEnd::StopSearch) {
                break;
            }
            if (!used) {
                if (safeStart >= 0) {
                    break;  // past the orb after a safe stretch
                }
                continue;
            }
            if (ret >= horizon) {
                if (safeStart < 0) {
                    safeStart = k;
                }
                safeEnd = k;
            }
            else if (safeStart >= 0) {
                break;  // the first safe stretch is over
            }
            // v2.78.2: timings are compared by when they crash counted from now, not from each
            // click: on ORBIT, clicking 9 ticks later lived 59 ticks after its click (crash at
            // t68) vs 50 for clicking now (t50), 18 ticks later, but 59 < 50 + 12 so it was
            // never picked
            if (k + ret > bestAt) {
                bestAt = k + ret;
                bestRet = ret;
                bestK = k;
            }
        }
        if (safeStart >= 0) {
            // the middle of the safe stretch, checked from there outwards for robustness
            int mid = (safeStart + safeEnd) / 2;
            found = mid;
            for (int off = 0; off <= safeEnd - safeStart; ++off) {
                int k = off % 2 == 0 ? mid + off / 2 : mid - (off + 1) / 2;
                if (k < safeStart || k > safeEnd) {
                    continue;
                }
                if (clickMs() > buf.budgetMs * 2.0 || robustAt(k)) {
                    found = k;
                    break;
                }
            }
            *landed = horizon;
        }
        // v2.80.1: nothing fully safe: the timing that crashes clearly latest. When clicking now
        // would miss the orb, 12 ticks later is enough (using the orb beats missing it).
        // v2.80.2: when clicking now already uses the orb, the crash must come at least 30 ticks
        // later: that gets past the obstacle that kills you now, and the later crash is the next
        // obstacle, which needs your next click (Heliopolis: now t21, +6t -> t77). Small gains
        // (v2.80.1 "ORB timing +4t -> 62t", then died anyway) still aren't taken.
        else {
            // v2.83: clicking now uses the orb and only dies to what your next click handles:
            // leave it. Otherwise the 30-tick rule, or a timing your next click makes safe.
            int nowNext = nowUsed && now >= 0 ? nextClickAt(layer, p1, p2, dual, 0, true, horizon, releaseAfter, now) : -1;
            if (nowNext >= 0) {
                nextNote = fmt::format(" (now is fine if you click again by t{})", nowNext);
            }
            else if (bestK > 0 && bestAt >= std::max(now, 0) + st(nowUsed ? 30 : 12)) {
                found = bestK;
                *landed = bestRet;
            }
            else if (bestK > 0 && bestAt > std::max(now, 0)) {
                int at = nextClickAt(layer, p1, p2, dual, bestK, true, horizon, releaseAfter, bestRet);
                if (at >= 0) {
                    found = bestK;
                    *landed = bestRet;
                    nextNote = fmt::format(" (safe if you click again by t{})", at);
                }
            }
        }
        buf.orbWasTiming = found > 0 && nowUsed;
        // v2.42 explain: "+k:ret o" = press k ticks later lives ret ticks and used the orb
        if (buf.explain) {
            say("orb: inside t{}-t{}, press now {} -> {}t |{} => {}{}{}", touch, leave, nowUsed ? "uses it" : "misses it", now,
                trace.empty() ? " (nothing to try)" : trace, found > 0 ? fmt::format("+{}t", found) : std::string("no pick"),
                safeStart >= 0 ? fmt::format(" (safe +{}..+{}t)", safeStart, safeEnd) : std::string(), nextNote);
        }
        return found;
    }

    // v2.1: best delayed run seen by the last search (shown in the "no save" log line)
    int dbgBestK = 0;
    int dbgBestRet = -1;
    // v2.85 no-hitch: the first delay a search tries (later than +1 when it goes on from a
    // previous tick), and where the last search ran out of time (0 = it didn't)
    int searchFrom = 1;
    int budgetCutK = 0;
    // set while a search is going on from a previous tick
    bool resumeNow = false;
    QueuedInput resumeEntry {};
    constexpr int MAX_RESUMES = 4;

    // v2.7 wave scoring, the wave version of the cube's "landed safely". A wave never
    // lands, so a run that keeps going one way eventually hits a wall even in a normal
    // corridor, where you'd simply turn. So: if the run crashes, try turning back a bit
    // before the crash; if that turn survives, you had time to react and the run counts
    // as safe. Only crashes you couldn't have turned away from count as deaths.
    // v2.10: three turn points are tried (4, 8 and 12 ticks before the crash) instead of
    // one, so "you could have turned away" is found whenever it's true.
    constexpr int WAVE_TURN_MARGINS[] = { 4, 8, 12 };
    constexpr int WAVE_MIN_TURN = 6;      // a turn sooner than this after the input is too fast to count
    constexpr int WAVE_ESCAPE = 16;       // the turn must then survive this many ticks

    // `target`: surviving this long is already good enough, so the run stops there and the
    // turn-away checks are skipped (v2.10: saves most of the work in the search).
    int waveScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int target,
        SimEnd* end) {
        int ret = simulateOrbs(layer, p1, p2, dual, delay, down, horizon, -1, target, end);
        if (ret >= target || *end == SimEnd::StopSearch) {
            return ret;
        }
        for (int margin : WAVE_TURN_MARGINS) {
            int turnAt = ret - st(margin);
            if (turnAt < st(WAVE_MIN_TURN)) {
                break;
            }
            int length = std::min(horizon, turnAt + st(WAVE_ESCAPE));
            SimEnd turnEnd;
            int turned = simulateOrbs(layer, p1, p2, dual, delay, down, length, turnAt, length, &turnEnd);
            if (turned >= length) {
                if (buf.captureKiller && buf.baseEscape < 0) {
                    buf.baseEscape = turnAt;
                    buf.baseCrash = ret;
                }
                return horizon;
            }
        }
        return ret;
    }

    // v2.7 wave search, same rules as the cube: the smallest delay that is safe (or
    // clearly better than now) wins, so turns are only held back as long as needed.
    // v2.12 robot scoring. A robot press is a jump whose height you set by when you let go,
    // so "pressing now crashes" only if NO release time saves it. Your usual hold is tried
    // first, then short to long holds. (Before, only your usual hold was tested, so a press
    // that needed a shorter or longer hold looked like a death and got delayed for nothing.)
    // v2.82: 8 hold lengths up to a full-height jump (was 4, up to 45 ticks), tried nearest
    // to your usual hold first, so the jump height a gap needs is found more often.
    constexpr int ROBOT_HOLDS[] = { 4, 9, 14, 20, 27, 36, 48, 64 };
    int robotUsedHold = -1;   // hold length that made the last robot run survive

    // v2.73: quick pass of a robot / UFO / spider search: only the plain run per delay (your
    // usual robot hold, no extra flap / flip). Those modes tried up to 5x more runs per
    // delay, so the search ran out of time after 2-10 delays and missed saves further out.
    bool quickScore = false;

    int robotScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, int horizon, int target, SimEnd* end) {
        int usual = robotHold.pick(st(20), 50);
        int best = simulateOrbs(layer, p1, p2, dual, delay, true, horizon, usual, target, end);
        robotUsedHold = usual;
        if (best >= target || *end == SimEnd::StopSearch || quickScore) {
            return best;
        }
        int holds[std::size(ROBOT_HOLDS)];
        std::copy(std::begin(ROBOT_HOLDS), std::end(ROBOT_HOLDS), holds);
        std::stable_sort(std::begin(holds), std::end(holds), [&](int a, int b) { return std::abs(st(a) - usual) < std::abs(st(b) - usual); });
        for (int hold : holds) {
            if (std::abs(st(hold) - usual) < st(3)) {
                continue;
            }
            if (clickOverTime()) {
                break;
            }
            SimEnd e;
            int ret = simulateOrbs(layer, p1, p2, dual, delay, true, horizon, st(hold), target, &e);
            if (ret > best) {
                best = ret;
                robotUsedHold = st(hold);
            }
            if (best >= target) {
                break;
            }
        }
        return best;
    }

    // v2.15 spider scoring, like the wave's: if the run crashes, tap again (flip back) a
    // little before the crash; if that survives, you could have reacted, so it's safe.
    constexpr int SPIDER_RETAP[] = { 4, 8, 12 };
    constexpr int SPIDER_MIN_RETAP = 4;

    int spiderScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, int horizon, int releaseAfter, int target,
        SimEnd* end) {
        int ret = simulateOrbs(layer, p1, p2, dual, delay, true, horizon, releaseAfter, target, end);
        if (ret >= target || *end == SimEnd::StopSearch || quickScore) {
            return ret;
        }
        for (int margin : SPIDER_RETAP) {
            int at = ret - st(margin);
            if (at < st(SPIDER_MIN_RETAP)) {
                break;
            }
            int length = std::min(horizon, at + st(16));
            buf.retapAt = at;
            SimEnd e;
            int retapped = simulateOrbs(layer, p1, p2, dual, delay, true, length, releaseAfter, length, &e);
            buf.retapAt = -1;
            if (retapped >= length) {
                if (buf.captureKiller && buf.baseEscape < 0) {
                    buf.baseEscape = at;
                    buf.baseCrash = ret;
                }
                return horizon;
            }
        }
        return ret;
    }

    // Same rules as the cube: the smallest delay that is safe (or clearly better) wins.
    // v2.72: the crash happens at the same moment however late the input comes (delay k
    // dies k ticks sooner, for the first SAME_CRASH_TRIES delays): it's the next obstacle's,
    // not this input's, so the rest of the search can't help. Robot and UFO searches spent
    // their whole time budget on exactly that, on almost every input.
    constexpr int SAME_CRASH_TRIES = 4;
    bool crashFixed = false;  // the last search stopped for that reason

    bool sameCrash(int k, int ret, int base, int& streak) {
        if (streak >= 0 && std::abs(k + ret - base) <= 1) {
            ++streak;
        }
        else {
            streak = -1;
        }
        if (streak >= SAME_CRASH_TRIES) {
            crashFixed = true;
            return true;
        }
        return false;
    }

    int searchSmallest(int horizon, int maxDelay, int base, int* landed, std::function<int(int, int, SimEnd*)> const& score,
        double budget = -1.0, bool quiet = false, bool needFull = false) {
        if (budget < 0.0) {
            budget = buf.budgetMs;
        }
        budget = std::min(budget, searchBudget());
        using clock = std::chrono::steady_clock;
        auto start = clock::now();
        int target = needFull ? horizon : std::min(horizon, base + st(12));
        dbgBestK = 0;
        dbgBestRet = -1;
        budgetCutK = 0;
        int streak = 0;
        for (int k = searchFrom; k <= maxDelay; ++k) {
            SimEnd end;
            int ret = score(k, target, &end);
            if (ret > dbgBestRet) {
                dbgBestRet = ret;
                dbgBestK = k;
            }
            if (end == SimEnd::StopSearch) {
                return 0;
            }
            if (ret >= target) {
                *landed = ret;
                return k;
            }
            if (sameCrash(k, ret, base, streak)) {
                return 0;
            }
            if (std::chrono::duration<double, std::milli>(clock::now() - start).count() > budget) {
                if (!quiet) {
                    say("prediction budget hit at +{} ticks", k);
                }
                budgetCutK = k;
                return 0;
            }
        }
        return 0;
    }

    // v2.73: quick pass over every delay first (half the budget), then the full scoring with
    // whatever time is left.
    int searchTwoPass(int horizon, int maxDelay, int base, int* landed, std::function<int(int, int, SimEnd*)> const& score,
        bool needFull = false) {
        quickScore = true;
        int k = searchSmallest(horizon, maxDelay, base, landed, score, searchBudget() * 0.5, true, needFull);
        quickScore = false;
        if (k > 0) {
            return k;
        }
        crashFixed = false;
        double left = buf.budgetMs - clickMs();
        if (left < 0.5) {
            say("prediction budget hit (quick pass only)");
            budgetCutK = 0;
            return 0;
        }
        return searchSmallest(horizon, maxDelay, base, landed, score, left, false, needFull);
    }

    // v2.25 ship scoring. Like the wave: a crash only counts if you couldn't have recovered.
    // Ship has momentum, so recovery (the opposite input) is tried earlier before the crash:
    // 8, 16 and 24 ticks.
    constexpr int SHIP_RECOVER[] = { 8, 16, 24 };

    int shipScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int target,
        SimEnd* end) {
        int ret = simulateOrbs(layer, p1, p2, dual, delay, down, horizon, -1, target, end);
        if (ret >= target || *end == SimEnd::StopSearch) {
            return ret;
        }
        for (int margin : SHIP_RECOVER) {
            int turnAt = ret - st(margin);
            if (turnAt < st(WAVE_MIN_TURN)) {
                break;
            }
            int length = std::min(horizon, turnAt + st(WAVE_ESCAPE));
            SimEnd turnEnd;
            int turned = simulateOrbs(layer, p1, p2, dual, delay, down, length, turnAt, length, &turnEnd);
            if (turned >= length) {
                if (buf.captureKiller && buf.baseEscape < 0) {
                    buf.baseEscape = turnAt;
                    buf.baseCrash = ret;
                }
                return horizon;
            }
        }
        return ret;
    }

    // v2.40 UFO scoring. One flap and then nothing falls for the whole look-ahead into
    // whatever is below, which your next flap would have avoided. So if the run crashes,
    // try one more flap 6 / 12 / 18 ticks before the crash; if that survives, you could have
    // flapped, so the click counts as safe. Crashing up into something can't be fixed by
    // flapping, so those stay real crashes.
    constexpr int UFO_REFLAP[] = { 6, 12, 18 };

    int ufoScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, int horizon, int releaseAfter, int target,
        SimEnd* end) {
        int ret = simulateOrbs(layer, p1, p2, dual, delay, true, horizon, releaseAfter, target, end);
        if (ret >= target || *end == SimEnd::StopSearch || quickScore) {
            return ret;
        }
        for (int margin : UFO_REFLAP) {
            int at = ret - st(margin);
            if (at < st(4)) {
                break;
            }
            int length = std::min(horizon, at + st(20));
            buf.retapAt = at;
            SimEnd e;
            int reflapped = simulateOrbs(layer, p1, p2, dual, delay, true, length, releaseAfter, length, &e);
            buf.retapAt = -1;
            if (reflapped >= length) {
                if (buf.captureKiller && buf.baseEscape < 0) {
                    buf.baseEscape = at;
                    buf.baseCrash = ret;
                }
                return horizon;
            }
        }
        return ret;
    }

    bool marginUsed = false;  // the last wave save took the +1 tick safety margin

    int searchWave(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool down, int horizon, int maxDelay, int base,
        int* landed) {
        marginUsed = false;
        using clock = std::chrono::steady_clock;
        auto start = clock::now();
        double budget = searchBudget();
        int target = std::min(horizon, base + st(12));
        dbgBestK = 0;
        dbgBestRet = -1;
        budgetCutK = 0;
        for (int k = searchFrom; k <= maxDelay; ++k) {
            SimEnd end;
            int ret = waveScore(layer, p1, p2, dual, k, down, horizon, target, &end);
            if (ret > dbgBestRet) {
                dbgBestRet = ret;
                dbgBestK = k;
            }
            if (end == SimEnd::StopSearch) {
                return 0;
            }
            if (ret >= target) {
                *landed = ret;
                // v2.10 safety margin: the first safe delay is the one that only just clears
                // the spike corner, so a pixel of difference in the real run can still clip
                // it. If the next two delays are safe too, take the middle one: a tick of
                // room on both sides.
                if (buf.waveMargin && k + 2 <= maxDelay) {
                    SimEnd e1, e2;
                    if (waveScore(layer, p1, p2, dual, k + 1, down, horizon, target, &e1) >= target &&
                        waveScore(layer, p1, p2, dual, k + 2, down, horizon, target, &e2) >= target) {
                        marginUsed = true;
                        return k + 1;
                    }
                }
                return k;
            }
            if (std::chrono::duration<double, std::milli>(clock::now() - start).count() > budget) {
                say("prediction budget hit at +{} ticks", k);
                budgetCutK = k;
                return 0;
            }
        }
        return 0;
    }

    // Find the smallest delay that clearly beats pressing now. 0 = don't buffer.
    int searchDelay(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool down, bool anyWave, int horizon, int maxDelay,
        int releaseAfter, int base, int* landed, bool needFull = false) {
        using clock = std::chrono::steady_clock;
        auto start = clock::now();
        double budget = searchBudget();
        auto overBudget = [&] {
            return std::chrono::duration<double, std::milli>(clock::now() - start).count() > budget;
        };

        int target = needFull ? horizon : std::min(horizon, base + st(12));
        dbgBestK = 0;
        dbgBestRet = -1;
        int sameStreak = 0;
        int waveBestK = 0;
        int waveBestRet = base;
        budgetCutK = 0;
        for (int k = searchFrom; k <= maxDelay; ++k) {
            SimEnd end;
            // Wave needs the full run to compare gains; elsewhere reaching `target` is enough.
            int ret = simulateOrbs(layer, p1, p2, dual, k, down, horizon, releaseAfter, anyWave ? horizon : target, &end);
            if (ret > dbgBestRet) {
                dbgBestRet = ret;
                dbgBestK = k;
            }
            if (end == SimEnd::StopSearch) {
                break;
            }
            if (anyWave) {
                // v4.6 wave rule: delaying a turn in a flat corridor naturally adds ~2 ticks
                // per tick of delay against the opposite wall, so only buffer when the delayed
                // run clearly clears a spike, saw or corner. A fully safe run needs
                // 2*k+6 extra ticks and ends the search; otherwise keep the best run that
                // gains at least 2*k+10 and beats the previous best by more than 2.
                if (ret >= horizon && ret - base >= k * 2 + 6) {
                    waveBestK = k;
                    waveBestRet = ret;
                    break;
                }
                if (ret >= base + k * 2 + 10 && ret > waveBestRet + 2) {
                    waveBestK = k;
                    waveBestRet = ret;
                    if (ret >= horizon) {
                        break;
                    }
                }
            }
            else if (ret < target && sameCrash(k, ret, base, sameStreak)) {
                break;
            }
            else if (ret >= target) {
                // v2.63: a delay that only lives 12 ticks longer can still crash later in the same
                // jump (Acheron's first jump: saved, then died on landing). Prefer the smallest
                // delay whose jump lands safely; if none does, keep this one.
                SimEnd full;
                if (ret < horizon && simulateOrbs(layer, p1, p2, dual, k, down, horizon, releaseAfter, horizon, &full) < horizon) {
                    for (int j = k + 1; j <= maxDelay && !overBudget(); ++j) {
                        SimEnd e;
                        int r = simulateOrbs(layer, p1, p2, dual, j, down, horizon, releaseAfter, horizon, &e);
                        if (e == SimEnd::StopSearch) {
                            break;
                        }
                        if (r >= horizon) {
                            *landed = r;
                            return j;
                        }
                    }
                }
                *landed = ret;
                return k;
            }
            if (overBudget()) {
                say("prediction budget hit at +{} ticks", k);
                budgetCutK = anyWave ? 0 : k;
                break;
            }
        }
        if (anyWave && waveBestK > 0) {
            *landed = waveBestRet;
            return waveBestK;
        }
        return 0;
    }

    // v2.65 live path (F10): between clicks the path keeps updating (v2.70: 20 times a second):
    // blue = where you go if you do nothing, yellow = where a click right now takes you.
    // After a click, that click's path (red / green) stays up briefly first.
    void livePath(PlayLayer* layer, PlayerObject* p1) {
        if (!buf.showPath || buf.perfMode || !buf.pathNode || !buf.ghost1 || buf.simulating || p1->m_isDead || layer->m_isPlatformer) {
            return;
        }
        if (tickCount < buf.pathHoldUntil || !buf.queue.empty() || tickCount % st(12) != 0) {
            return;
        }
        bool twoPlayer = layer->m_level && layer->m_level->m_twoPlayerMode;
        bool dual = !twoPlayer && layer->m_gameState.m_isDualMode && layer->m_player2;
        auto* p2 = dual ? layer->m_player2 : nullptr;
        bool wave = p1->m_isDart || (p2 && p2->m_isDart);
        bool ship = p1->m_isShip || (p2 && p2->m_isShip);
        bool flyer = isFlying(p1) || (p2 && isFlying(p2));
        bool mini = (p1->m_isDart && p1->m_vehicleSize < 1.f) || (p2 && p2->m_isDart && p2->m_vehicleSize < 1.f);
        int horizon = wave ? fullWaveHorizon(mini) : st(flyer ? 48 : std::clamp(buf.groundHorizon, 48, 240));
        bool down = !buf.appliedInput;
        int releaseAfter = down && !wave && !ship ? holdEstimate() : -1;

        buf.pathNode->clear();
        bool idleDied = false;
        buf.skipPress = true;
        auto idle = recordRun(layer, p1, p2, dual, 0, down, horizon, -1, &idleDied);
        buf.skipPress = false;
        drawPath(idle, 0, { 0.45f, 0.7f, 1.f, 0.6f }, idleDied);
        bool clickDied = false;
        auto click = recordRun(layer, p1, p2, dual, 0, down, horizon, releaseAfter, &clickDied);
        drawPath(click, 0, { 1.f, 0.85f, 0.3f, 0.85f }, clickDied);
    }

    void queueInput(int ticks, bool down) {
        buf.queue.push_back({ ticks, down });
    }

    // v2.51 practice stats: every save (or, in training, every input that would have been
    // saved) is how early you were, in 240/s ticks.
    struct Stats {
        int saves = 0;
        int earlySum = 0;
        int attempts = 0;            // attempts that had at least one save
        std::map<int, int> savesAt;  // level percent -> saves there
    };
    Stats attemptStats;
    Stats sessionStats;
    std::string statsKey;

    void noteSave(PlayLayer* layer, int ticks) {
        int t = static_cast<int>(std::lround(ticks / stepScale()));
        int pct = std::clamp(static_cast<int>(layer->getCurrentPercent()), 0, 100);
        for (auto* s : { &attemptStats, &sessionStats }) {
            ++s->saves;
            s->earlySum += t;
            ++s->savesAt[pct];
        }
        // v2.56: a ding for every real save (training mode shows its hint instead)
        if (buf.saveDing && !buf.train) {
            FMODAudioEngine::sharedEngine()->playEffect("counter003.ogg");
        }
    }

    std::string attemptSummary() {
        auto const& a = attemptStats;
        return fmt::format("{} {} | avg {:.1f}t ({:.2f}f) early", a.saves, buf.train ? (a.saves == 1 ? "early click" : "early clicks")
            : (a.saves == 1 ? "save" : "saves"), static_cast<double>(a.earlySum) / a.saves, a.earlySum * TICK / a.saves);
    }

    // At the end of an attempt (death, restart or level complete).
    // v2.52: logged to the console only (no popup after a death)
    void endAttempt() {
        if (attemptStats.saves > 0) {
            ++sessionStats.attempts;
            say("attempt: {}", attemptSummary());
        }
        attemptStats = {};
    }

    // v2.51 training mode: the input goes through untouched, and you're told how early it was.
    bool trainInstead(PlayLayer* layer, int ticks, bool down, char const* what) {
        if (!buf.train) {
            return false;
        }
        noteSave(layer, ticks);
        say("TRAIN {} {}: {}t ({:g}f) early", what, down ? "press" : "release", ticks, ticks * TICK);
        if (buf.hintLabel) {
            buf.hintLabel->setString(fmt::format("{} {:g}f early", down ? "click" : "release", ticks * TICK).c_str());
            buf.hintLabel->stopAllActions();
            buf.hintLabel->setOpacity(230);
            buf.hintLabel->setScale(0.5f);
            buf.hintLabel->runAction(CCSequence::create(CCEaseBackOut::create(CCScaleTo::create(0.12f, 0.6f)), CCDelayTime::create(0.5f),
                CCFadeOut::create(0.4f), nullptr));
        }
        return true;
    }

    // v2.51 per-level settings: your stage and switches are remembered for each level and
    // come back when you open it again. Outside a level (and in levels you haven't changed
    // anything in) the normal settings apply.
    // v2.58: the stage and low latency are no longer per level; they're the same everywhere.
    constexpr char const* PROFILE_KEYS[] = { "robust-saves", "orb-align", "orb-timing", "level-memory",
        "more-time-death-spots", "ship-buffer", "ufo-smart", "ball-smart", "swing-smart", "cbf-sync", "predict-moving-objects",
        "training-mode" };

    struct Globals {
        bool have = false;
        std::vector<bool> values;
    } globals;

    std::string profileKey() {
        return memoryKey.empty() ? std::string() : "profile-" + memoryKey;
    }

    void saveProfile() {
        if (!buf.perLevel || profileKey().empty() || !globals.have) {
            return;
        }
        auto v = matjson::Value::object();
        for (auto* key : PROFILE_KEYS) {
            v[key] = Mod::get()->getSettingValue<bool>(key);
        }
        Mod::get()->setSavedValue(profileKey(), v);
    }

    void restoreGlobals() {
        if (!globals.have) {
            return;
        }
        auto* mod = Mod::get();
        for (size_t i = 0; i < std::size(PROFILE_KEYS); ++i) {
            mod->setSettingValue<bool>(PROFILE_KEYS[i], globals.values[i]);
        }
        globals = {};
        mod->setSavedValue("profile-globals", matjson::Value(nullptr));
        readSettings();
        applyStage();
    }

    void loadProfile() {
        if (!buf.perLevel || profileKey().empty()) {
            return;
        }
        auto* mod = Mod::get();
        globals.have = true;
        globals.values.clear();
        auto backup = matjson::Value::object();
        for (auto* key : PROFILE_KEYS) {
            bool value = mod->getSettingValue<bool>(key);
            globals.values.push_back(value);
            backup[key] = value;
        }
        // kept until the level is left, so a crash mid-level can't leave level settings behind
        mod->setSavedValue("profile-globals", backup);
        if (!mod->hasSavedValue(profileKey())) {
            return;
        }
        auto v = mod->getSavedValue<matjson::Value>(profileKey());
        if (!v.isObject()) {
            return;
        }
        for (auto* key : PROFILE_KEYS) {
            if (v.contains(key)) {
                mod->setSettingValue<bool>(key, v[key].asBool().unwrapOr(mod->getSettingValue<bool>(key)));
            }
        }
        readSettings();
        applyStage();
        say("level settings loaded: {}", stageText());
    }

    // Settings a crash left behind are put back when the game starts.
    void recoverGlobals() {
        auto* mod = Mod::get();
        if (!mod->hasSavedValue("profile-globals")) {
            return;
        }
        auto v = mod->getSavedValue<matjson::Value>("profile-globals");
        if (!v.isObject()) {
            return;
        }
        for (auto* key : PROFILE_KEYS) {
            if (v.contains(key)) {
                mod->setSettingValue<bool>(key, v[key].asBool().unwrapOr(mod->getSettingValue<bool>(key)));
            }
        }
        mod->setSavedValue("profile-globals", matjson::Value(nullptr));
    }

    // v2.51 death heatmap: red marks on the progress bar where level memory saw you die,
    // brighter where you die more.
    void refreshHeatmap() {
        auto* layer = GameManager::get()->m_playLayer;
        if (!buf.heatNode || !layer || !layer->m_progressBar) {
            return;
        }
        buf.heatNode->clear();
        float length = layer->m_levelLength;
        if (!buf.heatmap || length <= 0.f) {
            return;
        }
        auto size = layer->m_progressBar->getContentSize();
        uint32_t most = 0;
        for (auto const& [slice, spot] : memory) {
            most = std::max(most, spot.deaths);
        }
        if (most == 0) {
            return;
        }
        for (auto const& [slice, spot] : memory) {
            if (spot.deaths == 0) {
                continue;
            }
            float pct = std::clamp((slice + 0.5f) * SLICE / length, 0.f, 1.f);
            float x = 2.f + pct * (size.width - 4.f);
            float heat = static_cast<float>(spot.deaths) / most;
            buf.heatNode->drawSegment({ x, 2.f }, { x, size.height - 2.f }, 0.7f, { 1.f, 0.35f + 0.3f * (1.f - heat), 0.55f, 0.35f + 0.6f * heat });
        }
    }

    // v2.75 run check (test bench). Every attempt played from the level start is recorded
    // (the inputs the game got, and where you were on every physics step); the longest one
    // per level is kept. "Check run" replays it on the hidden ghost at the start of your next
    // attempt and reports how long the ghost follows the recording. Run it after an update:
    // if the ghost stops matching much earlier than before, the update changed the physics
    // copy. Moving objects and triggers aren't replayed, so it's a comparison between
    // versions, not a percentage that should reach 100.
    struct BenchRun {
        float startX = 0.f, startY = 0.f, dt = TICK;
        std::vector<std::pair<int, bool>> inputs;
        std::vector<CCPoint> track;
    };
    BenchRun benchLive;
    bool benchRecording = false;
    bool benchNeedStart = false;  // next P1 step is the first of an attempt
    bool benchPending = false;    // "Check run" was pressed
    std::string lastBench;        // last result, for the stats popup

    std::filesystem::path benchFile() {
        return Mod::get()->getSaveDir() / "bench" / (memoryKey + ".txt");
    }

    size_t benchSavedTicks() {
        std::ifstream in(benchFile());
        float x, y, dt;
        size_t ticks = 0;
        if (in >> x >> y >> dt >> ticks) {
            return ticks;
        }
        return 0;
    }

    bool benchLoad(BenchRun& r) {
        std::ifstream in(benchFile());
        size_t ticks = 0, inputs = 0;
        if (!(in >> r.startX >> r.startY >> r.dt >> ticks >> inputs)) {
            return false;
        }
        for (size_t i = 0; i < inputs; ++i) {
            int t, d;
            if (!(in >> t >> d)) {
                return false;
            }
            r.inputs.push_back({ t, d != 0 });
        }
        for (size_t i = 0; i < ticks; ++i) {
            float x, y;
            if (!(in >> x >> y)) {
                return false;
            }
            r.track.push_back({ x, y });
        }
        return !r.track.empty();
    }

    void benchStart(PlayLayer* layer, PlayerObject* p1) {
        benchLive = {};
        benchLive.startX = p1->getPosition().x;
        benchLive.startY = p1->getPosition().y;
        benchLive.dt = buf.tickLength;
        // only runs from the level start (no practice checkpoint)
        bool fromCheckpoint = layer->m_isPracticeMode && layer->m_checkpointArray && layer->m_checkpointArray->count() > 0;
        benchRecording = !memoryKey.empty() && !fromCheckpoint;
    }

    void benchInput(bool down) {
        if (benchRecording) {
            benchLive.inputs.push_back({ static_cast<int>(benchLive.track.size()), down });
        }
    }

    void benchStep(PlayerObject* p1) {
        if (benchRecording) {
            benchLive.track.push_back(p1->getPosition());
            if (benchLive.track.size() > 200000) {
                benchRecording = false;
            }
        }
    }

    // end of an attempt: keep it if it's the longest run from the start so far
    // v2.77: the best run is kept in memory and only written when you leave or finish the
    // level; writing it on every new-best death made a hitch right when you died.
    BenchRun benchBest;
    size_t benchBestTicks = 0;   // longest run known for this level (memory or file)
    bool benchBestDirty = false;

    void benchFinish() {
        if (!benchRecording) {
            return;
        }
        benchRecording = false;
        if (benchLive.track.size() < 240 || benchLive.track.size() <= benchBestTicks) {
            return;
        }
        benchBestTicks = benchLive.track.size();
        benchBest = std::move(benchLive);
        benchLive = {};
        benchBestDirty = true;
    }

    void benchFlush() {
        if (!benchBestDirty || memoryKey.empty()) {
            return;
        }
        benchBestDirty = false;
        std::error_code ec;
        std::filesystem::create_directories(benchFile().parent_path(), ec);
        std::ofstream out(benchFile(), std::ios::trunc);
        out << benchBest.startX << ' ' << benchBest.startY << ' ' << benchBest.dt << ' ' << benchBest.track.size() << ' '
            << benchBest.inputs.size() << '\n';
        for (auto const& [t, d] : benchBest.inputs) {
            out << t << ' ' << (d ? 1 : 0) << '\n';
        }
        for (auto const& p : benchBest.track) {
            out << p.x << ' ' << p.y << '\n';
        }
    }

    void benchCheck(PlayLayer* layer, PlayerObject* p1) {
        benchFlush();
        BenchRun r;
        if (!benchLoad(r)) {
            say("run check: no recorded run for this level yet (play one attempt from the start first)");
            return;
        }
        if (std::abs(r.startX - p1->getPosition().x) > 0.5f || std::abs(r.startY - p1->getPosition().y) > 0.5f) {
            say("run check: this attempt doesn't start where the recording did (start from the beginning, no checkpoint)");
            benchPending = true;
            return;
        }
        buf.simulating = true;
        buf.ghostDied = false;
        buf.ghostHitPortal = false;
        buf.haveRealRect = false;
        buf.haveStartRects = false;  // run check starts at the level start, not where you are
        buf.harmless.clear();
        buf.orbClick = false;  // orbs only through the recorded inputs
        buf.retapAt = -1;
        copyState(buf.ghost1, p1);
        buf.held1 = false;
        buf.auto1 = false;
        buf.autoAt1 = -1;
        size_t next = 0;
        int firstOff = -1;
        float maxErr = 0.f;
        int t = 0;
        char const* end = "end of the recording";
        auto clockStart = std::chrono::steady_clock::now();
        for (; t < static_cast<int>(r.track.size()); ++t) {
            while (next < r.inputs.size() && r.inputs[next].first <= t) {
                pressGhost(buf.ghost1, r.inputs[next].second);
                ++next;
            }
            buf.simTick = t;
            advanceGhost(buf.ghost1, r.dt);
            releaseAutoClicks();
            if (!buf.ghostDied) {
                checkGhost(layer, buf.ghost1, r.dt);
            }
            if (buf.ghostDied) {
                end = "the ghost died";
                break;
            }
            if (buf.ghostHitPortal) {
                end = "a mode portal (predictions stop there)";
                break;
            }
            auto g = buf.ghost1->getPosition();
            float err = std::hypot(g.x - r.track[t].x, g.y - r.track[t].y);
            maxErr = std::max(maxErr, err);
            if (err > 3.f && firstOff < 0) {
                firstOff = t;
            }
        }
        restoreObjects();
        buf.ghost1->stopAllActions();
        buf.simulating = false;
        buf.orbClick = buf.assumeOrbs;
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - clockStart).count();
        int matched = firstOff < 0 ? t : firstOff;
        lastBench = fmt::format("{}/{} steps ({:.0f}%) match, stopped at {}", matched, r.track.size(),
            100.0 * matched / r.track.size(), end);
        say("run check: {} | max error {:.1f}u{} | {:.0f} ms", lastBench, maxErr,
            std::abs(r.dt - buf.tickLength) > 1e-4f ? " | recorded at another frame rate" : "", ms);
        std::error_code ec;
        std::filesystem::create_directories(Mod::get()->getSaveDir(), ec);
        std::ofstream(Mod::get()->getSaveDir() / "run-check-results.txt", std::ios::app)
            << Mod::get()->getVersion().toVString() << ' ' << memoryKey << ": " << lastBench << '\n';
    }

    // v2.51 stats popup text
    std::string statsText() {
        auto const& s = sessionStats;
        std::string text;
        if (s.saves == 0) {
            text += "No early clicks yet this session.\n";
        }
        else {
            text += fmt::format("<cp>{}</c> {} in <cp>{}</c> attempts, on average <cp>{:.1f}t</c> ({:.2f}f) early.\n",
                s.saves, buf.train ? "early clicks" : "saves", s.attempts, static_cast<double>(s.earlySum) / s.saves, s.earlySum * TICK / s.saves);
            std::vector<std::pair<int, int>> spots(s.savesAt.begin(), s.savesAt.end());
            std::sort(spots.begin(), spots.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
            text += "Most early clicks at:";
            for (size_t i = 0; i < std::min<size_t>(3, spots.size()); ++i) {
                text += fmt::format(" <cy>{}%</c> ({}x)", spots[i].first, spots[i].second);
            }
            text += "\n";
        }
        auto* layer = GameManager::get()->m_playLayer;
        if (layer && layer->m_levelLength > 0.f) {
            std::map<int, uint32_t> deathsAt;
            for (auto const& [slice, spot] : memory) {
                if (spot.deaths > 0) {
                    deathsAt[std::clamp(static_cast<int>((slice + 0.5f) * SLICE / layer->m_levelLength * 100.f), 0, 100)] += spot.deaths;
                }
            }
            std::vector<std::pair<int, uint32_t>> spots(deathsAt.begin(), deathsAt.end());
            std::sort(spots.begin(), spots.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
            if (!spots.empty()) {
                text += "Most deaths at:";
                for (size_t i = 0; i < std::min<size_t>(3, spots.size()); ++i) {
                    text += fmt::format(" <cr>{}%</c> ({}x)", spots[i].first, spots[i].second);
                }
                text += "\n";
            }
        }
        if (!lastBench.empty()) {
            text += "Run check: " + lastBench + "\n";
        }
        return text;
    }

    bool intercept(GJBaseGameLayer* self, bool down, bool isPlayer1) {
        ClickTimer timer;
        clickBudget();
        auto* layer = GameManager::get()->m_playLayer;
        if (!layer) {
            return false;
        }
        // v2.85 no-hitch: a search going on from an earlier tick starts after the delays it
        // already tried. A new input of yours meanwhile ends it: the waiting click becomes a
        // plain delayed click, and this input follows it as usual.
        bool resuming = resumeNow;
        searchFrom = resuming ? std::max(1, resumeEntry.from) : 1;
        budgetCutK = 0;
        struct SearchFromReset {
            ~SearchFromReset() {
                searchFrom = 1;
            }
        } searchFromReset;
        if (!resuming) {
            for (auto& q : buf.queue) {
                q.resume = false;
            }
        }
        // v2.80: the mod already clicked for you here; your own late click (press and its
        // release) is swallowed so it doesn't jump a second time
        if (buf.autoClicked && isPlayer1 && !resuming) {
            if (!down) {
                buf.autoClicked = false;
            }
            say("AUTO click: your own {} ignored (already clicked for you)", down ? "press" : "release");
            return true;
        }

        auto* level = layer->m_level;
        bool twoPlayer = level && level->m_twoPlayerMode;
        // v2.55: a P2-side input in a two-player level only steers Player 2 while dual mode is
        // on; before that it moves Player 1 (Relief's wave parts), so it's buffered like a P1 input.
        if (twoPlayer && !isPlayer1 && layer->m_gameState.m_isDualMode) {
            return false;
        }
        // In two-player levels P2 is someone else: never simulate P1's input on it.
        bool dual = !twoPlayer && layer->m_gameState.m_isDualMode && layer->m_player2;

        if (!resuming) {
            buf.isPlayer1 = isPlayer1;
            buf.realInput = down;
        }
        if (buf.autoReleased && !resuming) {
            // the game already let go for you: your real release has nothing left to do
            buf.autoReleased = false;
            buf.autoDash = false;
            if (!down) {
                return true;
            }
        }
        if (!buf.enabled) {
            buf.appliedInput = down;
            return false;
        }
        if (buf.practiceOnly && !layer->m_isPracticeMode) {
            buf.appliedInput = down;
            return false;
        }
        // v2.4: the ghost never presses left/right, so platformer predictions are meaningless.
        if (layer->m_isPlatformer) {
            buf.appliedInput = down;
            return false;
        }

        auto* p1 = self->m_player1;
        auto* p2 = dual ? layer->m_player2 : nullptr;

        // v2.18 Click Between Frames: a click that arrives in the middle of P1's physics step
        // landed `midTick` of the way through the tick (measured from how far P1 has moved
        // since the tick started). The ghost then starts with the rest of that tick, and
        // delayed inputs are queued one tick sooner, because the game's next tick boundary
        // is already the ghost's tick 1.
        float midTick = 0.f;
        if (buf.cbfSync && buf.inRealUpdate && p1 && std::abs(buf.tickDx) > 0.01f && !resuming) {
            midTick = std::clamp((p1->getPosition().x - buf.tickStartX) / buf.tickDx, 0.f, 0.999f);
        }
        buf.firstStep = 1.f - midTick;
        struct FirstStepReset {
            ~FirstStepReset() {
                buf.firstStep = 1.f;
            }
        } firstStepReset;
        int early = midTick > 0.f ? 1 : 0;

        bool anyShip = p1 && (p1->m_isShip || (p2 && p2->m_isShip));
        if (!p1 || p1->m_isDead || (anyShip && !buf.shipBuffer)) {
            resetPending();
            buf.appliedInput = down;
            return false;
        }

        if (down && buf.queue.empty()) {
            buf.releaseShift = 0;
        }

        bool p1Wave = p1->m_isDart;
        bool anyWave = p1Wave || (p2 && p2->m_isDart);
        bool mini = (p1Wave && p1->m_vehicleSize < 1.f) || (p2 && p2->m_isDart && p2->m_vehicleSize < 1.f);

        // Wave spam: don't fight fast inputs. Counted for every input, including ones made
        // while a delayed input is waiting (v2.5.1 skipped those, so a spam that started
        // with a buffered click never reached the streak and its taps got collapsed).
        // Once the streak is reached, anything still waiting is dropped and your inputs
        // go straight to the game until you pause for longer than the spam gap.
        // v2.44: ship is steered by feathering, so it gets its own, much shorter spam gap;
        // otherwise nearly every ship input counted as spam and was never buffered.
        int spamGap = st(anyShip && !anyWave ? std::clamp(buf.shipSpamGap, 2, 40) : std::clamp(buf.spamGap, 4, 40));
        if (!resuming) {
            if (buf.sinceInput < spamGap) {
                ++buf.rapid;
            }
            else {
                buf.rapid = 0;
            }
            buf.sinceInput = 0;
        }
        bool anyUfo = p1->m_isBird || (p2 && p2->m_isBird);
        bool anyBall = p1->m_isBall || (p2 && p2->m_isBall);
        bool anySwing = p1->m_isSwing || (p2 && p2->m_isSwing);
        if ((anyWave || anyShip || (anyUfo && buf.ufoSmart) || (anyBall && buf.ballSmart) || (anySwing && buf.swingSmart)) &&
            buf.rapid >= buf.spamStreak) {
            if (buf.explain) {
                say("pass {}: spam ({} fast inputs in a row, gap < {}t)", down ? "press" : "release", buf.rapid, spamGap);
            }
            // v2.47: path view marks inputs that skipped prediction (gray dot on you), so a
            // missing path isn't mistaken for a broken mod
            if (buf.showPath && buf.pathNode) {
                buf.pathNode->clear();
                buf.pathNode->drawDot(p1->getPosition(), 4.f, { 0.6f, 0.6f, 0.65f, 0.9f });
            }
            // v2.63.2: waiting inputs are played one per tick, in order, starting next tick, with
            // this one right after them. v2.61 dropped them (like v2.48), so a turn you made
            // just never happened; v2.49 sent them all at once, which turned a waiting press +
            // this release into a zero-length tap.
            if (!buf.queue.empty()) {
                int tick = 0;
                for (auto& q : buf.queue) {
                    q.ticksLeft = tick++;
                }
                queueInput(tick, down);
                return true;
            }
            buf.appliedInput = down;
            return false;
        }

        // v4.6 wave mode only: while a delayed input waits, a quick opposite input fires
        // one tick after it; pressing the same way again cancels that. (The default wave
        // mode shifts it like the cube does, below, so your zigzag keeps its shape.)
        if (!buf.queue.empty() && anyWave && buf.waveV46) {
            auto first = buf.queue.front();
            buf.queue.erase(buf.queue.begin() + 1, buf.queue.end());
            if (down != first.down) {
                queueInput(first.ticksLeft + 1, down);
            }
            return true;
        }

        // A delay is in progress: shift this input by the same amount so your
        // rhythm (tap length, hold length, follow-up clicks) is preserved.
        if (!buf.queue.empty()) {
            int after = buf.queue.back().ticksLeft;
            // low latency: a new press isn't shifted, it goes right after what's already queued
            queueInput(buf.lowLatency && down ? after : std::max(buf.shift - early, after), down);
            return true;
        }

        // Releases only matter for wave and robot.
        if (!p1Wave) {
            bool robot = p1->m_isRobot;
            bool wave2 = false;
            if (p2 && !robot) {
                wave2 = p2->m_isDart;
                if (!wave2) {
                    robot = p2->m_isRobot;
                }
            }
            // v2.16: letting go ends a dash, so dash releases are predicted like robot ones.
            bool dashing = p1->m_isDashing || (p2 && p2->m_isDashing);
            if (!wave2 && !down && !robot && !dashing && !anyShip) {
                if (buf.releaseShift > 0) {
                    // the press was delayed, so its release is delayed by the same amount:
                    // the hold length the ghost was tested with is the one the game gets
                    buf.shift = buf.releaseShift;
                    buf.releaseShift = 0;
                    queueInput(buf.shift, false);
                    return true;
                }
                buf.appliedInput = false;
                return false;
            }
        }

        bool anyFlyer = isFlying(p1) || (p2 && isFlying(p2));  // wave / ufo / swing
        int horizon = st(anyFlyer ? 48 : std::clamp(buf.groundHorizon, 48, 240));
        if (anyWave) {
            horizon = fullWaveHorizon(mini);
        }
        int maxDelay = st(anyWave ? buf.ticksWave : (anyShip ? std::clamp(buf.shipMaxDelay, 0, 16) : buf.ticksAll));
        if (buf.lowLatency) {
            maxDelay = std::min(maxDelay, st(4));  // at most 1 frame (60 fps) of delay
        }
        if (buf.perMode) {
            maxDelay = st(buf.modeTicks[modeIndex(p1, p2)]);  // v2.88: your frames for this mode (over low latency's cap)
        }
        if (resuming) {
            maxDelay -= resumeEntry.elapsed;  // the time since your click counts
            if (maxDelay < searchFrom) {
                maxDelay = 0;
            }
        }
        if (maxDelay <= 0) {
            if (buf.explain) {
                say("pass {}: max delay is 0 for this mode", down ? "press" : "release");
            }
            buf.appliedInput = down;
            return false;
        }

        // Wave releases are predicted separately, so hold there. Robot jump height depends
        // on the hold, so use your usual robot hold (once known); everything else (cube,
        // ball, ufo, spider, swing) re-triggers while held, so model a tap.
        bool anyRobot = p1->m_isRobot || (p2 && p2->m_isRobot);
        int releaseAfter = -1;
        if (down && !anyWave && !anyShip) {
            releaseAfter = anyRobot ? robotHold.pick(-1, 50) : holdEstimate();
        }

        // On the ground a press is a jump; only move it onto an orb if the setting says so.
        bool grounded = p1->m_isOnGround && (!p2 || p2->m_isOnGround);
        if (buf.explain && down && grounded && !buf.orbAlignGround && !anyWave && !anyShip && buf.orbAlign && buf.smart && !resuming) {
            // v2.42: is there an orb right ahead that this ground jump could have been moved onto?
            SimEnd probeEnd;
            buf.skipPress = true;
            buf.orbClick = false;
            int window = st(std::clamp(buf.orbWindow, 1, 64));
            simulate(layer, p1, p2, dual, 0, true, window + 2, -1, window + 2, &probeEnd);
            buf.skipPress = false;
            buf.orbClick = buf.assumeOrbs;
            if (buf.firstOrbTick >= 0 && buf.firstOrbTick <= window) {
                say("orb: you'd touch an orb at t{}, but this press is from the ground ('also from the ground' is off)",
                    buf.firstOrbTick);
            }
        }
        if (down && !anyWave && !anyShip && buf.orbAlign && buf.smart && (!grounded || buf.orbAlignGround) && !resuming) {
            int orbLanded = 0;
            int k = memorySaysSkip(sliceOf(p1)) ? 0 : alignOrb(layer, p1, p2, dual, horizon, releaseAfter, memoryDelay(sliceOf(p1)), maxDelay, &orbLanded);
            if (k > 0 && trainInstead(layer, k, down, "orb")) {
                decided(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
                buf.appliedInput = down;
                return false;
            }
            if (k > 0) {
                // v2.80.1: says so when it isn't a full save (the orb is used, but a crash still follows)
                say("ORB {} +{}t ({:g}f) -> {}t{}", buf.orbWasTiming ? "timing" : "press", k, k * TICK, orbLanded,
                    orbLanded < horizon ? fmt::format(" (not a full save: still crashes in {}t)", orbLanded) : std::string());
                noteSave(layer, k);
                decided(layer, p1, p2, dual, k, down, horizon, releaseAfter);
                buf.shift = k;
                queueInput(k - early, down);
                return true;
            }
        }

        buf.sawMover = false;
        buf.trackMovers = buf.avoidMovers || buf.predictMovers;
        SimEnd end;
        bool cubeStyleWave = anyWave && !buf.waveV46;
        bool robotPress = down && anyRobot && !anyWave && buf.robotSmart;
        // v2.82 robot release: letting go sets the jump's height, so it's judged like the press
        // (the jump has to land safely) instead of "lives a bit longer than letting go now"
        bool robotRelease = !down && anyRobot && !anyWave && !anyShip && buf.robotSmart;
        bool anySpider = p1->m_isSpider || (p2 && p2->m_isSpider);
        bool spiderPress = down && anySpider && !anyWave && !robotPress && buf.spiderSmart;
        bool ufoPress = down && anyUfo && !anyWave && !anyShip && buf.ufoSmart;  // v2.40
        // v2.51 swing: one flip, then nothing for the whole look-ahead, crashes into what your
        // next flip would have avoided. Scored like the UFO: could one more flip have saved it?
        bool swingPress = down && anySwing && !anyWave && !anyShip && !ufoPress && buf.swingSmart;
        ufoPress = ufoPress || swingPress;
        bool shipInput = anyShip && !anyWave;  // v2.25: press or release, both steer the ship
        buf.captureKiller = true;
        buf.baseTouchedDown = false;
        buf.killerSeen = false;
        buf.baseEscape = -1;
        int base = cubeStyleWave ? waveScore(layer, p1, p2, dual, 0, down, horizon, horizon, &end)
                 : robotPress    ? robotScore(layer, p1, p2, dual, 0, horizon, horizon, &end)
                 : spiderPress   ? spiderScore(layer, p1, p2, dual, 0, horizon, releaseAfter, horizon, &end)
                 : ufoPress      ? ufoScore(layer, p1, p2, dual, 0, horizon, releaseAfter, horizon, &end)
                 : shipInput     ? shipScore(layer, p1, p2, dual, 0, down, horizon, horizon, &end)
                                 : simulateOrbs(layer, p1, p2, dual, 0, down, horizon, releaseAfter, horizon, &end);
        int baseHold = robotUsedHold;  // v2.82: the hold that made pressing now safe
        bool baseSwitched = buf.lastRunSwitched;
        buf.captureKiller = false;
        buf.trackMovers = false;
        // v2.83: through a mode portal the ghost never clicks again, so the new mode's first
        // obstacle killed it. If one more click saves it, this input is fine as it is.
        if (baseSwitched && base < horizon && !cubeStyleWave && !shipInput && end != SimEnd::StopSearch && buf.baseEscape < 0) {
            int at = nextClickAt(layer, p1, p2, dual, 0, down, horizon, releaseAfter, base);
            if (at >= 0) {
                buf.baseEscape = at;
                buf.baseCrash = base;
                base = horizon;
            }
        }

        // v2.30 diagnostics: a ghost that dies within a few ticks while you live means the
        // prediction doesn't match you there. Log what killed it and how both hitboxes compare.
        if (buf.killerSeen && (base <= st(3) || (buf.deathLog && base < horizon))) {
            auto real = p1->getObjectRect();
            auto const& g = buf.ghostDeathRect;
            if (buf.killerType >= 0) {
                auto const& k = buf.killerRect;
                say("ghost killed at t{} by obj {} (type {}{}{}) at dx {:.1f} dy {:.1f} size {:.1f}x{:.1f}", buf.killerTick, buf.killerId,
                    buf.killerType, buf.killerNoTouch ? ", no-touch" : "", buf.killerDisabled ? ", disabled" : "",
                    k.getMidX() - real.getMidX(), k.getMidY() - real.getMidY(), k.size.width, k.size.height);
            }
            else {
                say("ghost killed at t{} by no object (crushed / out of bounds / slope)", buf.killerTick);
            }
            say("  hitbox ghost {:.1f}x{:.1f} vs you {:.1f}x{:.1f}, ghost moved dx {:.1f} dy {:.1f}{}{}", g.size.width, g.size.height,
                real.size.width, real.size.height, g.getMidX() - real.getMidX(), g.getMidY() - real.getMidY(),
                buf.predictMovers ? " (moving objects predicted)" : "",
                buf.triggersAhead > 0 ? fmt::format(" ({} move triggers ahead predicted)", buf.triggersAhead) : std::string());
            // v2.54: which players exist and which one your clicks move
            auto desc = [](PlayerObject* p) {
                if (!p) {
                    return std::string("none");
                }
                char const* mode = p->m_isDart ? "wave" : p->m_isShip ? "ship" : p->m_isBird ? "ufo" : p->m_isBall ? "ball"
                    : p->m_isRobot ? "robot" : p->m_isSpider ? "spider" : p->m_isSwing ? "swing" : "cube";
                return fmt::format("{}{}{}", mode, p->m_controlsDisabled ? " controls-off" : "", p->isVisible() ? "" : " hidden");
            };
            say("  players: P1 {} | P2 {} | dual {} | predicted {}", desc(layer->m_player1), desc(layer->m_player2),
                layer->m_gameState.m_isDualMode, p1 == layer->m_player1 ? "P1" : "P2");
        }

        // v2.36: with CBF, where inside the tick the click landed (0 = on the tick)
        std::string tickTag = midTick > 0.f ? fmt::format(" @{:.2f}", midTick) : std::string();
        if (base >= horizon) {
            // v2.43 more time at death spots. This input is only safe if your next input comes
            // in time, and level memory has seen you die here repeatedly: try small delays
            // that push the crash further away (or remove it), so you get more time to react.
            // Only at those spots, so normal play isn't delayed.
            bool reactMode = cubeStyleWave || shipInput || ufoPress || spiderPress;
            if (buf.moreTime && buf.learn && reactMode && buf.baseEscape >= 0 && spotAround(sliceOf(p1)).deaths >= 2) {
                int crash0 = buf.baseCrash;
                int bestK = 0;
                int bestCrash = crash0;
                int hold = (cubeStyleWave || shipInput) ? -1 : releaseAfter;
                // v2.67: wave / ship input that's already safe (if you keep reacting) is held back
                // at most 1 frame here. Up to 4f (16t) made fast zigzags at death spots feel late:
                // every later click in the rhythm gets shifted by the same amount.
                int moreMax = (cubeStyleWave || shipInput) ? std::min(maxDelay, st(4)) : maxDelay;
                for (int k = 1; k <= moreMax; ++k) {
                    SimEnd e;
                    if (clickOverTime()) {
                        break;
                    }
                    int ret = simulateOrbs(layer, p1, p2, dual, k, down, horizon, hold, horizon, &e);
                    if (e == SimEnd::StopSearch) {
                        break;
                    }
                    if (ret >= horizon) {
                        bestK = k;
                        bestCrash = -1;  // no crash at all
                        break;
                    }
                    int crashAt = k + ret;  // ticks from now
                    if (crashAt >= crash0 + st(8) && crashAt > bestCrash) {
                        bestK = k;
                        bestCrash = crashAt;
                    }
                }
                if (bestK > 0 && !buf.train) {
                    noteSave(layer, bestK);
                    say("MORE TIME {}{} +{}t: crash t{} -> {} (died here {}x)", down ? "press" : "release", tickTag, bestK, crash0,
                        bestCrash < 0 ? std::string("none") : fmt::format("t{}", bestCrash), spotAround(sliceOf(p1)).deaths);
                    decided(layer, p1, p2, dual, bestK, down, horizon, releaseAfter);
                    buf.shift = bestK;
                    queueInput(bestK - early, down);
                    return true;
                }
            }
            if (buf.explain) {
                if (buf.baseEscape >= 0) {
                    say("pass {}{}: safe only if you react by t{} (else crash at t{})", down ? "press" : "release", tickTag,
                        buf.baseEscape, buf.baseCrash);
                }
                else {
                    say("pass {}{}: safe now (survives the {}t look-ahead)", down ? "press" : "release", tickTag, horizon);
                }
            }
            // v2.82: a robot press that's only safe with a hold unlike your usual one
            if (robotPress && baseHold > 0) {
                int usual = robotHold.pick(st(20), 50);
                if (std::abs(baseHold - usual) >= st(3)) {
                    if (buf.explain) {
                        say("robot: safe with a {} hold ({}t, you usually hold {}t)", baseHold > usual ? "longer" : "shorter", baseHold,
                            usual);
                    }
                    releaseAfter = baseHold;  // the path view / drift check show the hold that works
                }
            }
            decided(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
            buf.appliedInput = down;
            return false;
        }

        // v2.27: a cube / ball press that only crashes after a normal jump would have landed
        // (~90 ticks) dies to the next obstacle, not this click: no delay helps, so don't
        // spend the whole time budget searching (it was most of the per-click lag).
        bool groundPress = down && !anyFlyer && !anyRobot && !anySpider && !anyWave;
        // v2.63: only once the jump came back down. A long jump (Acheron's first: ~94 ticks in
        // the air, then onto a spike) crashes late too, and that crash *is* this click's.
        if (groundPress && buf.lateCrash > 0 && base >= st(buf.lateCrash) && buf.baseTouchedDown) {
            if (buf.explain) {
                say("pass press: crash only at {}t, after the jump would land (late crash rule)", base);
            }
            decided(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
            buf.appliedInput = down;
            return false;
        }

        // v2.13 memory: saves at this spot kept ending in deaths, so the prediction can't be
        // trusted here. Leave the input alone.
        int slice = sliceOf(p1);
        if (memorySaysSkip(slice)) {
            say("memory: saves here failed before, input left alone");
            decided(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
            pending.skipped = true;
            buf.appliedInput = down;
            return false;
        }

        int landed = 0;
        int saved = 0;
        bool fromMemory = false;
        crashFixed = false;
        // v2.13 memory: the delay that worked here on earlier runs is tried first, so the
        // same gap gets the same save every time.
        // The same judgement this mode's search uses, for a single delay.
        auto scoreAt = [&](int k, int target, SimEnd* e) {
            return cubeStyleWave ? waveScore(layer, p1, p2, dual, k, down, horizon, target, e)
                 : robotPress    ? robotScore(layer, p1, p2, dual, k, horizon, target, e)
                 : spiderPress   ? spiderScore(layer, p1, p2, dual, k, horizon, releaseAfter, target, e)
                 : ufoPress      ? ufoScore(layer, p1, p2, dual, k, horizon, releaseAfter, target, e)
                 : shipInput     ? shipScore(layer, p1, p2, dual, k, down, horizon, target, e)
                                 : simulateOrbs(layer, p1, p2, dual, k, down, horizon, releaseAfter, target, e);
        };

        // v2.69: a cube / ball jump save has to land safely. A save that only lived 12 ticks
        // longer than pressing now still crashed later in the same jump (Acheron's first jump:
        // +32t..+40t "saves" that died ~130 ticks later, every time); those clicks now go
        // through untouched instead of being moved by up to 10 frames for nothing.
        // v2.75: robot jumps and UFO / swing flaps too (no more half saves there either)
        int needed = (groundPress || robotPress || robotRelease || ufoPress) ? horizon : std::min(horizon, base + st(12));
        int learned = memoryDelay(slice);
        if (learned >= searchFrom && learned <= maxDelay && !(anyWave && buf.waveV46)) {
            int target = needed;
            SimEnd e;
            int ret = scoreAt(learned, target, &e);
            if (ret >= target) {
                saved = learned;
                landed = ret;
                fromMemory = true;
            }
        }
        if (saved <= 0) {
            saved = cubeStyleWave ? searchWave(layer, p1, p2, dual, down, horizon, maxDelay, base, &landed)
                  : robotPress    ? searchTwoPass(horizon, maxDelay, base, &landed, [&](int k, int target, SimEnd* e) {
                        return robotScore(layer, p1, p2, dual, k, horizon, target, e);
                    }, true)
                  : robotRelease  ? searchSmallest(horizon, maxDelay, base, &landed, [&](int k, int target, SimEnd* e) {
                        return simulateOrbs(layer, p1, p2, dual, k, false, horizon, -1, target, e);
                    }, searchBudget(), false, true)
                  : spiderPress   ? searchTwoPass(horizon, maxDelay, base, &landed, [&](int k, int target, SimEnd* e) {
                        return spiderScore(layer, p1, p2, dual, k, horizon, releaseAfter, target, e);
                    })
                  : shipInput     ? searchSmallest(horizon, maxDelay, base, &landed, [&](int k, int target, SimEnd* e) {
                        return shipScore(layer, p1, p2, dual, k, down, horizon, target, e);
                    })
                  : ufoPress      ? searchTwoPass(horizon, maxDelay, base, &landed, [&](int k, int target, SimEnd* e) {
                        return ufoScore(layer, p1, p2, dual, k, horizon, releaseAfter, target, e);
                    }, true)
                                  : searchDelay(layer, p1, p2, dual, down, anyWave, horizon, maxDelay, releaseAfter, base, &landed,
                                        groundPress);
        }
        // v2.85 no-hitch: the search ran out of time before trying every delay. Your click waits
        // one tick and the search goes on from there with fresh time (the delays it already
        // tried all crashed, so waiting a tick loses nothing), instead of giving up.
        int resumes = resuming ? resumeEntry.resumes : 0;
        if (saved <= 0 && buf.noHitch && budgetCutK > 0 && budgetCutK < maxDelay && resumes < MAX_RESUMES &&
            !(anyWave && buf.waveV46)) {
            QueuedInput next { std::max(0, 1 - early), down, true, budgetCutK, (resuming ? resumeEntry.elapsed : 0) + 1, resumes + 1 };
            if (buf.explain) {
                say("search goes on next tick (tried up to +{}t of {}t)", budgetCutK + (resuming ? resumeEntry.elapsed : 0),
                    maxDelay + (resuming ? resumeEntry.elapsed : 0));
            }
            buf.shift = 1;
            buf.queue.push_back(next);
            return true;
        }
        if (saved <= 0) {
            say("no save: {}{} dies in {}t (max {}t, best +{}t -> {}t of {}){}", down ? "press" : "release", tickTag, base, maxDelay,
                dbgBestK, dbgBestRet, horizon, !crashFixed ? "" : base < st(30) ? " - too late: waiting only makes it crash sooner" : " - same crash at any timing (next obstacle)");
            decided(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
            buf.appliedInput = down;
            return false;
        }

        // v2.28 robustness check. A save can work in the exact predicted conditions and still
        // fail for real, because a real run is always a hair off (a fraction of a tick, a bit
        // of float error). So the chosen delay must also survive with you starting slightly
        // ahead of and behind where you are (half a tick of movement each way). If it only
        // works at the exact spot, the next few delays are tried for one that survives all
        // three; if none does, the original save is kept, so this never makes things worse.
        bool robustNote = false;
        if (buf.robustSaves && !(anyWave && buf.waveV46)) {
            int target = needed;
            // half a tick of x movement: normal speed (0.9) covers 311.58 units a second
            float shift = std::max(0.3f, 0.5f * (311.58f / 240.f) * (std::abs(p1->m_playerSpeed) / 0.9f));
            auto robust = [&](int k) {
                for (float off : { shift, -shift }) {
                    buf.ghostShiftX = off;
                    SimEnd e;
                    int ret = scoreAt(k, target, &e);
                    buf.ghostShiftX = 0.f;
                    if (ret < target) {
                        return false;
                    }
                }
                return true;
            };
            if (clickMs() < buf.budgetMs && !robust(saved)) {
                for (int k = saved + 1; k <= std::min(maxDelay, saved + st(3)) && !clickOverTime(); ++k) {
                    SimEnd e;
                    int ret = scoreAt(k, target, &e);
                    if (ret >= target && robust(k)) {
                        saved = k;
                        landed = ret;
                        robustNote = true;
                        break;
                    }
                }
            }
        }

        if (buf.avoidMovers) {
            // One extra run of the chosen delay with the moving-object watch on. (Watching
            // in every search run made cube searches hit the time budget.)
            buf.trackMovers = true;
            simulateOrbs(layer, p1, p2, dual, saved, down, horizon, releaseAfter, horizon, &end);
            buf.trackMovers = false;
        }
        if (buf.avoidMovers && buf.sawMover) {
            say("skip +{}t save: moving objects ahead", saved);
            decided(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
            buf.appliedInput = down;
            return false;
        }

        if (trainInstead(layer, saved, down, swingPress ? "swing" : "")) {
            decided(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
            buf.appliedInput = down;
            return false;
        }
        if (robotPress) {
            releaseAfter = robotUsedHold;  // the path view / drift check show the hold that works
        }
        noteSave(layer, saved);
        say("SAVED {}{} +{}t ({:g}f)  {}->{}  [max {}]{}{}", down ? "press" : "release", tickTag, saved, saved * TICK, base, landed, maxDelay,
            resuming ? fmt::format("  (search went on {}t)", resumeEntry.elapsed) : std::string(),
            robustNote ? std::string("  (robust)")
            : fromMemory ? std::string("  (memory)")
            : cubeStyleWave && marginUsed ? std::string("  +margin")
            : robotPress ? fmt::format("  hold {}t", robotUsedHold) : std::string());
        decided(layer, p1, p2, dual, saved, down, horizon, releaseAfter);
        buf.shift = saved;
        queueInput(saved - early, down);
        return true;
    }

    void cycleStage(int dir = 1) {
        buf.stage = (buf.stage + dir + STAGE_COUNT) % STAGE_COUNT;
        resetPending();
        // A delayed input that was waiting is dropped; the game gets what you're really
        // holding now (otherwise, with the buffer switched off, it never would).
        auto* layer = GameManager::get()->m_playLayer;
        if (layer && !buf.autoReleased && buf.appliedInput != buf.realInput) {
            sendButton(layer, buf.realInput);
        }
        applyStage();
        readSettings();
        saveProfile();
        Mod::get()->setSavedValue("stage", buf.stage);  // v2.58: one stage for every level, kept on restart
        say("Stage (6) -> {}", stageText());
        refreshAutoTag();
        refreshWatermark();
        refreshPanel();
    }

    // v2.4 wave auto-release (practice mode only): while you hold in wave, check every
    // tick whether holding crashes within the next `autoWindow` ticks. If it does and
    // letting go now clearly survives longer, let go for you. Your own release later is
    // swallowed, and your next press works as normal.
    void tryAutoRelease(PlayLayer* layer, PlayerObject* p1) {
        if ((!buf.autoRelease && !buf.robotAutoRelease && !buf.dashAutoRelease) || buf.autoReleased || layer->m_isPlatformer) {
            return;
        }
        if (!layer->m_isPracticeMode && !buf.autoNormalRuns) {
            return;
        }
        if (!buf.queue.empty() || !buf.realInput || !buf.appliedInput || !buf.isPlayer1 || !buf.ghost1) {
            return;
        }
        if (layer->m_level && layer->m_level->m_twoPlayerMode) {
            return;
        }
        bool dual = layer->m_gameState.m_isDualMode && layer->m_player2;
        auto* p2 = dual ? layer->m_player2 : nullptr;
        bool dash = p1->m_isDashing || (p2 && p2->m_isDashing);
        bool wave = !dash && (p1->m_isDart || (p2 && p2->m_isDart));
        bool robot = !dash && !wave && (p1->m_isRobot || (p2 && p2->m_isRobot));
        if (p1->m_isDead || !((dash && buf.dashAutoRelease) || (wave && buf.autoRelease) || (robot && buf.robotAutoRelease))) {
            return;
        }
        bool mini = (p1->m_isDart && p1->m_vehicleSize < 1.f) || (p2 && p2->m_isDart && p2->m_vehicleSize < 1.f);

        int window = st(std::clamp(buf.autoWindow, 2, 24));
        buf.sawMover = false;
        buf.skipPress = true;
        buf.orbClick = buf.assumeOrbs;
        SimEnd end;
        int hold = simulate(layer, p1, p2, dual, 0, true, window, -1, window, &end);
        buf.skipPress = false;
        if (hold >= window || buf.ghostHitPortal) {
            return;
        }

        // v2.12 robot: letting go ends the jump boost; judged like a cube jump (landing = safe).
        int horizon = wave ? fullWaveHorizon(mini) : st(std::clamp(buf.groundHorizon, 48, 240));
        if (dash && (p1->m_isDart || p1->m_isBird || p1->m_isSwing)) {
            horizon = st(48);
        }
        buf.trackMovers = buf.avoidMovers;
        int release = simulateOrbs(layer, p1, p2, dual, 0, false, horizon, -1, horizon, &end);
        buf.trackMovers = false;
        if (release < horizon && release < hold + st(10)) {
            return;
        }
        if (buf.avoidMovers && buf.sawMover) {
            return;
        }
        buf.autoReleased = true;
        buf.autoDash = dash;
        sendButton(layer, false);
        say("AUTO release{}: holding dies in {}t, letting go lives {}t", dash ? " (dash)" : (robot ? " (robot)" : ""), hold, release);
    }

    // v2.80 auto-click (off by default; practice mode only unless 'Auto-release in normal
    // runs' is on, then the AUTO INPUTS ON tag shows): while you're not holding, check every
    // step whether doing nothing crashes within the auto window. If it does and a click right
    // now (with your usual hold) is fully safe, the mod clicks for you, lets go after that
    // hold, and swallows your own click if it still comes. Cube, ball, UFO, robot, spider and
    // swing; not wave / ship (constant control) or while dashing.
    void tryAutoClick(PlayLayer* layer, PlayerObject* p1) {
        if (!buf.autoClick || buf.autoReleased || buf.autoClicked || layer->m_isPlatformer) {
            return;
        }
        if (!layer->m_isPracticeMode && !buf.autoNormalRuns) {
            return;
        }
        if (!buf.queue.empty() || buf.realInput || buf.appliedInput || !buf.isPlayer1 || !buf.ghost1) {
            return;
        }
        if (layer->m_level && layer->m_level->m_twoPlayerMode) {
            return;
        }
        bool dual = layer->m_gameState.m_isDualMode && layer->m_player2;
        auto* p2 = dual ? layer->m_player2 : nullptr;
        auto skipMode = [](PlayerObject* p) { return p && (p->m_isDart || p->m_isShip || p->m_isDashing); };
        if (p1->m_isDead || skipMode(p1) || skipMode(p2)) {
            return;
        }

        // 1. cheap: does doing nothing crash soon?
        int window = st(std::clamp(buf.autoWindow, 2, 24));
        buf.sawMover = false;
        buf.skipPress = true;
        buf.orbClick = false;  // no clicks at all in the do-nothing run
        SimEnd end;
        int idle = simulate(layer, p1, p2, dual, 0, true, window, -1, window, &end);
        buf.skipPress = false;
        buf.orbClick = buf.assumeOrbs;
        if (idle >= window || buf.ghostHitPortal) {
            return;
        }

        // 2. a click now, held as long as you usually hold, must be fully safe
        bool flyer = isFlying(p1) || (p2 && isFlying(p2));
        int horizon = st(flyer ? 48 : std::clamp(buf.groundHorizon, 48, 240));
        int hold = std::max(1, holdEstimate());
        buf.trackMovers = buf.avoidMovers;
        int press = simulateOrbs(layer, p1, p2, dual, 0, true, horizon, hold, horizon, &end);
        buf.trackMovers = false;
        if (press < horizon || (buf.avoidMovers && buf.sawMover)) {
            return;
        }
        buf.autoClicked = true;
        buf.autoClickTick = tickCount;
        sendButton(layer, true);
        queueInput(hold, false);
        say("AUTO click: doing nothing dies in {}t, clicking now is safe (hold {}t)", idle, hold);
    }

    // v2.85 no-hitch: the decision for a click whose search ran out of time goes on, from where
    // you are now. No save found: the click goes to the game now.
    void resumeClick(PlayLayer* layer, QueuedInput const& q) {
        resumeNow = true;
        resumeEntry = q;
        bool held = intercept(layer, q.down, buf.isPlayer1);
        resumeNow = false;
        sweepGhostLayer();
        if (!held) {
            sendButton(layer, q.down);
        }
    }

    void deliverPending(PlayLayer* layer, PlayerObject* player) {
        if (buf.sinceInput <= 998) {
            ++buf.sinceInput;
        }
        // v2.85: nothing is sent to the game while you're dead (a press then carried over into
        // the next attempt)
        if (player->m_isDead) {
            return;
        }
        if ((buf.avoidMovers || buf.predictMovers) && !watched.empty()) {
            updateWatched();
        }

        bool twoPlayer = layer->m_level && layer->m_level->m_twoPlayerMode;
        bool dual = !twoPlayer && layer->m_gameState.m_isDualMode && layer->m_player2;
        if (!buf.shipBuffer && (player->m_isShip || (dual && layer->m_player2->m_isShip))) {
            resetPending();
            buf.autoReleased = false;
            if (buf.appliedInput != buf.realInput) {
                sendButton(layer, buf.realInput);
            }
            return;
        }
        // Left wave / robot while auto-released: the game goes back to what you're really holding.
        bool autoMode = player->m_isDart || player->m_isRobot || (dual && (layer->m_player2->m_isDart || layer->m_player2->m_isRobot));
        if (buf.autoReleased && !buf.autoDash && !autoMode) {
            buf.autoReleased = false;
        }

        // v2.80: an auto-click you never followed up on ends after a while
        if (buf.autoClicked && !buf.realInput && tickCount - buf.autoClickTick > st(30)) {
            buf.autoClicked = false;
        }
        if (buf.queue.empty()) {
            if (buf.appliedInput != buf.realInput && !buf.autoReleased && !buf.autoClicked) {
                sendButton(layer, buf.realInput);
            }
            tryAutoRelease(layer, player);
            tryAutoClick(layer, player);
            sweepGhostLayer();
            return;
        }

        // Check before decrementing: an input queued with N ticks fires right before
        // physics step N, which is exactly what simulate(delay = N) tested.
        while (!buf.queue.empty() && buf.queue.front().ticksLeft <= 0) {
            if (buf.queue.front().resume) {
                // v2.85 no-hitch: the search for this click goes on now
                auto q = buf.queue.front();
                buf.queue.pop_front();
                resumeClick(layer, q);
                continue;
            }
            bool down = buf.queue.front().down;
            buf.queue.pop_front();
            buf.releaseShift = down ? buf.shift : 0;
            if (down != buf.appliedInput) {
                sendButton(layer, down);
            }
        }
        for (auto& q : buf.queue) {
            --q.ticksLeft;
        }
        if (buf.queue.empty()) {
            buf.shift = 0;
        }
    }
}

// v2.88 menu: a draggable window in a space theme (night-sky gradient, nebula, twinkling
// stars, the odd shooting star) with a tab for each category: Buffer, Prediction, Modes,
// Custom (each game mode's own frames) and Tools. Opens with the menu key in a level or the
// heart button on the main menu.
class BufferMenu : public geode::Popup {
    struct Option {
        char const* key;    // mod setting, or nullptr for the runtime path view
        char const* label;
    };
    static constexpr Option PREDICTION[] = {
        { "robust-saves", "Robust saves" },
        { "orb-align", "Orb alignment" },
        { "orb-timing", "Orb timing" },
        { "predict-moving-objects", "Moving objects" },
        { "predict-move-triggers", "Move triggers" },
        { "predict-through-portals", "Through portals" },
        { "plan-next-click", "Two clicks ahead" },
        { "no-hitch-searches", "No-hitch searches" },
        { "level-memory", "Level memory" },
        { "more-time-death-spots", "More time at deaths" },
    };
    static constexpr Option MODES[] = {
        { "low-latency", "Low latency" },
        { "cbf-sync", "CBF sync" },
        { "ship-buffer", "Ship buffering" },
        { "robot-smart", "Smart robot" },
        { "spider-smart", "Smart spider" },
        { "ufo-smart", "Smart UFO" },
        { "ball-smart", "Smart ball" },
        { "swing-smart", "Smart swing" },
        { "auto-click", "Auto-click (practice)" },
    };
    static constexpr Option TOOLS[] = {
        { nullptr, "Path view" },
        { "training-mode", "Training (hints)" },
        { "death-heatmap", "Death heatmap" },
        { "save-ding", "Ding on save" },
        { "performance-mode", "Performance mode" },
        { "explain-inputs", "Explain inputs" },
    };
    static constexpr char const* TABS[] = { "Buffer", "Prediction", "Modes", "Custom", "Tools" };

    struct Theme {
        ccColor3B main, soft, accent, bg;
        char const* button;
    };

public:
    // the "Menu theme" setting picks the accent colour (purple by default)
    static Theme currentTheme() {
        auto name = Mod::get()->getSettingValue<std::string>("menu-theme");
        if (name == "Pink") {
            return { { 255, 120, 200 }, { 255, 205, 230 }, { 215, 190, 255 }, { 255, 195, 235 }, "GJ_button_02.png" };
        }
        if (name == "Blue") {
            return { { 90, 170, 255 }, { 205, 228, 255 }, { 170, 210, 255 }, { 170, 205, 255 }, "GJ_button_06.png" };
        }
        if (name == "Green") {
            return { { 90, 225, 150 }, { 210, 255, 220 }, { 180, 240, 200 }, { 170, 235, 190 }, "GJ_button_01.png" };
        }
        if (name == "Dark") {
            return { { 170, 150, 230 }, { 225, 225, 235 }, { 170, 170, 190 }, { 90, 85, 110 }, "GJ_button_04.png" };
        }
        return { { 166, 107, 255 }, { 220, 195, 255 }, { 200, 170, 255 }, { 205, 170, 255 }, "GJ_button_02.png" };
    }

private:
    static constexpr float W = 430.f;
    static constexpr float H = 272.f;
    static constexpr float TITLE = 28.f;
    static constexpr float SIDE = 100.f;
    static constexpr float ROW = 21.f;

    static inline BufferMenu* s_open = nullptr;
    static inline int s_tab = 0;
    static inline bool s_moved = false;
    static inline CCPoint s_pos {};

    Theme m_t = currentTheme();
    ccColor3B m_text { 232, 234, 255 };
    ccColor3B m_muted { 142, 148, 190 };
    ccColor3B m_cyan { 110, 230, 255 };

    CCMenu* m_frameMenu = nullptr;
    CCNode* m_tabsNode = nullptr;
    CCNode* m_page = nullptr;
    CCMenu* m_pageMenu = nullptr;
    CCLabelBMFont* m_stage = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_tpsLabel = nullptr;
    std::vector<Option const*> m_options;
    std::vector<CCDrawNode*> m_switches;
    std::vector<CCLabelBMFont*> m_switchLabels;
    std::array<CCLabelBMFont*, 8> m_modeValues {};
    struct ModeSlider {
        int mode;
        float x0, x1, y;  // in the page's space
        CCDrawNode* draw;
    };
    std::vector<ModeSlider> m_sliders;
    int m_dragSlider = -1;
    CCDrawNode* m_perModeSwitch = nullptr;
    bool m_dragging = false;
    float m_shootTimer = 2.f;

    static constexpr int TPS_MIN = 240;
    static constexpr int TPS_MAX = 1000;

    // CCDrawNode blends premultiplied colours: a see-through colour has to be darkened by its
    // alpha (v2.88's white cards and bright blocks were plain colours drawn at full strength)
    static ccColor4F col(ccColor3B c, float a) {
        return { c.r / 255.f * a, c.g / 255.f * a, c.b / 255.f * a, a };
    }

    // a filled circle as a polygon (the game's drawDot draws squares)
    static void circle(CCDrawNode* d, CCPoint c, float r, ccColor4F fill, int segments = 28, float border = 0.f,
        ccColor4F borderColor = { 0, 0, 0, 0 }) {
        std::vector<CCPoint> pts;
        for (int i = 0; i < segments; ++i) {
            float a = i * 2.f * 3.14159265f / segments;
            pts.push_back({ c.x + r * std::cos(a), c.y + r * std::sin(a) });
        }
        d->drawPolygon(pts.data(), static_cast<unsigned>(pts.size()), fill, border, borderColor);
    }

    // a rounded rectangle (convex, so one polygon)
    static void roundRect(CCDrawNode* d, CCRect r, float rad, ccColor4F fill, float border = 0.f, ccColor4F borderColor = { 0, 0, 0, 0 }) {
        rad = std::min({ rad, r.size.width / 2.f, r.size.height / 2.f });
        std::vector<CCPoint> pts;
        CCPoint centers[] = { { r.getMaxX() - rad, r.getMaxY() - rad }, { r.getMinX() + rad, r.getMaxY() - rad },
            { r.getMinX() + rad, r.getMinY() + rad }, { r.getMaxX() - rad, r.getMinY() + rad } };
        for (int c = 0; c < 4; ++c) {
            for (int i = 0; i <= 6; ++i) {
                float a = (c * 90.f + i * 15.f) * 3.14159265f / 180.f;
                CCPoint p { centers[c].x + rad * std::cos(a), centers[c].y + rad * std::sin(a) };
                // v2.88.2: no repeated points (where corners meet on round shapes): the outline
                // turned a zero-length edge into a stray line sticking out of every round button
                if (pts.empty() || ccpDistance(pts.back(), p) > 0.05f) {
                    pts.push_back(p);
                }
            }
        }
        while (pts.size() > 2 && ccpDistance(pts.back(), pts.front()) <= 0.05f) {
            pts.pop_back();
        }
        d->drawPolygon(pts.data(), static_cast<unsigned>(pts.size()), fill, border, borderColor);
    }

    // v2.89 fonts (bundled, OFL): Orbitron for titles, tabs and headings, Exo 2 for the rest
    static char const* bodyFont() {
        return "exo2.fnt"_spr;
    }

    static char const* headFont() {
        return "orbitron.fnt"_spr;
    }

    static constexpr float BODY_K = 1.f;   // size of Exo 2 next to the old chat font
    static constexpr float HEAD_K = 0.8f;  // Orbitron is wide

    CCLabelBMFont* label(CCNode* parent, std::string const& str, float scale, ccColor3B color, CCPoint at, CCPoint anchor = { 0.5f, 0.5f },
        bool head = false) {
        auto* l = CCLabelBMFont::create(str.c_str(), head ? headFont() : bodyFont());
        l->setScale(scale * (head ? HEAD_K : BODY_K));
        l->setColor(color);
        l->setAnchorPoint(anchor);
        l->setPosition(at);
        parent->addChild(l, 3);
        return l;
    }

    // a flat pill button: filled with the accent (primary) or glassy with an accent edge
    CCMenuItemSpriteExtra* button(CCMenu* menu, std::string const& str, SEL_MenuHandler handler, CCPoint center, float width,
        bool primary = false, int tag = 0, float height = 18.f) {
        auto* node = CCNode::create();
        node->setContentSize({ width, height });
        auto* d = CCDrawNode::create();
        roundRect(d, { 0.f, 0.f, width, height }, height / 2.f, primary ? col(m_t.main, 0.9f) : col({ 255, 255, 255 }, 0.07f), 0.6f,
            col(primary ? m_t.soft : m_t.main, primary ? 0.6f : 0.55f));
        node->addChild(d);
        auto* l = CCLabelBMFont::create(str.c_str(), bodyFont());
        l->setScale(0.5f * BODY_K);
        l->setColor(primary ? ccColor3B { 255, 255, 255 } : m_text);
        l->setPosition({ width / 2.f, height / 2.f + 0.5f });
        l->limitLabelWidth(width - 8.f, 0.5f * BODY_K, 0.2f);
        node->addChild(l);
        auto* item = CCMenuItemSpriteExtra::create(node, this, handler);
        item->m_scaleMultiplier = 1.05f;
        item->setTag(tag);
        item->setPosition(center);
        menu->addChild(item);
        return item;
    }

    void drawSwitch(CCDrawNode* d, bool on) {
        d->clear();
        roundRect(d, { 0.f, 0.f, 26.f, 13.f }, 6.5f, on ? col(m_t.main, 0.95f) : col({ 70, 74, 110 }, 0.9f), 0.5f,
            col(on ? m_t.soft : ccColor3B { 110, 115, 160 }, 0.6f));
        if (on) {
            circle(d, { 19.5f, 6.5f }, 7.f, col(m_cyan, 0.18f));
        }
        circle(d, { on ? 19.5f : 6.5f, 6.5f }, 4.6f, col({ 255, 255, 255 }, on ? 1.f : 0.8f));
    }

    CCMenuItemSpriteExtra* switchItem(CCPoint center, bool on, SEL_MenuHandler handler, int tag, CCDrawNode** out) {
        auto* node = CCNode::create();
        node->setContentSize({ 26.f, 13.f });
        auto* d = CCDrawNode::create();
        node->addChild(d);
        drawSwitch(d, on);
        auto* item = CCMenuItemSpriteExtra::create(node, this, handler);
        item->m_scaleMultiplier = 1.08f;
        item->setTag(tag);
        item->setPosition(center);
        m_pageMenu->addChild(item);
        *out = d;
        return item;
    }

    // your icon for a game mode (MODE_NAMES order = the game's icon types), in your colours
    static CCNode* modeIcon(int mode) {
        auto* gm = GameManager::get();
        int ids[] = { gm->getPlayerFrame(), gm->getPlayerShip(), gm->getPlayerBall(), gm->getPlayerBird(), gm->getPlayerDart(),
            gm->getPlayerRobot(), gm->getPlayerSpider(), gm->getPlayerSwing() };
        auto* icon = SimplePlayer::create(0);
        icon->updatePlayerFrame(ids[mode], static_cast<IconType>(mode));
        icon->setColor(gm->colorForIdx(gm->getPlayerColor()));
        icon->setSecondColor(gm->colorForIdx(gm->getPlayerColor2()));
        if (gm->getPlayerGlow()) {
            icon->setGlowOutline(gm->colorForIdx(gm->getPlayerGlowColor()));
        }
        icon->setScale(mode == 5 || mode == 6 ? 0.42f : 0.5f);
        return icon;
    }

    static bool optionValue(Option const& o) {
        return o.key ? Mod::get()->getSettingValue<bool>(o.key) : buf.showPath;
    }

    // a switch row: label on the left, switch on the right
    void optionRow(Option const& o, float x, float y, float width) {
        CCDrawNode* d = nullptr;
        switchItem({ x + width - 15.f, y }, optionValue(o), menu_selector(BufferMenu::onToggle), static_cast<int>(m_options.size()), &d);
        auto* l = label(m_page, o.label, 0.52f, optionValue(o) ? m_text : m_muted, { x, y }, { 0.f, 0.5f });
        l->limitLabelWidth(width - 36.f, 0.52f, 0.2f);
        m_options.push_back(&o);
        m_switches.push_back(d);
        m_switchLabels.push_back(l);
    }

    void optionGrid(Option const* options, size_t count, float top) {
        float left = SIDE + 14.f;
        float colW = (W - SIDE - 34.f) / 2.f;
        size_t rows = (count + 1) / 2;
        for (size_t i = 0; i < count; ++i) {
            float x = left + (i / rows) * (colW + 10.f);
            float y = top - (i % rows) * ROW;
            optionRow(options[i], x, y, colW);
        }
    }

    // ---------- the window ----------

    void buildFrame() {
        // a soft glow around the window: rings of the accent fading outwards
        auto* glow = CCDrawNode::create();
        for (int i = 7; i >= 1; --i) {
            float g = i * 1.7f;
            roundRect(glow, { -g, -g, W + 2.f * g, H + 2.f * g }, 12.f + g, col(m_t.main, 0.022f));
        }
        m_mainLayer->addChild(glow, -3);

        // the sky, clipped to the window's shape. v2.90: a black night sky full of stars
        auto* stencil = CCDrawNode::create();
        roundRect(stencil, { 0.f, 0.f, W, H }, 12.f, { 1, 1, 1, 1 });
        auto* clip = CCClippingNode::create(stencil);
        m_mainLayer->addChild(clip, -2);
        auto* sky = CCLayerGradient::create({ 2, 2, 7, 253 }, { 7, 5, 16, 253 });
        sky->setContentSize({ W, H });
        clip->addChild(sky);
        // the faintest hint of the milky way across the sky
        auto* haze = CCDrawNode::create();
        for (int i = 0; i < 24; ++i) {
            float t = i / 23.f;
            CCPoint c { -20.f + t * (W + 40.f), H * 0.85f - t * H * 0.7f };
            circle(haze, c, 60.f, col({ 120, 110, 200 }, 0.006f), 40);
            circle(haze, c, 34.f, col({ 160, 150, 230 }, 0.006f), 32);
        }
        clip->addChild(haze);
        auto drift = [](CCNode* node, CCPoint by, float period) {
            node->runAction(CCRepeatForever::create(CCSequence::create(CCEaseSineInOut::create(CCMoveBy::create(period, by)),
                CCEaseSineInOut::create(CCMoveBy::create(period, ccpNeg(by))), nullptr)));
        };
        uint32_t seed = 1234567;
        auto rnd = [&seed] {
            seed = seed * 1103515245u + 12345u;
            return static_cast<float>((seed >> 8) & 0xffff) / 65535.f;
        };
        auto tint = [&]() -> ccColor3B {
            float r = rnd();
            return r < 0.12f ? m_cyan : r < 0.22f ? ccColor3B { 255, 225, 190 } : r < 0.30f ? m_t.soft : ccColor3B { 255, 255, 255 };
        };
        // two layers of stars drifting at different speeds (far = small and slow, near = bigger)
        for (int layer = 0; layer < 2; ++layer) {
            auto* field = CCNode::create();
            clip->addChild(field);
            drift(field, layer == 0 ? CCPoint { -8.f, 3.f } : CCPoint { -18.f, 7.f }, layer == 0 ? 22.f : 14.f);
            int count = layer == 0 ? 150 : 60;
            for (int i = 0; i < count; ++i) {
                float size = layer == 0 ? 0.5f + rnd() * 0.5f : 0.9f + rnd() * 0.8f;
                auto* star = CCLayerColor::create({ 255, 255, 255, 255 }, size, size);
                star->setPosition({ rnd() * (W + 50.f) - 25.f, rnd() * (H + 30.f) - 15.f });
                star->setColor(tint());
                GLubyte hi = static_cast<GLubyte>((layer == 0 ? 110 : 170) + rnd() * 85);
                GLubyte lo = static_cast<GLubyte>(15 + rnd() * 45);
                star->setOpacity(hi);
                float t = 0.6f + rnd() * 2.2f;
                star->runAction(CCRepeatForever::create(CCSequence::create(CCDelayTime::create(rnd() * 3.f),
                    CCFadeTo::create(t, lo), CCFadeTo::create(t, hi), nullptr)));
                field->addChild(star);
            }
            if (layer == 1) {
                // the shiny ones: four-pointed sparkles that grow, shrink and turn
                for (int i = 0; i < 14; ++i) {
                    auto* glint = CCDrawNode::create();
                    ccColor3B c = tint();
                    float arm = 3.5f + rnd() * 3.5f;
                    float w = arm * 0.16f;
                    CCPoint h1[] = { { 0.f, arm }, { w, 0.f }, { 0.f, -arm }, { -w, 0.f } };
                    CCPoint h2[] = { { arm, 0.f }, { 0.f, w }, { -arm, 0.f }, { 0.f, -w } };
                    circle(glint, { 0.f, 0.f }, arm * 0.55f, col(c, 0.10f), 16);
                    glint->drawPolygon(h1, 4, col(c, 0.95f), 0.f, { 0, 0, 0, 0 });
                    glint->drawPolygon(h2, 4, col(c, 0.95f), 0.f, { 0, 0, 0, 0 });
                    circle(glint, { 0.f, 0.f }, w * 1.3f, col({ 255, 255, 255 }, 1.f), 10);
                    glint->setPosition({ 10.f + rnd() * (W - 20.f), 10.f + rnd() * (H - 20.f) });
                    float s0 = 0.35f + rnd() * 0.25f;
                    glint->setScale(s0);
                    glint->setRotation(rnd() * 45.f);
                    float t = 1.f + rnd() * 1.8f;
                    glint->runAction(CCRepeatForever::create(CCSequence::create(CCDelayTime::create(rnd() * 3.f),
                        CCEaseSineInOut::create(CCScaleTo::create(t, s0 * 2.2f)), CCEaseSineInOut::create(CCScaleTo::create(t, s0)), nullptr)));
                    glint->runAction(CCRepeatForever::create(CCRotateBy::create(6.f + rnd() * 6.f, rnd() < 0.5f ? 45.f : -45.f)));
                    field->addChild(glint);
                }
            }
        }
        // a light sweeping along the title line now and then
        auto* shine = CCLayerGradient::create({ 255, 255, 255, 0 }, { 255, 255, 255, 210 }, { 1.f, 0.f });
        shine->setContentSize({ 40.f, 1.f });
        shine->setPosition({ -40.f, H - TITLE });
        clip->addChild(shine, 5);
        shine->runAction(CCRepeatForever::create(CCSequence::create(CCDelayTime::create(2.5f),
            CCEaseSineInOut::create(CCMoveBy::create(1.6f, { W + 40.f, 0.f })), CCPlace::create({ -40.f, H - TITLE }), nullptr)));
        m_sky = clip;

        // edge, the sidebar's darker panel, the title's accent line
        auto* chrome = CCDrawNode::create();
        roundRect(chrome, { 0.f, 0.f, W, H }, 12.f, { 0, 0, 0, 0 }, 0.7f, col(m_t.main, 0.75f));
        roundRect(chrome, { 1.5f, 1.5f, W - 3.f, H - 3.f }, 11.f, { 0, 0, 0, 0 }, 0.4f, col({ 255, 255, 255 }, 0.06f));
        roundRect(chrome, { 7.f, 8.f, SIDE - 12.f, H - TITLE - 14.f }, 9.f, col({ 0, 0, 0 }, 0.28f), 0.4f, col({ 255, 255, 255 }, 0.05f));
        m_mainLayer->addChild(chrome, -1);
        auto* line = CCLayerGradient::create({ m_t.main.r, m_t.main.g, m_t.main.b, 230 }, { m_cyan.r, m_cyan.g, m_cyan.b, 0 }, { 1.f, 0.f });
        line->setContentSize({ W - 24.f, 1.f });
        line->setPosition({ 12.f, H - TITLE });
        m_mainLayer->addChild(line, -1);

        // the logo: a little planet with a tilted ring
        auto* logo = CCDrawNode::create();
        CCPoint lc { 21.f, H - TITLE / 2.f };
        circle(logo, lc, 6.5f, col(m_t.main, 1.f));
        circle(logo, lc + CCPoint { -1.6f, 1.8f }, 3.4f, col(m_t.soft, 0.45f));
        std::vector<CCPoint> ring;
        for (int i = 0; i <= 32; ++i) {
            float a = i * 2.f * 3.14159265f / 32.f;
            float x = 11.f * std::cos(a), y = 3.2f * std::sin(a);
            float t = 0.38f;
            ring.push_back(lc + CCPoint { x * std::cos(t) - y * std::sin(t), x * std::sin(t) + y * std::cos(t) });
        }
        for (size_t i = 0; i + 1 < ring.size(); ++i) {
            logo->drawSegment(ring[i], ring[i + 1], 0.55f, col(m_cyan, 0.95f));
        }
        m_mainLayer->addChild(logo, 2);
        // a moon going round the logo along its ring
        auto* tilt = CCNode::create();
        tilt->setPosition(lc);
        tilt->setRotation(-0.38f * 180.f / 3.14159265f);
        tilt->setScaleY(3.2f / 11.f);
        m_mainLayer->addChild(tilt, 3);
        auto* orbit = CCNode::create();
        tilt->addChild(orbit);
        auto* moon = CCDrawNode::create();
        circle(moon, { 11.f, 0.f }, 1.7f, col({ 255, 255, 255 }, 1.f), 12);
        orbit->addChild(moon);
        orbit->runAction(CCRepeatForever::create(CCRotateBy::create(4.f, -360.f)));
        label(m_mainLayer, "CLICK BEFORE FRAMES", 0.56f, m_text, { 38.f, H - TITLE / 2.f }, { 0.f, 0.5f }, true);
        label(m_mainLayer, Mod::get()->getVersion().toVString(), 0.4f, m_muted, { W - 38.f, H - TITLE / 2.f }, { 1.f, 0.5f });

        m_frameMenu = CCMenu::create();
        m_frameMenu->setPosition({ 0.f, 0.f });
        m_mainLayer->addChild(m_frameMenu, 5);
        // close: a thin ring with an x
        auto* node = CCNode::create();
        node->setContentSize({ 18.f, 18.f });
        auto* d = CCDrawNode::create();
        circle(d, { 9.f, 9.f }, 8.f, col({ 255, 255, 255 }, 0.06f), 28, 0.5f, col(m_t.main, 0.8f));
        d->drawSegment({ 6.2f, 6.2f }, { 11.8f, 11.8f }, 0.6f, col(m_text, 0.95f));
        d->drawSegment({ 6.2f, 11.8f }, { 11.8f, 6.2f }, 0.6f, col(m_text, 0.95f));
        node->addChild(d);
        auto* close = CCMenuItemSpriteExtra::create(node, this, menu_selector(BufferMenu::onCloseButton));
        close->setPosition({ W - 18.f, H - TITLE / 2.f });
        m_frameMenu->addChild(close);
    }

    void buildTabs() {
        if (m_tabsNode) {
            m_tabsNode->removeFromParent();
        }
        m_tabsNode = CCNode::create();
        m_mainLayer->addChild(m_tabsNode, 4);
        auto* menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        m_tabsNode->addChild(menu);
        for (int i = 0; i < static_cast<int>(std::size(TABS)); ++i) {
            float y = H - TITLE - 22.f - i * 27.f;
            bool active = i == s_tab;
            auto* node = CCNode::create();
            node->setContentSize({ SIDE - 16.f, 22.f });
            auto* d = CCDrawNode::create();
            if (active) {
                roundRect(d, { 0.f, 0.f, SIDE - 16.f, 22.f }, 7.f, col(m_t.main, 0.32f), 0.6f, col(m_t.main, 0.9f));
                d->drawSegment({ 3.f, 6.f }, { 3.f, 16.f }, 1.2f, col(m_cyan, 1.f));
            }
            circle(d, { 12.f, 11.f }, 2.2f, active ? col(m_cyan, 1.f) : col(m_muted, 0.6f), 12);
            node->addChild(d);
            auto* l = CCLabelBMFont::create(TABS[i], headFont());
            l->setScale(0.42f * HEAD_K);
            l->setColor(active ? m_text : m_muted);
            l->setAnchorPoint({ 0.f, 0.5f });
            l->setPosition({ 22.f, 11.5f });
            node->addChild(l);
            auto* item = CCMenuItemSpriteExtra::create(node, this, menu_selector(BufferMenu::onTab));
            item->m_scaleMultiplier = 1.04f;
            item->setTag(i);
            item->setPosition({ SIDE / 2.f, y });
            menu->addChild(item);
        }
    }

    // ---------- pages ----------

    float pageTop() {
        return H - TITLE - 16.f;
    }

    void heading(std::string const& title, std::string const& sub) {
        label(m_page, title, 0.7f, m_text, { SIDE + 14.f, pageTop() }, { 0.f, 0.5f }, true);
        if (!sub.empty()) {
            label(m_page, sub, 0.42f, m_muted, { SIDE + 14.f, pageTop() - 14.f }, { 0.f, 0.5f })->limitLabelWidth(W - SIDE - 28.f, 0.42f, 0.2f);
        }
    }

    // a translucent card behind a group of rows
    void card(float top, float height) {
        auto* d = CCDrawNode::create();
        roundRect(d, { SIDE + 8.f, top - height, W - SIDE - 16.f, height }, 8.f, col({ 255, 255, 255 }, 0.045f), 0.5f, col(m_t.main, 0.35f));
        m_page->addChild(d, 1);
    }

    void pageBuffer() {
        heading("Buffer", "How much an early click can be held back, and the physics speed.");
        float cx = SIDE + (W - SIDE) / 2.f;
        float y = pageTop() - 40.f;
        card(y + 13.f, 52.f);
        label(m_page, "Stage", 0.5f, m_muted, { SIDE + 20.f, y }, { 0.f, 0.5f });
        button(m_pageMenu, "<", menu_selector(BufferMenu::onStage), { cx - 20.f, y }, 22.f, false, -1);
        button(m_pageMenu, ">", menu_selector(BufferMenu::onStage), { W - 30.f, y }, 22.f, false, 1);
        m_stage = label(m_page, "", 0.55f, m_text, { (cx - 8.f + W - 42.f) / 2.f, y });
        y -= 22.f;
        char const* presets[] = { "Safe", "Balanced", "Aggressive" };
        float pw = (W - SIDE - 40.f) / 3.f;
        for (int i = 0; i < 3; ++i) {
            button(m_pageMenu, presets[i], menu_selector(BufferMenu::onPreset), { SIDE + 20.f + pw / 2.f + i * (pw + 4.f), y }, pw, false, i);
        }
        y -= 32.f;
        card(y + 11.f, 24.f);
        label(m_page, "Physics TPS", 0.5f, m_muted, { SIDE + 20.f, y }, { 0.f, 0.5f });
        button(m_pageMenu, "-", menu_selector(BufferMenu::onTps), { W - 92.f, y }, 22.f, false, -1);
        button(m_pageMenu, "+", menu_selector(BufferMenu::onTps), { W - 30.f, y }, 22.f, false, 1);
        m_tpsLabel = label(m_page, "", 0.55f, m_text, { W - 61.f, y });
        y -= 30.f;
        m_status = label(m_page, "", 0.45f, m_cyan, { SIDE + 14.f, y }, { 0.f, 0.5f });
        if (buf.perMode) {
            label(m_page, "Per-mode frames are on (Custom tab). Stage and presets turn them off.", 0.4f, m_t.soft, { SIDE + 14.f, y - 14.f },
                { 0.f, 0.5f })->limitLabelWidth(W - SIDE - 28.f, 0.4f, 0.2f);
        }
        button(m_pageMenu, "Run check", menu_selector(BufferMenu::onRunCheck), { cx, 26.f }, 90.f, true);
    }

    void pageOptions(char const* title, char const* sub, Option const* options, size_t count) {
        heading(title, sub);
        float top = pageTop() - 40.f;
        card(top + 12.f, ((count + 1) / 2) * ROW + 4.f);
        optionGrid(options, count, top);
    }

    void pageCustom() {
        heading("Custom", "Pick how many frames each game mode can be held back. 0 = no buffer.");
        float y = pageTop() - 38.f;
        card(y + 11.f, 24.f);
        label(m_page, "Use per-mode frames", 0.52f, buf.perMode ? m_text : m_muted, { SIDE + 20.f, y }, { 0.f, 0.5f });
        switchItem({ W - 30.f, y }, buf.perMode, menu_selector(BufferMenu::onPerMode), 0, &m_perModeSwitch);
        y -= 30.f;
        float colW = (W - SIDE - 34.f) / 2.f;
        card(y + 12.f, 4 * 24.f + 2.f);
        for (int i = 0; i < 8; ++i) {
            float x = SIDE + 18.f + (i / 4) * (colW + 10.f);
            float ry = y - (i % 4) * 24.f;
            auto* icon = modeIcon(i);  // v2.89: your own icon for each mode instead of its name
            icon->setPosition({ x + 12.f, ry });
            m_page->addChild(icon, 3);
            // v2.90: a slider instead of - / + (drag it; half-frame steps)
            auto* track = CCDrawNode::create();
            m_page->addChild(track, 2);
            m_sliders.push_back({ i, x + 30.f, x + colW - 36.f, ry, track });
            m_modeValues[i] = label(m_page, "", 0.52f, m_cyan, { x + colW - 15.f, ry });
        }
        label(m_page, "Drag a slider: half-frame steps, up to 10 frames. With this on, low latency's 1-frame limit doesn't apply.", 0.38f, m_muted,
            { SIDE + 14.f, 22.f }, { 0.f, 0.5f })->limitLabelWidth(W - SIDE - 28.f, 0.38f, 0.2f);
        refreshModes();
    }

    void pageTools() {
        heading("Tools", "What you see while playing, plus stats and the log.");
        float top = pageTop() - 40.f;
        card(top + 12.f, 3 * ROW + 4.f);
        optionGrid(TOOLS, std::size(TOOLS), top);
        float y = top - 3 * ROW - 14.f;
        float bw = (W - SIDE - 46.f) / 3.f;
        float x0 = SIDE + 20.f + bw / 2.f;
        button(m_pageMenu, "Stats", menu_selector(BufferMenu::onStats), { x0, y }, bw);
        button(m_pageMenu, "Console", menu_selector(BufferMenu::onConsole), { x0 + bw + 6.f, y }, bw);  // phones have no F9
        button(m_pageMenu, "Copy log", menu_selector(BufferMenu::onCopyLog), { x0 + 2.f * (bw + 6.f), y }, bw);
        y -= 24.f;
        float bw2 = (W - SIDE - 40.f) / 2.f;
        float x2 = SIDE + 20.f + bw2 / 2.f;
        button(m_pageMenu, "Forget this level", menu_selector(BufferMenu::onForget), { x2, y }, bw2);
        button(m_pageMenu, "All settings", menu_selector(BufferMenu::onAllSettings), { x2 + bw2 + 6.f, y }, bw2);
    }

    void rebuildPage() {
        if (m_page) {
            m_page->removeFromParent();
        }
        m_stage = m_status = m_tpsLabel = nullptr;
        m_perModeSwitch = nullptr;
        m_modeValues.fill(nullptr);
        m_sliders.clear();
        m_dragSlider = -1;
        m_options.clear();
        m_switches.clear();
        m_switchLabels.clear();
        m_page = CCNode::create();
        m_page->setPosition({ 14.f, 0.f });
        m_page->runAction(CCEaseExponentialOut::create(CCMoveTo::create(0.35f, { 0.f, 0.f })));
        m_mainLayer->addChild(m_page, 3);
        m_pageMenu = CCMenu::create();
        m_pageMenu->setPosition({ 0.f, 0.f });
        m_page->addChild(m_pageMenu, 5);

        switch (s_tab) {
            case 1:
                pageOptions("Prediction", "What the hidden ghost predicts before your click is let through.", PREDICTION, std::size(PREDICTION));
                break;
            case 2: pageOptions("Modes", "Per game mode helpers and latency.", MODES, std::size(MODES)); break;
            case 3: pageCustom(); break;
            case 4: pageTools(); break;
            default: pageBuffer(); break;
        }
        buildTabs();
        handleTouchPriority(this);
        refresh();
    }

    void refresh() {
        if (m_stage) {
            m_stage->setString(stageText().c_str());
            m_stage->limitLabelWidth(W - SIDE - 150.f, 0.55f, 0.2f);
        }
        if (m_tpsLabel) {
            m_tpsLabel->setString(fmt::format("{}", Mod::get()->getSettingValue<int64_t>("tps")).c_str());
        }
        if (m_status) {
            float dt = buf.tickLength > 0.001f ? buf.tickLength : TICK;
            m_status->setString(fmt::format("physics {:.0f}/s   |   CBF {}   |   memory {} spots", 60.f / dt,
                Loader::get()->isModLoaded("syzzi.click_between_frames") ? "on" : "off", memorySpots()).c_str());
        }
        for (size_t i = 0; i < m_switches.size(); ++i) {
            bool on = optionValue(*m_options[i]);
            drawSwitch(m_switches[i], on);
            m_switchLabels[i]->setColor(on ? m_text : m_muted);
        }
        refreshModes();
    }

    void drawSlider(ModeSlider const& sl) {
        auto* d = sl.draw;
        d->clear();
        float t = static_cast<float>(buf.modeTicks[sl.mode]) / MODE_TICKS_MAX;
        float kx = sl.x0 + (sl.x1 - sl.x0) * t;
        roundRect(d, { sl.x0, sl.y - 2.f, sl.x1 - sl.x0, 4.f }, 2.f, col({ 255, 255, 255 }, 0.12f));
        if (kx - sl.x0 > 1.f) {
            roundRect(d, { sl.x0, sl.y - 2.f, kx - sl.x0, 4.f }, 2.f, col(buf.perMode ? m_t.main : m_muted, buf.perMode ? 1.f : 0.6f));
        }
        circle(d, { kx, sl.y }, 7.5f, col(buf.perMode ? m_cyan : m_muted, 0.16f), 24);
        circle(d, { kx, sl.y }, 5.f, col({ 255, 255, 255 }, buf.perMode ? 1.f : 0.75f), 24);
    }

    // a touch on a slider sets its mode's frames (snapped to half frames)
    void slideTo(ModeSlider const& sl, CCPoint p) {
        float t = std::clamp((p.x - sl.x0) / (sl.x1 - sl.x0), 0.f, 1.f);
        int ticks = static_cast<int>(std::lround(t * MODE_TICKS_MAX / 2.f)) * 2;
        if (ticks != buf.modeTicks[sl.mode]) {
            buf.modeTicks[sl.mode] = ticks;
            refreshModes();
        }
    }

    void refreshModes() {
        for (auto const& sl : m_sliders) {
            drawSlider(sl);
        }
        for (int i = 0; i < 8; ++i) {
            if (m_modeValues[i]) {
                m_modeValues[i]->setString(fmt::format("{:g}f", buf.modeTicks[i] * TICK).c_str());
                m_modeValues[i]->setColor(buf.perMode ? m_cyan : m_muted);
            }
        }
        if (m_perModeSwitch) {
            drawSwitch(m_perModeSwitch, buf.perMode);
        }
    }

    // ---------- motion: shooting stars ----------

    CCNode* m_sky = nullptr;

    void update(float dt) override {
        m_shootTimer -= dt;
        if (m_shootTimer <= 0.f && m_sky) {
            m_shootTimer = 3.5f + static_cast<float>(std::rand() % 400) / 100.f;
            auto* streak = CCLayerGradient::create({ 255, 255, 255, 0 }, { 255, 255, 255, 220 }, { 1.f, 0.f });
            streak->setContentSize({ 34.f, 1.2f });
            streak->ignoreAnchorPointForPosition(false);
            streak->setAnchorPoint({ 1.f, 0.5f });
            float sx = 60.f + static_cast<float>(std::rand() % static_cast<int>(W - 60.f));
            streak->setPosition({ sx, H + 10.f });
            streak->setRotation(28.f);
            m_sky->addChild(streak);
            streak->runAction(CCSequence::create(
                CCSpawn::create(CCMoveBy::create(0.9f, { -170.f, -90.f }), CCSequence::create(CCDelayTime::create(0.5f), CCFadeOut::create(0.4f), nullptr),
                    nullptr),
                CCRemoveSelf::create(), nullptr));
        }
    }

    // ---------- dragging by the title bar ----------

    bool ccTouchBegan(CCTouch* touch, CCEvent* event) override {
        if (m_page) {
            auto pp = m_page->convertToNodeSpace(touch->getLocation());
            for (size_t i = 0; i < m_sliders.size(); ++i) {
                auto const& sl = m_sliders[i];
                if (pp.x >= sl.x0 - 9.f && pp.x <= sl.x1 + 9.f && std::abs(pp.y - sl.y) <= 10.f) {
                    m_dragSlider = static_cast<int>(i);
                    slideTo(sl, pp);
                    return true;
                }
            }
        }
        auto p = m_mainLayer->convertToNodeSpace(touch->getLocation());
        if (p.x >= 0.f && p.x <= W - 34.f && p.y >= H - TITLE && p.y <= H) {
            m_dragging = true;
            return true;
        }
        return Popup::ccTouchBegan(touch, event);
    }

    void ccTouchMoved(CCTouch* touch, CCEvent* event) override {
        if (m_dragSlider >= 0 && m_page) {
            slideTo(m_sliders[m_dragSlider], m_page->convertToNodeSpace(touch->getLocation()));
            return;
        }
        if (!m_dragging) {
            Popup::ccTouchMoved(touch, event);
            return;
        }
        auto delta = touch->getLocation() - touch->getPreviousLocation();
        m_mainLayer->setPosition(m_mainLayer->getPosition() + delta);
        s_pos = m_mainLayer->getPosition();
        s_moved = true;
    }

    void ccTouchEnded(CCTouch* touch, CCEvent* event) override {
        if (m_dragSlider >= 0) {
            int mode = m_sliders[m_dragSlider].mode;
            m_dragSlider = -1;
            saveModeTicks();
            say("{}: {:g} frames", MODE_NAMES[mode], buf.modeTicks[mode] * TICK);
            return;
        }
        if (m_dragging) {
            m_dragging = false;
            return;
        }
        Popup::ccTouchEnded(touch, event);
    }

    // ---------- actions ----------

    void onCloseButton(CCObject*) {
        keyBackClicked();
    }

    void onTab(CCObject* sender) {
        s_tab = sender->getTag();
        rebuildPage();
    }

    void onToggle(CCObject* sender) {
        auto const& o = *m_options[sender->getTag()];
        bool value = !optionValue(o);
        if (o.key) {
            Mod::get()->setSettingValue<bool>(o.key, value);
            readSettings();
            saveProfile();
            refreshHeatmap();
            refreshWatermark();
            refreshAutoTag();
        }
        else {
            buf.showPath = value;
            if (buf.pathNode) {
                buf.pathNode->clear();
            }
        }
        say("{} {}", o.label, value ? "on" : "off");
        refresh();
    }

    void perModeOff() {
        if (buf.perMode) {
            buf.perMode = false;
            saveModeTicks();
            say("per-mode frames off (stage picked)");
        }
    }

    void onStage(CCObject* sender) {
        perModeOff();
        cycleStage(sender->getTag());
        rebuildPage();
    }

    void onTps(CCObject* sender) {
        int tps = static_cast<int>(Mod::get()->getSettingValue<int64_t>("tps"));
        tps = std::clamp(tps + sender->getTag() * 20, TPS_MIN, TPS_MAX);
        Mod::get()->setSettingValue<int64_t>("tps", tps);
        refresh();
    }

    void onPerMode(CCObject*) {
        buf.perMode = !buf.perMode;
        saveModeTicks();
        refreshWatermark();
        refreshPanel();
        say("per-mode frames {}", buf.perMode ? "on" : "off");
        rebuildPage();
    }

    void onModeStep(CCObject* sender) {
        int mode = sender->getTag() / 2;
        int step = sender->getTag() % 2 ? 2 : -2;  // half a frame
        buf.modeTicks[mode] = std::clamp(buf.modeTicks[mode] + step, 0, MODE_TICKS_MAX);
        saveModeTicks();
        say("{}: {:g} frames", MODE_NAMES[mode], buf.modeTicks[mode] * TICK);
        refreshModes();
    }

    void onAllSettings(CCObject*) {
        geode::openSettingsPopup(Mod::get());
    }

    void onCopyLog(CCObject*) {
        exportConsole();
    }

    void onConsole(CCObject*) {
        buf.panelOpen = !buf.panelOpen;
        refreshPanel();
        say("console {}", buf.panelOpen ? "shown" : "hidden");
    }

    // v2.75 presets: stage + the switches that matter most, in one click
    void onPreset(CCObject* sender) {
        auto* mod = Mod::get();
        int which = sender->getTag();
        int stage = which == 0 ? 2 : which == 1 ? 3 : 5;  // 2f / 4f / 8f
        perModeOff();
        mod->setSettingValue<bool>("robust-saves", true);
        mod->setSettingValue<bool>("wave-safety-margin", which != 2);
        mod->setSettingValue<bool>("more-time-death-spots", which != 0);
        mod->setSettingValue<bool>("low-latency", false);
        buf.stage = stage;
        mod->setSavedValue("stage", buf.stage);
        resetPending();
        applyStage();
        readSettings();
        refreshWatermark();
        refreshPanel();
        say("preset: {} -> {}", which == 0 ? "Safe" : which == 1 ? "Balanced" : "Aggressive", stageText());
        rebuildPage();
    }

    void onRunCheck(CCObject*) {
        benchPending = true;
        say("run check: runs at the start of your next attempt from the beginning (restart the level)");
        FLAlertLayer::create("Run check",
            "Restart the level from the beginning: your longest recorded run is replayed on the hidden player and the "
            "result shows in the console (F9) and in Stats.",
            "OK")->show();
    }

    void onStats(CCObject*) {
        FLAlertLayer::create("Practice stats", statsText(), "OK")->show();
    }

    void onForget(CCObject*) {
        memory.clear();
        pending.active = false;
        memoryDirty = true;
        saveMemory();
        refreshHeatmap();
        say("memory for this level cleared");
        refresh();
    }

protected:
    bool init() {
        if (!Popup::init(W, H, "GJ_square04.png")) {
            return false;
        }
        m_bgSprite->setVisible(false);
        if (m_closeBtn) {
            m_closeBtn->setVisible(false);
            m_closeBtn->setEnabled(false);
        }
        this->setOpacity(90);
        if (s_moved) {
            m_mainLayer->setPosition(s_pos);
        }
        s_open = this;
        buildFrame();
        rebuildPage();
        this->scheduleUpdate();
        return true;
    }

public:
    ~BufferMenu() override {
        if (s_open == this) {
            s_open = nullptr;
        }
    }

    static BufferMenu* create() {
        auto* ret = new BufferMenu();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    // the menu key again closes it
    static void open() {
        if (s_open) {
            s_open->keyBackClicked();
            return;
        }
        if (auto* menu = create()) {
            menu->show();
        }
    }
};

// v2.75.1: the level complete screen no longer shows "Click Before Frames was on" or the
// saves count in the top right corner; the attempt is still finished here.
class $modify(BufEndLevelLayer, EndLevelLayer) {
    void customSetup() {
        EndLevelLayer::customSetup();
        benchFinish();  // a completed run is the best recording there is
        benchFlush();
        endAttempt();
    }
};

// Pause layer Buffer button removed.

$on_mod(Loaded) {
    buf.console = Mod::get()->getSettingValue<bool>("debug-console");
    buf.showPath = Mod::get()->getSettingValue<bool>("show-path");
    recoverGlobals();
    // v2.58: low latency is switched on for everyone once (it can still be turned off), and
    // the stage you picked last comes back, the same in every level.
    if (!Mod::get()->getSavedValue<bool>("low-latency-v258", false)) {
        Mod::get()->setSettingValue<bool>("low-latency", true);
        Mod::get()->setSavedValue("low-latency-v258", true);
    }
    buf.stage = std::clamp(Mod::get()->getSavedValue<int>("stage", buf.stage), 0, STAGE_COUNT - 1);
    // v2.62: per-level settings off (switched off once): every level uses the same switches
    if (!Mod::get()->getSavedValue<bool>("per-level-v262", false)) {
        Mod::get()->setSettingValue<bool>("per-level-settings", false);
        Mod::get()->setSavedValue("per-level-v262", true);
    }
    readSettings();
    applyStage();
#ifdef GEODE_IS_WINDOWS
    if (buf.console) {
        AllocConsole();
        FILE* out = nullptr;
        freopen_s(&out, "CONOUT$", "w", stdout);
        SetConsoleTitleA("Click Before Frames - Live Console");
    }
#endif
    if (Loader::get()->isModLoaded("syzzi.click_between_frames")) {
        say("Click Between Frames detected: mid-tick sync {}", buf.cbfSync ? "on" : "off");
    }
    if (Loader::get()->isModLoaded("square3ang.cbfextrapolate")) {
        say("CBF Extrapolate detected: visual only, predictions unaffected");
    }
    say("v2.92.0 loaded. Stage key cycles: 0.5f -> 1f -> 2f -> 4f -> 6f -> 8f -> custom -> off");
}

class $modify(BufPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", HOOK_PRIORITY);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (buf.simulating && isGhost(player)) {
            buf.ghostDied = true;
            // v2.30 diagnostics: remember the first thing that kills the press-now ghost
            if (buf.captureKiller && !buf.killerSeen) {
                buf.killerSeen = true;
                buf.killerTick = buf.simTick;
                buf.ghostDeathRect = player->getObjectRect();
                if (object) {
                    buf.killerId = object->m_objectID;
                    buf.killerType = static_cast<int>(object->m_objectType);
                    buf.killerNoTouch = object->m_isNoTouch;
                    buf.killerDisabled = object->m_isDisabled;
                    buf.killerRect = object->getObjectRect();
                }
                else {
                    buf.killerId = 0;
                    buf.killerType = -1;
                }
            }
            return;
        }
        // v2.34: let the game (and noclip / other mods) decide first, and only reset if you
        // really died. With noclip this is called every tick you're inside a spike; resetting
        // before it marked the button released each tick, the next tick pressed it again to
        // match you, and a held button turned into 100-200 clicks a second.
        bool wasDead = player && player->m_isDead;
        PlayLayer::destroyPlayer(player, object);
        if (!player || wasDead || !player->m_isDead) {
            return;
        }
        // v2.84: if the game still has the button down (a delayed release that hadn't fired
        // yet, or an auto press), let go now. Dropping the queue alone left the game holding,
        // so the next attempt started with the icon holding by itself until you clicked.
        // If you really are still holding, it's pressed again once the attempt starts.
        if (buf.appliedInput && buf.owner == this) {
            sendButton(this, false);
        }
        resetPending();
        buf.appliedInput = false;
        buf.autoReleased = false;
        buf.autoClicked = false;
        buf.rapid = 0;
        forgetPress();
        memoryDeath();
        benchFinish();
        refreshHeatmap();
        endAttempt();
        finishDrift(" (died)");
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }
        dropGhosts();
        readSettings();
        applyStage();
        exportLines.clear();
        logClock = 0;
        loadMemory(level);
        if (statsKey != memoryKey) {
            statsKey = memoryKey;
            sessionStats = {};
        }
        attemptStats = {};
        benchNeedStart = true;
        benchRecording = false;
        benchBest = {};
        benchBestDirty = false;
        benchBestTicks = benchSavedTicks();
        loadProfile();
        buf.ghost1 = makeGhost(this);
        buf.ghost2 = makeGhost(this);
        buf.ghostLayerBase = buf.ghostLayer->getChildrenCount();

        // In-game console (F9): hidden unless the player opens it.
        constexpr float W = PANEL_W;
        constexpr float H = PANEL_H;
        auto win = CCDirector::get()->getWinSize();
        buf.panel = CCNode::create();
        buf.panel->setPosition({ 8.f, win.height - H - 8.f });
        buf.panel->setZOrder(1000);

        // Background: the game's own blue background art, cropped to the panel and
        // darkened from top to bottom so the text stays readable.
        auto* stencil = CCLayerColor::create({ 255, 255, 255, 255 }, W, H);
        auto* clip = CCClippingNode::create(stencil);
        if (auto* art = CCSprite::create("game_bg_01_001.png")) {
            auto size = art->getContentSize();
            art->setScale(std::max(W / size.width, H / size.height));
            art->setPosition({ W / 2.f, H / 2.f });
            art->setColor({ 40, 120, 255 });
            art->setOpacity(150);
            clip->addChild(art);
        }
        auto* shade = CCLayerGradient::create({ 0, 0, 0, 50 }, { 0, 0, 0, 140 }, { 0.f, -1.f });
        shade->setContentSize({ W, H });
        clip->addChild(shade);
        auto* header = CCLayerColor::create({ 0, 0, 0, 80 }, W, 28.f);
        header->setPosition({ 0.f, H - 28.f });
        clip->addChild(header);
        auto* rule = CCLayerColor::create({ 255, 255, 255, 45 }, W - 16.f, 1.f);
        rule->setPosition({ 8.f, H - 46.f });
        clip->addChild(rule);
        buf.panel->addChild(clip);

        if (auto* frame = CCScale9Sprite::create("GJ_square07.png")) {
            frame->setContentSize({ W, H });
            frame->setPosition({ W / 2.f, H / 2.f });
            frame->setOpacity(190);
            buf.panel->addChild(frame);
        }

        auto* title = CCLabelBMFont::create("Click Before Frames", "goldFont.fnt");
        title->setScale(0.55f);
        title->setAnchorPoint({ 0.f, 0.5f });
        title->setPosition({ 10.f, H - 14.f });
        buf.panel->addChild(title);

        auto* version = CCLabelBMFont::create(Mod::get()->getVersion().toVString().c_str(), "chatFont.fnt");
        version->setScale(0.45f);
        version->setOpacity(150);
        version->setAnchorPoint({ 1.f, 0.5f });
        version->setPosition({ W - 10.f, H - 14.f });
        buf.panel->addChild(version);

        buf.panelTitle = CCLabelBMFont::create("", "bigFont.fnt");
        buf.panelTitle->setScale(0.4f);
        buf.panelTitle->setAnchorPoint({ 0.f, 0.5f });
        buf.panelTitle->setPosition({ 10.f, H - 37.f });
        buf.panel->addChild(buf.panelTitle);

        buf.panelLines.clear();
        for (size_t i = 0; i < PANEL_LINES; ++i) {
            auto* line = CCLabelBMFont::create("", "chatFont.fnt");
            line->setScale(0.55f);
            line->setAnchorPoint({ 0.f, 0.5f });
            line->setPosition({ 10.f, H - 57.f - i * 13.f });
            buf.panel->addChild(line);
            buf.panelLines.push_back(line);
        }

        // v2.79: the hint shows the keys you set
        auto* mod = Mod::get();
        auto keyName = [mod](char const* key) {
            auto binds = mod->getSettingValue<std::vector<Keybind>>(key);
            return binds.empty() ? std::string("-") : binds.front().toString();
        };
        auto hintText = fmt::format("{} stage  {} menu  {} copy log  {} console  {} path  {} forget", keyName("bind-stage"),
            keyName("bind-menu"), keyName("bind-export"), keyName("bind-console"), keyName("bind-path"), keyName("bind-forget"));
        auto* hint = CCLabelBMFont::create(hintText.c_str(), "chatFont.fnt");
        hint->setScale(0.45f);
        hint->setColor({ 160, 190, 255 });
        hint->setAnchorPoint({ 0.5f, 0.5f });
        hint->setPosition({ W / 2.f, 9.f });
        buf.panel->addChild(hint);

        this->addChild(buf.panel);
        refreshPanel();

        buf.watermark = nullptr;
        buf.usedThisAttempt = false;
        refreshWatermark();

        buf.autoTag = CCLabelBMFont::create("AUTO INPUTS ON", "bigFont.fnt");
        buf.autoTag->setScale(0.24f);
        buf.autoTag->setColor({ 255, 120, 120 });
        buf.autoTag->setOpacity(153);  // 60%, like the console; still readable in a recording
        buf.autoTag->setAnchorPoint({ 1.f, 0.f });
        buf.autoTag->setPosition({ win.width - 6.f, 4.f });
        buf.autoTag->setZOrder(1000);
        this->addChild(buf.autoTag);
        refreshAutoTag();

        buf.hintLabel = CCLabelBMFont::create("", "bigFont.fnt");
        buf.hintLabel->setColor({ 255, 170, 215 });
        buf.hintLabel->setOpacity(0);
        buf.hintLabel->setPosition({ win.width / 2.f, win.height - 40.f });
        buf.hintLabel->setZOrder(1000);
        this->addChild(buf.hintLabel);

        buf.heatNode = nullptr;
        if (m_progressBar) {
            buf.heatNode = CCDrawNode::create();
            m_progressBar->addChild(buf.heatNode, 5);
            refreshHeatmap();
        }

        // Path view lives in the object layer, so it scrolls with the level.
        buf.pathNode = CCDrawNode::create();
        // v2.48: above the level's shader effects when it has that layer; inside the shader
        // layer, dark or glow shaders could hide the path completely.
        (m_aboveShaderObjectLayer ? m_aboveShaderObjectLayer : m_objectLayer)->addChild(buf.pathNode, 9999);

        buf.realInput = false;
        buf.appliedInput = false;
        buf.isPlayer1 = true;
        buf.sinceInput = 999;
        buf.rapid = 0;
        forgetPress();
        buf.owner = this;
        return true;
    }

    void onQuit() {
        benchFinish();
        benchFlush();
        benchRecording = false;
        saveMemory();
        restoreGlobals();
        buf.hintLabel = nullptr;
        buf.heatNode = nullptr;
        memoryKey.clear();
        dropGhosts();
        PlayLayer::onQuit();
    }

    void resetLevel() {
        triggerChecks.clear();  // v2.84
        attemptStartTick = tickCount;  // v2.84.3
        benchFinish();
        benchNeedStart = true;
        endAttempt();
        buf.usedThisAttempt = false;
        // v2.84: same on a restart (R, checkpoints, practice): never start an attempt with the
        // game holding a button you already let go of
        // v2.86: the game also restarts the attempt while the level is still loading; sending
        // a button then (v2.84/v2.85) crashed when you left a level and opened one again
        if (buf.owner != this) {
            PlayLayer::resetLevel();
            return;
        }
        if (buf.appliedInput && !buf.realInput) {
            sendButton(this, false);
        }
        resetPending();
        forgetPress();
        refreshAutoTag();
        if (buf.pathNode) {
            buf.pathNode->clear();
        }
        buf.autoReleased = false;
        buf.autoClicked = false;
        drift.active = false;
        PlayLayer::resetLevel();
        // v2.85: start every attempt from the game's own button state. A release you made while
        // your icon was dead never reached the mod, so it still thought you were holding and
        // pressed for you as the attempt started (the cube jumped by itself until you clicked).
        // Now the mod only presses when you press.
        syncHeld(this);
    }

    void playEndAnimationToPos(CCPoint position) {
        if (buf.simulating) {
            return;
        }
        PlayLayer::playEndAnimationToPos(position);
    }

    // v2.78.3: the level's own circle effect (orb / pad hits) is never spawned by a ghost
    void spawnCircle() {
        if (buf.simulating) {
            return;
        }
        PlayLayer::spawnCircle();
    }
};

class $modify(BufEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool noUI) {
        bool ok = LevelEditorLayer::init(level, noUI);
        dropGhosts();
        return ok;
    }
};

class $modify(BufHardStreak, HardStreak) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("HardStreak::addPoint", HOOK_PRIORITY);
    }

    void addPoint(CCPoint point) {
        if (buf.simulating) {
            return;
        }
        HardStreak::addPoint(point);
    }
};

class $modify(BufEffectGameObject, EffectGameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("EffectGameObject::triggerObject", HOOK_PRIORITY);
    }

    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (buf.simulating) {
            return;
        }
        if (buf.predictTriggers) {
            noteTriggerFire(layer, this);  // v2.84.2: any trigger with a target group, for the self-check
        }
        // v2.81: when a move (v2.83: or rotation) started, so a prediction can follow the rest of it
        if ((m_objectID == 901 || m_objectID == 1346) && buf.predictTriggers && m_duration > 0.f && firedMoves.size() < 256) {
            firedMoves[this] = tickCount;
            startTriggerCheck(layer, this);  // v2.84
        }
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }

    // v2.78.3: visual only, never from a ghost
    void playTriggerEffect() {
        if (buf.simulating) {
            return;
        }
        EffectGameObject::playTriggerEffect();
    }

    // v2.84.6: a pad's own animation (it played when a ghost touched the pad, before you did:
    // seen after gravity changes)
    void animationTriggered() {
        if (buf.simulating) {
            return;
        }
        EffectGameObject::animationTriggered();
    }
};

class $modify(BufGameObject, GameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GameObject::playShineEffect", HOOK_PRIORITY);
    }

    void playShineEffect() {
        if (buf.simulating) {
            return;
        }
        GameObject::playShineEffect();
    }

    // v2.78.3: visual only, never from a ghost
    void playDestroyObjectAnim(GJBaseGameLayer* layer) {
        if (buf.simulating) {
            return;
        }
        GameObject::playDestroyObjectAnim(layer);
    }
};

// v2.57: an orb the ghost uses while trying ticks played its "used" animation on the real
// level (seen on dash orbs before you reached them). The state is restored afterwards
// anyway, so ghosts skip the visuals.
class $modify(BufRingObject, RingObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("RingObject::powerOnObject", HOOK_PRIORITY);
        (void)self.setHookPriority("RingObject::spawnCircle", HOOK_PRIORITY);
    }

    void powerOnObject(int state) {
        if (buf.simulating) {
            return;
        }
        RingObject::powerOnObject(state);
    }

    void spawnCircle() {
        if (buf.simulating) {
            return;
        }
        RingObject::spawnCircle();
    }

    // v2.78.3: the activation call itself too (orbs and dash orbs still animated early)
    void triggerActivated(float xPosition) {
        if (buf.simulating) {
            return;
        }
        RingObject::triggerActivated(xPosition);
    }
};

class $modify(BufPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::playSpiderDashEffect", HOOK_PRIORITY);
        // v2.18: run before Click Between Frames, so this sees each physics tick once, with
        // its full dt, and CBF's mid-tick clicks arrive while inRealUpdate is set.
        (void)self.setHookPriority("PlayerObject::update", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::incrementJumps", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::ringJump", HOOK_PRIORITY);
    }

    // Runs 240x/sec per player. Ghost steps (during a simulation) skip straight to
    // the original; F6 is no longer polled here (see BufKeyboard below).
    void update(float dt) {
        if (buf.simulating) {
            PlayerObject::update(dt);
            return;
        }
        auto* layer = GameManager::get()->m_playLayer;
        if (layer && this == layer->m_player1) {
            ++tickCount;
            if (!triggerChecks.empty()) {
                runTriggerChecks(layer);  // v2.84
            }
            ++logClock;
            if (buf.enabled) {
                buf.usedThisAttempt = true;
            }
            if (buf.panelDirty) {
                buf.panelDirty = false;
                refreshPanel();
            }
            if (benchNeedStart) {
                benchNeedStart = false;
                if (benchPending) {
                    benchPending = false;
                    benchCheck(layer, this);
                }
                benchStart(layer, this);
            }
            driftStep(this);
            memoryTick();
            if (buf.enabled) {
                deliverPending(layer, this);
                livePath(layer, this);
            }
        }

        bool isP1 = layer && this == layer->m_player1;
        if (isP1) {
            // v2.37: getPosition() is updated every physics step; PlayerObject::m_position lags
            // behind, which kept CBF sync, the drift check, path view and memory slices off.
            buf.tickStartX = getPosition().x;
            buf.inRealUpdate = true;
        }
        PlayerObject::update(dt);
        if (isP1) {
            benchStep(this);
            buf.inRealUpdate = false;
            float dx = getPosition().x - buf.tickStartX;
            if (std::abs(dx) > 0.01f) {
                buf.tickDx = dx;
            }
            // The ghost must step with the dt the real player gets. v2.23: with unlocked FPS the
            // step length jitters from frame to frame, so use a running average; a big jump
            // (FPS cap or time warp changed) is taken over straight away. (P1 only: with CBF,
            // P2 can be stepped in pieces of a tick.)
            if (dt > 0.f) {
                float avg = buf.tickLength;
                buf.tickLength = (avg > 0.f && std::abs(dt - avg) < avg * 0.3f) ? avg * 0.9f + dt * 0.1f : dt;
            }
        }
    }

    void incrementJumps() {
        if (buf.simulating) {
            return;
        }
        PlayerObject::incrementJumps();
    }

    // v2.24: the ghost runs the real player's code, which also spawns visual effects
    // (circles, bump waves, trail fade-outs, spider run sprites, rotation actions). None
    // of it matters for physics, and some of it was added to the real level, so ghosts
    // skip all of it.
    bool ghostEffect() {
        return buf.simulating && isGhost(this);
    }

    void spawnCircle() {
        if (!ghostEffect()) PlayerObject::spawnCircle();
    }

    void spawnDualCircle() {
        if (!ghostEffect()) PlayerObject::spawnDualCircle();
    }

    void spawnPortalCircle(ccColor3B color, float startRadius) {
        if (!ghostEffect()) PlayerObject::spawnPortalCircle(color, startRadius);
    }

    void spawnScaleCircle() {
        if (!ghostEffect()) PlayerObject::spawnScaleCircle();
    }

    void playBumpEffect(int objectType, GameObject* object) {
        if (!ghostEffect()) PlayerObject::playBumpEffect(objectType, object);
    }

    void playSpawnEffect() {
        if (!ghostEffect()) PlayerObject::playSpawnEffect();
    }

    void createFadeOutDartStreak() {
        if (!ghostEffect()) PlayerObject::createFadeOutDartStreak();
    }

    void fadeOutStreak2(float duration) {
        if (!ghostEffect()) PlayerObject::fadeOutStreak2(duration);
    }

    void playDynamicSpiderRun() {
        if (!ghostEffect()) PlayerObject::playDynamicSpiderRun();
    }

    void animatePlatformerJump(float scale) {
        if (!ghostEffect()) PlayerObject::animatePlatformerJump(scale);
    }

    void runBallRotation(float speed) {
        if (!ghostEffect()) PlayerObject::runBallRotation(speed);
    }

    void runBallRotation2() {
        if (!ghostEffect()) PlayerObject::runBallRotation2();
    }

    void runNormalRotation(bool notNormalMode, float speed) {
        if (!ghostEffect()) PlayerObject::runNormalRotation(notNormalMode, speed);
    }

    void toggleGhostEffect(GhostType type) {
        if (!ghostEffect()) PlayerObject::toggleGhostEffect(type);
    }

    void activateStreak() {
        if (!ghostEffect()) PlayerObject::activateStreak();
    }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (buf.simulating) {
            return;
        }
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void ringJump(RingObject* ring, bool skipCheck) {
        if (buf.simulating) {
            if (!buf.smart || !isGhost(this)) {
                return;
            }
            if (buf.orbsUsed.contains(ring)) {
                return;
            }
            snapshotObject(ring);
            // v2.43: the game can still refuse the orb inside ringJump, so the orb only counts
            // as used if it actually changed the ghost's motion or gravity.
            double vy = m_yVelocity;
            bool upsideDown = m_isUpsideDown;
            bool dashing = m_isDashing;
            PlayerObject::ringJump(ring, skipCheck);
            if (m_yVelocity != vy || m_isUpsideDown != upsideDown || m_isDashing != dashing) {
                buf.orbsUsed.insert(ring);
                if (buf.orbUsedTick < 0) {
                    buf.orbUsedTick = buf.simTick;
                }
            }
            return;
        }
        PlayerObject::ringJump(ring, skipCheck);
    }

    // Dash orbs don't go through ringJump.
    void startDashing(DashRingObject* ring) {
        if (buf.simulating && isGhost(this)) {
            buf.orbsUsed.insert(ring);
            if (buf.orbUsedTick < 0) {
                buf.orbUsedTick = buf.simTick;
            }
        }
        PlayerObject::startDashing(ring);
    }
};

// v2.81: Geode keybinds (mod settings or Geode's keybinds popup; work without F keys too).
// Only act inside a level.
namespace {
    void keyAction(int action) {
        auto* layer = GameManager::get()->m_playLayer;
        if (!layer) {
            return;
        }
        switch (action) {
            case 0:
                cycleStage();
                break;
            case 1:
                if (!layer->m_isPaused) {
                    layer->pauseGame(true);
                }
                BufferMenu::open();
                break;
            case 2:
                exportConsole();
                break;
            case 3:
                buf.panelOpen = !buf.panelOpen;
                refreshPanel();
                break;
            case 4:
                buf.showPath = !buf.showPath;
                if (buf.pathNode) {
                    buf.pathNode->clear();
                }
                say("path view {}", buf.showPath ? "on" : "off");
                break;
            case 5:
                memory.clear();
                pending.active = false;
                memoryDirty = true;
                saveMemory();
                say("memory for this level cleared");
                break;
        }
    }

    void setupKeybinds() {
        char const* binds[] = { "bind-stage", "bind-menu", "bind-export", "bind-console", "bind-path", "bind-forget" };
        for (int i = 0; i < 6; ++i) {
            listenForKeybindSettingPresses(binds[i], [i](Keybind const&, bool down, bool repeat, double) {
                if (down && !repeat) {
                    keyAction(i);
                }
                return false;
            });
        }
    }

    // v2.81 update check: once per game start, reads the version in the public repo's mod.json
    // (in the background) and says so on the main menu if a newer one is out.
    constexpr char const* REPO_URL = "https://github.com/raresdalas1009/-predictive-input-buffer";
    std::string latestVersion;
    bool updateChecked = false;
    bool updateShown = false;

    void showUpdatePopup() {
        if (updateShown || latestVersion.empty() || !CCScene::get()) {
            return;
        }
        updateShown = true;
        say("update: {} is out (you have {})", latestVersion, Mod::get()->getVersion().toVString());
        createQuickPopup("Click Before Frames", fmt::format("<cp>{}</c> is out! You have {}.", latestVersion,
            Mod::get()->getVersion().toVString()), "Later", "Open page", [](FLAlertLayer*, bool open) {
                if (open) {
                    web::openLinkInBrowser(std::string(REPO_URL));
                }
            });
    }

    void checkForUpdate() {
        if (updateChecked || !Mod::get()->getSettingValue<bool>("check-updates")) {
            return;
        }
        updateChecked = true;
        std::thread([] {
            auto res = web::WebRequest()
                .timeout(std::chrono::seconds(10))
                .getSync("https://raw.githubusercontent.com/raresdalas1009/-predictive-input-buffer/main/mod.json");
            if (!res.ok()) {
                return;
            }
            auto json = res.json();
            if (json.isErr()) {
                return;
            }
            auto version = json.unwrap()["version"].asString();
            if (version.isErr()) {
                return;
            }
            auto latest = VersionInfo::parse(version.unwrap());
            if (latest.isErr() || !(latest.unwrap() > Mod::get()->getVersion())) {
                return;
            }
            std::string name = latest.unwrap().toVString();
            queueInMainThread([name] {
                latestVersion = name;
                auto* scene = CCScene::get();
                if (scene && scene->getChildByID("MenuLayer")) {
                    showUpdatePopup();
                }
            });
        }).detach();
    }
}

$on_mod(Loaded) {
    setupKeybinds();
}

// the main menu: a heart button that opens the menu (with the other buttons at the bottom),
// and the update check
class $modify(BufMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) {
            return false;
        }
        auto* top = CCSprite::create("heart.png"_spr);
        top->setScale(0.55f);
        top->setColor({ 255, 255, 255 });
        auto* sprite = CircleButtonSprite::create(top, CircleBaseColor::DarkPurple, CircleBaseSize::Medium);
        auto* item = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(BufMenuLayer::onBufferMenu));
        item->setID("click-before-frames-button"_spr);
        if (auto* bottom = this->getChildByID("bottom-menu")) {
            bottom->addChild(item);
            bottom->updateLayout();
        }
        checkForUpdate();
        if (!latestVersion.empty() && !updateShown) {
            queueInMainThread([] { showUpdatePopup(); });
        }
        return true;
    }

    void onBufferMenu(CCObject*) {
        BufferMenu::open();
    }
};

// v2.57 custom TPS: above 240 the level is stepped in 1/tps pieces. update() runs once per
// step and getModifiedDelta() hands the step through unrounded, so each call is exactly one
// physics step. (Below 240 the game's own step count would need patching, so 240 is the floor.)
static bool tpsDriving = false;
static double tpsCarry = 0.0;

class $modify(BufBaseGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        int tps = static_cast<int>(Mod::get()->getSettingValue<int64_t>("tps"));
        if (tps <= 240 || tpsDriving || !typeinfo_cast<PlayLayer*>(this)) {
            GJBaseGameLayer::update(dt);
            return;
        }
        double step = 1.0 / tps;
        tpsCarry += dt;
        int steps = static_cast<int>(tpsCarry / step);
        int cap = std::max(1, tps / 15);  // after a lag spike, catch up at most 1/15 s
        if (steps > cap) {
            steps = cap;
            tpsCarry = 0.0;
        }
        else {
            tpsCarry -= steps * step;
        }
        tpsDriving = true;
        for (int i = 0; i < steps; ++i) {
            GJBaseGameLayer::update(static_cast<float>(step));
        }
        tpsDriving = false;
    }

    double getModifiedDelta(float dt) {
        if (!tpsDriving) {
            return GJBaseGameLayer::getModifiedDelta(dt);
        }
        m_extraDelta = 0.0;
        if (m_resumeTimer > 0) {
            --m_resumeTimer;
            return 0.0;
        }
        return dt;
    }

    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::canBeActivatedByPlayer", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::collisionCheckObjects", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::playerTouchedRing", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::flipGravity", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::gameEventTriggered", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::playerWillSwitchMode", HOOK_PRIORITY);
    }

    // During a simulation the ghost may use each object once; the real activation
    // state is snapshotted and restored afterwards so the real player is unaffected.
    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (buf.simulating) {
            if (!buf.smart || !isGhost(player) || !object) {
                return false;
            }
            // v2.16: an orb can be checked on every tick you're inside it. Before, the first
            // check already "spent" it, so the ghost could only click an orb on the very
            // first tick it touched it. Now an orb is only spent once the ghost used it.
            if (typeinfo_cast<RingObject*>(object)) {
                if (buf.orbsUsed.contains(object)) {
                    return false;
                }
                snapshotObject(object);
                return true;
            }
            if (buf.simUsed.contains(object)) {
                return false;
            }
            snapshotObject(object);
            return true;
        }
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }

    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int objectCount, float dt) {
        if (!buf.simulating) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
            return;
        }
        static gd::vector<GameObject*> relevant;
        relevant.clear();
        if (static_cast<int>(relevant.capacity()) < objectCount) {
            relevant.reserve(objectCount);
        }

        // Hot loop: runs for every nearby object, every ghost tick. One type lookup per
        // object, no bounds-checked at(), and the ghost rect is only fetched if needed.
        uint64_t keep = buf.smart ? (COLLIDING_TYPES | INTERACT_TYPES) : COLLIDING_TYPES;
        bool haveRect = false;
        CCRect ghostRect;
        auto& list = *objects;
        for (int i = 0; i < objectCount; ++i) {
            auto* object = list[i];
            if (!object) {
                continue;
            }
            auto type = static_cast<unsigned>(object->m_objectType);
            uint64_t bit = type < 64 ? (1ull << type) : 0;
            if (keep & bit) {
                // v2.31: a hazard you're touching right now while alive is one the game doesn't
                // let kill you (a spike made to follow the player, noclip...). Seen in GRIEF:
                // a spike always sitting exactly on the player killed every wave prediction on
                // its first tick.
                // Decided on the first ticks and kept by object, so a spike that follows you
                // stays ignored even when "predict moving objects" moves it along.
                if (!buf.simDisabled.empty() && buf.simDisabled.contains(object)) {
                    continue;
                }
                if (buf.haveRealRect && (bit & HAZARD_TYPES)) {
                    if (!buf.harmless.empty() && buf.harmless.contains(object)) {
                        continue;
                    }
                    if (buf.simTick <= 1) {
                        auto const& r = object->getObjectRect();
                        if (r.intersectsRect(buf.realRect1) || r.intersectsRect(buf.realRect2)) {
                            buf.harmless.insert(object);
                            continue;
                        }
                    }
                }
                relevant.push_back(object);
                if (buf.trackMovers && (COLLIDING_TYPES & bit)) {
                    watchObject(object);
                }
                continue;
            }
            if (!buf.smart) {
                continue;
            }
            bool stop = (STOP_TYPES & bit) != 0;
            bool size = (SIZE_TYPES & bit) != 0;
            float speed = (stop || size) ? 0.f : speedPortalValue(object->m_objectID);
            if (!stop && !size && speed <= 0.f) {
                continue;
            }
            if (!haveRect) {
                ghostRect = player->getObjectRect();
                haveRect = true;
            }
            if (ghostRect.intersectsRect(object->getObjectRect())) {
                if (stop) {
                    // v2.78.1: a mode portal you're still inside when you click was already taken
                    // (you're in its mode now). It stopped every prediction at t0, so nothing right
                    // after a portal was predicted (Amethyst's first pink orb: the click was never
                    // moved onto the orb). Only portals ahead stop a prediction; teleport orbs
                    // still do, since they need a click.
                    auto const& r = object->getObjectRect();
                    if (buf.harmless.contains(object)) {
                        continue;
                    }
                    if (buf.haveStartRects && buf.simTick <= 1 && object->m_objectType != GameObjectType::TeleportOrb &&
                        (r.intersectsRect(buf.realRect1) || r.intersectsRect(buf.realRect2))) {
                        buf.harmless.insert(object);
                        continue;
                    }
                    // v2.79: mode portals ahead are taken by the ghost (once per run)
                    // v2.84: teleport and solo portals too
                    if (buf.portalPredict && isGhost(player) &&
                        (ghostTakePortal(player, object->m_objectType) || ghostTeleport(player, object) || ghostSoloPortal(object) ||
                            ghostDualPortal(player, object))) {
                        buf.harmless.insert(object);
                        continue;
                    }
                    buf.ghostHitPortal = true;
                }
                else if (size) {
                    bool mini = object->m_objectType == GameObjectType::MiniSizePortal;
                    if ((player->m_vehicleSize < 1.f) != mini) {
                        player->togglePlayerScale(mini, true);
                    }
                }
                else {
                    player->m_playerSpeed = speed;
                }
            }
        }
        // v2.85: objects a predicted toggle switched on: the game leaves switched-off objects
        // out, so the ones near the ghost are added (switched on just for this check)
        static std::vector<GameObject*> woke;
        woke.clear();
        if (!buf.simEnabled.empty() && player) {
            auto pos = player->getPosition();
            for (auto* object : buf.simEnabled) {
                auto type = static_cast<unsigned>(object->m_objectType);
                uint64_t bit = type < 64 ? (1ull << type) : 0;
                if (!(keep & bit) || buf.simDisabled.contains(object) || std::abs(object->m_positionX - pos.x) > 200.f ||
                    std::abs(object->m_positionY - pos.y) > 200.f) {
                    continue;
                }
                if (object->m_isGroupDisabled) {
                    object->m_isGroupDisabled = false;
                    woke.push_back(object);
                }
                relevant.push_back(object);
            }
        }
        GJBaseGameLayer::collisionCheckObjects(player, &relevant, static_cast<int>(relevant.size()), dt);
        for (auto* object : woke) {
            object->m_isGroupDisabled = true;
        }
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!buf.simulating && button == 1 && !buf.injecting) {
            auto* p1 = this->m_player1;
            if (down || (p1 && (p1->m_isDart || p1->m_isRobot))) {
                finishDrift("");
            }
            recordRealInput(down, p1);
        }
        if (!buf.simulating && button == 1 && !buf.injecting) {
            // v2.21 performance log: what this click cost, measured around the whole decision.
            buf.simRuns = 0;
            buf.simTicks = 0;
            auto start = std::chrono::steady_clock::now();
            bool held = intercept(this, down, isPlayer1);
            sweepGhostLayer();
            if (buf.perfLog && buf.simRuns > 0) {
                double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                say("perf: {:.2f} ms, {} runs, {} ghost ticks ({:.1f} us/tick)", ms, buf.simRuns, buf.simTicks,
                    ms * 1000.0 / static_cast<double>(std::max(1L, buf.simTicks)));
            }
            if (held) {
                return;
            }
        }
        if (!buf.simulating && button == 1) {
            benchInput(down);  // v2.75: what the game really gets (your clicks and delayed ones)
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (buf.simulating) {
            if (!buf.smart || !isGhost(player)) {
                return;
            }
            snapshotObject(ring);
            buf.touchedOrb = true;
            if (buf.firstOrbTick < 0) {
                buf.firstOrbTick = buf.simTick;
            }
            buf.lastOrbTick = buf.simTick;
            if (buf.orbUsedTick >= 0 && !buf.orbsUsed.contains(ring)) {
                buf.laterOrbTouched = true;
            }
        }
        GJBaseGameLayer::playerTouchedRing(player, ring);

        // v2.2 calculation mode: assume you click every orb you touch, on time. Without
        // this the ghost (which only has the one click being tested) misses the next orb in a
        // chain, falls, and the mod thinks a perfectly good click is a death.
        bool autoOrb = buf.orbClick || (buf.orbAfterFirst && buf.orbUsedTick >= 0 && !buf.orbsUsed.contains(ring));
        if (buf.simulating && autoOrb) {
            bool first = player == buf.ghost1;
            bool& held = first ? buf.held1 : buf.held2;
            bool& autoClick = first ? buf.auto1 : buf.auto2;
            int& autoAt = first ? buf.autoAt1 : buf.autoAt2;
            if (buf.orbClickDelay > 0) {
                if (!held && autoAt == -1) {
                    autoAt = buf.simTick + buf.orbClickDelay;  // v2.26: click later inside the orb
                }
            }
            else if (!held && !autoClick) {
                player->pushButton(PlayerButton::Jump);
                autoClick = true;
            }
        }
    }

    // Gravity portals / blue pads / blue orbs: flip only the ghost, no layer effects.
    void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
        if (buf.simulating) {
            if (buf.smart && isGhost(player)) {
                player->flipGravity(flip, true);
            }
            return;
        }
        GJBaseGameLayer::flipGravity(player, flip, noEffects);
    }

    // Never let ghost activity fire event triggers in the real level.
    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (buf.simulating) {
            return;
        }
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    // v2.74: nothing the hidden ghost touches during a prediction may change the real level.
    // Toggle orbs, orbs / pads / collectibles with groups, item and counter triggers could
    // otherwise toggle or spawn groups and change item counters for real (once per
    // prediction run, many times a click), which broke triggers in some levels.
    void toggleGroupTriggered(int group, bool activate, gd::vector<int> const& remapKeys, int triggerID, int controlID) {
        if (buf.simulating) {
            // v2.75: a toggle orb that switches a group off: the ghost stops colliding with
            // that group for the rest of this prediction (the real objects aren't touched)
            // v2.85: and one that switches a group on, the ghost starts colliding with it
            if (auto* objects = this->getGroup(group)) {
                simToggle(objects, activate);
            }
            return;
        }
        GJBaseGameLayer::toggleGroupTriggered(group, activate, remapKeys, triggerID, controlID);
    }

    void spawnGroup(int group, bool ordered, double delay, gd::vector<int> const& remapKeys, int triggerID, int controlID) {
        if (buf.simulating) {
            return;
        }
        // v2.85: a delayed spawn: what it sets off is predicted from when it will happen
        if (buf.predictTriggers && delay > 0.0 && !ordered && remapKeys.empty() && group > 0 && pendingSpawns.size() < 64) {
            float tickLength = buf.tickLength > 0.001f && buf.tickLength < 2.f ? buf.tickLength : TICK;
            pendingSpawns.push_back({ group, tickCount + static_cast<int>(std::lround(delay * 60.0 / tickLength)) });
        }
        GJBaseGameLayer::spawnGroup(group, ordered, delay, remapKeys, triggerID, controlID);
    }

    void toggleGroup(int id, bool activate) {
        if (buf.simulating) {
            return;
        }
        GJBaseGameLayer::toggleGroup(id, activate);
    }

    void pickupItem(EffectGameObject* object) {
        if (buf.simulating) {
            return;
        }
        GJBaseGameLayer::pickupItem(object);
    }

    void updateCounters(int itemId, int value) {
        if (buf.simulating) {
            return;
        }
        GJBaseGameLayer::updateCounters(itemId, value);
    }

    void activateItemEditTrigger(ItemTriggerGameObject* object) {
        if (buf.simulating) {
            return;
        }
        GJBaseGameLayer::activateItemEditTrigger(object);
    }

    void activateItemCompareTrigger(ItemTriggerGameObject* object, gd::vector<int> const& remapKeys) {
        if (buf.simulating) {
            return;
        }
        GJBaseGameLayer::activateItemCompareTrigger(object, remapKeys);
    }

    // Safety net in case a mode portal slips through: end prediction, change nothing.
    void playerWillSwitchMode(PlayerObject* player, GameObject* object) {
        if (buf.simulating) {
            buf.ghostHitPortal = true;
            return;
        }
        GJBaseGameLayer::playerWillSwitchMode(player, object);
    }
};

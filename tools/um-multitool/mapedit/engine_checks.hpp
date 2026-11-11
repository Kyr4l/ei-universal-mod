// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Script checks from game.exe's own script engine (docs/game-exe/gs-functions.md, script-vm.md, bugs.md): what the
// script checker (mob_script_check.hpp, shared with um.dll) cannot know from the command list alone. Settings > Checks >
// Engine Script Checks turns them off.
//   - commands the Run switch has no case for: the compiler accepts them, the game does nothing
//   - a script text ending right after "Script" / "DeclareScript": the compiler crashes while the map loads (bugs.md 12)
//   - QStart without QFinish in the same script: the objective table is left open (QStart's notes)
//   - IsInSquare / AddRectToArea with literal corners in the wrong order: x1 <= x <= x2 never holds
//   - CreateParticleSource with a name outside the 64 effects of game.exe's table (0x78B120): nothing is created
//   - a script's parameter (this) used inside a For body: the compiler does not resolve script parameters there
#pragma once

#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "script_ast.hpp"

namespace enginecheck {

struct Issue { char severity; int line; std::string message; };

inline std::string Low(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline const std::set<std::string>& NoOps() {
    static const std::set<std::string> s = {"playfx", "setcameraorientation", "setcameraposition", "setdirectiontoobject", "setplayeraggression",
                                            "setsunlight", "showbitmap", "showcredits", "startanimation", "waitendanimation"};
    return s;
}

// game.exe's particle table (0x78B120, read from the Russian exe): the names CreateParticleSource accepts, any case.
inline const std::set<std::string>& Particles() {
    static const std::set<std::string> s = [] {
        std::set<std::string> out;
        const char* const names[] = {"AcidRay", "FireArrow", "FireBall", "CampFire", "FireBlast", "LightningBlast", "Fire", "Smoke", "VulcanSmoke",
            "Healing", "PoisonFog", "Geyser", "Tornado", "Casting", "Nuke", "Mushroom", "Link", "BlueGas", "SphereAcid", "SphereElectricity",
            "SphereFire", "ClayRing", "Teleport", "Modifier1", "Modifier2", "Modifier3", "Modifier4", "Modifier5", "Modifier6", "Modifier7",
            "Modifier8", "Modifier9", "Modifier10", "Modifier11", "Modifier12", "Antimagic", "CastingFire", "CastingElectricity", "CastingAcid",
            "CastingDivination", "CastingIllusion", "CastingDomination", "CastingEnchantment", "CastingHealing", "ZoneExit", "Portal", "PortalStar",
            "Cylinder1", "Cylinder2", "FireStar", "AcidStar", "Sparks", "VisionStar1", "VisionStar2", "VisionStar3", "Regeneration", "Silence",
            "FeebleMind", "FeetCloud1", "FeetCloud2", "CurseStars", "CurseHolder", "StartTrans", "Transform"};
        for (const char* n : names) out.insert(Low(n));
        return out;
    }();
    return s;
}

namespace detail {

inline bool Num(const scriptast::Expr& e, double& v) {
    if (e.kind != scriptast::Expr::Number) return false;
    v = std::atof(e.text.c_str());
    return true;
}

struct Walker {
    std::vector<Issue>& out;
    std::set<std::string> params; // the current script's parameters (lower-case)
    int forDepth = 0;
    bool qstart = false, qfinish = false;
    int qstartLine = 0;

    void ExprCheck(const scriptast::Expr& e) {
        if (e.kind == scriptast::Expr::Name && forDepth > 0 && params.count(Low(e.text)))
            out.push_back({'W', e.line, "'" + e.text + "' is a parameter of this script: game.exe's compiler does not resolve script parameters inside a For body"});
        if (e.kind != scriptast::Expr::Call) return;
        const std::string n = Low(e.text);
        if (NoOps().count(n)) out.push_back({'W', e.line, e.text + "() does nothing in game.exe (no case in the script engine's Run switch)"});
        if (n == "qstart") { qstart = true; qstartLine = e.line; }
        if (n == "qfinish") qfinish = true;
        double a = 0, b = 0, c = 0, d = 0;
        if (n == "isinsquare" && e.args.size() == 6 && Num(e.args[2], a) && Num(e.args[3], b) && Num(e.args[4], c) && Num(e.args[5], d) && (a > c || b > d))
            out.push_back({'W', e.line, "IsInSquare(): the corners must be ordered (x1 <= x2, y1 <= y2): as written it is never true"});
        if (n == "addrecttoarea" && e.args.size() == 5 && Num(e.args[1], a) && Num(e.args[2], b) && Num(e.args[3], c) && Num(e.args[4], d) && (a > c || b > d))
            out.push_back({'W', e.line, "AddRectToArea(): the corners must be ordered (x1 <= x2, y1 <= y2): IsInArea never finds a point in it"});
        if (n == "createparticlesource" && e.args.size() == 6 && e.args[5].kind == scriptast::Expr::String && !Particles().count(Low(e.args[5].text)))
            out.push_back({'W', e.line, "CreateParticleSource(): '" + e.args[5].text + "' is not one of game.exe's 64 particle effects: nothing is created"});
        if ((n == "copystats" || n == "copyitems") && e.args.size() == 3)
            for (size_t k = 1; k < 3; ++k)
                if (e.args[k].kind == scriptast::Expr::String && e.args[k].text.find("::") == std::string::npos)
                    out.push_back({'W', e.line, e.text + "(): '" + e.args[k].text + "' has no \"party::hero\": game.exe uses the default party's hero by that name (bugs.md 11)"});
        for (const scriptast::Expr& x : e.args) ExprCheck(x);
    }
    void Stmts(const std::vector<scriptast::Stmt>& v) {
        for (const scriptast::Stmt& s : v) {
            for (const scriptast::Expr& c : s.conditions) ExprCheck(c);
            if (s.kind == scriptast::Stmt::Loop) {
                for (const scriptast::Expr& x : s.expr.args) ExprCheck(x);
                ++forDepth;
                Stmts(s.body);
                --forDepth;
            } else if (s.kind != scriptast::Stmt::Comment) ExprCheck(s.expr);
            Stmts(s.then);
            Stmts(s.otherwise);
        }
    }
};

} // namespace detail

// game.exe's lexer reads a word as the LONGEST name of its function table that starts it (keywords and commands alike):
// "SleepCount" is read as Sleep + Count. A name declared by the script that begins with a command's name breaks there.
inline std::string TablePrefix(const std::string& word) {
    // the 220 words of game.exe's table g_0x78BCA0 (punctuation left out), as dumped from the Russian exe
    static const char* const table[] = {"WorldScript", "Script", "DeclareScript", "GlobalVars", "if", "then", "ActivateTrap", "AddLoot", "AddRectToArea", "AddRoundToArea", "AddMob", "AddObject", "AddUnitToParty", "AddUnitToServer", "AddUnitUnderControl", "AttachParticleSource", "AttachParticles", "Attack", "BlockUnit", "Cast", "CastSpellPoint", "CastSpellUnit", "ConsoleFloat", "ConsoleString", "CopyItems", "CopyLoot", "CopyStats", "Crawl", "CreateFX", "CreateFXSource", "CreateLightning", "CreateParticleSource", "CreateParty", "CreatePointLight", "CreateRandomizedFXSource", "DeleteArea", "DeleteFXSource", "DeleteLightning", "DeleteParticleSource", "DeletePointLight", "Follow", "EnableLever", "EraseQuestItem", "FixItems", "FixWorldTime", "GSSetVar", "GSSetVarMax", "GSDelVar", "GiveDexterity", "GiveIntelligence", "GiveItem", "GiveMoney", "GiveQuestItem", "GiveSkill", "GiveStrength", "GiveUnitQuestItem", "GiveUnitSpell", "GodMode", "Guard", "HideObject", "Idle", "InvokeAlarm", "InflictDamage", "KillScript", "KillUnit", "LeaveToZone", "Lie", "MoveParticleSource", "MovePointLight", "MoveToPoint", "MoveToObject", "PlayAnimation", "PlayFX", "PlayCamera", "PlayMovie", "PlayMusic", "SetPlayerAggression", "QStart", "QFinish", "QObjArea", "QObjSeeObject", "QObjSeeUnit", "QObjKillUnit", "QObjKillGroup", "QObjUse", "QObjGetItem", "QuestComplete", "RecalcMercBriefings", "RedeployParty", "RemoveParty", "RemoveObject", "RemoveObjectFromServer", "RemoveQuestItem", "RemoveUnitFromControl", "RemoveUnitFromParty", "RemoveUnitFromServer", "ResetTarget", "Rest", "RotateTo", "Run", "RunWorldTime", "SendEvent", "SendStringEvent", "Sentry", "SetBackGroundColor", "SetCameraOrientation", "SetCameraPosition", "SetCP", "SetCPFast", "SetCurrentParty", "SetDiplomacy", "SetDirectionToObject", "SetEnemy", "SetParticleSourceSize", "SetPlayer", "SetScience", "SetSpellAggression", "SetSunLight", "SetWaterLevel", "SetWind", "Sleep", "SleepUntil", "SleepUntilIdle", "Stand", "StartAnimation", "ShowCredits", "ShowBitmap", "SwitchLeverState", "SwitchLeverStateEx", "UMAg", "UMAggression", "UMClear", "UMCorpseWatcher", "UMFear", "UMFollow", "UMGuard", "UMGuardEx", "UMPatrol", "UMPatrolAddPoint", "UMPatrolAddPointLook", "UMPatrolClear", "UMPlayer", "UMRevenge", "UMSentry", "UMStandard", "UMSuspection", "WaitEndAnimation", "WaitSegment", "Walk", "For", "ForIf", "Every", "Any", "Sum", "IsEqual", "IsLess", "IsGreater", "IsEqualString", "Add", "Sub", "Mul", "Div", "Not", "HaveItem", "GetObject", "GetObjectByName", "GetObjectByID", "IsInSquare", "UnitInSquare", "GetMercsNumber", "GetZValue", "GetBSZValue", "HP", "MaxHP", "Mana", "MaxMana", "DistanceUnitUnit", "DistanceUnitPoint", "Random", "GetLeverState", "GetX", "GetY", "GetZ", "GetFutureX", "GetFutureY", "GroupSize", "GroupAdd", "GroupSub", "GroupCross", "GroupHas", "GSGetVar", "IsUnitInWater", "UnitSee", "GroupSee", "PlayerSee", "GetDiplomacy", "IsEnemy", "IsAlive", "IsCameraPlaying", "IsDead", "IsPlayerInDanger", "IsPlayerInSafety", "GetPlayerUnits", "GetUnitOfPlayer", "GetPlayer", "GetObjectID", "GetAIClass", "GetLootItemsCount", "GetMoney", "IsAlarm", "AlarmTime", "AlarmPosX", "AlarmPosY", "IsNight", "GetWorldTime", "IsInArea", "WasLooted", "IsUnitVisible", "GetLeader", "IsUnitBlocked", };
    std::string best;
    const std::string w = Low(word);
    for (const char* t : table) {
        const std::string n = Low(t);
        if (w.size() > n.size() && w.compare(0, n.size(), n) == 0 && n.size() > best.size()) best = t;
    }
    return best;
}

inline std::vector<Issue> Check(const std::string& script) {
    std::vector<Issue> out;
    // the compiler crash: Script / DeclareScript as the text's last word
    {
        std::vector<scriptast::detail::Tok> toks = scriptast::detail::Lex(script);
        int last = -1;
        for (int i = static_cast<int>(toks.size()) - 1; i >= 0; --i)
            if (toks[static_cast<size_t>(i)].kind != scriptast::detail::Tok::Comment && toks[static_cast<size_t>(i)].kind != scriptast::detail::Tok::End) { last = i; break; }
        if (last >= 0) {
            const std::string w = Low(toks[static_cast<size_t>(last)].text);
            if (toks[static_cast<size_t>(last)].kind == scriptast::detail::Tok::Word && (w == "script" || w == "declarescript"))
                out.push_back({'E', toks[static_cast<size_t>(last)].line, "the script ends right after '" + toks[static_cast<size_t>(last)].text +
                                                                              "': game.exe's compiler crashes loading this map (bugs.md 12)"});
        }
    }
    const scriptast::Program p = scriptast::Parse(script);
    {   // names the script declares that the lexer would split
        std::set<std::string> seen;
        auto name = [&](const std::string& n, int line, const char* what) {
            if (n.empty() || !seen.insert(Low(n)).second) return;
            const std::string pre = TablePrefix(n);
            if (!pre.empty()) out.push_back({'E', line, std::string(what) + " '" + n + "' begins with '" + pre + "': game.exe's lexer reads it as that name and the rest (rename it)"});
        };
        for (const scriptast::Typed& g : p.globals) name(g.name, p.globalsLine, "the variable");
        for (const scriptast::Script& sc : p.scripts) {
            if (!sc.world) name(sc.name, sc.line, "the script");
            for (const scriptast::Typed& t : sc.params) name(t.name, sc.line, "the parameter");
        }
    }
    for (const scriptast::Script& s : p.scripts) {
        detail::Walker w{out, {}, 0, false, false, 0};
        for (const scriptast::Typed& t : s.params) w.params.insert(Low(t.name));
        w.Stmts(s.body);
        if (w.qstart && !w.qfinish)
            out.push_back({'W', w.qstartLine, "QStart() without QFinish() in the same script: the quest's objective list is never closed"});
    }
    return out;
}

} // namespace enginecheck

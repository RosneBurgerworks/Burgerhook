/*
 * CatBot.cpp
 *
 *  Created on: Dec 30, 2017
 *      Author: nullifiedcat
 */

#include <settings/Bool.hpp>
#include "CatBot.hpp"
#include "common.hpp"
#include "hack.hpp"
#include "PlayerTools.hpp"
#include "e8call.hpp"
#include "NavBot.hpp"
#include "navparser.hpp"
#include "SettingCommands.hpp"
#include "glob.h"

namespace hacks::catbot
{
static settings::Boolean auto_disguise{ "misc.autodisguise", "false" };

static settings::Int abandon_if_ipc_bots_gte{ "cat-bot.abandon-if.ipc-bots-gte", "0" };
static settings::Int abandon_if_humans_lte{ "cat-bot.abandon-if.humans-lte", "0" };
static settings::Int abandon_if_players_lte{ "cat-bot.abandon-if.players-lte", "0" };

static settings::Boolean micspam{ "cat-bot.micspam.enable", "false" };
static settings::Int micspam_on{ "cat-bot.micspam.interval-on", "3" };
static settings::Int micspam_off{ "cat-bot.micspam.interval-off", "60" };

static settings::Boolean auto_crouch{ "cat-bot.auto-crouch", "false" };
static settings::Boolean always_crouch{ "cat-bot.always-crouch", "false" };
static settings::Boolean random_votekicks{ "cat-bot.votekicks", "false" };
static settings::Boolean votekick_rage_only{ "cat-bot.votekicks.rage-only", "false" };
static settings::Boolean autovote_map{ "cat-bot.autovote-map", "false" };

settings::Boolean catbotmode{ "cat-bot.enable", "false" };
settings::Boolean anti_motd{ "cat-bot.anti-motd", "false" };

struct catbot_user_state
{
    int treacherous_kills{ 0 };
};

static std::unordered_map<unsigned, catbot_user_state> human_detecting_map{};

int globerr(const char *path, int eerrno)
{
    logging::Info("%s: %s\n", path, strerror(eerrno));
    // let glob() keep going
    return 0;
}

bool hasEnding(std::string const &fullString, std::string const &ending)
{
    if (fullString.length() >= ending.length())
        return (0 == fullString.compare(fullString.length() - ending.length(), ending.length(), ending));
    else
        return false;
}

std::vector<std::string> config_list(std::string in)
{
    std::string complete_in = paths::getConfigPath() + "/" + in;
    if (!hasEnding(complete_in, ".conf"))
        complete_in = complete_in + ".conf";
    std::vector<std::string> config_vec;
    size_t i;
    int flags = 0;
    glob_t results;
    int ret;

    flags |= 0;
    ret = glob(complete_in.c_str(), flags, globerr, &results);
    if (ret != 0)
    {
        std::string ret_str;
        switch (ret)
        {
        case GLOB_ABORTED:
            ret_str = "filesystem problem";
            break;
        case GLOB_NOMATCH:
            ret_str = "no match of pattern";
            break;
        case GLOB_NOSPACE:
            ret_str = "out of memory";
            break;
        default:
            ret_str = "Unknown problem";
            break;
        }

        logging::Info("problem with %s (%s), stopping early\n", in.c_str(), ret_str.c_str());
        return config_vec;
    }

    for (i = 0; i < results.gl_pathc; i++)
        // /configs/ is 9 extra chars i have to remove
        config_vec.push_back(std::string(results.gl_pathv[i]).substr(paths::getDataPath().length() + 9));

    globfree(&results);
    return config_vec;
}
static std::string blacklist;
void do_random_votekick()
{
    std::vector<int> targets;
    player_info_s local_info;

    if (CE_BAD(LOCAL_E) || !GetPlayerInfo(LOCAL_E->m_IDX, &local_info))
        return;
    for (int i = 1; i < g_GlobalVars->maxClients; ++i)
    {
        player_info_s info;
        if (!GetPlayerInfo(i, &info) || !info.friendsID)
            continue;
        if (g_pPlayerResource->GetTeam(i) != g_pLocalPlayer->team)
            continue;
        if (info.friendsID == local_info.friendsID)
            continue;
        auto &pl = playerlist::AccessData(info.friendsID);
        if (votekick_rage_only && pl.state != playerlist::k_EState::RAGE)
            continue;
        if (pl.state != playerlist::k_EState::RAGE && pl.state != playerlist::k_EState::DEFAULT)
            continue;

        targets.push_back(info.userID);
    }

    if (targets.empty())
        return;

    int target = targets[rand() % targets.size()];
    player_info_s info;
    if (!GetPlayerInfo(GetPlayerForUserID(target), &info))
        return;
    hack::ExecuteCommand("callvote kick \"" + std::to_string(target) + " cheating\"");
}
void SendNetMsg(INetMessage &msg)
{

}

class CatBotEventListener2 : public IGameEventListener2
{
    void FireGameEvent(IGameEvent *) override
    {
        // vote for current map if catbot mode and autovote is on
        if (catbotmode && autovote_map)
            g_IEngine->ServerCmd("next_map_vote 0");
    }
};

CatBotEventListener2 &listener2()
{
    static CatBotEventListener2 object{};
    return object;
}

Timer timer_votekicks{};
static Timer timer_catbot_list{};
static Timer timer_abandon{};

static int count_ipc = 0;
static std::vector<unsigned> ipc_list{ 0 };

static bool waiting_for_quit_bool{ false };
static Timer waiting_for_quit_timer{};

static std::vector<unsigned> ipc_blacklist{};

#if ENABLE_IPC
void update_ipc_data(ipc::user_data_s &data)
{
    data.ingame.bot_count = count_ipc;
}
#endif

Timer level_init_timer{};

Timer micspam_on_timer{};
Timer micspam_off_timer{};

Timer crouchcdr{};
void smart_crouch()
{
    if (g_Settings.bInvalid)
        return;
    if (!current_user_cmd)
        return;
    if (*always_crouch)
    {
        current_user_cmd->buttons |= IN_DUCK;
        if (crouchcdr.test_and_set(10000))
            current_user_cmd->buttons &= ~IN_DUCK;
        return;
    }
    bool foundtar      = false;
    static bool crouch = false;
    if (crouchcdr.test_and_set(2000))
    {
        for (int i = 0; i <= g_IEngine->GetMaxClients(); i++)
        {
            auto ent = ENTITY(i);
            if (CE_BAD(ent) || ent->m_Type() != ENTITY_PLAYER || ent->m_iTeam() == LOCAL_E->m_iTeam() || !(ent->hitboxes.GetHitbox(0)) || !(ent->m_bAlivePlayer()) || !player_tools::shouldTarget(ent))
                continue;
            bool failedvis = false;
            for (int j = 0; j < 18; j++)
                if (IsVectorVisible(g_pLocalPlayer->v_Eye, ent->hitboxes.GetHitbox(j)->center))
                    failedvis = true;
            if (failedvis)
                continue;
            for (int j = 0; j < 18; j++)
            {
                if (!LOCAL_E->hitboxes.GetHitbox(j))
                    continue;
                // Check if they see my hitboxes
                if (!IsVectorVisible(ent->hitboxes.GetHitbox(0)->center, LOCAL_E->hitboxes.GetHitbox(j)->center) && !IsVectorVisible(ent->hitboxes.GetHitbox(0)->center, LOCAL_E->hitboxes.GetHitbox(j)->min) && !IsVectorVisible(ent->hitboxes.GetHitbox(0)->center, LOCAL_E->hitboxes.GetHitbox(j)->max))
                    continue;
                foundtar = true;
                crouch   = true;
            }
        }
        if (!foundtar && crouch)
            crouch = false;
    }
    if (crouch)
        current_user_cmd->buttons |= IN_DUCK;
}

CatCommand print_ammo("debug_print_ammo", "debug",
                      []()
                      {
                          if (CE_BAD(LOCAL_E) || !LOCAL_E->m_bAlivePlayer() || CE_BAD(LOCAL_W))
                              return;
                          logging::Info("Current slot: %d", re::C_BaseCombatWeapon::GetSlot(RAW_ENT(LOCAL_W)));
                          for (int i = 0; i < 10; i++)
                              logging::Info("Ammo Table %d: %d", i, CE_INT(LOCAL_E, netvar.m_iAmmo + i * 4));
                      });
static Timer disguise{};
static std::string health = "Health: 0/0";
static std::string ammo   = "Ammo: 0/0";
static int max_ammo;
static CachedEntity *local_w;
// TODO: add more stuffs
static void cm()
{
    if (!*catbotmode)
        return;

    if (CE_GOOD(LOCAL_E))
    {
        if (LOCAL_W != local_w)
        {
            local_w  = LOCAL_W;
            max_ammo = 0;
        }
        float max_hp  = g_pPlayerResource->GetMaxHealth(LOCAL_E);
        float curr_hp = CE_INT(LOCAL_E, netvar.iHealth);
        int ammo0     = CE_INT(LOCAL_E, netvar.m_iClip2);
        int ammo2     = CE_INT(LOCAL_E, netvar.m_iClip1);
        if (ammo0 + ammo2 > max_ammo)
            max_ammo = ammo0 + ammo2;
        health = format("Health: ", curr_hp, "/", max_hp);
        ammo   = format("Ammo: ", ammo0 + ammo2, "/", max_ammo);
    }
    if (g_Settings.bInvalid)
        return;

    if (CE_BAD(LOCAL_E) || CE_BAD(LOCAL_W))
        return;

    if (*auto_crouch)
        smart_crouch();

    //
    static const int classes[3]{ tf_spy, tf_sniper, tf_pyro };
    if (*auto_disguise && g_pPlayerResource->GetClass(LOCAL_E) == tf_spy && !IsPlayerDisguised(LOCAL_E) && disguise.test_and_set(3000))
    {
        int teamtodisguise = (LOCAL_E->m_iTeam() == TEAM_RED) ? TEAM_RED - 1 : TEAM_BLU - 1;
        int classtojoin    = classes[rand() % 3];
        g_IEngine->ClientCmd_Unrestricted(format("disguise ", classtojoin, " ", teamtodisguise).c_str());
    }
}

static Timer unstuck{};
static int unstucks;
void update()
{
    if (g_Settings.bInvalid)
        return;

    if (!catbotmode)
        return;

    if (CE_BAD(LOCAL_E))
        return;

    if (LOCAL_E->m_bAlivePlayer())
    {
        unstuck.update();
        unstucks = 0;
    }
    if (unstuck.test_and_set(10000))
    {
        unstucks++;
        // Send menuclosed to tell the server that we want to respawn
        hack::command_stack().push("menuclosed");
        // If that didnt work, force pick a team and class
        if (unstucks > 3)
            hack::command_stack().push("autoteam; join_class sniper");
    }

    if (micspam)
    {
        if (micspam_on && micspam_on_timer.test_and_set(*micspam_on * 1000))
            g_IEngine->ClientCmd_Unrestricted("+voicerecord");
        if (micspam_off && micspam_off_timer.test_and_set(*micspam_off * 1000))
            g_IEngine->ClientCmd_Unrestricted("-voicerecord");
    }

    if (random_votekicks && timer_votekicks.test_and_set(5000))
        do_random_votekick();
    if (timer_abandon.test_and_set(2000) && level_init_timer.check(13000))
    {
        count_ipc = 0;
        ipc_list.clear();
        int count_total = 0;

        for (int i = 1; i <= g_IEngine->GetMaxClients(); ++i)
        {
            if (g_IEntityList->GetClientEntity(i))
                ++count_total;
            else
                continue;

            player_info_s info{};
            if (!GetPlayerInfo(i, &info))
                continue;
            if (playerlist::AccessData(info.friendsID).state == playerlist::k_EState::CAT)
                --count_total;

            if (playerlist::AccessData(info.friendsID).state == playerlist::k_EState::IPC || playerlist::AccessData(info.friendsID).state == playerlist::k_EState::TEXTMODE)
            {
                ipc_list.push_back(info.friendsID);
                ++count_ipc;
            }
        }

        if (abandon_if_ipc_bots_gte)
        {
            if (count_ipc >= int(abandon_if_ipc_bots_gte))
            {
                // Store local IPC Id and assign to the quit_id variable for later comparisions
                unsigned local_ipcid = ipc::peer->client_id;
                unsigned quit_id     = local_ipcid;

                // Iterate all the players marked as bot
                for (auto &id : ipc_list)
                {
                    // We already know we shouldn't quit, so just break out of the loop
                    if (quit_id < local_ipcid)
                        break;

                    // Reduce code size
                    auto &peer_mem = ipc::peer->memory;

                    // Iterate all ipc peers
                    for (unsigned i = 0; i < cat_ipc::max_peers; i++)
                    {
                        // If that ipc peer is alive and in has the steamid of that player
                        if (!peer_mem->peer_data[i].free && peer_mem->peer_user_data[i].friendid == id)
                        {
                            // Check against blacklist
                            if (std::find(ipc_blacklist.begin(), ipc_blacklist.end(), i) != ipc_blacklist.end())
                                continue;

                            // Found someone with a lower ipc id
                            if (i < local_ipcid)
                            {
                                quit_id = i;
                                break;
                            }
                        }
                    }
                }
                // Only quit if you are the player with the lowest ipc id
                if (quit_id == local_ipcid)
                {
                    // Clear blacklist related stuff
                    waiting_for_quit_bool = false;
                    ipc_blacklist.clear();

                    logging::Info("Abandoning because there are %d local players "
                                  "in game, and abandon_if_ipc_bots_gte is %d.",
                                  count_ipc, int(abandon_if_ipc_bots_gte));
                    tfmm::abandon();
                    return;
                }
                else
                {
                    if (!waiting_for_quit_bool)
                    {
                        // Waiting for that ipc id to quit, we use this timer in order to blacklist
                        // ipc peers which refuse to quit for some reason
                        waiting_for_quit_bool = true;
                        waiting_for_quit_timer.update();
                    }
                    else
                    {
                        // IPC peer isn't leaving, blacklist for now
                        if (waiting_for_quit_timer.test_and_set(10000))
                        {
                            ipc_blacklist.push_back(quit_id);
                            waiting_for_quit_bool = false;
                        }
                    }
                }
            }
            else
            {
                // Reset Bool because no reason to quit
                waiting_for_quit_bool = false;
                ipc_blacklist.clear();
            }
        }
        if (abandon_if_humans_lte)
        {
            if (count_total - count_ipc <= int(abandon_if_humans_lte))
            {
                logging::Info("Abandoning because there are %d non-bots in "
                              "game, and abandon_if_humans_lte is %d.",
                              count_total - count_ipc, int(abandon_if_humans_lte));
                tfmm::abandon();
                return;
            }
        }
        if (abandon_if_players_lte)
        {
            if (count_total <= int(abandon_if_players_lte))
            {
                logging::Info("Abandoning because there are %d total players "
                              "in game, and abandon_if_players_lte is %d.",
                              count_total, int(abandon_if_players_lte));
                tfmm::abandon();
                return;
            }
        }
    }
}

void init()
{
    // g_IEventManager2->AddListener(&listener(), "player_death", false);
    g_IEventManager2->AddListener(&listener2(), "vote_maps_changed", false);
}

void shutdown()
{
    // g_IEventManager2->RemoveListener(&listener());
    g_IEventManager2->RemoveListener(&listener2());
}

#if ENABLE_VISUALS
static void draw()
{
    if (!catbotmode || !anti_motd)
        return;
    if (CE_BAD(LOCAL_E) || !LOCAL_E->m_bAlivePlayer())
        return;
    AddCenterString(health, colors::green);
    AddCenterString(ammo, colors::yellow);
}
#endif

static InitRoutine runinit(
    []()
    {
        EC::Register(EC::CreateMove, cm, "cm_catbot", EC::average);
        EC::Register(EC::CreateMove, update, "cm2_catbot", EC::average);
        EC::Register(EC::LevelInit, level_init, "levelinit_catbot", EC::average);
        EC::Register(EC::Shutdown, shutdown, "shutdown_catbot", EC::average);
#if ENABLE_VISUALS
        EC::Register(EC::Draw, draw, "draw_catbot", EC::average);
#endif
        init();
    });
} // namespace hacks::catbot

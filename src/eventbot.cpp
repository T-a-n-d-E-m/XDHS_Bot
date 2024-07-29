// This is free and unencumbered software released into the public domain.
//
// Anyone is free to copy, modify, publish, use, compile, sell, or
// distribute this software, either in source code form or as a compiled
// binary, for any purpose, commercial or non-commercial, and by any
// means.
//
// In jurisdictions that recognize copyright laws, the author or authors
// of this software dedicate any and all copyright interest in the
// software to the public domain. We make this dedication for the benefit
// of the public at large and to the detriment of our heirs and
// successors. We intend this dedication to be an overt act of
// relinquishment in perpetuity of all present and future rights to this
// software under copyright law.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
// IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
// OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
//
// For more information, please refer to <http://unlicense.org/>

// FIXME: Weird bug after creating a draft or using /edit_draft to change the banner for a posted draft: The first embed button clicked after this will cause the embed to flash and redraw. This only happens after the first button click though... why?

// TODO: Make most commands ephemeral so it doesn't matter where they are used.
// TODO: Store the pod allocations somewhere so they can be manipulated after they've been posted.
// TODO: Need a /swap_players command? Swap two players in different pods, update roles and threads accordingly.
// TODO: Create a message that explains what all the sign up options are and what the expectation for minutemages is.
// Note: Only one minutemage will be asked to fill a seat.
// FIXME: dpp::utility::read_file can throw... just use slurp
// FIXME: dpp::message::add_file is deprecated

// Nice functionality, but not needed before going live
// TODO: Add "Devotion Week" and "Meme Week" to the banner creation command.
// TODO: Alert hosts when a drafter is a first time player and recommend longer timers.
// TODO: Do we want to send automated messages to people when their drop count exceeds a certain threshold?

// Code/performance improvements
// TODO: Thread pools for database connections
// TODO: All the blit_ functions can be rewritten to use SIMD ops
// TODO: Cleanup inconsistent use of char* and std::string in database functions.
// TODO: Rename "Event" to draft where appropriate


// C libraries
#include <alloca.h>
#include <assert.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// C++ libraries
#include <cinttypes>

// System libraries
#include <curl/curl.h>

// User libraries
#include <dpp/dpp.h>
#include <fmt/format.h>

// Local libraries
#ifndef STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_FAILURE_USERMSG
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS (1<<11)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "stb_image.h"
#pragma GCC diagnostic pop
#endif // #ifndef

#ifndef STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#include "stb_image_resize2.h"
#pragma GCC diagnostic pop
#endif // #ifndef

#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#endif // #ifndef

#ifndef STB_TRUETYPE_IMPLEMENTATION
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#endif // #ifndef


struct Config {
	char* mysql_host;
	char* mysql_username;
	char* mysql_password;
	char* mysql_database;
	unsigned short mysql_port;
	char* logfile_path;
	char* discord_token;
	char* xmage_server;
	char* eventbot_host;
	char* api_key;
	char* imgur_client_secret;

	// There's no real need to ever free this structure as the OS will clean it up for us on program exit, but
	// leak testing with Valgrind is easier if we free it ourselves.
	~Config() {
		if(mysql_host != NULL)          free(mysql_host);
		if(mysql_username != NULL)      free(mysql_username);
		if(mysql_password != NULL)      free(mysql_password);
		if(mysql_database != NULL)      free(mysql_database);
		if(logfile_path != NULL)        free(logfile_path);
		if(discord_token != NULL)       free(discord_token);
		if(xmage_server != NULL)        free(xmage_server);
		if(eventbot_host != NULL)       free(eventbot_host);
		if(api_key != NULL)             free(api_key);
		if(imgur_client_secret != NULL) free(imgur_client_secret);
	}
} g_config;
#include "config.h"

#include "date/tz.h"  // Howard Hinnant's date and timezone library.
#include "constants.h"
#include "curl.h"
#include "image.h"
#include "database.h"
#include "result.h"
#include "log.h"
#include "scope_exit.h"
#include "utf8.h"

// Some useful shorthands for common types.
using  u8 = std::uint8_t;
using  s8 = std::int8_t;
using u16 = std::uint16_t;
using s16 = std::int16_t;
using u32 = std::uint32_t;
using s32 = std::int32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;
using f32 = float;
using f64 = double;

#define BIT_SET(value,mask) (((value) & (mask)) != (0))

// FIXME: This is an awful hack so I don't have to deal with timezone conversion stuff. Add this to timestamps inserted in the database by Badge Bot by this amount. This is the UTC offset of where the server running this code is.
static const int SERVER_TIME_ZONE_OFFSET = (60*60*10);

// How many seconds before a draft the pre-draft reminder message should be sent.
static const time_t SECONDS_BEFORE_DRAFT_TO_SEND_REMINDER   = (60*60*1);

// How many seconds before a draft to remind tentatives to confirm their status.
static const time_t SECONDS_BEFORE_DRAFT_TO_PING_TENTATIVES = (60*10);

// How long after a draft to wait before deleting the sign up posts from the #-pre-register channel.
static const time_t SECONDS_AFTER_DRAFT_TO_DELETE_POSTS     = (60*60*5);

// How often often to spin up the thread that sends the pre-draft reminders, tentatives ping, etc.
static const dpp::timer JOB_THREAD_TICK_RATE                = 15;

// How long we allow for deck construction.
static const time_t DECK_CONSTRUCTION_MINUTES               = (10*60);

// The directory where the RELEASE build is run from.
static const char* EXPECTED_WORKING_DIR                 = "/opt/EventBot";

static const char* CONFIG_FILE_NAME = "bot.ini";

// The bot is designed to run in two modes, Debug and Release. Debug builds will only run on the XDHS Dev server and Release builds will only run on the public XDHS server.
// In the future we might want to control these values with a bot command, but for now we'll simply hard code them in.
#ifdef DEBUG
// The bot will be running in debug mode on the XDHS Dev server.
static const char* BUILD_MODE                    = "Debug";
static const u64 GUILD_ID                        = 882164794566791179;
static const u64 PRE_REGISTER_CHANNEL_ID         = 907524659099099178; // Default channel to post the draft sign up.
static const u64 CURRENT_DRAFT_MANAGEMENT_ID     = 1087299085612109844;
static const u64 IN_THE_MOMENT_DRAFT_CHANNEL_ID  = 1075355794507305001;
static const u64 BOT_COMMANDS_CHANNEL_ID         = 885048614190190593;
static const u64 DECK_SCREENSHOTS_CHANNEL_ID     = 1155769474520199279;
static const u64 ROLE_SELF_ASSIGNMENT_CHANNEL_ID = 1155771897225674752;
static const u64 P1P1_AND_DRAFT_LOG_CHANNEL_ID   = 1155772743485235200;
static const u64 FEEDBACK_CHANNEL_ID             = 1155773361104887880;
static const u64 CALENDAR_CHANNEL_ID             = 1155774664732323952;
static const u64 XDHS_TEAM_ROLE_ID               = 885054778978234408;
static const u64 XDHS_HOST_ROLE_ID               = 1091275398639267881;
static const u64 MINUTEMAGE_ROLE_ID              = 1156767797192437891;
#endif

#ifdef RELEASE
// The bot will be running in release mode on the XDHS public server.
static const char* BUILD_MODE                    = "Release";
static const u64 GUILD_ID                        = 528728694680715324;
static const u64 PRE_REGISTER_CHANNEL_ID         = 753639027428687962; // Default channel to post the draft sign up.
static const u64 CURRENT_DRAFT_MANAGEMENT_ID     = 921027014822068234;
static const u64 IN_THE_MOMENT_DRAFT_CHANNEL_ID  = 535127333401657354;
static const u64 BOT_COMMANDS_CHANNEL_ID         = 753637350877429842;
static const u64 DECK_SCREENSHOTS_CHANNEL_ID     = 647073844649000962;
static const u64 ROLE_SELF_ASSIGNMENT_CHANNEL_ID = 663422413891174400;
static const u64 P1P1_AND_DRAFT_LOG_CHANNEL_ID   = 796861143594958868;
static const u64 FEEDBACK_CHANNEL_ID             = 822015209756950528;
static const u64 CALENDAR_CHANNEL_ID             = 794227134892998666;
static const u64 XDHS_TEAM_ROLE_ID               = 639451893399027722;
static const u64 XDHS_HOST_ROLE_ID               = 1051631435506794657;
static const u64 MINUTEMAGE_ROLE_ID              = 843796946984370176;
#endif

// Have the bot slash commands been registered?
static bool g_commands_registered                = false;

// Some serious errors will ping this person as the error needs attention ASAP.
static const u64 TANDEM_DISCORD_ID               = 767299805348233217;

// The name of the currently active draft or the next upcoming draft.
// NOTE: When the bot starts there is a brief window of time where this has not yet been set.
static std::string g_current_draft_code;

// This signal handling is extremely basic but it should be all this bot needs.
static int g_exit_code = 0;

static void sig_handler(int signo) {
	// TODO: All database writes need to be done as transactions so a sudden shutdown of the service here won't mess up the database.
	switch(signo) {
		case SIGINT:  // Fall through
		case SIGABRT: // Fall through
		case SIGHUP:  // Fall through
		case SIGTERM:
			log(LOG_LEVEL_INFO, "Caught signal %s", strsignal(signo));
			break;

		default: log(LOG_LEVEL_INFO, "Caught unhandled signal: %d", signo);
	}
	g_exit_code = signo;
}

static std::string to_upper(const std::string_view src) {
	const size_t len = src.length();
	std::string result;
	result.reserve(len);
	for(size_t i = 0; i < len; ++i) {
		result += std::toupper(src[i]);
	}
	return result;
}

static std::string random_string(const int len) {
	static const char* c = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	std::string result;
	result.reserve(len+1);
	for(int i = 0; i < len; ++i) {
		result += c[rand() % strlen(c)];
	}
	return result;
}


// Reject any banner images that exceed this size.
static const size_t DOWNLOAD_BYTES_MAX = (3 * 1024 * 1024);

// Send a message to a channel. Mostly used for posting what the bot is currently doing.
static void send_message(dpp::cluster& bot, const u64 guild_id, const u64 channel_id, const std::string& text) {
	dpp::message message;
	message.set_type(dpp::message_type::mt_default);
	message.set_guild_id(guild_id);
	message.set_channel_id(channel_id);
	message.set_allowed_mentions(false, false, false, false, {}, {});
	message.set_content(text);
	bot.message_create(message);
}

struct MTG_Draftable_Set {
	const char* code;
	const char* name;
	int pack_images;
	bool key_art;
};

// Not an complete list of all Magic sets - Just the sets we draft or may draft.
// TODO: This is probably not the best way to handle this as new sets (official or not) need to keep this updated.
// Perhaps move it to the database and use a /add_set command?
static const MTG_Draftable_Set g_draftable_sets[] = {
	{"LEA", "Limited Edition Alpha",                       1, false},
	{"LEB", "Limited Edition Beta",                        1, false},
	{"ARN", "Arabian Nights",                              1, false},
	{"ATQ", "Antiquities",                                 1, false},
	{"LEG", "Legends",                                     1, false},
	{"DRK", "The Dark",                                    1, false},
	{"FEM", "Fallen Empires",                              1, false},
	{"ICE", "Ice Age",                                     5, false},
	{"CHR", "Chronicles",                                  1, false},
	{"HML", "Homelands",                                   1, false},
	{"ALL", "Alliances",                                   1, false},
	{"MIR", "Mirage",                                      5, false},
	{"VIS", "Visions",                                     1, false},
	{"WTH", "Weatherlight",                                1, false},
	{"POR", "Portal",                                      5, false},
	{"P02", "Portal Second Age",                           1, false},
	{"TMP", "Tempest",                                     3, false},
	{"STH", "Stronghold",                                  1, false},
	{"EXO", "Exodus",                                      1, false},
	{"USG", "Urza's Saga",                                 3, false},
	{"ULG", "Urza's Legacy",                               1, false},
	{"UDS", "Urza's Destiny",                              1, false},
	{"PTK", "Portal Three Kingdoms",                       1, false},
	{"MMQ", "Mercadian Masques",                           3, false},
	{"NEM", "Nemesis",                                     1, false},
	{"PCY", "Prophecy",                                    1, false},
	{"INV", "Invasion",                                    3, false},
	{"PLS", "Planeshift",                                  1, false},
	{"APC", "Apocalypse",                                  1, false},
	{"ODY", "Odyssey",                                     3, false},
	{"TOR", "Torment",                                     1, false},
	{"JUD", "Judgment",                                    1, false},
	{"ONS", "Onslaught",                                   3, false},
	{"LGN", "Legions",                                     3, false},
	{"SCG", "Scourge",                                     3, false},
	{"MRD", "Mirrodin",                                    3, false},
	{"DST", "Darksteel",                                   1, false},
	{"5DN", "Fifth Dawn",                                  1, false},
	{"CHK", "Champions of Kamigawa",                       3, false},
	{"BOK", "Betrayers of Kamigawa",                       1, false},
	{"SOK", "Saviors of Kamigawa",                         1, false},
	{"RAV", "Ravnica: City of Guilds",                     5, false},
	{"GPT", "Guildpact",                                   1, false},
	{"DIS", "Dissension",                                  1, false},
	{"CSP", "Coldsnap",                                    3, false},
	{"TSP", "Time Spiral",                                 3, false},
	{"PLC", "Planar Choas",                                1, false},
	{"FUT", "Future Sight",                                1, false},
	{"10E", "Tenth Edition",                               3, false},
	{"ME1", "Masters Edition",                             1, false},
	{"LRW", "Lorwyn",                                      3, false},
	{"MOR", "Morningtide",                                 1, false},
	{"SHM", "Shadowmoor",                                  3, false},
	{"EVE", "Eventide",                                    1, false},
	{"ME2", "Masters Edition II",                          1, false},
	{"ALA", "Shards of Alara",                             3, false},
	{"CON", "Conflux",                                     1, false},
	{"ARB", "Alara Reborn",                                1, false},
	{"M10", "Magic 2010",                                  3, false},
	{"ME3", "Masters Edition III",                         1, false},
	{"ZEN", "Zendikar",                                    3, false},
	{"WWK", "Worldwake",                                   1, false},
	{"ROE", "Rise of the Eldrazi",                         3, false},
	{"M11", "Magic 2011",                                  3, false},
	{"ME4", "Masters Edition IV",                          1, false},
	{"SOM", "Scars of Mirrodin",                           5, false},
	{"MBS", "Mirrodin Besieged",                           3, false},
	{"NPH", "New Phyrexia",                                3, false},
	{"M12", "Magic 2012",                                  5, false},
	{"ISD", "Innistrad",                                   5, false},
	{"DKA", "Dark Ascension",                              3, false},
	{"AVR", "Avacyn Restored",                             5, false},
	{"M13", "Magic 2013",                                  5, false},
	{"RTR", "Return to Ravnica",                           5, false},
	{"GTC", "Gatecrash",                                   5, false},
	{"DGM", "Dragon's Maze",                               3, false},
	{"MMA", "Modern Masters",                              3, false},
	{"M14", "Magic 2014",                                  5, false},
	{"THS", "Theros",                                      4, false},
	{"BNG", "Born of the Gods",                            3, false},
	{"JOU", "Journey into Nyx",                            3, false},
	{"CNS", "Conspiracy",                                  3, false},
	{"VMA", "Vintage Masters",                             2, false},
	{"M15", "Magic 2015",                                  5, false},
	{"KTK", "Khans of Tarkir",                             5, false},
	{"FRF", "Fate Reforged",                               3, false},
	{"DTK", "Dragons of Tarkir",                           4, false},
	{"TPR", "Tempest Remastered",                          1, false},
	{"MM2", "Modern Masters 2015",                         3, false},
	{"ORI", "Magic Origins",                               4, false},
	{"BFZ", "Battle for Zendikar",                         5, false},
	{"OGW", "Oath of the Gatewatch",                       3, false},
	{"SOI", "Shadows over Innistrad",                      3, false},
	{"EMA", "Eternal Masters",                             3, false},
	{"EMN", "Eldritch Moon",                               3, false},
	{"CN2", "Conspiracy: Take the Crown",                  3, false},
	{"KLD", "Kaladesh",                                    3, false},
	{"AER", "Aether Revolt",                               3, false},
	{"MM3", "Modern Masters 2017",                         3, false},
	{"AKH", "Amonkhet",                                    3, false},
	{"HOU", "Hour of Devastation",                         5, false},
	{"XLN", "Ixalan",                                      3, false},
	{"IMA", "Iconic Masters",                              3, false},
	{"RIX", "Rivals of Ixalan",                            5, false},
	{"A25", "Masters 25",                                  3, false},
	{"DOM", "Dominaria",                                   5, false},
	{"M19", "Core Set 2019",                               5, false},
	{"GRN", "Guilds of Ravnica",                           5, false},
	{"UMA", "Ultimate Masters",                            3, false},
	{"RNA", "Ravnica Allegiance",                          5, false},
	{"WAR", "War of the Spark",                            3, false},
	{"MH1", "Modern Horizons",                             3, false},
	{"M20", "Core Set 2020",                               3, false},
	{"ELD", "Throne of Eldraine",                          3, false},
	{"MB1", "Mystery Booster",                             1, false},
	{"THB", "Theros Beyond Death",                         3, false},
	{"IKO", "Ikoria: Lair of Behemoths",                   3, false},
	{"M21", "Core Set 2021",                               3, false},
	{"2XM", "Double Masters",                              3, false},
	{"AKR", "Amonkhet Remastered",                         0,  true},
	{"ZNR", "Zendikar Rising",                             3, false},
	{"KLR", "Kaladesh Remastered",                         0,  true},
	{"CMD", "Commander Legends",                           1, false},
	{"KHM", "Kaldheim",                                    4, false},
	{"TSR", "Time Spiral Remastered",                      3, false},
	{"STX", "Strixhaven: School of Mages",                 5, false},
	{"MH2", "Modern Horizons 2",                           3, false},
	{"AFR", "Adventures in the Forgotten Realms",          1, false},
	{"MID", "Innistrad: Midnight Hunt",                    1, false},
	{"VOW", "Innistrad: Crimson Vow",                      1, false},
	{"NEO", "Kamigawa: Neon Dynasty",                      1, false},
	{"SNC", "Streets of New Capenna",                      1, false},
	{"CLB", "Commander Legends: Battle for Baldur's Gate", 1, false},
	{"2X2", "Double Masters 2022",                         1, false},
	{"DMU", "Dominaria United",                            1, false},
	{"BRO", "The Brothers' War",                           1, false},
	{"DMR", "Dominaria Remastered",                        1, false},
	{"ONE", "Phyrexia: All Will Be One",                   1, false},
	{"SIR", "Shadows over Innistrad Remastered",           0,  true},
	{"MOM", "March of the Machine",                        1, false},
	{"LTR", "The Lord of the Rings: Tales of Middle-earth",1, false},
	{"WOE", "Wilds of Eldraine",                           1, false},
	{"LCI", "Lost Caverns of Ixalan",                      1, false},
	{"RVR", "Ravnica Remastered",                          1, false},
	{"MKM", "Murders at Karlov Manor",                     1, false},
	{"OTJ", "Outlaws of Thunder Junction",                 1, false},
	{"MH3", "Modern Horizons 3",                           1, false},
	{"BLB", "Bloomburrow",                                 1, false},
	{"DSK", "Duskmourn: House of Horror",                  0, false}, // TODO: Needs art
	{"FDN", "Foundations",                                 0, false}, // TODO: Needs art

	// FIXME: Find art for these from their full name, not set codes.
	{"INVR", "Invasion Remastered",                        0,  true},
	{"KMGR", "Kamigawa Remastered",                        0,  true},
	{"PMMA", "Pre Mirage Masters",                         0, false},
	{"GBMA", "Garbage Masters",                            0, false},
	{"SLCS", "The Sliver Core Set",                        0, false},
	{"USGR", "Urza Block Redeemed",                        0, false},
	{"TWAR", "Total WAR",                                  0, false},
	{"ATQR", "Antiquities Reforged",                       0, false},
	{"ISDR", "Innistrad Remastered ",                      0, false}, // NOTE: Not to be confused with SIR
	{"CRSN", "Core Resonance",                             0, false},
	{"DOMR", "Dominaria Revised",                          0, false},
	{"10LE", "10 Life Edition",                            0, false},
	{"GLNT", "Ravnica Revolution: GLINT",                  0, false},
};

static const size_t SET_COUNT = sizeof(g_draftable_sets) / sizeof(MTG_Draftable_Set);

#if 0
void emit_set_list_code() {
	// Find the longest set name
	size_t max = 0;
	for(size_t i = 0; i < SET_COUNT; ++i) {
		size_t len = strlen(g_draftable_sets[i].name) + strlen(g_draftable_sets[i].code);
		max = (max > len) ? max : len;
	}
	char* whitespace = (char*) alloca(max+1);
	fprintf(stdout, "static const MTG_Draftable_Set g_draftable_sets[] = {\n");
	for(size_t i = 0; i < SET_COUNT; ++i) {
		size_t needed = max - (strlen(g_draftable_sets[i].code)+strlen(g_draftable_sets[i].name));
		for(size_t j = 0; j < needed; ++j) {
			whitespace[j] = ' ';
		}
		whitespace[needed] = 0;
		fprintf(stdout, "\t{\"%s\", \"%s\",%s%d, false},\n",
			g_draftable_sets[i].code, g_draftable_sets[i].name, whitespace, g_draftable_sets[i].pack_images);
	}
	fprintf(stdout, "};");
}
#endif

// Hardly the most efficient way to search, but until the set list has thousands of entries this will be fast enough.
static const char* get_set_name_from_code(const char* code) {
	for(size_t i = 0; i < SET_COUNT; ++i) {
		if(strcasecmp(g_draftable_sets[i].code, code) == 0) {
			return g_draftable_sets[i].name;
		}
	}
	return NULL;
}

static const MTG_Draftable_Set* get_set_from_name(const char* name) {
	for(size_t i = 0; i < SET_COUNT; ++i) {
		if(strcasecmp(g_draftable_sets[i].name, name) == 0) {
			return &g_draftable_sets[i];
		}
	}
	return NULL;
}

static const MTG_Draftable_Set* get_set_from_code(const char* code) {
	for(size_t i = 0; i < SET_COUNT; ++i) {
		if(strcasecmp(g_draftable_sets[i].code, code) == 0) {
			return &g_draftable_sets[i];
		}
	}
	return NULL;
}


// Parse the set_codes string (e.g. TMP,EXO,ELD,ISD,...) and expand them into full set names (e.g. Tempest, Exodus, Eldraine, Innistrad, ...) writing the results into the out variable.
static const void expand_set_list(const char* set_codes, const size_t len, char* out, size_t out_len) {
	if((set_codes == NULL) || (len == 0) || (out == NULL) || (out_len == 0)) return;

	// Make a mutable copy of the set_codes string
	char* str = (char*)alloca(len+1);
	memcpy(str, set_codes, len+1);
	char* str_ptr = str;

	char token[256]; // FIXME: A malicious or careless user could overflow this...
	char* token_ptr = token;

	int token_count = 0;
	int offset = 0;

	while(true) {
		while(*str_ptr != ',' && *str_ptr != '/' && *str_ptr != '-' && *str_ptr != '\0') {
			*token_ptr++ = *str_ptr++;
		}

		*token_ptr = '\0';

		const char* name = token;
		const char* lookup = get_set_name_from_code(token);
		if(lookup != NULL) {
			name = lookup;
		}
		offset += snprintf(out + offset, out_len - offset, "> %s\n", name);

		if(*str_ptr == '\0') break;

		++token_count;
		token_ptr = token;
		str_ptr++;
	}
	out[offset] = 0;
}

// The maximum length of a draft format string that we will handle. Anything longer is rejected as an error.
static const size_t FORMAT_STRING_LEN_MAX = 128;

// The maximum amount of packs we support in a draft. As of writing this, four is the highest (IIRC) number of packs we've ever used in a draft, and ever needing more than six is extremely unlikely.
static const int PACKS_PER_DRAFT_MAX = 6;


struct Set_List {
	int count;
	const MTG_Draftable_Set* set[PACKS_PER_DRAFT_MAX];
};

// Parse a string like "TSP/FUT/PLC" and get pointers to an MTG_Draftable_Set for each.
static Set_List get_set_list_from_string(const char* format) {
	size_t len = strlen(format);
	char* str = (char*)alloca(len+1);
	memcpy(str, format, len+1);
	for(size_t i = 0; i < len; ++i) {
		str[i] = toupper(str[i]);
	}
	char* start = str;
	char* end = start;
	bool done = false;
	Set_List list;
	list.count = 0;
	while(!done) {
		while(*end != '/' && *end != '-' && *end != '\\' && *end != '|' && *end != '\0') {
			end++;
		}
		if(*end == 0) done = true;

		*end = 0;
		if(strlen(start) == 0) break;

		const MTG_Draftable_Set* set = get_set_from_code(start);
		if(set != NULL) {
			list.set[list.count++] = set;
		}

		end++;
		start = end;
	}

	return list;
}


// Turn a format string like "TSP/TSP/TSP" into "3x Time Spiral", or "MIR/VIS/WTH" into "Mirage/Visions/Weatherlight"
static void expand_format_string(const char* format, size_t len, char* out, size_t out_len) {
	if((format == NULL) || (len == 0) || (out == NULL) || (out_len == 0)) return;

	// Make a mutable copy of the format string
	char* str = (char*) alloca(len + 1);
	memcpy(str, format, len + 1);

	char* str_ptr = str;

	char tokens[PACKS_PER_DRAFT_MAX][FORMAT_STRING_LEN_MAX];
	char* token_ptr = &tokens[0][0];
	int token_count = 0;

	printf("%s\n", str);

	while(true) {
		while(*str_ptr != '/' && *str_ptr != '\\' && *str_ptr != '\0') {
			*token_ptr++ = *str_ptr++;
		}

		*token_ptr = '\0';

		++token_count;

		if(*str_ptr == '\0') break;

		token_ptr = &tokens[token_count][0];
		++str_ptr;
	}

	if(token_count == 1) {
		const char* set_name = get_set_name_from_code(&tokens[0][0]);
		if(set_name != NULL) {
			snprintf(out, out_len, "%s/%s/%s", format, format, format);
		} else {
			snprintf(out, out_len, "%s", format);
		}
		return;
	}

	// Compare all set codes with each other to see if all are the same.
	bool all_sets_the_same = true;
	for(int i = 0; i < token_count; ++i) {
		for(int j = i; j < token_count; ++j) {
			if(i == j) continue;
			if(strcmp(&tokens[i][0], &tokens[j][0]) != 0) {
				all_sets_the_same = false;
				break;
			}
		}
	}

	if(all_sets_the_same == true) {
		const char* set_name = get_set_name_from_code(&tokens[0][0]);
		if(set_name == NULL) set_name = format; // Use the set name passed in
		(void)snprintf(out, out_len, "%dx %s", token_count, get_set_name_from_code(&tokens[0][0]));
	} else {
		int offset = 0;
		for(int i = 0; i < token_count; ++i) {
			const char* set_name = get_set_name_from_code(&tokens[i][0]);
			if(set_name == NULL) set_name = format; // Unknown set - Use the set name passed in
			offset += snprintf(out + offset, out_len - offset, "%s", set_name);
			if(i != token_count-1) { // If not the last in the list, add a separator.
				offset += snprintf(out + offset, out_len - offset, "%s", "/");
			}
		}
	}
}

// The maximum number of leagues to be pinged when a draft sign up is posted. Increase this if a league ever needs to ping more than two roles.
static const size_t LEAGUE_PINGS_MAX = 2;

enum LEAGUE_ID {
	LEAGUE_ID_AMERICAS_CHRONO,
	LEAGUE_ID_EURO_CHRONO,
	LEAGUE_ID_ASIA_CHRONO,
	LEAGUE_ID_PACIFIC_CHRONO,
	LEAGUE_ID_AMERICAS_BONUS,
	LEAGUE_ID_EURO_BONUS,
};

static constexpr const std::string_view to_string(const LEAGUE_ID id) {
	switch(id) {
		case LEAGUE_ID_AMERICAS_CHRONO: return {"Americas Chrono"};
		case LEAGUE_ID_EURO_CHRONO:     return {"Euro Chrono"};
		case LEAGUE_ID_ASIA_CHRONO:     return {"Asia Chrono"};
		case LEAGUE_ID_PACIFIC_CHRONO:  return {"Pacific Chrono"};
		case LEAGUE_ID_AMERICAS_BONUS:  return {"Americas Bonus"};
		case LEAGUE_ID_EURO_BONUS:      return {"Euro Bonus"};
	}

	return {""};
}

struct Start_Time {
	int hour;
	int minute;
};

struct XDHS_League {
	LEAGUE_ID id;
	char region_code;                   // (E)uro, (A)mericas, A(S)ia, (P)acific, A(T)lantic
	char league_type;                   // (C)hrono or (B)onus
	u32 color;                          // Color for the league
	const char* time_zone;              // IANA time zone identifier
	Start_Time time;                    // When the draft starts
	const char* ping[LEAGUE_PINGS_MAX]; // Which roles to ping when the sign up sheet goes up
};

// Lookup table for each of our leagues. The order these are listed doesn't matter. In the future we may want bot commands to create, edit and delete leagues but to keep things simple for now we'll hard code these into the bot.
static const XDHS_League g_xdhs_leagues[] = {
	{
		LEAGUE_ID_AMERICAS_CHRONO,
		'A','C',
		0x002c7652,
		"America/New_York",
		{20,50},
		{"Americas", NULL},
	},
	{
		LEAGUE_ID_EURO_CHRONO,
		'E','C',
		0x000d5ba1,
		"Europe/Berlin",
		{19,50},
		{"Euro", NULL},
	},
	{
		LEAGUE_ID_ASIA_CHRONO,
		'S','C',
		0x00793fab,
		"Europe/Berlin",
		{10,50},
		{"Asia", NULL},
	},
	{
		LEAGUE_ID_PACIFIC_CHRONO,
		'P','C',
		0x00b82f4b,
		"America/New_York",
		{20,50},
		{"Pacific", NULL},
	},
	{
		LEAGUE_ID_AMERICAS_BONUS,
		'A','B',
		0x006aa84f,
		"America/New_York",
		{20,50},
		{"Americas", NULL},
	},
	{
		LEAGUE_ID_EURO_BONUS,
		'E','B',
		0x0061a0da,
		"Europe/Berlin",
		{19,50},
		{"Euro", NULL},
	}
};
static const size_t XDHS_LEAGUE_COUNT = sizeof(g_xdhs_leagues) / sizeof(XDHS_League);


// The maximum allowed byte length of a draft code.
static const size_t DRAFT_CODE_LENGTH_MAX = strlen("SSS.WW-RT");

struct Draft_Code {
	u16 season; // max 3 digits
	u8 week; // max 2 digits
	const XDHS_League *league;
};


static Result<Draft_Code> parse_draft_code(const char* draft_code) {
	if(draft_code == NULL) return MAKE_ERROR_RESULT(ERROR_INVALID_FUNCTION_PARAMETER);
	const size_t len = strlen(draft_code);
	if(len > DRAFT_CODE_LENGTH_MAX) return MAKE_ERROR_RESULT(ERROR_MALFORMED_DRAFT_CODE);
	char str[DRAFT_CODE_LENGTH_MAX+1]; // Mutable copy
	memcpy(str, draft_code, len);

	char* start = str;
	char* end = str;

	Draft_Code out;

	// Season
	while(isdigit(*end)) end++;
	if(*end != '.') return MAKE_ERROR_RESULT(ERROR_MALFORMED_DRAFT_CODE);
	*end = 0;
	if(strlen(start) == 0) return MAKE_ERROR_RESULT(ERROR_MALFORMED_DRAFT_CODE);
	if(strlen(start) > 3) return MAKE_ERROR_RESULT(ERROR_MALFORMED_DRAFT_CODE);
	out.season = strtol(start, NULL, 10);
	start = ++end;

	// Week
	while(isdigit(*end)) end++;
	if(*end != '-') return MAKE_ERROR_RESULT(ERROR_MALFORMED_DRAFT_CODE);
	*end = 0;
	if(strlen(start) == 0) return MAKE_ERROR_RESULT(ERROR_MALFORMED_DRAFT_CODE);
	if(strlen(start) > 2) return MAKE_ERROR_RESULT(ERROR_MALFORMED_DRAFT_CODE);
	out.week = strtol(start, NULL, 10);
	end++;

	const char region_code = *end++;
	const char league_type = *end;

	for(size_t i = 0; i < XDHS_LEAGUE_COUNT; ++i) {
		if((g_xdhs_leagues[i].region_code == region_code) && (g_xdhs_leagues[i].league_type == league_type)) {
			out.league = &g_xdhs_leagues[i];
			return {out};
		}
	}

	return MAKE_ERROR_RESULT(ERROR_LEAGUE_NOT_FOUND);
}

static inline int pack_time(int year, int month, int day, int hour, int minute) {
	int result = 0;

	result |= ((year-2000) & 0x7f) << 20; // 0-99
	result |=  (month      & 0x0f) << 16; // 1-12
	result |=  (day        & 0x1f) << 11; // 1-31
	result |=  (hour       & 0x1f) <<  6; // 0-23
	result |=  (minute     & 0x3f) <<  0; // 0-59

	return result;
}

static inline void unpack_time(const int packed, int* year, int* month, int* day, int* hour, int* minute) {
	*year   = ((packed >> 20) & 0x7f) + 2000;
	*month  =  (packed >> 16) & 0x0f;
	*day    =  (packed >> 11) & 0x1f;
	*hour   =  (packed >>  6) & 0x1f;
	*minute =  (packed >>  0) & 0x3f;
}

static const size_t TIME_STRING_MAX = strlen("YYYY-MM-DD HH:MM") + 1;

static inline void make_time_string(const int packed, char out[TIME_STRING_MAX]) {
	int year, month, day, hour, minute;
	unpack_time(packed, &year, &month, &day, &hour, &minute);
	snprintf(out, TIME_STRING_MAX, "%d-%02d-%02d %02d:%02d", year, month, day, hour, minute);
}

static inline time_t make_timestamp(const char* time_zone, int year, int month, int day, int hour, int minute) {
	auto zoned_time = date::make_zoned(time_zone,
		date::local_days{date::year{year}/month/day} +
		std::chrono::hours(hour) +
		std::chrono::minutes(minute));
	return std::chrono::system_clock::to_time_t(zoned_time.get_sys_time());
}

static inline time_t unpack_and_make_timestamp(const int packed, const char* time_zone) {
	int year, month, day, hour, minute;
	unpack_time(packed, &year, &month, &day, &hour, &minute);
	return make_timestamp(time_zone, year, month, day, hour, minute);
}

struct Draft_Duration {
	int hours;
	int minutes;
};

static const Draft_Duration DEFAULT_DRAFT_DURATION = {3,0};

struct Date {
	int year, month, day;
};

// Do some rudimentary validation on the date string sent with the create_event command and parse the provided values. Returns NULL if no error and fills the 'out' variable with the parsed values, or an string describing the problem.
// Despite the harsh sounding error strings, this tries to be quite generous and forgiving. For example, it will accept a date written as YY/M/D
// Yes, this could use std::regex but this uses less memory, less cycles and compiles faster.
#define split_date(str, min_len, max_len, out) \
{ \
	const char* start = str; \
	while(isdigit(*str)) str++;	   \
	if(*str != '-' && *str != '.' && *str != '\\' && *str != '/' && *str != '\0') { \
		return MAKE_ERROR_RESULT(ERROR_MALFORMED_DATE_STRING);  \
	} \
	*str++ = 0; \
	if(strlen(start) < min_len || strlen(start) > max_len) return MAKE_ERROR_RESULT(ERROR_MALFORMED_DATE_STRING); \
	out = strtol(start, NULL, 10); \
}
static const Result<Date> parse_date_string(const char* date_string) {
	if(strlen(date_string) < strlen("YY-M-D")) return MAKE_ERROR_RESULT(ERROR_MALFORMED_DATE_STRING);
	if(strlen(date_string) > strlen("YYYY-MM-DD")) return MAKE_ERROR_RESULT(ERROR_MALFORMED_DATE_STRING);

	// Make a mutable copy of the date string, including terminator.
	char str[strlen("YYYY-MM-DD")+1];
	memcpy(str, date_string, strlen(date_string)+1);
	char* str_ptr = str;

	Date result;

	split_date(str_ptr, 2, 4, result.year);
	split_date(str_ptr, 1, 2, result.month);
	split_date(str_ptr, 1, 2, result.day);

	if(result.year <= 99) result.year += 2000;

	// String parsed - check if this looks like a valid date.
	// TODO: The date library probably could do this, right?

	time_t current_time = time(NULL);
	struct tm t = *localtime(&current_time);
	int current_year = t.tm_year + 1900;

	// TODO: Check the date is in the future

	if(result.year < current_year) {
		return MAKE_ERROR_RESULT(ERROR_DATE_IS_IN_PAST);
	}

	if(result.month < 1 || result.month > 12) {
		return MAKE_ERROR_RESULT(ERROR_INVALID_MONTH);
	} else
	if(result.month == 1 || result.month == 3 || result.month == 5 || result.month == 7 || result.month == 8 || result.month == 10 || result.month == 12) {
		if(result.day > 31) return MAKE_ERROR_RESULT(ERROR_INVALID_DAY_31);
	} else
	if (result.month == 4 || result.month == 6 || result.month == 9 || result.month == 11) {
		if(result.day > 30) return MAKE_ERROR_RESULT(ERROR_INVALID_DAY_30);
	} else {
		// Febuary
		if(((result.year % 4 == 0) && (result.year % 100 != 0)) || (result.year % 400 == 0)) {
			// Leap year
			if(result.day > 29) return MAKE_ERROR_RESULT(ERROR_INVALID_DAY_29);
		} else {
			if(result.day > 28) return MAKE_ERROR_RESULT(ERROR_INVALID_DAY_28);
		}
	}

	return {result};
}

// Do some rudimentary validation on the start time string sent with create_draft command and parse the provided values. Returns true and fills the 'out' variable if no problem was found, false otherwise.
static const Result<Start_Time> parse_start_time_string(const char* start_time_string) {
	if(start_time_string == NULL) return MAKE_ERROR_RESULT(ERROR_INVALID_FUNCTION_PARAMETER);
	if(strlen(start_time_string) < strlen("H:M")) return MAKE_ERROR_RESULT(ERROR_MALFORMED_START_TIME_STRING);
	if(strlen(start_time_string) > strlen("HH:MM")) return MAKE_ERROR_RESULT(ERROR_MALFORMED_START_TIME_STRING);

	// Make a copy of the date string, including terminator.
	char str[strlen("HH:MM")+1];
	memcpy(str, start_time_string, strlen(start_time_string)+1);
	char* str_ptr = str;

	Start_Time result;

	// Parse the hour
	const char* hour = str_ptr;
	while(isdigit(*str_ptr)) {
		str_ptr++;
	}
	if(*str_ptr != ':' && *str_ptr != '-' && *str_ptr != ',' && *str_ptr != '.') {
		return MAKE_ERROR_RESULT(ERROR_MALFORMED_START_TIME_STRING);
	}
	*str_ptr++ = 0;
	result.hour = (int) strtol(hour, NULL, 10);
	if(result.hour < 0 || result.hour > 23) return MAKE_ERROR_RESULT(ERROR_INVALID_HOUR);

	// Parse the minutes
	const char* minute = str_ptr;
	while(isdigit(*str_ptr)) {
		str_ptr++;
	}
	if(*str_ptr != '\0') {
		return MAKE_ERROR_RESULT(ERROR_MALFORMED_START_TIME_STRING);
	}
	result.minute = (int) strtol(minute, NULL, 10);
	if(result.minute < 1 && result.minute > 59) return MAKE_ERROR_RESULT(ERROR_INVALID_MINUTE);

	return {result};
}

// Discord has as hard limit on how many characters are allowed in a post.
static const size_t DISCORD_MESSAGE_CHARACTER_LIMIT = 2000;

// The maximum allowed characters in a Discord username or nickname.
static const size_t DISCORD_NAME_LENGTH_MAX = 32;

// The maximum allowed byte length of a draft format string.
static const size_t DRAFT_FORMAT_LENGTH_MAX = 64;

// The maximum allowed byte length of a draft format description string.
static const size_t DRAFT_FORMAT_DESCRIPTION_LENGTH_MAX = 128;

// The maximum allowed byte length for each 'blurb' paragraph in the draft details post.
static const size_t DRAFT_BLURB_LENGTH_MAX = 512;

// Maximum length of the filename for a downloaded banner file.
static const size_t BANNER_FILENAME_MAX = 64;

// The maximum allowed byte length of a set list. e.g. This might be a list of all set codes of sets in a chaos draft.
static const size_t SET_LIST_LENGTH_MAX = 256;

// The maximum allowed byte length for the XMage server string.
static const size_t XMAGE_SERVER_LENGTH_MAX = 32;

// The maximum number of bytes needed for a ping string. The space on the end is intentional.
static const size_t PING_STRING_LENGTH_MAX = LEAGUE_PINGS_MAX * strlen("<@&18446744073709551616> ");

// The maximum allowed byte length for a IANA time zone string. See: https://en.wikipedia.org/wiki/List_of_tz_database_time_zones
static const size_t IANA_TIME_ZONE_LENGTH_MAX = 64;

// How many blurbs a post can have. NOTE: If we instead end up getting this data from our master spreadsheet we won't need more than one here.
static const size_t BLURB_COUNT = 3;

// The maximum allowed byte length of a XDHS league name.
static const size_t LEAGUE_NAME_LENGTH_MAX = 32;

// NOTE: As we're storing the values of these in the database, the order of these must not change!
enum DRAFT_STATUS {
	DRAFT_STATUS_INVALID            =  0,

	DRAFT_STATUS_CREATED            =  1, // The draft has been created but not posted.
	DRAFT_STATUS_POSTED             =  2, // The draft has been posted and sign ups are open.
	DRAFT_STATUS_REMINDER_SENT      =  4, // The pre-draft reminder has been sent to everyone signed up.
	DRAFT_STATUS_TENTATIVES_PINGED  =  8, // The warning to tentatives has been posted, if there are any.
	DRAFT_STATUS_LOCKED             = 16, // The draft is now partially locked and ready to be fired.
	DRAFT_STATUS_FIRED              = 32, // The draft is now fully locked and play has commenced.
	DRAFT_STATUS_COMPLETE           = 64, // The draft has concluded.

	DRAFT_STATUS_END                = 128,
};

static constexpr const std::string_view to_string(const DRAFT_STATUS status) {
	switch(status) {
		case DRAFT_STATUS_INVALID:           return {"DRAFT_STATUS_INVALID"};

		case DRAFT_STATUS_CREATED:           return {"DRAFT_STATUS_CREATED"};
		case DRAFT_STATUS_POSTED:            return {"DRAFT_STATUS_POSTED"};
		case DRAFT_STATUS_REMINDER_SENT:     return {"DRAFT_STATUS_REMINDER_SENT"};
		case DRAFT_STATUS_TENTATIVES_PINGED: return {"DRAFT_STATUS_TENTATIVES_PINGED"};
		case DRAFT_STATUS_LOCKED:            return {"DRAFT_STATUS_LOCKED"};
		case DRAFT_STATUS_FIRED:             return {"DRAFT_STATUS_FIRED"};
		case DRAFT_STATUS_COMPLETE:          return {"DRAFT_STATUS_COMPLETE"};

		case DRAFT_STATUS_END:               return {"DRAFT_STATUS_END"};
	}
	return {""};
}

static std::string draft_status_to_string(int status) {
	std::string result;
	result.reserve(64);

	bool first = true;
	for(int i = (int)DRAFT_STATUS_CREATED; i < (int)DRAFT_STATUS_END; i *= 2) {
		if(status & i) {
			if(first == false) {
				result += " | ";
			} else {
				first = false;
			}
			result += std::string{to_string((DRAFT_STATUS)i)};
		}
	}

	return result;
}

// All data needed for a #-pre-register post is available in this structure.
struct Draft_Event {
	Draft_Event() {
		memset(this, 0, sizeof(Draft_Event));
	}

	DRAFT_STATUS status;

	char pings[PING_STRING_LENGTH_MAX + 1];
	char draft_code[DRAFT_CODE_LENGTH_MAX + 1];
	char league_name[LEAGUE_NAME_LENGTH_MAX + 1];
	char format[DRAFT_FORMAT_LENGTH_MAX + 1];
	char time_zone[IANA_TIME_ZONE_LENGTH_MAX + 1];
	time_t time;
	float duration; // Duration of the draft. e.g. 3.5 would be 3 hours and 30 minutes.
	char blurbs[BLURB_COUNT][DRAFT_BLURB_LENGTH_MAX + 1];
	char draft_guide_url[URL_LENGTH_MAX + 1];
	char card_list_url[URL_LENGTH_MAX + 1];
	char set_list[SET_LIST_LENGTH_MAX + 1]; // The set list... expanded or unexpanded?

	u32 color; // Color to use for vertical strip on the sign up post.
	char xmage_server[XMAGE_SERVER_LENGTH_MAX + 1];
	bool draftmancer_draft; // Will the draft portion take place on Draftmancer?
	char banner_file[BANNER_FILENAME_MAX + 1]; // Relative path to the banner image for this draft.
	time_t banner_timestamp;

	u64 signup_channel_id;
	u64 reminder_channel_id;
	u64 hosting_channel_id;

	u64 details_id; // Message ID of the post in #-pre-register describing the format.
	u64 signups_id; // Message ID of the sign up sheet posted in #-pre-register.
	u64 reminder_id; // Message ID of the reminder message sent to all sign ups #-in-the-moment-draft.
};
static_assert(std::is_trivially_copyable<Draft_Event>(), "struct Draft_Event is not trivially copyable");


enum POD_ALLOCATION_REASON {
	POD_ALLOCATION_REASON_UNALLOCATED,

	POD_ALLOCATION_REASON_SINGLE_POD, // Only enough players for a single pod

	POD_ALLOCATION_REASON_HOST,
	POD_ALLOCATION_REASON_RO3,
	POD_ALLOCATION_REASON_ERO3,
	POD_ALLOCATION_REASON_CONTENTION,
	POD_ALLOCATION_REASON_SHARK,
	POD_ALLOCATION_REASON_NEWBIE, // Members with < 4 drafts player get priority for pod 2
	POD_ALLOCATION_PREFERENCE,
	POD_ALLOCATION_RANDOM // Flexible players are randomly assigned to whatever empty seats are left
};

static const char* emoji_for_reason(const POD_ALLOCATION_REASON r) {
	switch(r) {
		case POD_ALLOCATION_REASON_UNALLOCATED: return ":grey_question:"; // Should never happen
		case POD_ALLOCATION_REASON_SINGLE_POD:  return ":one:";
		case POD_ALLOCATION_REASON_HOST:        return ":tophat:";
		case POD_ALLOCATION_REASON_RO3:         return ":three:";
		case POD_ALLOCATION_REASON_ERO3:        return ":three:";
		case POD_ALLOCATION_REASON_CONTENTION:  return ":trophy:";
		case POD_ALLOCATION_REASON_SHARK:       return ":shark:";
		case POD_ALLOCATION_REASON_NEWBIE:      return ":new:";
		case POD_ALLOCATION_PREFERENCE:         return ":ballot_box_with_check:";
		case POD_ALLOCATION_RANDOM:             return ":game_die:";
		default:
			break;
	}
	return NULL;
}

// Our drafts can have no fewer seats than this.
static const int POD_SEATS_MIN = 6;

// Our drafts can have no more seats than this.
static const int POD_SEATS_MAX = 10;

// The maximum number of pods this bot can handle.
static const int PODS_MAX = 8;

// The maximum number of players this bot can handle in a single tournament.
static const int PLAYERS_MAX = 64;

struct Pod_Player {
	POD_ALLOCATION_REASON reason;
	u64 member_id;
	char preferred_name[DISCORD_NAME_LENGTH_MAX + 1];
};
static_assert(std::is_trivially_copyable<Pod_Player>(), "struct Draft_Event is not trivially copyable");

struct Draft_Pod {
	int seats; // How many seats at this table. Either 6, 8 or 10
	int count; // How many seats have been filled with Pod_Players
	Pod_Player players[POD_SEATS_MAX];
};

static bool pod_is_full(Draft_Pod* pod) {
	return pod->seats == pod->count;
}

struct Draft_Tournament {
	Draft_Tournament() {
		memset(this, 0, sizeof(Draft_Tournament));
	}

	int pod_count;
	Draft_Pod pods[PODS_MAX];
};

static Draft_Pod* get_next_empty_draft_pod_high(Draft_Tournament* tournament) {
	for(int i = 0; i < tournament->pod_count; ++i) {
		Draft_Pod* pod = &tournament->pods[i];
		if(pod_is_full(pod) == false) {
			return pod;
		}
	}
	return NULL;
}

static Draft_Pod* get_next_empty_draft_pod_low(Draft_Tournament* tournament) {
	for(int i = tournament->pod_count - 1; i >= 0; i--) {
		Draft_Pod* pod = &tournament->pods[i];
		if(pod_is_full(pod) == false) {
			return pod;
		}
	}
	return NULL;
}

static Draft_Pod* get_random_empty_draft_pod(Draft_Tournament* tournament) {
	// FIXME: This is a really bad way of doing this but  I just need something that works for now...
	// FIXME: This could infinite loop if no pod is available.
	while(true) {
		int i = rand() % tournament->pod_count;
		Draft_Pod* pod = &tournament->pods[i];
		if(pod_is_full(pod) == false) {
			return pod;
		}
	}
}

static void add_player_to_pod(Draft_Pod* pod, u64 member_id, POD_ALLOCATION_REASON reason, const char* preferred_name) {
	// FIXME: Check capacity
	Pod_Player* player = &pod->players[pod->count++];
	player->reason = reason;
	player->member_id = member_id;
	strcpy(player->preferred_name, preferred_name);
}

// With player_count players, how many pods should be created?
// Reference: https://i.imgur.com/tpNo13G.png
// TODO: This needs to support a player_count of any size
// FIXME: Brute forcing this is silly, but as I write this I can't work out the function to calculate the correct number of seats...
static Draft_Tournament set_up_pod_count_and_sizes(int player_count) {
	// Round up player count to even number.
	if((player_count % 2) == 1) player_count++;

	// As we only need to consider an even number of players, we can halve the player count and use it as an array index.
	player_count /= 2;
	//                                   Player count: 0 2 4 6 8 10 ...
	static const int pods_needed_for_player_count[] = {0,0,0,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,6,6,6,6,7,7,7,7,8,8,8,8};

	static const int seats_per_pod[(PLAYERS_MAX/2)+1/*plus 1 for 0 players*/][PODS_MAX] = {
		{ 0, 0, 0, 0, 0, 0, 0, 0}, //  0
		{ 0, 0, 0, 0, 0, 0, 0, 0}, //  2
		{ 0, 0, 0, 0, 0, 0, 0, 0}, //  4
		{ 6, 0, 0, 0, 0, 0, 0, 0}, //  6
		{ 8, 0, 0, 0, 0, 0, 0, 0}, //  8
		{10, 0, 0, 0, 0, 0, 0, 0}, // 10
		{ 6, 6, 0, 0, 0, 0, 0, 0}, // 12
		{ 8, 6, 0, 0, 0, 0, 0, 0}, // 14
		{ 8, 8, 0, 0, 0, 0, 0, 0}, // 16
		{10, 8, 0, 0, 0, 0, 0, 0}, // 18
		{ 8, 6, 6, 0, 0, 0, 0, 0}, // 20
		{ 8, 8, 6, 0, 0, 0, 0, 0}, // 22
		{ 8, 8, 8, 0, 0, 0, 0, 0}, // 24
		{10, 8, 8, 0, 0, 0, 0, 0}, // 26
		{ 8, 8, 6, 6, 0, 0, 0, 0}, // 28
		{ 8, 8, 8, 6, 0, 0, 0, 0}, // 30
		{ 8, 8, 8, 8, 0, 0, 0, 0}, // 32
		{10, 8, 8, 8, 0, 0, 0, 0}, // 34
		{ 8, 8, 8, 6, 6, 0, 0, 0}, // 36
		{ 8, 8, 8, 8, 6, 0, 0, 0}, // 38
		{ 8, 8, 8, 8, 8, 0, 0, 0}, // 40
		{10, 8, 8, 8, 8, 0, 0, 0}, // 42
		{ 8, 8, 8, 8, 6, 6, 0, 0}, // 44
		{ 8, 8, 8, 8, 8, 6, 0, 0}, // 46
		{ 8, 8, 8, 8, 8, 8, 0, 0}, // 48
		{10, 8, 8, 8, 8, 8, 0, 0}, // 50
		{ 8, 8, 8, 8, 8, 6, 6, 0}, // 52
		{ 8, 8, 8, 8, 8, 8, 6, 0}, // 54
		{ 8, 8, 8, 8, 8, 8, 8, 0}, // 56
		{10, 8, 8, 8, 8, 8, 8, 0}, // 58
		{ 8, 8, 8, 8, 8, 8, 6, 6}, // 60
		{ 8, 8, 8, 8, 8, 8, 8, 6}, // 62
		{ 8, 8, 8, 8, 8, 8, 8, 8}, // 64
	};

	Draft_Tournament tournament;
	tournament.pod_count = pods_needed_for_player_count[player_count];
	for(int p = 0; p < tournament.pod_count; ++p) {
		Draft_Pod* pod = &tournament.pods[p];
		pod->seats = seats_per_pod[player_count][p];
		pod->count = 0;
		for(int s = 0; s < pod->seats; ++s) {
			pod->players[s].reason = POD_ALLOCATION_REASON_UNALLOCATED;
			pod->players[s].member_id = 0;
			pod->players[s].preferred_name[0] = 0;
		}
	}

	return tournament;
}

static Database_Result<Database_No_Value> database_add_draft(const u64 guild_id, const Draft_Event* event) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	const char* query = R"(
		INSERT INTO draft_events(
			guild_id,     -- 0
			pings,        -- 1
			draft_code,   -- 2
			league_name,  -- 3
			format,       -- 4
			time_zone,    -- 5
			time,         -- 6
			duration,     -- 7
			blurb_1,      -- 8
			blurb_2,      -- 9
			blurb_3,      -- 10
			draft_guide,  -- 11
			card_list,    -- 12
			set_list,     -- 13
			color,        -- 14
			xmage_server, -- 15
			draftmancer_draft,   -- 16
			banner_file,         -- 17
			signup_channel_id,   -- 18
			reminder_channel_id, -- 19
			hosting_channel_id   -- 20
		)
		VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
		)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(21);
	MYSQL_INPUT( 0, MYSQL_TYPE_LONGLONG, &guild_id,                 sizeof(guild_id));
	MYSQL_INPUT( 1, MYSQL_TYPE_STRING,   event->pings,              strlen(event->pings));
	MYSQL_INPUT( 2, MYSQL_TYPE_STRING,   event->draft_code,         strlen(event->draft_code));
	MYSQL_INPUT( 3, MYSQL_TYPE_STRING,   event->league_name,        strlen(event->league_name));
	MYSQL_INPUT( 4, MYSQL_TYPE_STRING,   event->format,             strlen(event->format));
	MYSQL_INPUT( 5, MYSQL_TYPE_STRING,   event->time_zone,          strlen(event->time_zone));
	MYSQL_INPUT( 6, MYSQL_TYPE_LONG,     &event->time,              sizeof(event->time));
	MYSQL_INPUT( 7, MYSQL_TYPE_FLOAT,    &event->duration,          sizeof(event->duration));
	MYSQL_INPUT( 8, MYSQL_TYPE_STRING,   &event->blurbs[0][0],      strlen(&event->blurbs[0][0]));
	MYSQL_INPUT( 9, MYSQL_TYPE_STRING,   &event->blurbs[1][0],      strlen(&event->blurbs[1][0]));
	MYSQL_INPUT(10, MYSQL_TYPE_STRING,   &event->blurbs[2][0],      strlen(&event->blurbs[2][0]));
	MYSQL_INPUT(11, MYSQL_TYPE_STRING,   event->draft_guide_url,    strlen(event->draft_guide_url));
	MYSQL_INPUT(12, MYSQL_TYPE_STRING,   event->card_list_url,      strlen(event->card_list_url));
	MYSQL_INPUT(13, MYSQL_TYPE_STRING,   event->set_list,           strlen(event->set_list));
	MYSQL_INPUT(14, MYSQL_TYPE_LONG,     &event->color,             sizeof(event->color));
	MYSQL_INPUT(15, MYSQL_TYPE_STRING,   event->xmage_server,       strlen(event->xmage_server));
	MYSQL_INPUT(16, MYSQL_TYPE_TINY,     &event->draftmancer_draft, sizeof(event->draftmancer_draft));
	MYSQL_INPUT(17, MYSQL_TYPE_STRING,   event->banner_file,        strlen(event->banner_file));
	MYSQL_INPUT(18, MYSQL_TYPE_LONGLONG, &event->signup_channel_id, sizeof(event->signup_channel_id));
	MYSQL_INPUT(19, MYSQL_TYPE_LONGLONG, &event->reminder_channel_id, sizeof(event->reminder_channel_id));
	MYSQL_INPUT(20, MYSQL_TYPE_LONGLONG, &event->hosting_channel_id, sizeof(event->hosting_channel_id));

	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_edit_draft(const u64 guild_id, const std::shared_ptr<Draft_Event> event) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	const char* query = R"(
		UPDATE draft_events SET
			format=?,       -- 0
			time=?,         -- 1
			duration=?,     -- 2
			blurb_1=?,      -- 3
			blurb_2=?,      -- 4
			blurb_3=?,      -- 5
			draft_guide=?,  -- 6
			card_list=?,    -- 7
			set_list=?,     -- 8
			color=?,        -- 9
			xmage_server=?, -- 10
			draftmancer_draft=?, -- 11
			banner_file=?,  -- 12
			signup_channel_id=?,    -- 13
			reminder_channel_id=?, -- 14
			hosting_channel_id=? -- 15
		WHERE guild_id=? AND draft_code=? -- 16,17
		)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(18);
	MYSQL_INPUT( 0, MYSQL_TYPE_STRING,   event->format,          strlen(event->format));
	MYSQL_INPUT( 1, MYSQL_TYPE_LONG,     &event->time,           sizeof(event->time));
	MYSQL_INPUT( 2, MYSQL_TYPE_FLOAT,    &event->duration,       sizeof(event->duration));
	MYSQL_INPUT( 3, MYSQL_TYPE_STRING,   &event->blurbs[0][0],   strlen(&event->blurbs[0][0]));
	MYSQL_INPUT( 4, MYSQL_TYPE_STRING,   &event->blurbs[1][0],   strlen(&event->blurbs[1][0]));
	MYSQL_INPUT( 5, MYSQL_TYPE_STRING,   &event->blurbs[2][0],   strlen(&event->blurbs[2][0]));
	MYSQL_INPUT( 6, MYSQL_TYPE_STRING,   event->draft_guide_url, strlen(event->draft_guide_url));
	MYSQL_INPUT( 7, MYSQL_TYPE_STRING,   event->card_list_url,   strlen(event->card_list_url));
	MYSQL_INPUT( 8, MYSQL_TYPE_STRING,   event->set_list,        strlen(event->set_list));
	MYSQL_INPUT( 9, MYSQL_TYPE_LONG,     &event->color,          sizeof(event->color));
	MYSQL_INPUT(10, MYSQL_TYPE_STRING,   event->xmage_server,    strlen(event->xmage_server));
	MYSQL_INPUT(11, MYSQL_TYPE_TINY,     &event->draftmancer_draft, sizeof(event->draftmancer_draft));
	MYSQL_INPUT(12, MYSQL_TYPE_STRING,   event->banner_file,     strlen(event->banner_file));
	MYSQL_INPUT(13, MYSQL_TYPE_LONGLONG, &event->signup_channel_id, sizeof(event->signup_channel_id));
	MYSQL_INPUT(14, MYSQL_TYPE_LONGLONG, &event->reminder_channel_id, sizeof(event->reminder_channel_id));
	MYSQL_INPUT(15, MYSQL_TYPE_LONGLONG, &event->hosting_channel_id, sizeof(event->hosting_channel_id));
	MYSQL_INPUT(16, MYSQL_TYPE_LONGLONG, &guild_id,              sizeof(guild_id));
	MYSQL_INPUT(17, MYSQL_TYPE_STRING,   event->draft_code,      strlen(event->draft_code));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

// TODO: Rename database_get_draft?
static Database_Result<std::shared_ptr<Draft_Event>> database_get_event(const u64 guild_id, const std::string_view draft_code) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);

	const char* query = R"(
		SELECT
			status,              -- 0
			draft_code,          -- 1
			pings,               -- 2
			league_name,         -- 3
			format,              -- 4
			time_zone,           -- 5
			time,                -- 6
			duration,            -- 7
			blurb_1,             -- 8
			blurb_2,             -- 9
			blurb_3,             -- 10
			draft_guide,         -- 11
			card_list,           -- 12
			set_list,            -- 13
			color,               -- 14
			xmage_server,        -- 15
			draftmancer_draft,   -- 16
			banner_file,         -- 17
			banner_timestamp,    -- 18
			signup_channel_id,   -- 19
			reminder_channel_id, -- 20
			hosting_channel_id,  -- 21
			details_id,          -- 22
			signups_id,          -- 23
			reminder_id          -- 24
		FROM draft_events
		WHERE guild_id=? AND draft_code=?
	)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id, sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING, draft_code.data(), draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	auto result = std::make_shared<Draft_Event>();

	MYSQL_OUTPUT_INIT(25);
	MYSQL_OUTPUT( 0, MYSQL_TYPE_LONG,     &result->status,         sizeof(result->status));
	MYSQL_OUTPUT( 1, MYSQL_TYPE_STRING,   result->draft_code,      DRAFT_CODE_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 2, MYSQL_TYPE_STRING,   result->pings,           PING_STRING_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 3, MYSQL_TYPE_STRING,   result->league_name,     LEAGUE_NAME_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 4, MYSQL_TYPE_STRING,   result->format,          DRAFT_FORMAT_DESCRIPTION_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 5, MYSQL_TYPE_STRING,   result->time_zone,       IANA_TIME_ZONE_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 6, MYSQL_TYPE_LONG,     &result->time,           sizeof(result->time));
	MYSQL_OUTPUT( 7, MYSQL_TYPE_FLOAT,    &result->duration,       sizeof(result->duration));
	MYSQL_OUTPUT( 8, MYSQL_TYPE_STRING,   &result->blurbs[0][0],   DRAFT_BLURB_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 9, MYSQL_TYPE_STRING,   &result->blurbs[1][0],   DRAFT_BLURB_LENGTH_MAX + 1);
	MYSQL_OUTPUT(10, MYSQL_TYPE_STRING,   &result->blurbs[2][0],   DRAFT_BLURB_LENGTH_MAX + 1);
	MYSQL_OUTPUT(11, MYSQL_TYPE_STRING,   result->draft_guide_url, URL_LENGTH_MAX + 1);
	MYSQL_OUTPUT(12, MYSQL_TYPE_STRING,   result->card_list_url,   URL_LENGTH_MAX + 1);
	MYSQL_OUTPUT(13, MYSQL_TYPE_STRING,   result->set_list,        SET_LIST_LENGTH_MAX + 1);
	MYSQL_OUTPUT(14, MYSQL_TYPE_LONG,     &result->color,          sizeof(result->color));
	MYSQL_OUTPUT(15, MYSQL_TYPE_STRING,   result->xmage_server,    XMAGE_SERVER_LENGTH_MAX + 1);
	MYSQL_OUTPUT(16, MYSQL_TYPE_LONG,     &result->draftmancer_draft, sizeof(result->draftmancer_draft));
	MYSQL_OUTPUT(17, MYSQL_TYPE_STRING,   result->banner_file,     BANNER_FILENAME_MAX + 1);
	MYSQL_OUTPUT(18, MYSQL_TYPE_LONGLONG, &result->banner_timestamp, sizeof(result->banner_timestamp));
	MYSQL_OUTPUT(19, MYSQL_TYPE_LONGLONG, &result->signup_channel_id,     sizeof(result->signup_channel_id));
	MYSQL_OUTPUT(20, MYSQL_TYPE_LONGLONG, &result->reminder_channel_id,     sizeof(result->reminder_channel_id));
	MYSQL_OUTPUT(21, MYSQL_TYPE_LONGLONG, &result->hosting_channel_id,     sizeof(result->hosting_channel_id));
	MYSQL_OUTPUT(22, MYSQL_TYPE_LONGLONG, &result->details_id,     sizeof(result->details_id));
	MYSQL_OUTPUT(23, MYSQL_TYPE_LONGLONG, &result->signups_id,     sizeof(result->signups_id));
	MYSQL_OUTPUT(24, MYSQL_TYPE_LONGLONG, &result->reminder_id,    sizeof(result->reminder_id));
	MYSQL_OUTPUT_BIND_AND_STORE();

	MYSQL_FETCH_AND_RETURN_ZERO_OR_ONE_ROWS();
}

static const Database_Result<std::vector<Draft_Event>> database_get_all_events(const u64 guild_id) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);

	const char* query = R"(
		SELECT
			status,              -- 0
			draft_code,          -- 1
			pings,               -- 2
			league_name,         -- 3
			format,              -- 4
			time_zone,           -- 5
			time,                -- 6
			duration,            -- 7
			blurb_1,             -- 8
			blurb_2,             -- 9
			blurb_3,             -- 10
			draft_guide,         -- 11
			card_list,           -- 12
			set_list,            -- 13
			color,               -- 14
			xmage_server,        -- 15
			draftmancer_draft,   -- 16
			banner_file,         -- 17
			banner_timestamp,    -- 18
			signup_channel_id,   -- 19
			reminder_channel_id, -- 20
			hosting_channel_id,  -- 21
			details_id,          -- 22
			signups_id,          -- 23
			reminder_id          -- 24
		FROM draft_events
		WHERE guild_id=?
	)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(1);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id, sizeof(guild_id));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	//auto results = std::vector<Draft_Event>();
	Draft_Event result;

	MYSQL_OUTPUT_INIT(25);
	MYSQL_OUTPUT( 0, MYSQL_TYPE_LONG,     &result.status,         sizeof(result.status));
	MYSQL_OUTPUT( 1, MYSQL_TYPE_STRING,   result.draft_code,      DRAFT_CODE_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 2, MYSQL_TYPE_STRING,   result.pings,           PING_STRING_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 3, MYSQL_TYPE_STRING,   result.league_name,     LEAGUE_NAME_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 4, MYSQL_TYPE_STRING,   result.format,          DRAFT_FORMAT_DESCRIPTION_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 5, MYSQL_TYPE_STRING,   result.time_zone,       IANA_TIME_ZONE_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 6, MYSQL_TYPE_LONG,     &result.time,           sizeof(result.time));
	MYSQL_OUTPUT( 7, MYSQL_TYPE_FLOAT,    &result.duration,       sizeof(result.duration));
	MYSQL_OUTPUT( 8, MYSQL_TYPE_STRING,   &result.blurbs[0][0],   DRAFT_BLURB_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 9, MYSQL_TYPE_STRING,   &result.blurbs[1][0],   DRAFT_BLURB_LENGTH_MAX + 1);
	MYSQL_OUTPUT(10, MYSQL_TYPE_STRING,   &result.blurbs[2][0],   DRAFT_BLURB_LENGTH_MAX + 1);
	MYSQL_OUTPUT(11, MYSQL_TYPE_STRING,   result.draft_guide_url, URL_LENGTH_MAX + 1);
	MYSQL_OUTPUT(12, MYSQL_TYPE_STRING,   result.card_list_url,   URL_LENGTH_MAX + 1);
	MYSQL_OUTPUT(13, MYSQL_TYPE_STRING,   result.set_list,        SET_LIST_LENGTH_MAX + 1);
	MYSQL_OUTPUT(14, MYSQL_TYPE_LONG,     &result.color,          sizeof(result.color));
	MYSQL_OUTPUT(15, MYSQL_TYPE_STRING,   result.xmage_server,    XMAGE_SERVER_LENGTH_MAX + 1);
	MYSQL_OUTPUT(16, MYSQL_TYPE_LONG,     &result.draftmancer_draft, sizeof(result.draftmancer_draft));
	MYSQL_OUTPUT(17, MYSQL_TYPE_STRING,   result.banner_file,     BANNER_FILENAME_MAX + 1);
	MYSQL_OUTPUT(18, MYSQL_TYPE_LONGLONG, &result.banner_timestamp, sizeof(result.banner_timestamp));
	MYSQL_OUTPUT(19, MYSQL_TYPE_LONGLONG, &result.signup_channel_id, sizeof(result.signup_channel_id));
	MYSQL_OUTPUT(20, MYSQL_TYPE_LONGLONG, &result.reminder_channel_id, sizeof(result.reminder_channel_id));
	MYSQL_OUTPUT(21, MYSQL_TYPE_LONGLONG, &result.hosting_channel_id, sizeof(result.hosting_channel_id));
	MYSQL_OUTPUT(22, MYSQL_TYPE_LONGLONG, &result.details_id,     sizeof(result.details_id));
	MYSQL_OUTPUT(23, MYSQL_TYPE_LONGLONG, &result.signups_id,     sizeof(result.signups_id));
	MYSQL_OUTPUT(24, MYSQL_TYPE_LONGLONG, &result.reminder_id,    sizeof(result.reminder_id));
	MYSQL_OUTPUT_BIND_AND_STORE();

	std::vector<Draft_Event> results;

	MYSQL_FETCH_AND_RETURN_MULTIPLE_ROWS();
}

// NOTE: As we're storing the values of these in the database, the order of these must not change! Add new values to the end.
enum SIGNUP_STATUS : int {
	SIGNUP_STATUS_NONE        = 0,
	SIGNUP_STATUS_COMPETITIVE = 1,
	SIGNUP_STATUS_CASUAL      = 2,
	SIGNUP_STATUS_FLEXIBLE    = 4,
	SIGNUP_STATUS_TENTATIVE   = 8,
	SIGNUP_STATUS_MINUTEMAGE  = 16,
	SIGNUP_STATUS_DECLINE     = 32, // This person was signed up but has clicked the Decline button.
	SIGNUP_STATUS_REMOVED     = 64, // The host has removed the player (likely a no-show) from the sign up sheet.

	SIGNUP_STATUS_INVALID     = -1,

	SIGNUP_STATUS_PLAYING     = SIGNUP_STATUS_COMPETITIVE | SIGNUP_STATUS_CASUAL | SIGNUP_STATUS_FLEXIBLE,
	SIGNUP_STATUS_NOT_PLAYING = SIGNUP_STATUS_TENTATIVE | SIGNUP_STATUS_MINUTEMAGE | SIGNUP_STATUS_DECLINE | SIGNUP_STATUS_REMOVED
};


static constexpr const std::string_view to_string(const SIGNUP_STATUS s) {
	switch(s) {
		case SIGNUP_STATUS_NONE:        return {"none"};
		case SIGNUP_STATUS_COMPETITIVE: return {"competitive"};
		case SIGNUP_STATUS_CASUAL:      return {"casual"};
		case SIGNUP_STATUS_FLEXIBLE:    return {"flexible"};
		case SIGNUP_STATUS_TENTATIVE:   return {"tentative"};
		case SIGNUP_STATUS_MINUTEMAGE:  return {"minutemage"};
		case SIGNUP_STATUS_DECLINE:     return {"decline"};
		case SIGNUP_STATUS_REMOVED:     return {"removed"};

		case SIGNUP_STATUS_INVALID:     return {"invalid"};

		case SIGNUP_STATUS_PLAYING:     return {"playing"};
		case SIGNUP_STATUS_NOT_PLAYING: return {"not playing"};
	}
	return {""};
}


struct Draft_Signup_Status {
	u64 member_id;
	time_t timestamp;
	SIGNUP_STATUS status;

	// Cache the members preferred name so we don't have to look it up every time someone changes their sign up status.
	// NOTE: Doing this creates a bug - If a member were to sign up to a draft and then later change their guild nickname, this nickname change would not be shown on the sign up post until that same member clicked one of the sign up buttons again. I can live with this!
	char preferred_name[DISCORD_NAME_LENGTH_MAX + 1];
};

static Database_Result<Draft_Signup_Status> database_get_members_sign_up_status(const u64 guild_id, const std::string_view draft_code, const u64 member_id) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	const char* query = "SELECT status, time, preferred_name FROM draft_signups WHERE guild_id=? AND member_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,          sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_LONGLONG, &member_id,         sizeof(member_id));
	MYSQL_INPUT(2, MYSQL_TYPE_STRING,   draft_code.data(),  draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE()

	// Defaults for "not currently signed up"
	Draft_Signup_Status      result;
	result.member_id         = member_id;
	result.timestamp         = 0;
	result.status            = SIGNUP_STATUS_NONE;
	result.preferred_name[0] = '\0';

	MYSQL_OUTPUT_INIT(3);
	MYSQL_OUTPUT(0, MYSQL_TYPE_LONG,     &result.status,         sizeof(result.status));
	MYSQL_OUTPUT(1, MYSQL_TYPE_LONGLONG, &result.timestamp,      sizeof(result.timestamp));
	MYSQL_OUTPUT(2, MYSQL_TYPE_STRING,   &result.preferred_name, DISCORD_NAME_LENGTH_MAX + 1);
	MYSQL_OUTPUT_BIND_AND_STORE();

	MYSQL_FETCH_AND_RETURN_ZERO_OR_ONE_ROWS();
}

static Database_Result<Database_No_Value> database_sign_up_to_a_draft(const u64 guild_id, const std::string_view draft_code, const u64 member_id, const std::string& preferred_name, const time_t timestamp, const SIGNUP_STATUS status) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	const char* query = "INSERT INTO draft_signups (guild_id, member_id, preferred_name, draft_code, time, status) VALUES(?,?,?,?,?,?) ON DUPLICATE KEY UPDATE preferred_name=?, time=?, status=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(9);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,              sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_LONGLONG, &member_id,             sizeof(member_id));
	MYSQL_INPUT(2, MYSQL_TYPE_STRING,   preferred_name.c_str(), preferred_name.length());
	MYSQL_INPUT(3, MYSQL_TYPE_STRING,   draft_code.data(),      draft_code.length());
	MYSQL_INPUT(4, MYSQL_TYPE_LONGLONG, &timestamp,             sizeof(timestamp));
	MYSQL_INPUT(5, MYSQL_TYPE_TINY,     &status,                sizeof(status));
	MYSQL_INPUT(6, MYSQL_TYPE_STRING,   preferred_name.c_str(), preferred_name.length());
	MYSQL_INPUT(7, MYSQL_TYPE_LONGLONG, &timestamp,             sizeof(timestamp));
	MYSQL_INPUT(8, MYSQL_TYPE_TINY,     &status,                sizeof(status));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

// Get a list of all sign ups for a draft, sorted by time
// TODO: This gets passed around a bunch of threads so likely should be a shared_ptr
static const Database_Result<std::vector<Draft_Signup_Status>> database_get_draft_sign_ups(const u64 guild_id, const std::string_view draft_code) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	const char* query = "SELECT member_id, preferred_name, time, status FROM draft_signups WHERE guild_id=? AND draft_code=? ORDER BY time";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,          sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING,   draft_code.data(),  draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	Draft_Signup_Status result;

	MYSQL_OUTPUT_INIT(4);
	MYSQL_OUTPUT(0, MYSQL_TYPE_LONGLONG, &result.member_id,     sizeof(result.member_id));
	MYSQL_OUTPUT(1, MYSQL_TYPE_STRING,   result.preferred_name, DISCORD_NAME_LENGTH_MAX + 1);
	MYSQL_OUTPUT(2, MYSQL_TYPE_LONGLONG, &result.timestamp,     sizeof(result.timestamp));
	MYSQL_OUTPUT(3, MYSQL_TYPE_LONG,     &result.status,        sizeof(result.status));
	MYSQL_OUTPUT_BIND_AND_STORE();

	std::vector<Draft_Signup_Status> results;

	MYSQL_FETCH_AND_RETURN_MULTIPLE_ROWS();
}

struct Draft_Sign_Up {
	u64 member_id;
	char preferred_name[DISCORD_NAME_LENGTH_MAX + 1];
	SIGNUP_STATUS status;
	time_t time;

	my_bool rank_is_null;
	int rank;

	my_bool is_shark_is_null;
	bool is_shark;

	my_bool points_is_null;
	int points;

	my_bool devotion_is_null;
	int devotion;

	my_bool win_rate_is_null;
	f32 win_rate;

	bool is_host; // NOTE: Not written to by a database query - set elsewhere.
	POD_ALLOCATION_REASON reason;
};

// TODO: This function needs a more accurate name - it's also too similar to database_get_draft_sign_ups
static Database_Result<std::vector<Draft_Sign_Up>> database_get_sign_ups(const u64 guild_id, const std::string_view draft_code, const char* league, int season) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = R"(
		SELECT
			draft_signups.member_id,
			draft_signups.preferred_name,
			draft_signups.status,
			draft_signups.time,
			leaderboards.rank,
			shark.is_shark,
			leaderboards.points,
			devotion.value AS devotion,
			win_rate_recent.overall AS win_rate
		FROM draft_signups
		LEFT JOIN leaderboards ON draft_signups.member_id=leaderboards.member_id AND leaderboards.league=? AND leaderboards.season=?-- League code from spreadsheet: PC, AC, EB etc
		LEFT JOIN shark ON draft_signups.member_id=shark.id
		LEFT JOIN devotion ON draft_signups.member_id=devotion.id
		LEFT JOIN win_rate_recent ON draft_signups.member_id=win_rate_recent.id
		WHERE
			draft_signups.guild_id=?
		AND
			draft_signups.draft_code=?
		ORDER BY draft_signups.time
		;)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(4);
	MYSQL_INPUT(0, MYSQL_TYPE_STRING,   league,            strlen(league));
	MYSQL_INPUT(1, MYSQL_TYPE_LONG,     &season,           sizeof(season));
	MYSQL_INPUT(2, MYSQL_TYPE_LONGLONG, &guild_id,         sizeof(guild_id));
	MYSQL_INPUT(3, MYSQL_TYPE_STRING,   draft_code.data(), draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	Draft_Sign_Up result;

	MYSQL_BIND output[9];
	memset(output, 0, sizeof(output));
	unsigned long length[9];
	my_bool is_error[9];
	my_bool is_null[9]; // NOTE: Not all are used here

	MYSQL_OUTPUT(0, MYSQL_TYPE_LONGLONG, &result.member_id, sizeof(result.member_id));
	MYSQL_OUTPUT(1, MYSQL_TYPE_STRING, result.preferred_name, DISCORD_NAME_LENGTH_MAX + 1);
	MYSQL_OUTPUT(2, MYSQL_TYPE_LONG, &result.status, sizeof(result.status));
	MYSQL_OUTPUT(3, MYSQL_TYPE_LONGLONG, &result.time, sizeof(result.time));

	output[4].buffer_type = MYSQL_TYPE_LONG;
	output[4].buffer = (void*) &result.rank;
	output[4].buffer_length = sizeof(result.rank);
	output[4].is_null = &result.rank_is_null;
	output[4].length = &length[4];
	output[4].error = &is_error[4];

	output[5].buffer_type = MYSQL_TYPE_TINY;
	output[5].buffer = (void*) &result.is_shark;
	output[5].buffer_length = sizeof(result.is_shark);
	output[5].is_null = &result.is_shark_is_null;
	output[5].length = &length[5];
	output[5].error = &is_error[5];

	output[6].buffer_type = MYSQL_TYPE_LONG;
	output[6].buffer = (void*) &result.points;
	output[6].buffer_length = sizeof(result.points);
	output[6].is_null = &result.points_is_null;
	output[6].length = &length[6];
	output[6].error = &is_error[6];

	output[7].buffer_type = MYSQL_TYPE_LONG;
	output[7].buffer = (void*) &result.devotion;
	output[7].buffer_length = sizeof(result.devotion);
	output[7].is_null = &result.devotion_is_null;
	output[7].length = &length[7];
	output[7].error = &is_error[7];

	output[8].buffer_type = MYSQL_TYPE_FLOAT;
	output[8].buffer = (void*) &result.win_rate;
	output[8].buffer_length = sizeof(result.win_rate);
	output[8].is_null = &result.win_rate_is_null;
	output[8].length = &length[8];
	output[8].error = &is_error[8];

	MYSQL_OUTPUT_BIND_AND_STORE();

	std::vector<Draft_Sign_Up> results;

	MYSQL_FETCH_AND_RETURN_MULTIPLE_ROWS();
}

#if 0
// TODO: This function needs a more accurate name - it's also too similar to database_get_draft_sign_ups
static Database_Result<std::vector<Draft_Sign_Up>> database_get_playing_sign_ups(const u64 guild_id, const std::string_view draft_code, const char* league, int season) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = R"(
		SELECT
			draft_signups.member_id,
			draft_signups.preferred_name,
			draft_signups.status,
			draft_signups.time,
			leaderboards.rank,
			shark.is_shark,
			leaderboards.points,
			devotion.value AS devotion,
			win_rate_recent.overall AS win_rate
		FROM draft_signups
		LEFT JOIN leaderboards ON draft_signups.member_id=leaderboards.member_id AND leaderboards.league=? AND leaderboards.season=?-- League code from spreadsheet: PC, AC, EB etc
		LEFT JOIN shark ON draft_signups.member_id=shark.id
		LEFT JOIN devotion ON draft_signups.member_id=devotion.id
		LEFT JOIN win_rate_recent ON draft_signups.member_id=win_rate_recent.id
		WHERE
			draft_signups.guild_id=?
		AND
			draft_signups.draft_code=?
		AND draft_signups.status & (1|2|4) -- FIXME: Magic numbers
		AND NOT draft_signups.status & (8|16|32|64) -- FIXME: Magic numbers
		ORDER BY draft_signups.time
		;)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(4);
	MYSQL_INPUT(0, MYSQL_TYPE_STRING, league, strlen(league));
	MYSQL_INPUT(1, MYSQL_TYPE_LONG, &season, sizeof(season));
	MYSQL_INPUT(2, MYSQL_TYPE_LONGLONG, &guild_id, sizeof(guild_id));
	MYSQL_INPUT(3, MYSQL_TYPE_STRING, draft_code.data(), draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	Draft_Sign_Up result;

	MYSQL_BIND output[9];
	memset(output, 0, sizeof(output));
	unsigned long length[9];
	my_bool is_error[9];
	my_bool is_null[9]; // NOTE: Not all are used here

	MYSQL_OUTPUT(0, MYSQL_TYPE_LONGLONG, &result.member_id, sizeof(result.member_id));
	MYSQL_OUTPUT(1, MYSQL_TYPE_STRING, result.preferred_name, DISCORD_NAME_LENGTH_MAX + 1);
	MYSQL_OUTPUT(2, MYSQL_TYPE_LONG, &result.status, sizeof(result.status));
	MYSQL_OUTPUT(3, MYSQL_TYPE_LONGLONG, &result.time, sizeof(result.time));

	output[4].buffer_type = MYSQL_TYPE_LONG;
	output[4].buffer = (void*) &result.rank;
	output[4].buffer_length = sizeof(result.rank);
	output[4].is_null = &result.rank_is_null;
	output[4].length = &length[4];
	output[4].error = &is_error[4];

	output[5].buffer_type = MYSQL_TYPE_TINY;
	output[5].buffer = (void*) &result.is_shark;
	output[5].buffer_length = sizeof(result.is_shark);
	output[5].is_null = &result.is_shark_is_null;
	output[5].length = &length[5];
	output[5].error = &is_error[5];

	output[6].buffer_type = MYSQL_TYPE_LONG;
	output[6].buffer = (void*) &result.points;
	output[6].buffer_length = sizeof(result.points);
	output[6].is_null = &result.points_is_null;
	output[6].length = &length[6];
	output[6].error = &is_error[6];

	output[7].buffer_type = MYSQL_TYPE_LONG;
	output[7].buffer = (void*) &result.devotion;
	output[7].buffer_length = sizeof(result.devotion);
	output[7].is_null = &result.devotion_is_null;
	output[7].length = &length[7];
	output[7].error = &is_error[7];

	output[8].buffer_type = MYSQL_TYPE_FLOAT;
	output[8].buffer = (void*) &result.win_rate;
	output[8].buffer_length = sizeof(result.win_rate);
	output[8].is_null = &result.win_rate_is_null;
	output[8].length = &length[8];
	output[8].error = &is_error[8];

	MYSQL_OUTPUT_BIND_AND_STORE();

	std::vector<Draft_Sign_Up> results;

	MYSQL_FETCH_AND_RETURN_MULTIPLE_ROWS();
}
#endif

struct Member {
	u64 member_id;
	const char preferred_name[DISCORD_NAME_LENGTH_MAX + 1];
};

static const Database_Result<std::vector<Member>> database_get_sign_up_names_autocomplete(const u64 guild_id, const std::string_view draft_code, std::string& prefix, int limit) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	prefix += "%";
	const char* query = "SELECT member_id, preferred_name FROM draft_signups WHERE guild_id=? AND draft_code=? AND preferred_name LIKE ? ORDER BY preferred_name LIMIT ?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(4);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,         sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING,   draft_code.data(), draft_code.length());
	MYSQL_INPUT(2, MYSQL_TYPE_STRING,   prefix.c_str(),    prefix.length());
	MYSQL_INPUT(3, MYSQL_TYPE_LONGLONG, &limit,            sizeof(limit));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	Member result = {0};

	MYSQL_OUTPUT_INIT(2);
	MYSQL_OUTPUT(0, MYSQL_TYPE_LONGLONG, &result.member_id, sizeof(result.member_id));
	MYSQL_OUTPUT(1, MYSQL_TYPE_STRING, &result.preferred_name[0], DISCORD_NAME_LENGTH_MAX + 1);
	MYSQL_OUTPUT_BIND_AND_STORE();

	std::vector<Member> results;

	MYSQL_FETCH_AND_RETURN_MULTIPLE_ROWS();
}

static const Database_Result<std::vector<std::string>> database_get_draft_codes_for_post_draft_autocomplete(const u64 guild_id, std::string& prefix, int limit) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);

	prefix += "%"; // The LIKE operator has to be part of the parameter, not in the query string.
	const char* query = "SELECT draft_code FROM draft_events WHERE guild_id=? AND status=? AND draft_code LIKE ? ORDER BY draft_code LIMIT ?";
	MYSQL_STATEMENT();

	const DRAFT_STATUS status = DRAFT_STATUS_CREATED;

	MYSQL_INPUT_INIT(4);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,      sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_LONG,     &status,        sizeof(status));
	MYSQL_INPUT(2, MYSQL_TYPE_STRING,   prefix.c_str(), prefix.length());
	MYSQL_INPUT(3, MYSQL_TYPE_LONGLONG, &limit,		    sizeof(limit));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	char result[DRAFT_CODE_LENGTH_MAX + 1];

	MYSQL_OUTPUT_INIT(1);
	MYSQL_OUTPUT(0, MYSQL_TYPE_STRING, &result[0], DRAFT_CODE_LENGTH_MAX + 1);
	MYSQL_OUTPUT_BIND_AND_STORE();

	std::vector<std::string> results;

	MYSQL_FETCH_AND_RETURN_MULTIPLE_ROWS();
}

static const Database_Result<std::vector<std::string>> database_get_draft_codes_for_edit_draft_autocomplete(const u64 guild_id, std::string& prefix, int limit) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);

	prefix += "%"; // The LIKE operator has to be part of the parameter, not in the query string.
	const char* query = "SELECT draft_code FROM draft_events WHERE guild_id=? AND draft_code LIKE ? ORDER BY draft_code LIMIT ?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,      sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING,   prefix.c_str(), prefix.length());
	MYSQL_INPUT(2, MYSQL_TYPE_LONGLONG, &limit,         sizeof(limit));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	char result[DRAFT_CODE_LENGTH_MAX + 1];

	MYSQL_OUTPUT_INIT(1);
	MYSQL_OUTPUT(0, MYSQL_TYPE_STRING, &result[0], DRAFT_CODE_LENGTH_MAX + 1);
	MYSQL_OUTPUT_BIND_AND_STORE();

	std::vector<std::string> results;

	MYSQL_FETCH_AND_RETURN_MULTIPLE_ROWS();
}

static const Database_Result<std::vector<std::string>> database_get_draft_codes_for_delete_draft_autocomplete(const u64 guild_id, std::string& prefix, int limit) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);

	prefix += "%"; // The LIKE operator has to be part of the parameter, not in the query string.
	const char* query = "SELECT draft_code FROM draft_events WHERE guild_id=? AND status>=? AND status<? AND draft_code LIKE ? ORDER BY draft_code LIMIT ?";
	MYSQL_STATEMENT();

	const DRAFT_STATUS status1 = DRAFT_STATUS_CREATED;
	const DRAFT_STATUS status2 = DRAFT_STATUS_COMPLETE;

	MYSQL_INPUT_INIT(5);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,      sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_LONG,     &status1,       sizeof(status1));
	MYSQL_INPUT(2, MYSQL_TYPE_LONG,     &status2,       sizeof(status2));
	MYSQL_INPUT(3, MYSQL_TYPE_STRING,   prefix.c_str(), prefix.length());
	MYSQL_INPUT(4, MYSQL_TYPE_LONGLONG, &limit,         sizeof(limit));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	char result[DRAFT_CODE_LENGTH_MAX + 1];

	MYSQL_OUTPUT_INIT(1);
	MYSQL_OUTPUT(0, MYSQL_TYPE_STRING, &result[0], DRAFT_CODE_LENGTH_MAX + 1);
	MYSQL_OUTPUT_BIND_AND_STORE();

	std::vector<std::string> results;

	MYSQL_FETCH_AND_RETURN_MULTIPLE_ROWS();
}

static Database_Result<Database_No_Value> database_set_details_message_id(const u64 guild_id, const std::string_view draft_code, const u64 message_id) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "UPDATE draft_events SET details_id=? WHERE guild_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &message_id,       sizeof(message_id));
	MYSQL_INPUT(1, MYSQL_TYPE_LONGLONG, &guild_id,         sizeof(guild_id));
	MYSQL_INPUT(2, MYSQL_TYPE_STRING,   draft_code.data(), draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_set_signups_message_id(const u64 guild_id, const std::string_view draft_code, const u64 message_id) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "UPDATE draft_events SET signups_id=? WHERE guild_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &message_id,       sizeof(message_id));
	MYSQL_INPUT(1, MYSQL_TYPE_LONGLONG, &guild_id,         sizeof(guild_id));
	MYSQL_INPUT(2, MYSQL_TYPE_STRING,   draft_code.data(), draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_set_reminder_message_id(const u64 guild_id, const std::string_view draft_code, const u64 message_id) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "UPDATE draft_events SET reminder_id=? WHERE guild_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &message_id,       sizeof(message_id));
	MYSQL_INPUT(1, MYSQL_TYPE_LONGLONG, &guild_id,         sizeof(guild_id));
	MYSQL_INPUT(2, MYSQL_TYPE_STRING,   draft_code.data(), draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<std::string> database_get_next_upcoming_draft(const u64 guild_id) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "SELECT draft_code FROM draft_events WHERE status>? AND status<? AND guild_id=? ORDER BY time ASC LIMIT 1";
	MYSQL_STATEMENT();

	const DRAFT_STATUS status1 = DRAFT_STATUS_POSTED;
	const DRAFT_STATUS status2 = DRAFT_STATUS_COMPLETE;

	MYSQL_INPUT_INIT(3)
	MYSQL_INPUT(0, MYSQL_TYPE_LONG,     &status1,   sizeof(status1));
	MYSQL_INPUT(1, MYSQL_TYPE_LONG,     &status2,   sizeof(status2));
	MYSQL_INPUT(2, MYSQL_TYPE_LONGLONG, &guild_id,  sizeof(guild_id));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	char result[DRAFT_CODE_LENGTH_MAX + 1];

	MYSQL_OUTPUT_INIT(1);
	MYSQL_OUTPUT(0, MYSQL_TYPE_STRING, &result[0], DRAFT_CODE_LENGTH_MAX);
	MYSQL_OUTPUT_BIND_AND_STORE();

	MYSQL_FETCH_AND_RETURN_ZERO_OR_ONE_ROWS();
}

static Database_Result<Database_No_Value> database_clear_draft_post_ids(const u64 guild_id, const std::string_view draft_code) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "UPDATE draft_events SET details_id=0, signups_id=0 WHERE guild_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,         sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING,   draft_code.data(), draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_set_draft_status(const u64 guild_id, const std::string_view draft_code, const DRAFT_STATUS status) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "UPDATE draft_events SET status=status|? WHERE guild_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT(0, MYSQL_TYPE_LONG,     &status,           sizeof(status));
	MYSQL_INPUT(1, MYSQL_TYPE_LONGLONG, &guild_id,         sizeof(guild_id));
	MYSQL_INPUT(2, MYSQL_TYPE_STRING,   draft_code.data(), draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_purge_draft_event(const u64 guild_id, const std::string_view draft_code) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "DELETE FROM draft_events WHERE guild_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,         sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING,   draft_code.data(), draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

struct Draft_Post_IDs {
	u64 channel;
	u64 details;
	u64 signups;
};

// TODO: Don't need this? No function calls this that doesn't call database_get_event
static Database_Result<Draft_Post_IDs> database_get_draft_post_ids(const u64 guild_id, const std::string_view draft_code) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "SELECT signup_channel_id, details_id, signups_id FROM draft_events WHERE guild_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,         sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING,   draft_code.data(), draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	Draft_Post_IDs result;

	MYSQL_OUTPUT_INIT(3);
	MYSQL_OUTPUT(0, MYSQL_TYPE_LONGLONG, &result.channel, sizeof(result.channel));
	MYSQL_OUTPUT(1, MYSQL_TYPE_LONGLONG, &result.details, sizeof(result.details));
	MYSQL_OUTPUT(2, MYSQL_TYPE_LONGLONG, &result.signups, sizeof(result.signups));
	MYSQL_OUTPUT_BIND_AND_STORE();

	MYSQL_FETCH_AND_RETURN_SINGLE_ROW();
}

static Database_Result<Database_No_Value> database_add_temp_role(const u64 guild_id, const std::string_view draft_code, const u64 role_id) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "REPLACE INTO temp_roles (guild_id, draft_code, role_id) VALUES(?,?,?)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,         sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING,   draft_code.data(), draft_code.length());
	MYSQL_INPUT(2, MYSQL_TYPE_LONGLONG, &role_id,          sizeof(role_id));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<std::vector<u64>> database_get_temp_roles(const u64 guild_id, const std::string_view draft_code) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "SELECT role_id FROM temp_roles WHERE guild_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id, sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING, draft_code.data(), draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	u64 result;

	MYSQL_OUTPUT_INIT(1);
	MYSQL_OUTPUT(0, MYSQL_TYPE_LONGLONG, &result, sizeof(result));
	MYSQL_OUTPUT_BIND_AND_STORE();

	std::vector<u64> results;

	MYSQL_FETCH_AND_RETURN_MULTIPLE_ROWS();
}

static Database_Result<Database_No_Value> database_del_temp_roles(const u64 guild_id, const std::string draft_code) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "DELETE FROM temp_roles WHERE guild_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id, sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING, draft_code.data(), draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

#if 0
static Database_Result<Database_No_Value> database_add_temp_thread(const u64 guild_id, const u64 thread_id, const std::string_view draft_code) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "REPLACE INTO temp_threads (guild_id, draft_code, thread_id) VALUES(?,?,?)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,         sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING,   draft_code.data(), draft_code.length());
	MYSQL_INPUT(2, MYSQL_TYPE_LONGLONG, &thread_id,        sizeof(thread_id));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}
#endif

#if 0
static Database_Result<std::vector<u64>> database_get_temp_threads(const u64 guild_id, const std::string_view draft_code) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "SELECT thread_id FROM temp_threads WHERE guild_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id, sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING, draft_code.data(), draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	u64 result;

	MYSQL_OUTPUT_INIT(1);
	MYSQL_OUTPUT(0, MYSQL_TYPE_LONGLONG, &result, sizeof(result));
	MYSQL_OUTPUT_BIND_AND_STORE();

	std::vector<u64> results;

	MYSQL_FETCH_AND_RETURN_MULTIPLE_ROWS();
}
#endif


#if 0
static Database_Result<Database_No_Value> database_del_temp_threads(const u64 guild_id, const std::string_view draft_code) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char *query = "DELETE FROM temp_threads WHERE guild_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,  sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING,   draft_code.data(), draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}
#endif


#if 0
// Not used, but may want this back in the short term.
static Database_Result<Database_No_Value> database_add_temp_member_role(const u64 guild_id, const u64 member_id, const u64 role_id) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "REPLACE INTO temp_members (guild_id, member_id, role_id) VALUES(?,?,?)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,  sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_LONGLONG, &member_id, sizeof(member_id));
	MYSQL_INPUT(2, MYSQL_TYPE_LONGLONG, &role_id,   sizeof(role_id));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}
#endif

static Database_Result<Database_No_Value> database_add_noshow(const u64 guild_id, const u64 member_id, const std::string_view draft_code) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "REPLACE INTO noshows (guild_id, member_id, draft_code) VALUES(?,?,?)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,         sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_LONGLONG, &member_id,        sizeof(member_id));
	MYSQL_INPUT(2, MYSQL_TYPE_STRING,   draft_code.data(), draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_add_dropper(const u64 guild_id, const u64 member_id, const std::string_view draft_code, const std::string_view note) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "REPLACE INTO droppers (guild_id, member_id, draft_code, note) VALUES(?,?,?,?)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(4);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,         sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_LONGLONG, &member_id,        sizeof(member_id));
	MYSQL_INPUT(2, MYSQL_TYPE_STRING,   draft_code.data(), draft_code.length());
	MYSQL_INPUT(3, MYSQL_TYPE_STRING,   note.data(),       note.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_delete_member_from_all_sign_ups(const u64 guild_id, const u64 member_id) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "DELETE FROM draft_signups WHERE guild_id=? AND member_id=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,  sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_LONGLONG, &member_id, sizeof(member_id));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_update_banner_timestamp(const u64 guild_id, const std::string_view draft_code, const time_t timestamp) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "UPDATE draft_events SET banner_timestamp=? WHERE guild_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &timestamp,        sizeof(timestamp));
	MYSQL_INPUT(1, MYSQL_TYPE_LONGLONG, &guild_id,         sizeof(guild_id));
	MYSQL_INPUT(2, MYSQL_TYPE_STRING,   draft_code.data(), draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}


static const size_t XMAGE_VERSION_STRING_MAX = 128;

struct XMage_Version {
	char version[XMAGE_VERSION_STRING_MAX + 1];
	u64 timestamp;
};

static Database_Result<XMage_Version> database_get_xmage_version() {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "SELECT version, timestamp FROM xmage_version ORDER BY timestamp DESC LIMIT 1";
	MYSQL_STATEMENT();

	MYSQL_EXECUTE();

	XMage_Version result;

	MYSQL_OUTPUT_INIT(2);
	MYSQL_OUTPUT(0, MYSQL_TYPE_STRING,   &result.version[0], XMAGE_VERSION_STRING_MAX);
	MYSQL_OUTPUT(1, MYSQL_TYPE_LONGLONG, &result.timestamp,  sizeof(result.timestamp));
	MYSQL_OUTPUT_BIND_AND_STORE();

	MYSQL_FETCH_AND_RETURN_SINGLE_ROW();
}

struct Stats {
	u64 timestamp;
	struct {
		char name[DEVOTION_BADGE_NAME_LENGTH_MAX + 1];
		int value;
		int next;
	} devotion;

	struct {
		char name[VICTORY_BADGE_NAME_LENGTH_MAX + 1];
		int value;
		int next;
	} victory;

	struct {
		char name[TROPHIES_BADGE_NAME_LENGTH_MAX + 1];
		int value;
		int next;
	} trophies;

	struct {
		char name[SHARK_BADGE_NAME_LENGTH_MAX + 1];
		int value;
		int next;
	} shark;

	struct {
		char name[HERO_BADGE_NAME_LENGTH_MAX + 1];
		int value;
		int next;
	} hero;

	struct {
		float chrono; // TODO: Still called 'league' in the database schema.
		float bonus;
		float overall;
	} win_rate_recent;

	struct {
		float chrono; // TODO: Still called 'league' in the database schema.
		float bonus;
		float overall;
	} win_rate_all_time;

	struct {
		u64 timestamp;
		char url[URL_LENGTH_MAX + 1];
	} badge_card;
};

static Database_Result<Stats> database_get_stats(const u64 member_id) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = R"(
	SELECT
		stats.timestamp, -- 0
		devotion.name, devotion.value, devotion.next, -- 1, 2, 3
		victory.name, victory.value, victory.next, -- 4, 5, 6
		trophies.name, trophies.value, trophies.next, -- 7, 8, 9
		shark.name, shark.value, shark.next, -- 10, 11, 12
		hero.name, hero.value, hero.next, -- 13, 14, 15
		ROUND(win_rate_recent.league,2), ROUND(win_rate_recent.bonus,2), ROUND(win_rate_recent.overall,2), -- 16, 17, 18
		ROUND(win_rate_all_time.league,2), ROUND(win_rate_all_time.bonus,2), ROUND(win_rate_all_time.overall,2), -- 19, 20, 21
		badges.url, badges.timestamp -- 22, 23
	FROM stats
	INNER JOIN devotion ON stats.id = devotion.id
	INNER JOIN victory ON stats.id = victory.id
	INNER JOIN trophies ON stats.id = trophies.id
	INNER JOIN shark ON stats.id = shark.id
	INNER JOIN hero ON stats.id = hero.id
	INNER JOIN win_rate_recent ON stats.id = win_rate_recent.id
	INNER JOIN win_rate_all_time ON stats.id = win_rate_all_time.id
	INNER JOIN badges ON stats.id = badges.id
	WHERE stats.id=?
	)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(1);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &member_id, sizeof(member_id));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	Stats result;
	memset(&result, 0, sizeof(result));

	MYSQL_OUTPUT_INIT(24);
	MYSQL_OUTPUT( 0, MYSQL_TYPE_LONGLONG, &result.timestamp, sizeof(result.timestamp));
	MYSQL_OUTPUT( 1, MYSQL_TYPE_STRING, result.devotion.name, 32+1); // FIXME: Magic number
	MYSQL_OUTPUT( 2, MYSQL_TYPE_LONG, &result.devotion.value, sizeof(result.devotion.value));
	MYSQL_OUTPUT( 3, MYSQL_TYPE_LONG, &result.devotion.next, sizeof(result.devotion.next));
	MYSQL_OUTPUT( 4, MYSQL_TYPE_STRING, result.victory.name, 32+1);
	MYSQL_OUTPUT( 5, MYSQL_TYPE_LONG, &result.victory.value, sizeof(result.victory.value));
	MYSQL_OUTPUT( 6, MYSQL_TYPE_LONG, &result.victory.next, sizeof(result.victory.next));
	MYSQL_OUTPUT( 7, MYSQL_TYPE_STRING, result.trophies.name, 32+1);
	MYSQL_OUTPUT( 8, MYSQL_TYPE_LONG, &result.trophies.value, sizeof(result.trophies.value));
	MYSQL_OUTPUT( 9, MYSQL_TYPE_LONG, &result.trophies.next, sizeof(result.trophies.next));
	MYSQL_OUTPUT(10, MYSQL_TYPE_STRING, result.shark.name, 32+1);
	MYSQL_OUTPUT(11, MYSQL_TYPE_LONG, &result.shark.value, sizeof(result.shark.value));
	MYSQL_OUTPUT(12, MYSQL_TYPE_LONG, &result.shark.next, sizeof(result.shark.next));
	MYSQL_OUTPUT(13, MYSQL_TYPE_STRING, result.hero.name, 32+1);
	MYSQL_OUTPUT(14, MYSQL_TYPE_LONG, &result.hero.value, sizeof(result.hero.value));
	MYSQL_OUTPUT(15, MYSQL_TYPE_LONG, &result.hero.next, sizeof(result.hero.next));
	MYSQL_OUTPUT(16, MYSQL_TYPE_FLOAT, &result.win_rate_recent.chrono, sizeof(result.win_rate_recent.chrono));
	MYSQL_OUTPUT(17, MYSQL_TYPE_FLOAT, &result.win_rate_recent.bonus, sizeof(result.win_rate_recent.bonus));
	MYSQL_OUTPUT(18, MYSQL_TYPE_FLOAT, &result.win_rate_recent.overall, sizeof(result.win_rate_recent.overall));
	MYSQL_OUTPUT(19, MYSQL_TYPE_FLOAT, &result.win_rate_all_time.chrono, sizeof(result.win_rate_all_time.chrono));
	MYSQL_OUTPUT(20, MYSQL_TYPE_FLOAT, &result.win_rate_all_time.bonus, sizeof(result.win_rate_all_time.bonus));
	MYSQL_OUTPUT(21, MYSQL_TYPE_FLOAT, &result.win_rate_all_time.overall, sizeof(result.win_rate_all_time.overall));
	MYSQL_OUTPUT(22, MYSQL_TYPE_STRING, result.badge_card.url, URL_LENGTH_MAX + 1); // FIXME: Magic number
	MYSQL_OUTPUT(23, MYSQL_TYPE_LONGLONG, &result.badge_card.timestamp, sizeof(result.badge_card.timestamp));
	MYSQL_OUTPUT_BIND_AND_STORE();

	MYSQL_FETCH_AND_RETURN_ZERO_OR_ONE_ROWS();
}

// NOTE: Discord has a limit of 100 characters for auto complete options.
static const size_t COMMAND_NAME_LENGTH_MAX = 32; // TODO: Validate/enforce this in the spreadsheet. FIXME: This does not match the db schema max
static const size_t COMMAND_SUMMARY_LENGTH_MAX = 64;

struct Command_Summary {
	char name[COMMAND_NAME_LENGTH_MAX + 1];
	char summary[COMMAND_SUMMARY_LENGTH_MAX + 1]; // FIXME: Magic number
};

static const Database_Result<std::vector<Command_Summary>> database_get_help_messages_for_autocomplete(const u64 guild_id, std::string& prefix, int limit) {
	(void)guild_id;
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	const std::string search = "%" + prefix + "%";
	//prefix += "%";
	// NOTE: Discord allows a max of 25 auto complete options, but we only want 24 here to
	// leave room for the 'all commands' option.
	const char* query = "SELECT name, summary FROM commands WHERE hidden=0 AND LOWER(summary) LIKE LOWER(?) ORDER BY name LIMIT ?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT(0, MYSQL_TYPE_STRING, search.c_str(), search.length());
	MYSQL_INPUT(1, MYSQL_TYPE_LONG, &limit, sizeof(limit));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	Command_Summary result;
	MYSQL_OUTPUT_INIT(2);
	MYSQL_OUTPUT(0, MYSQL_TYPE_STRING, result.name, COMMAND_NAME_LENGTH_MAX + 1);
	MYSQL_OUTPUT(1, MYSQL_TYPE_STRING, result.summary, COMMAND_SUMMARY_LENGTH_MAX + 1);
	MYSQL_OUTPUT_BIND_AND_STORE();

	std::vector<Command_Summary> results;

	MYSQL_FETCH_AND_RETURN_MULTIPLE_ROWS();
}

struct Command {
	bool host; // TODO: Still called "team" in the db schema
	char content[DISCORD_MESSAGE_CHARACTER_LIMIT + 1]; // TODO: Enforce this in the spreadsheet.
};

static const Database_Result<Command> database_get_help_message(const u64 guild_id, const std::string_view name) {
	(void)guild_id;
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	const char* query = "SELECT team, content FROM commands WHERE summary=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(1);
	MYSQL_INPUT(0, MYSQL_TYPE_STRING, name.data(), name.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	Command result;

	MYSQL_OUTPUT_INIT(2);
	MYSQL_OUTPUT(0, MYSQL_TYPE_LONGLONG, &result.host, sizeof(result.host));
	MYSQL_OUTPUT(1, MYSQL_TYPE_STRING, result.content, DISCORD_MESSAGE_CHARACTER_LIMIT + 1);
	MYSQL_OUTPUT_BIND_AND_STORE();

	MYSQL_FETCH_AND_RETURN_SINGLE_ROW();
}

static const Database_Result<std::vector<Command_Summary>> database_get_all_help_messages(const u64 guild_id) {
	(void)guild_id;
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	const char* query = "SELECT name, summary FROM commands WHERE hidden=0 ORDER BY name";
	MYSQL_STATEMENT();

	MYSQL_EXECUTE();

	Command_Summary result;

	MYSQL_OUTPUT_INIT(2);
	MYSQL_OUTPUT(0, MYSQL_TYPE_STRING, result.name, COMMAND_NAME_LENGTH_MAX + 1);
	MYSQL_OUTPUT(1, MYSQL_TYPE_STRING, result.summary, COMMAND_SUMMARY_LENGTH_MAX + 1);
	MYSQL_OUTPUT_BIND_AND_STORE();

	std::vector<Command_Summary> results;

	MYSQL_FETCH_AND_RETURN_MULTIPLE_ROWS();
}

static const int BANNER_IMAGE_WIDTH = 825;
static const int BANNER_IMAGE_HEIGHT = 550;

// Pack images are scaled to this size
static const int PACK_IMAGE_WIDTH  = 275;
static const int PACK_IMAGE_HEIGHT = 430;

static const int KEY_ART_WIDTH = BANNER_IMAGE_WIDTH;
static const int KEY_ART_HEIGHT = BANNER_IMAGE_HEIGHT;


struct Text_Dim {
	int w;
	int h;
};

static Text_Dim get_text_dimensions(stbtt_fontinfo* font, const int size, const u8* str) {
	f32 scale = stbtt_ScaleForPixelHeight(font, size);

	Text_Dim dim;

	int ascent, descent, linegap;
	stbtt_GetFontVMetrics(font, &ascent, &descent, &linegap);

	dim.h = ceil(scale * (ascent - descent));

	f32 xpos = 0.0f;
	u32 ch = 0;
	int index = 0;
	while((str[index] != 0) && ((ch = u8_nextchar(str, &index)) != 0)) {
		f32 x_shift = xpos - (f32) floor(xpos);
		int advance, lsb;
		stbtt_GetCodepointHMetrics(font, ch, &advance, &lsb);
		int x0, y0, x1, y1;
	  	stbtt_GetCodepointBitmapBoxSubpixel(font, ch, scale, scale, x_shift, 0, &x0, &y0, &x1, &y1);
		xpos += advance * scale;
		if(str[index+1] != 0 && isutf(str[index+1])) {
			int tmp = index;
			xpos += scale * stbtt_GetCodepointKernAdvance(font, ch, u8_nextchar(str, &tmp));
		}
	}

	dim.w = ceil(xpos);

	return dim;
}

static void render_text_to_image(stbtt_fontinfo* font, const u8* str, const int size, Image* canvas, int x, int y, const Pixel color) {
	f32 scale = stbtt_ScaleForPixelHeight(font, size);
	int ascent, descent, linegap;
	stbtt_GetFontVMetrics(font, &ascent, &descent, &linegap);
	int baseline = (int) (ascent * scale);

	static const int GLYPH_WIDTH_MAX = 200;
	static const int GLYPH_HEIGHT_MAX = 300;
	u8 bitmap_buffer[GLYPH_WIDTH_MAX * GLYPH_HEIGHT_MAX];

	Image bitmap;
	bitmap.data = (u8*)bitmap_buffer;
	bitmap.channels = 1;

	f32 xpos = (f32)x;
	int ch = 0;
	int index = 0;
	while((str[index]) != 0 && ((ch = u8_nextchar(str, &index)) != 0)) {
		f32 x_shift = xpos - (f32) floor(xpos);
		int advance, lsb;
		stbtt_GetCodepointHMetrics(font, ch, &advance, &lsb);
		int x0, y0, x1, y1;
	  	stbtt_GetCodepointBitmapBoxSubpixel(font, ch, scale, scale, x_shift, 0, &x0, &y0, &x1, &y1);
		bitmap.w = x1-x0;
		bitmap.h = y1-y0;
	  	stbtt_MakeCodepointBitmapSubpixel(font, (u8*)bitmap.data, bitmap.w, bitmap.h, GLYPH_WIDTH_MAX, scale, scale, x_shift, 0, ch);

		if(canvas->channels == 4) {
			blit_A8_to_RGBA(&bitmap, GLYPH_WIDTH_MAX, color, canvas, (int)xpos + x0, y + baseline + y0);
		} else
		if(canvas->channels == 1) {
			blit_A8_to_A8(&bitmap, GLYPH_WIDTH_MAX, canvas, (int)xpos + x0, y + baseline + y0);
		} else {
			log(LOG_LEVEL_ERROR, "Unsupported channel count {} in {}", canvas->channels, __FUNCTION__);
		}

		xpos += advance * scale;
		if(str[index+1] != 0 && isutf(str[index+1])) {
			int index_copy = index;
			xpos += scale * stbtt_GetCodepointKernAdvance(font, ch, u8_nextchar(str, &index_copy));
		}
	}
}

void draw_shadowed_text(stbtt_fontinfo* font, int font_size, int max_width, const u8* str, u32 shadow_color, u32 text_color, Image* out, int ypos) {
	// The output of the stbtt_truetype library isn't as nice as Freetype so to generate smoother looking glyphs we render the text larger than we need, then scale it down to the requested size. This produces better aliasing, IMO!
	static const int upscale_factor = 2;
	const int upscaled_font_size = (font_size * upscale_factor); // TODO: Rename to font_size_upscaled or something

	Text_Dim dim = get_text_dimensions(font, upscaled_font_size, str);

	// FIXME: There are no bounds checks done here

	Result<Image> upscaled = make_image(dim.w, dim.h, 1, 0x00000000);
	SCOPE_EXIT(free(upscaled.value.data));
	if(is_error(upscaled)) return;// MAKE_ERROR_RESULT(upscaled.error);
	render_text_to_image(font, str, upscaled_font_size, &upscaled.value, 0, 0, {.c=shadow_color});

	Result<Image> downscaled;
	if((upscaled.value.w / upscale_factor) < max_width) {
		downscaled = make_image(dim.w / upscale_factor, dim.h / upscale_factor, 1, 0x00000000);
	} else {
		// The text is too wide, it will need to be scaled down more than upscale_factor
		f32 ratio = ((f32)max_width / (f32)dim.w);
		int height = ceil(((f32)dim.h * ratio));
		downscaled = make_image(max_width, height, 1, 0x00000000);
	}
	SCOPE_EXIT(free(downscaled.value.data));
	if(is_error(downscaled)) return;

	stbir_resize_uint8_srgb((const u8*)upscaled.value.data, upscaled.value.w, upscaled.value.h, 0,
					        (u8*)downscaled.value.data, downscaled.value.w, downscaled.value.h, 0, STBIR_1CHANNEL);

	const int xpos = (BANNER_IMAGE_WIDTH / 2) - (downscaled.value.w / 2);
	ypos -= (downscaled.value.h / 2);

	blit_A8_to_RGBA(&downscaled.value, downscaled.value.w, {.c=shadow_color}, out, xpos-1, ypos-1); // left top
	blit_A8_to_RGBA(&downscaled.value, downscaled.value.w, {.c=shadow_color}, out, xpos-1, ypos+1); // left bottom
	blit_A8_to_RGBA(&downscaled.value, downscaled.value.w, {.c=shadow_color}, out, xpos-1, ypos);   // left centre
	blit_A8_to_RGBA(&downscaled.value, downscaled.value.w, {.c=shadow_color}, out, xpos+1, ypos);   // right centre
	blit_A8_to_RGBA(&downscaled.value, downscaled.value.w, {.c=shadow_color}, out, xpos,   ypos-1); // center top
	blit_A8_to_RGBA(&downscaled.value, downscaled.value.w, {.c=shadow_color}, out, xpos,   ypos+1); // center bottom
	blit_A8_to_RGBA(&downscaled.value, downscaled.value.w, {.c=shadow_color}, out, xpos+1, ypos-1); // right top
	blit_A8_to_RGBA(&downscaled.value, downscaled.value.w, {.c=shadow_color}, out, xpos+1, ypos+1); // right bottom

	blit_A8_to_RGBA(&downscaled.value, downscaled.value.w, {.c=text_color}, out, xpos, ypos);
}

enum DRAFT_TYPE {
	DRAFT_TYPE_NOT_APPLICABLE,

	DRAFT_TYPE_DEVOTION_GIANT,
	DRAFT_TYPE_DEVOTION_SPHINX,
	DRAFT_TYPE_DEVOTION_DEMON,
	DRAFT_TYPE_DEVOTION_DRAGON,
	DRAFT_TYPE_DEVOTION_TITAN,
	DRAFT_TYPE_DEVOTION_GOD,

	DRAFT_TYPE_COMMUNITY_CHOICE,

	DRAFT_TYPE_HERO_20,
	DRAFT_TYPE_HERO_40,
	DRAFT_TYPE_HERO_60,

	DRAFT_TYPE_COUNT
};

static constexpr const std::string_view to_string(const DRAFT_TYPE dt) {
	switch(dt) {
		case DRAFT_TYPE_NOT_APPLICABLE: return {""};

		case DRAFT_TYPE_DEVOTION_GIANT:  return {"Devotion - Giant"};
		case DRAFT_TYPE_DEVOTION_SPHINX: return {"Devotion - Sphinx"};
		case DRAFT_TYPE_DEVOTION_DEMON:  return {"Devotion - Demon"};
		case DRAFT_TYPE_DEVOTION_DRAGON: return {"Devotion - Dragon"};
		case DRAFT_TYPE_DEVOTION_TITAN:  return {"Devotion - Titan"};
		case DRAFT_TYPE_DEVOTION_GOD:    return {"Devotion - God"};

		case DRAFT_TYPE_COMMUNITY_CHOICE: return {"Community Choice"};

		case DRAFT_TYPE_HERO_20: return {"Hero - 20"};
		case DRAFT_TYPE_HERO_40: return {"Hero - 40"};
		case DRAFT_TYPE_HERO_60: return {"Hero - 60"};

		default: break;
	}

	return {""};
}

#if 0
struct Draft_Type {
	DRAFT_TYPE value;
	const char* name;
};
#endif


struct Icon {
	DRAFT_TYPE type;
	const char* file; // Relative path
	int x, y;         // Position on the screen
};

static const Icon g_icons[] = {
	{DRAFT_TYPE_DEVOTION_GIANT,   "gfx/banner/icons/devotion_giant.png",   100, 100},
	{DRAFT_TYPE_DEVOTION_SPHINX,  "gfx/banner/icons/devotion_sphinx.png",  100, 100},
	{DRAFT_TYPE_DEVOTION_DEMON,   "gfx/banner/icons/devotion_demon.png",   100, 100},
	{DRAFT_TYPE_DEVOTION_DRAGON,  "gfx/banner/icons/devotion_dragon.png",  100, 100},
	{DRAFT_TYPE_DEVOTION_TITAN,   "gfx/banner/icons/devotion_titan.png",   100, 100},
	{DRAFT_TYPE_DEVOTION_GOD,     "gfx/banner/icons/devotion_god.png",     100, 100},
	{DRAFT_TYPE_COMMUNITY_CHOICE, "gfx/banner/icons/community_choice.png", 258, 104},
	{DRAFT_TYPE_HERO_20,          "gfx/banner/icons/hero_20.png",          100, 100},
	{DRAFT_TYPE_HERO_40,          "gfx/banner/icons/hero_40.png",          100, 100},
	{DRAFT_TYPE_HERO_60,          "gfx/banner/icons/hero_60.png",          100, 100},
};

static const size_t ICON_COUNT = sizeof(g_icons) / sizeof(Icon);

const Icon* get_icon(DRAFT_TYPE type) {
	for(size_t i = 0; i < ICON_COUNT; ++i) {
		if(g_icons[i].type == type) return &g_icons[i];
	}
	return NULL;
}

// NOTE: Don't forget to free the returned buffer!
static u8* file_slurp(const char* path, size_t* size) {
	u8* file_contents = NULL;
	FILE* f = fopen(path, "rb");
	if(f != NULL) {
		SCOPE_EXIT(fclose(f));
		struct stat s;
		int result = stat(path, &s);
		if(result != -1) {
			*size = s.st_size;
			file_contents = (u8*) malloc(*size);
			if(file_contents != NULL) {
				size_t got = fread(file_contents, 1, *size, f);
				if(got != *size) {
					free(file_contents);
					file_contents = NULL;
				}
			}
		}
	}
	return file_contents;
}

// Fill this structure and pass it to the render function.
// TODO: Themes? We could have themes for different times of the year, or types of draft etc.
struct Banner_Opts {
	DRAFT_TYPE draft_type;
	u32 league_color;
	std::string datetime;
	std::string title;
	std::string subtitle;
	std::vector<std::string> images;
};

static stbtt_fontinfo g_banner_font;
static bool g_banner_font_loaded = false;
static const char* g_banner_font_file = "gfx/banner/SourceSansPro-Black.otf";

const Result<std::string> render_banner(Banner_Opts* opts) {
	if(g_banner_font_loaded == false) {
		size_t size = 0;
		u8* buffer = file_slurp(g_banner_font_file, &size); // NOTE: Intentionally never freed if the file is successfully loaded.
		int result = stbtt_InitFont(&g_banner_font, buffer, stbtt_GetFontOffsetForIndex(buffer, 0));
		if(result == 0) {
			free(buffer);
			return MAKE_ERROR_RESULT(ERROR_LOAD_FONT_FAILED);
		}
		g_banner_font_loaded = true;
	}

	static const char* BANNER_FRAME_TOP_FILE    = "gfx/banner/frame_top.png";
	static const char* BANNER_FRAME_SIDE_FILE   = "gfx/banner/frame_side.png"; // left and right
	static const char* BANNER_FRAME_BOTTOM_FILE = "gfx/banner/frame_bottom.png";
	static const char* BANNER_GRADIENT_FILE     = "gfx/banner/gradient.png";
	static const char* BANNER_SUBTITLE_FILE 	= "gfx/banner/subtitle.png";

	static const int BANNER_DATETIME_YPOS      = 10; //15;
	static const int BANNER_DATETIME_FONT_SIZE = 35; //40;

	static const int BANNER_TITLE_WIDTH_MAX = 730; //800; // Scale down the format string if longer than this.
	static const int BANNER_TITLE_FONT_SIZE = 40; //44;
	static const int BANNER_TITLE_TEXT_YPOS = 82; // 88;

	static const int BANNER_SUBTITLE_FRAME_YPOS = 108;
	static const int BANNER_SUBTITLE_WIDTH_MAX  = 660;
	static const int BANNER_SUBTITLE_YPOS       = 125;
	static const int BANNER_SUBTITLE_FONT_SIZE  = 24;

	static const int BANNER_PACK_DIVIDER_YPOS = 108; // Starting row to draw the divider between packs

	Result<Image> banner = make_image(BANNER_IMAGE_WIDTH, BANNER_IMAGE_HEIGHT, 4, 0xFF000000);
	SCOPE_EXIT(free(banner.value.data));
	if(is_error(banner)) {
		return MAKE_ERROR_RESULT(banner.error);
	}

	// blit the background image(s)
	if(opts->images.size() == 1) {
		// A single piece of key art is to be used.
		Result<Image> scaled = make_image(KEY_ART_WIDTH, KEY_ART_HEIGHT, 3, 0x00000000);
		SCOPE_EXIT(free(scaled.value.data));
		if(is_error(scaled)) return MAKE_ERROR_RESULT(scaled.error);

		Result<Image> img = load_image(opts->images[0].c_str(), 3);
		SCOPE_EXIT(stbi_image_free(img.value.data));
		if(is_error(img)) return MAKE_ERROR_RESULT(img.error);

		stbir_resize_uint8_srgb((const u8*)img.value.data, img.value.w, img.value.h, 0,
				(u8*)scaled.value.data, scaled.value.w, scaled.value.h, 0, STBIR_RGB);
		blit_RGB_to_RGBA(&scaled.value, &banner.value, 0, 0);
	} else
	if(opts->images.size() == 3) {
		// Three pack images given.
		Result<Image> scaled = make_image(PACK_IMAGE_WIDTH, PACK_IMAGE_HEIGHT, 3, 0x00000000);
		SCOPE_EXIT(free(scaled.value.data));
		if(is_error(scaled)) return MAKE_ERROR_RESULT(scaled.error);

		for(size_t f = 0; f < opts->images.size(); ++f) {
			Result<Image> img = load_image(opts->images[f].c_str(), 3);
			SCOPE_EXIT(stbi_image_free(img.value.data));
			if(is_error(img)) return MAKE_ERROR_RESULT(img.error);

			stbir_resize_uint8_srgb((const u8*)img.value.data, img.value.w, img.value.h, 0,
					(u8*)scaled.value.data, scaled.value.w, scaled.value.h, 0, STBIR_RGB);
			blit_RGB_to_RGBA(&scaled.value, &banner.value, f * PACK_IMAGE_WIDTH, BANNER_IMAGE_HEIGHT - scaled.value.h);
		}

		// Draw a thin line to separate each pack.
		// FIXME: Replace this with a draw_rect function to avoid unnecessary heap allocations for each line.
		Result<Image> line = make_image(3, BANNER_IMAGE_HEIGHT-BANNER_PACK_DIVIDER_YPOS, 4, 0xFF000000);
		SCOPE_EXIT(free(line.value.data));
		if(is_error(line)) return MAKE_ERROR_RESULT(line.error);

		for(int i = 1; i < 3; ++i) {
			blit_RGBA_to_RGBA(&line.value, &banner.value, (i * PACK_IMAGE_WIDTH)-1, BANNER_PACK_DIVIDER_YPOS);
		}
	} else {
		return MAKE_ERROR_RESULT(ERROR_INVALID_PACK_COUNT, opts->images.size());
	}

	// Blit the gradient. TODO: This could be done in code instead of using an image...
	{
		Result<Image> grad = load_image(BANNER_GRADIENT_FILE, 1);
		SCOPE_EXIT(stbi_image_free(grad.value.data));
		if(is_error(grad)) return MAKE_ERROR_RESULT(grad.error);

		blit_A8_to_RGBA(&grad.value, grad.value.w, {.c=0xFF000000}, &banner.value, 0, 0);
	}

	// Blit the title box frames and color them
	{
		{
			// Top
			Result<Image> frame = load_image(BANNER_FRAME_TOP_FILE, 1);
			SCOPE_EXIT(stbi_image_free(frame.value.data));
			if(is_error(frame)) return MAKE_ERROR_RESULT(frame.error);
			blit_A8_to_RGBA(&frame.value, frame.value.w, {.c=opts->league_color}, &banner.value, 9, 57);
		}
		{
			// Bottom
			Result<Image> frame = load_image(BANNER_FRAME_BOTTOM_FILE, 1);
			SCOPE_EXIT(stbi_image_free(frame.value.data));
			if(is_error(frame)) return MAKE_ERROR_RESULT(frame.error);
			blit_A8_to_RGBA(&frame.value, frame.value.w, {.c=opts->league_color}, &banner.value, 9, 530);
		}
		{
			// Left & right
			Result<Image> frame = load_image(BANNER_FRAME_SIDE_FILE, 1);
			SCOPE_EXIT(stbi_image_free(frame.value.data));
			if(is_error(frame)) return MAKE_ERROR_RESULT(frame.error);

			blit_A8_to_RGBA(&frame.value, frame.value.w, {.c=opts->league_color}, &banner.value, 9, 108);
			blit_A8_to_RGBA(&frame.value, frame.value.w, {.c=opts->league_color}, &banner.value, 807, 108);
		}
	}

	// Blit the date/time text
	{
		Text_Dim dim = get_text_dimensions(&g_banner_font, BANNER_DATETIME_FONT_SIZE, (const u8*)opts->datetime.c_str());
		Result<Image> img = make_image(dim.w, dim.h, 1, 0x00000000);
		SCOPE_EXIT(free(img.value.data));
		if(is_error(img)) return MAKE_ERROR_RESULT(img.error);

		render_text_to_image(&g_banner_font, (const u8*)opts->datetime.c_str(), BANNER_DATETIME_FONT_SIZE, &img.value, 0, 0, {.c=0xFFFFFFFF});
		if(img.value.w < (BANNER_IMAGE_WIDTH - 10)) {
			blit_A8_to_RGBA(&img.value, img.value.w, {.c=0xFFFFFFFF}, &banner.value, (BANNER_IMAGE_WIDTH/2)-(img.value.w/2), BANNER_DATETIME_YPOS);
		} else {
			// Scale it to fit.
			f32 ratio = ((f32)(BANNER_IMAGE_WIDTH-10) / dim.w);
			int height = ceil(((f32)dim.h * ratio));

			Result<Image> scaled = make_image((BANNER_IMAGE_WIDTH-10), height, 1, 0x00000000);
			SCOPE_EXIT(free(scaled.value.data));
			if(is_error(scaled)) return MAKE_ERROR_RESULT(scaled.error);

			stbir_resize_uint8_srgb((const u8*)img.value.data, img.value.w, img.value.h, 0,
								    (u8*)scaled.value.data, scaled.value.w, scaled.value.h, 0, STBIR_1CHANNEL);
			blit_A8_to_RGBA(&scaled.value, scaled.value.w, {.c=0xFFFFFFFF}, &banner.value, (BANNER_IMAGE_WIDTH/2)-(scaled.value.w/2), BANNER_DATETIME_YPOS);
		}
	}

	// Blit the title text
	draw_shadowed_text(&g_banner_font, BANNER_TITLE_FONT_SIZE, BANNER_TITLE_WIDTH_MAX, (const u8*)opts->title.c_str(), 0xFF000000, 0xFFFFFFFF, &banner.value, BANNER_TITLE_TEXT_YPOS);

	if(opts->subtitle.length() > 0) {
		// Blit the subtitle box
		Result<Image> sub = load_image(BANNER_SUBTITLE_FILE, 1);
		SCOPE_EXIT(stbi_image_free(sub.value.data));
		if(is_error(sub)) return MAKE_ERROR_RESULT(sub.error);

		blit_A8_to_RGBA_no_alpha(&sub.value, sub.value.w, {.c=opts->league_color}, &banner.value, (BANNER_IMAGE_WIDTH/2)-(sub.value.w/2), BANNER_SUBTITLE_FRAME_YPOS);

		// Blit the devotion/hero/etc. icon
		const Icon* icon = get_icon(opts->draft_type);
		if(icon != NULL) {
			Result<Image> icon_image = load_image(icon->file, 4);
			SCOPE_EXIT(stbi_image_free(icon_image.value.data));
			if(is_error(icon_image)) return MAKE_ERROR_RESULT(icon_image.error);

			blit_RGBA_to_RGBA(&icon_image.value, &banner.value, icon->x, icon->y);
		}

		// Draw the subtitle text
		draw_shadowed_text(&g_banner_font, BANNER_SUBTITLE_FONT_SIZE, BANNER_SUBTITLE_WIDTH_MAX, (const u8*)opts->subtitle.c_str(), 0xFF000000, 0xFF04CDFF, &banner.value, BANNER_SUBTITLE_YPOS);
	}

	// Save the file
	// TODO: Only need to save RGB, this saves having to clear the alpha channel, but does stbii_write support this?
	stbi_write_png_compression_level = 9; // TODO: What's the highest stbi supports?
	image_max_alpha(&banner.value);
	std::string file_path = fmt::format("/tmp/EventBot_Banner_{}.png", random_string(16));
	if(stbi_write_png(file_path.c_str(), banner.value.w, banner.value.h, 4, (u8*)banner.value.data, banner.value.w*4) == 0) {
		return MAKE_ERROR_RESULT(ERROR_FAILED_TO_SAVE_BANNER);
	}

	return {file_path};
}

// Users on Discord have two names per guild: Their global name or an optional per-guild nickname.
static std::string get_members_preferred_name(const u64 guild_id, const u64 member_id) {
	std::string preferred_name;
	const dpp::guild_member member = dpp::find_guild_member(guild_id, member_id); // FIXME: This can throw!
	const std::string nickname = member.get_nickname();
	if(nickname.length() > 0) {
		preferred_name = nickname;
	} else {
		const dpp::user* user = dpp::find_user(member_id);
		if(user != nullptr) {
			if(user->global_name.length() > 0) {
				preferred_name = user->global_name;
			} else {
				preferred_name = user->username;
			}
		} else {
			// TODO: Now what? Return an error message?
			log(LOG_LEVEL_ERROR, "Failed to find preferred name for member %lu.", member_id);
		}
	}
	return preferred_name;
}


static void delete_draft_posts(dpp::cluster& bot, const u64 guild_id, const std::string& draft_code) {
	auto ids = database_get_draft_post_ids(guild_id, draft_code);
	if(!is_error(ids)) {
		// Delete the details post.
		bot.message_delete(ids.value.details, ids.value.channel, [ids](const dpp::confirmation_callback_t& callback) {
			if(!callback.is_error()) {
				log(LOG_LEVEL_DEBUG, "Deleted message %lu from channel %lu.", ids.value.details, ids.value.channel);
			} else {
				log(LOG_LEVEL_ERROR, "Failed to delete message %lu from channel %lu.", ids.value.details, ids.value.channel);
			}
		});

		// Delete the sign ups post.
		bot.message_delete(ids.value.signups, ids.value.channel, [ids](const dpp::confirmation_callback_t& callback) {
			if(!callback.is_error()) {
				log(LOG_LEVEL_DEBUG, "Deleted message %lu from channel %lu.", ids.value.signups, ids.value.channel);
			} else {
				log(LOG_LEVEL_ERROR, "Failed to delete message %lu from channel %lu.", ids.value.signups, ids.value.channel);
			}
		});
	} else {
		log(LOG_LEVEL_ERROR, ids.errstr);
	}
}

static void delete_temp_roles(dpp::cluster& bot, const u64 guild_id, const std::string& draft_code) {
	// Get the temporary roles created for this draft
	auto roles = database_get_temp_roles(guild_id, draft_code.c_str());
	if(is_error(roles)) {
		log(LOG_LEVEL_ERROR, roles.errstr);
		// TODO: Now what?
	}

	// Delete the roles from Discord
	for(auto role : roles.value) {
		bot.role_delete(guild_id, role, [role](const dpp::confirmation_callback_t& callback) {
			if(callback.is_error()) {
				log(LOG_LEVEL_ERROR, "Failed to delete role %lu", role);
				// TODO: Send message to Discord?
			}
		});
	}

	// Delete the roles from the database
	database_del_temp_roles(guild_id, draft_code.c_str());
}


struct {
	const char* header;
	int mask;
} g_draft_sign_up_columns[3] = {
	{
		":white_check_mark:Playing [{:d}]",
		SIGNUP_STATUS_PLAYING
	},
	{
		":grey_question:Tentative [{:d}]",
		SIGNUP_STATUS_TENTATIVE
	},
	{
		":superhero:Minutemage [{:d}]",
		SIGNUP_STATUS_MINUTEMAGE
	}
};

static void add_sign_up_buttons_to_message(dpp::message& message, const std::shared_ptr<Draft_Event> draft_event) {
	// Remove any existing buttons.
	message.components.clear();

	bool playing_locked = false;
	bool tentative_locked = false;
	bool minutemage_locked = true;

	time_t now = time(NULL);
	time_t draft_start = unpack_and_make_timestamp(draft_event->time, draft_event->time_zone);

	// FIXME: This works, but it should probably check the draft status instead, right?
	if(draft_start - now <= SECONDS_BEFORE_DRAFT_TO_SEND_REMINDER) {
		minutemage_locked = false;
	}

	if(draft_start - now <= SECONDS_BEFORE_DRAFT_TO_PING_TENTATIVES) {
		tentative_locked = true;
	}

	if(now >= draft_start) {
		// Lock everything
		minutemage_locked = true;
		tentative_locked = true;
		playing_locked = true;
	}

	dpp::component button1;
	button1.set_type(dpp::cot_button);
	button1.set_label("Competitive");
	button1.set_style(dpp::cos_success);
	if(playing_locked == false) {
		button1.set_emoji("✅");
	} else {
		button1.set_emoji("🔒");
		button1.set_disabled(true);
	}
	button1.set_id(draft_event->draft_code + std::string("_competitive"));

	dpp::component button2;
	button2.set_type(dpp::cot_button);
	button2.set_label("Casual");
	button2.set_style(dpp::cos_success);
	if(playing_locked == false) {
		button2.set_emoji("✅");
	} else {
		button2.set_emoji("🔒");
		button2.set_disabled(true);
	}
	button2.set_id(draft_event->draft_code + std::string("_casual"));

	dpp::component button3;
	button3.set_type(dpp::cot_button);
	button3.set_label("Flexible");
	button3.set_style(dpp::cos_success);
	if(playing_locked == false) {
		button3.set_emoji("✅");
	} else {
		button3.set_emoji("🔒");
		button3.set_disabled(true);
	}
	button3.set_id(draft_event->draft_code + std::string("_flexible"));

	dpp::component button_row_one;
	button_row_one.set_type(dpp::cot_action_row);
	button_row_one.add_component(button1);
	button_row_one.add_component(button2);
	button_row_one.add_component(button3);
	message.add_component(button_row_one);

	dpp::component button4;
	button4.set_type(dpp::cot_button);
	button4.set_label("Tentative");
	button4.set_style(dpp::cos_primary);
	if(tentative_locked == false) {
		button4.set_emoji("❔");
	} else {
		button4.set_emoji("🔒");
		button4.set_disabled(true);
	}
	button4.set_id(draft_event->draft_code + std::string("_tentative"));

	dpp::component button5;
	button5.set_type(dpp::cot_button);
	button5.set_label("Minutemage");
	button5.set_style(dpp::cos_primary);
	if(minutemage_locked == false) {
		button5.set_emoji("🦸");
	} else {
		button5.set_emoji("🔒");
		button5.set_disabled(true);
	}
	button5.set_id(draft_event->draft_code + std::string("_minutemage"));

	dpp::component button6;
	button6.set_type(dpp::cot_button);
	button6.set_label("Decline");
	button6.set_style(dpp::cos_danger);
	if(playing_locked == false) {
		button6.set_emoji("⛔");
	} else {
		button6.set_emoji("🔒");
		button6.set_disabled(true);
	}
	button6.set_id(draft_event->draft_code + std::string("_decline"));

	dpp::component button_row_two;
	button_row_two.set_type(dpp::cot_action_row);
	button_row_two.add_component(button4);
	button_row_two.add_component(button5);
	button_row_two.add_component(button6);
	message.add_component(button_row_two);
}

static void add_sign_up_embed_to_message(const u64 guild_id, dpp::message& message, std::shared_ptr<Draft_Event> draft_event) {
	dpp::embed embed;
	if(message.embeds.size() > 0) {
		// Copy the existing embed so we don't loose the attached image.
		embed = message.embeds[0];
	}

	message.embeds.clear();

	embed.set_color(draft_event->color);

	int year, month, day, hour, minute;
	unpack_time(draft_event->time, &year, &month, &day, &hour, &minute);
	time_t start_timestamp = make_timestamp(draft_event->time_zone, year, month, day, hour, minute);

	std::string embed_title = fmt::format(":hourglass_flowing_sand: Draft starts: <t:{}:R>", start_timestamp);
	embed.set_title(embed_title);

	if(draft_event->draftmancer_draft == false) {
		embed.set_description(fmt::format("The draft and games will take place on **{}**.", draft_event->xmage_server));
	} else {
		embed.set_description(fmt::format("The draft will take place on **https://www.draftmancer.com**, games on **{}**.", draft_event->xmage_server));
	}

	const auto sign_ups = database_get_draft_sign_ups(guild_id, draft_event->draft_code);
	if(is_error(sign_ups)) {
		log(LOG_LEVEL_ERROR, sign_ups.errstr);
		// TODO: Now what?
	}

	// Create the three embed fields (Playing, Tentative, Minutemage) and the list of players for each.
	embed.fields.clear();
	for(int i = 0; i < 3; ++i) {
		std::string names;
		names.reserve(512);
		int count = 0;
		for(const auto& sign_up : sign_ups.value) {
			if(sign_up.status & g_draft_sign_up_columns[i].mask) {
				if(names.length() > 0) {
					names += "\n";
				}
				names += "> ";
				if((sign_up.status & SIGNUP_STATUS_REMOVED) == SIGNUP_STATUS_REMOVED) names += "~~";
				if(i == 0) { // Playing column
					int pod = 0;
					if(sign_up.status == SIGNUP_STATUS_COMPETITIVE) pod = 1;
					else if(sign_up.status == SIGNUP_STATUS_CASUAL) pod = 2;
					if(pod == 0) {
						names += fmt::format("[?] ");
					} else {
						names += fmt::format("[{}] ", pod);
					}
				}
				names += sign_up.preferred_name;
				if((sign_up.status & SIGNUP_STATUS_REMOVED) == SIGNUP_STATUS_REMOVED) {
					names += "~~";
				} else {
					++count;
				}
			}
		}

		embed.add_field(fmt::format(fmt::runtime(g_draft_sign_up_columns[i].header), count), names, true);
	}

	// Attach banner if it hasn't already been attached, or if the file on disk is newer.
	struct stat file_attributes;
	stat(draft_event->banner_file, &file_attributes);
	if(embed.image.has_value() == 0 || draft_event->banner_timestamp < file_attributes.st_mtime) {
		log(LOG_LEVEL_DEBUG, "Updating banner image");
		message.add_file("banner.png", dpp::utility::read_file(draft_event->banner_file));
		embed.set_image("attachment://banner.png");
		(void)database_update_banner_timestamp(guild_id, draft_event->draft_code, file_attributes.st_mtime);
	}

	time_t now = time(NULL);
	time_t draft_start = unpack_and_make_timestamp(draft_event->time, draft_event->time_zone);
	if(now >= draft_start) {
		dpp::embed_footer footer;
		footer.set_text("🔒 Sign up for this draft is now locked. 🔒");
		//footer.set_icon("https://em-content.zobj.net/source/skype/289/locked_1f512.png");
		embed.set_footer(footer);
	}

	message.add_embed(embed);
}

static void redraw_details(dpp::cluster& bot, const u64 guild_id, const u64 message_id, const u64 channel_id, std::shared_ptr<Draft_Event> draft) {
	bot.message_get(message_id, channel_id, [&bot, guild_id, message_id, channel_id, draft](const dpp::confirmation_callback_t& callback) {
		if(!callback.is_error()) {
			dpp::message message = std::get<dpp::message>(callback.value);

			char description[1024]; // FIXME: This can overflow.
			expand_format_string(draft->format, strlen(draft->format), description, 1024);

			int year, month, day, hour, minute;
			unpack_time(draft->time, &year, &month, &day, &hour, &minute);
			const time_t start_timestamp = make_timestamp(draft->time_zone, year, month, day, hour, minute);
			Draft_Duration duration = {(int)draft->duration, (int)(60.0f * (draft->duration - (int)draft->duration))};
			const time_t end_timestamp = start_timestamp + ((60*60*duration.hours) + (60*duration.minutes));

			std::string text; // Contains the entire text of the details post.
			text += "~~　　　　　　　　　　　　　　　　　　~~\n"; // NOTE: These are ideographic spaces.
			text += fmt::format("# {} The sign-up for the {} **{}** draft ({}: {}) is now up!\n\nThis draft will take place on **<t:{}:F> - <t:{}:t>**", draft->pings, draft->league_name, description, draft->draft_code, draft->format, start_timestamp, end_timestamp);

			// Blurbs
			for(size_t i = 0; i < BLURB_COUNT; ++i) {
				if(strlen(&draft->blurbs[i][0]) > 0) {
					text += fmt::format("\n\n{}", &draft->blurbs[i][0]);
				}
			}

			// Set list
			if(strlen(draft->set_list) > 0) {
				char buffer[1024]; // FIXME: Magic number, could also overflow
				expand_set_list(draft->set_list, strlen(draft->set_list), buffer, 1024);
				text += fmt::format("\n{}", buffer); // NOTE: This single newline is intentional.
			}

			// Card list
			if(strlen(draft->card_list_url) > 0) {
				text += fmt::format("\n\n:arrow_right: View the card list here: {}", draft->card_list_url);
			}

			// Draft Guide
			if(strlen(draft->draft_guide_url) > 0) {
				text += fmt::format("\n\n:scroll: View the draft guide here: {}", draft->draft_guide_url);
			}

			message.set_content(text);

			bot.message_edit(message, [&bot, message_id, channel_id](const dpp::confirmation_callback_t& callback) {
				if(!callback.is_error()) {
				} else {
					log(LOG_LEVEL_ERROR, "message_edit(%lu, %lu) failed: %s", message_id, channel_id, callback.get_error().message.c_str());
				}
			});
		} else {
			log(LOG_LEVEL_ERROR, "message_get(%lu, %lu) failed: %s", message_id, channel_id, callback.get_error().message.c_str());
		}
	});
}

// Called whenever a name is added to the sign up list or when the state of the buttons needs to be updated.
static void redraw_signup(dpp::cluster& bot, const u64 guild_id, const u64 message_id, const u64 channel_id, std::shared_ptr<Draft_Event> draft) {
	bot.message_get(message_id, channel_id, [&bot, guild_id, message_id, channel_id, draft](const dpp::confirmation_callback_t& callback) {
		if(!callback.is_error()) {
			dpp::message message = std::get<dpp::message>(callback.value);

			message.set_content("");
			add_sign_up_embed_to_message(guild_id, message, draft);
			add_sign_up_buttons_to_message(message, draft);

			bot.message_edit(message, [&bot, message_id, channel_id](const dpp::confirmation_callback_t& callback) {
				if(!callback.is_error()) {
				} else {
					log(LOG_LEVEL_ERROR, "message_edit(%lu, %lu) failed: %s", message_id, channel_id, callback.get_error().message.c_str());
				}
			});
		} else {
			log(LOG_LEVEL_ERROR, "message_get(%lu, %lu) failed: %s", message_id, channel_id, callback.get_error().message.c_str());
		}
	});
}


static void edit_draft(dpp::cluster& bot, const u64 guild_id, const std::shared_ptr<Draft_Event> draft) {
	redraw_details(bot, guild_id, draft->details_id, draft->signup_channel_id, draft);

	redraw_signup(bot, guild_id, draft->signups_id, draft->signup_channel_id, draft);

	if(draft->reminder_id != 0) {
		redraw_signup(bot, guild_id, draft->reminder_id, draft->reminder_channel_id, draft);
	}
}

static void post_draft(dpp::cluster& bot, const u64 guild_id, const std::shared_ptr<Draft_Event> draft) {
	// First create details message.
	dpp::message details;
	details.set_type(dpp::message_type::mt_default);
	details.set_guild_id(guild_id);
	details.set_channel_id(draft->signup_channel_id);
	details.set_content(":hourglass:");
	details.set_allowed_mentions(true, true, true, false, {}, {});
	bot.message_create(details, [&bot, guild_id, draft](const dpp::confirmation_callback_t& callback) {
		if(!callback.is_error()) {
			const dpp::message& message = std::get<dpp::message>(callback.value);
			(void)database_set_details_message_id(guild_id, draft->draft_code, message.id);

			draft->details_id = message.id;

			// The sign up message.
			dpp::message sign_up;
			sign_up.set_type(dpp::message_type::mt_default);
			sign_up.set_guild_id(guild_id);
			sign_up.set_channel_id(draft->signup_channel_id);
			sign_up.set_content(":hourglass:");
			bot.message_create(sign_up, [&bot, guild_id, draft](const dpp::confirmation_callback_t& callback) {
				if(!callback.is_error()) {
					const dpp::message& message = std::get<dpp::message>(callback.value);
					(void)database_set_signups_message_id(guild_id, draft->draft_code, message.id);

					draft->signups_id = message.id;

					draft->status = DRAFT_STATUS_POSTED;
					(void)database_set_draft_status(guild_id, draft->draft_code, DRAFT_STATUS_POSTED);

					edit_draft(bot, guild_id, draft);
				} else {
					log(LOG_LEVEL_ERROR, "message_create() failed: %s", callback.get_error().message.c_str());
				}
			});
		} else {
			log(LOG_LEVEL_ERROR, "message_create() failed: %s", callback.get_error().message.c_str());
		}
	});
}


static void post_pre_draft_reminder(dpp::cluster& bot, const u64 guild_id, const char* draft_code) {
	auto draft_event = database_get_event(guild_id, draft_code);
	if(is_error(draft_event)) {
		log(LOG_LEVEL_ERROR, draft_event.errstr);
		return;
	};

	const auto sign_ups = database_get_draft_sign_ups(guild_id, draft_code);
	if(is_error(sign_ups)) {
		// TODO: Now what?
	}

	dpp::message message;
	message.set_type(dpp::message_type::mt_default);
	message.set_guild_id(guild_id);
	message.set_channel_id(draft_event.value->reminder_channel_id);
	message.set_allowed_mentions(true, true, true, true, {}, {});

	std::string text;
	text.reserve(512);
	for(const auto& sign_up : sign_ups.value) {
		if(sign_up.status == SIGNUP_STATUS_DECLINE) continue;
		text += fmt::format("<@{}> ", sign_up.member_id);
	}

	text += "\n\n";
	text += fmt::format("# :bell: This is the pre-draft reminder for {}: {} :bell:\n\n", draft_event.value->draft_code, draft_event.value->format);
	text += "Please confirm your status on the sign up sheet below.\n\n";
	text += "Minutemage sign ups are now open. If needed, a minutemage will be selected at random to fill an empty seat.\n\n";
	text += fmt::format("If playing, check your XMage install is up-to-date by starting the XMage launcher, updating if necessary, and connecting to {}.", draft_event.value->xmage_server);

	const auto xmage_version = database_get_xmage_version();
	if(!is_error(xmage_version)) {
		u64 timestamp = xmage_version.value.timestamp + SERVER_TIME_ZONE_OFFSET;
		// Note: The leading space is intentional as this joins with the previous line.
		text += fmt::format(" The latest XMage release is {}, released <t:{}:R>.", xmage_version.value.version, timestamp);
	}

	message.set_content(text);

	bot.message_create(message, [&bot, guild_id, draft_event = draft_event.value](const dpp::confirmation_callback_t& callback) {
		if(!callback.is_error()) {
			const dpp::message& message = std::get<dpp::message>(callback.value);
			log(LOG_LEVEL_DEBUG, "Created reminder post: %lu", (u64)message.id);

			// Create the sign ups part.
			dpp::message signup;
			signup.set_type(dpp::message_type::mt_default);
			signup.set_guild_id(guild_id);
			signup.set_channel_id(draft_event->reminder_channel_id);
			add_sign_up_embed_to_message(guild_id, signup, draft_event);
			add_sign_up_buttons_to_message(signup, draft_event);
			bot.message_create(signup, [&bot, guild_id, draft_event](const dpp::confirmation_callback_t& callback) {
				if(!callback.is_error()) {
					const dpp::message& message = std::get<dpp::message>(callback.value);
					log(LOG_LEVEL_DEBUG, "Created reminder sign up post: %lu", (u64)message.id);
					(void)database_set_reminder_message_id(guild_id, draft_event->draft_code, message.id);
					(void)database_set_draft_status(GUILD_ID, draft_event->draft_code, DRAFT_STATUS_REMINDER_SENT);
					// Redraw the #-pre-register sign up so the minutemage button gets unlocked.
					redraw_signup(bot, guild_id, draft_event->signups_id, draft_event->signup_channel_id, draft_event);
				}
			});

		} else {
			log(LOG_LEVEL_DEBUG, callback.get_error().message.c_str());
		}
	});
}

static void ping_tentatives(dpp::cluster& bot, const u64 guild_id, const char* draft_code) {
	const auto draft_event = database_get_event(guild_id, draft_code);
	if(is_error(draft_event)) {
		log(LOG_LEVEL_ERROR, "database_get_event(%lu, %s) failed.", guild_id, draft_code);
		return;
	}

	const auto sign_ups = database_get_draft_sign_ups(guild_id, draft_code);
	if(is_error(sign_ups)) {
		log(LOG_LEVEL_ERROR, "database_get_draft_sign_ups(%lu, %s) failed.", guild_id, draft_code);
		return;
	}

	int tentative_count = 0;
	for(const auto& sign_up : sign_ups.value) {
		if(sign_up.status == SIGNUP_STATUS_TENTATIVE) tentative_count++;
	}

	if(tentative_count > 0) {
		dpp::message message;
		message.set_type(dpp::message_type::mt_default);
		message.set_guild_id(guild_id);
		message.set_channel_id(draft_event.value->reminder_channel_id);
		message.set_allowed_mentions(true, true, true, true, {}, {});

		std::string text;
		text.reserve(512);

		// Ping everyone who is still listed as tentative.
		for(const auto& sign_up : sign_ups.value) {
			if(sign_up.status == SIGNUP_STATUS_TENTATIVE) {
				text += fmt::format("<@{}> ", sign_up.member_id);
			}
		}

		text += "\n\n";
		text += fmt::format("**:warning: Tentatives, this is your 10 minute reminder for {}: {} :warning:**\n", draft_event.value->draft_code, draft_event.value->format);
		text += fmt::format("Please confirm whether you are joining the imminent draft by clicking your desired pod role or Decline if you are not drafting today: https://discord.com/channels/{}/{}/{}", guild_id, draft_event.value->reminder_channel_id, draft_event.value->reminder_id);

		message.set_content(text);

		bot.message_create(message, [&bot, guild_id, draft = draft_event.value](const dpp::confirmation_callback_t& callback) {
			if(!callback.is_error()) {

			} else {
				log(LOG_LEVEL_DEBUG, callback.get_error().message.c_str());
			}
		});
	} else {
		log(LOG_LEVEL_DEBUG, "No tentatives to ping");
	}

	redraw_signup(bot, guild_id, draft_event.value->signups_id, draft_event.value->signup_channel_id, draft_event.value);
	redraw_signup(bot, guild_id, draft_event.value->reminder_id, draft_event.value->reminder_channel_id, draft_event.value);

	database_set_draft_status(GUILD_ID, draft_event.value->draft_code, DRAFT_STATUS_TENTATIVES_PINGED);
}

static void ping_minutemages(dpp::cluster& bot, const u64 guild_id, const char* draft_code) {
	const auto draft_event = database_get_event(guild_id, draft_code);
	if(is_error(draft_event)) return;

	const auto sign_ups = database_get_draft_sign_ups(guild_id, draft_code);
	if(is_error(sign_ups)) return;

	int confirmed_count = 0;
	for(const auto& sign_up : sign_ups.value) {
		if((sign_up.status & SIGNUP_STATUS_PLAYING) > 0) {
			if((sign_up.status & SIGNUP_STATUS_REMOVED) != SIGNUP_STATUS_REMOVED) {
				confirmed_count++;
			}
		}
	}

	// Do we need to find more players?
	if(((confirmed_count % 2) == 1) || (confirmed_count < POD_SEATS_MIN)) {
		dpp::message message;
		message.set_type(dpp::message_type::mt_default);
		message.set_guild_id(guild_id);
		message.set_channel_id(draft_event.value->reminder_channel_id);
		message.set_allowed_mentions(true, true, true, true, {}, {});
		std::string text;

		// Sometimes there are only 4 confirmed sign ups so we need to ping for two people.
		size_t heroes_needed = (confirmed_count < POD_SEATS_MIN) ? (POD_SEATS_MIN - confirmed_count) : 1;

		// Count how many minutemage sign ups there are.
		std::vector<const Draft_Signup_Status*> minutemages;
		for(const auto& sign_up : sign_ups.value) {
			if(sign_up.status == SIGNUP_STATUS_MINUTEMAGE) {
				minutemages.push_back(&sign_up);
			}
		}

		// Ping minutemages first, if any.
		int heroes_added = 0;
		if(minutemages.size() > 0) {
			if(minutemages.size() <= heroes_needed) {
				// Easiest case - All minutemages (and maybe more!) are needed to fire.
				text += ":superhero: Paging minutemage";
				if(minutemages.size() == 1) {
					text += " ";
				} else {
					text += "s ";
				}

				for(size_t i = 0; i < minutemages.size(); ++i) {
					text += fmt::format("<@{}>", minutemages[i]->member_id);
					if(i != minutemages.size()-1) {
						text += " ";
					}

					// Add them to the playing list as a flexible player.
					const std::string preferred_name = get_members_preferred_name(guild_id, minutemages[i]->member_id);
					(void)database_sign_up_to_a_draft(guild_id, draft_code, minutemages[i]->member_id, preferred_name, time(NULL), SIGNUP_STATUS_FLEXIBLE);
					heroes_added++;
				}

				text += fmt::format("! You are needed on {} for {}.",
					draft_event.value->draftmancer_draft == true ? "Draftmancer" : draft_event.value->xmage_server,
					draft_event.value->format);

				heroes_needed -= minutemages.size();
				if(heroes_needed > 0) {
					text += "\n";
				}

			} else
			if(minutemages.size() > heroes_needed) {
				// More minutemages than needed - Select enough at random.
				text += ":superhero: Paging minutemages ";

				for(size_t i = 0; i < heroes_needed; ++i) {
					const int r = rand() & minutemages.size();
					text += fmt::format("<@{}> ", minutemages[r]->member_id);

					// Add them to the playing list as a flexible player.
					const std::string preferred_name = get_members_preferred_name(guild_id, minutemages[i]->member_id);
					(void)database_sign_up_to_a_draft(guild_id, draft_code, minutemages[i]->member_id, preferred_name, time(NULL), SIGNUP_STATUS_FLEXIBLE);
					heroes_added++;

					// Remove them so they can't be added twice
					minutemages.erase(minutemages.begin() + r);
				}

				text += fmt::format("! You are needed on {} for {}.",
					draft_event.value->draftmancer_draft == true ? "Draftmancer" : draft_event.value->xmage_server,
					draft_event.value->format);

				heroes_needed = 0;
			}
		}

		if(heroes_added > 0) {
		}

		if(heroes_needed > 0) {
			// Ping the @minutemage role.
			text += fmt::format(":superhero: <@&{}> {} more minutemages needed on {} for {}.",
				MINUTEMAGE_ROLE_ID,
				heroes_needed,
				draft_event.value->draftmancer_draft == true ? "Draftmancer" : draft_event.value->xmage_server,
				draft_event.value->format);
		}

		message.set_content(text);

		bot.message_create(message, [](const dpp::confirmation_callback_t& callback) {
			if(!callback.is_error()) {
			} else {
				log(LOG_LEVEL_DEBUG, callback.get_error().message.c_str());
			}
		});
	}

	// Redraw the sign up buttons so all buttons appear locked and any automatically added minutemages are showin in the playing column.
	redraw_signup(bot, GUILD_ID, draft_event.value->signups_id, draft_event.value->signup_channel_id, draft_event.value);
	redraw_signup(bot, GUILD_ID, draft_event.value->reminder_id, draft_event.value->reminder_channel_id, draft_event.value);
}

// Post a message to the hosts-only #-current-draft-management channel outlining the procedures for correctly managing the firing of the draft.
static void post_host_guide(dpp::cluster& bot, const char* draft_code) {
	auto draft = database_get_event(GUILD_ID, draft_code);
	if(has_value(draft)) {
		std::string text = fmt::format("# :alarm_clock: Attention hosts! Draft {} has now been locked. :alarm_clock:\n\n", draft_code);

		text += "## Use the following EventBot commands to manage the draft.\n\n";

		text += "### :one: Before pod allocations can be posted the :white_check_mark:Playing column on the sign-up sheet needs to show only players who are confirmed to be playing. The following commands can be used to add or remove players from the sheet:\n";
		text += "	**/add_player** - Add a player to the :white_check_mark:Playing column. Use this for adding Minutemages or players who want to play but didn't sign up before the draft was locked.\n";
		text += "	**/remove_player** - Remove a player from the :white_check_mark:Playing column. Use this for no-shows or people volunteering to drop to make an even number of players.\n";
		text += "\n";

		text += "### :two: Once all players in the :white_check_mark:Playing column are confirmed to be playing:\n";
		text += fmt::format("	**/post_allocations** - Create threads for each pod and post the pod allocations to <#{}> channel. This will also give all players the 'current draft' role and a 'Pod-X' role. Use these roles to ping all players or players in a specific pod.\n", draft.value->reminder_channel_id);
		text += "\n";

		text += "### :three: The follow commands can be used during the draft:\n";
		text += "	**/timer**   - After a Draftmancer draft, use this command to post a 10 minute countdown timer to remind players to finish constructing their decks in a timely manner.\n";
		text += "	**/dropper** - Increment the drop counter for a player. This needs to be done before the draft is completed.\n";
		text += "\n";

		text += "### :four: After all pods have completed round 3:\n";
		text += fmt::format("	**/finish** - Post the post-draft reminder message to <#{}> draft.\n", draft.value->reminder_channel_id);

		send_message(bot, GUILD_ID, draft.value->hosting_channel_id, text);
	}
}

static void set_bot_presence(dpp::cluster& bot) {
	dpp::presence_status status;
	dpp::activity_type type;
	std::string description;

	auto draft_code = database_get_next_upcoming_draft(GUILD_ID);
	if(!is_error(draft_code)) {
		if(draft_code.value.length() > 0) {
			auto draft = database_get_event(GUILD_ID, draft_code.value);
			if(!is_error(draft)) {
				if(draft.value->status == DRAFT_STATUS_LOCKED) {
					status = dpp::presence_status::ps_online;
					type = dpp::activity_type::at_watching;
					description = fmt::format("🔒{}", draft_code.value);
				} else {
					status = dpp::presence_status::ps_online;
					type = dpp::activity_type::at_watching;
					description = fmt::format("✅{}", draft_code.value);
				}
			} else {
				// Database error.
				status = dpp::presence_status::ps_dnd;
				type = dpp::activity_type::at_custom;
				description = "⚠️";
			}
		} else {
			// No scheduled drafts. This should never happen, right?
			status = dpp::presence_status::ps_idle;
			type = dpp::activity_type::at_watching;
			description = "⏰-pre-register 💤";
		}
	} else {
		// Database error.
		status = dpp::presence_status::ps_dnd;
		type = dpp::activity_type::at_custom;
		description = "⚠️";
	}

	bot.set_presence({status, type, description});
}

// Output the required MySQL tables for this bot. These tables could be created programmatically but I prefer to limit table creation/deletion to root.
static void output_sql() {
	fprintf(stdout, "-- Use ./eventbot -sql to create this file.\n\n");
	fprintf(stdout, "CREATE DATABASE IF NOT EXISTS %s; USE %s;\n\n", g_config.mysql_database, g_config.mysql_database);

	fprintf(stdout, "\n");
	fprintf(stdout, "CREATE TABLE IF NOT EXISTS stats(id BIGINT PRIMARY KEY NOT NULL UNIQUE, timestamp BIGINT DEFAULT 0);\n");
	fprintf(stdout, "CREATE TABLE IF NOT EXISTS devotion (id BIGINT PRIMARY KEY NOT NULL UNIQUE, name VARCHAR(%d) NOT NULL, value SMALLINT DEFAULT 0, next SMALLINT DEFAULT 0);\n", DEVOTION_BADGE_NAME_LENGTH_MAX);
	fprintf(stdout, "CREATE TABLE IF NOT EXISTS victory (id BIGINT PRIMARY KEY NOT NULL UNIQUE, name VARCHAR(%d) NOT NULL, value SMALLINT DEFAULT 0, next SMALLINT DEFAULT 0);\n", VICTORY_BADGE_NAME_LENGTH_MAX);
	fprintf(stdout, "CREATE TABLE IF NOT EXISTS trophies (id BIGINT PRIMARY KEY NOT NULL UNIQUE, name VARCHAR(%d) NOT NULL, value SMALLINT DEFAULT 0, next SMALLINT DEFAULT 0);\n", TROPHIES_BADGE_NAME_LENGTH_MAX);
	fprintf(stdout, "CREATE TABLE IF NOT EXISTS shark (id BIGINT PRIMARY KEY NOT NULL UNIQUE, name VARCHAR(%d) NOT NULL, value SMALLINT DEFAULT 0, next SMALLINT DEFAULT 0, is_shark BOOLEAN NOT NULL DEFAULT 0);\n", SHARK_BADGE_NAME_LENGTH_MAX);
	fprintf(stdout, "CREATE TABLE IF NOT EXISTS hero (id BIGINT PRIMARY KEY NOT NULL UNIQUE, name VARCHAR(%d) NOT NULL, value SMALLINT DEFAULT 0, next SMALLINT DEFAULT 0);\n", HERO_BADGE_NAME_LENGTH_MAX);
	fprintf(stdout, "CREATE TABLE IF NOT EXISTS win_rate_recent (id BIGINT PRIMARY KEY NOT NULL UNIQUE, league FLOAT NOT NULL DEFAULT 0, bonus FLOAT NOT NULL DEFAULT 0, overall FLOAT NOT NULL DEFAULT 0);\n");
	fprintf(stdout, "CREATE TABLE IF NOT EXISTS win_rate_all_time (id BIGINT PRIMARY KEY NOT NULL UNIQUE, league FLOAT NOT NULL DEFAULT 0, bonus FLOAT NOT NULL DEFAULT 0, overall FLOAT NOT NULL DEFAULT 0);\n");

	fprintf(stdout, "\n");

	fprintf(stdout, "CREATE TABLE IF NOT EXISTS xmage_version (version VARCHAR(64) NOT NULL UNIQUE, timestamp BIGINT NOT NULL UNIQUE);\n");
	// TODO: Delete name when BadgeBot is turned off
	fprintf(stdout, "CREATE TABLE IF NOT EXISTS commands (name VARCHAR(64) NOT NULL UNIQUE PRIMARY KEY, team BOOLEAN NOT NULL DEFAULT 0, hidden BOOLEAN NOT NULL DEFAULT 0, content VARCHAR(%lu) NOT NULL, summary VARCHAR(%d) NOT NULL);\n", DISCORD_MESSAGE_CHARACTER_LIMIT, DISCORD_AUTOCOMPLETE_STRING_LENGTH_MAX);

	fprintf(stdout, "\n");

	fprintf(stdout, "CREATE TABLE IF NOT EXISTS draft_events(\n");
	// Draft details
	fprintf(stdout, "status INT NOT NULL DEFAULT %d,\n", DRAFT_STATUS_CREATED);
	fprintf(stdout, "guild_id BIGINT NOT NULL,\n");
	fprintf(stdout, "pings VARCHAR(%lu) NOT NULL,\n", PING_STRING_LENGTH_MAX);
	fprintf(stdout, "draft_code VARCHAR(%lu) NOT NULL PRIMARY KEY,\n", DRAFT_CODE_LENGTH_MAX);
	fprintf(stdout, "league_name VARCHAR(%lu) NOT NULL,\n", LEAGUE_NAME_LENGTH_MAX);
	fprintf(stdout, "format VARCHAR(%lu) NOT NULL,\n", DRAFT_FORMAT_LENGTH_MAX);
	fprintf(stdout, "time_zone VARCHAR(%lu) NOT NULL,\n", IANA_TIME_ZONE_LENGTH_MAX);
	fprintf(stdout, "time INT NOT NULL,\n");
	fprintf(stdout, "duration FLOAT NOT NULL DEFAULT 3.0,\n");
	fprintf(stdout, "blurb_1 VARCHAR(%lu) NOT NULL DEFAULT \"\",\n", DRAFT_BLURB_LENGTH_MAX);
	fprintf(stdout, "blurb_2 VARCHAR(%lu) NOT NULL DEFAULT \"\",\n", DRAFT_BLURB_LENGTH_MAX);
	fprintf(stdout, "blurb_3 VARCHAR(%lu) NOT NULL DEFAULT \"\",\n", DRAFT_BLURB_LENGTH_MAX);
	fprintf(stdout, "draft_guide VARCHAR(%lu) NOT NULL DEFAULT \"\",\n", URL_LENGTH_MAX);
	fprintf(stdout, "card_list VARCHAR(%lu) NOT NULL DEFAULT \"\",\n", URL_LENGTH_MAX);
	fprintf(stdout, "set_list VARCHAR(%lu) NOT NULL DEFAULT \"\",\n", SET_LIST_LENGTH_MAX);
	// Draft sign up
	fprintf(stdout, "color INT NOT NULL,\n"); // Color of the vertical stripe down the left side of the embed.
	fprintf(stdout, "xmage_server VARCHAR(%lu) NOT NULL DEFAULT \"\",\n", XMAGE_SERVER_LENGTH_MAX);
	fprintf(stdout, "draftmancer_draft BOOLEAN NOT NULL DEFAULT 0,\n");
	fprintf(stdout, "banner_file VARCHAR(%lu) NOT NULL,\n", BANNER_FILENAME_MAX);
	fprintf(stdout, "banner_timestamp BIGINT NOT NULL DEFAULT 0,\n");

	//fprintf(stdout, "deleted BOOLEAN NOT NULL DEFAULT 0,\n"); // Has the event been deleted?
	fprintf(stdout, "signup_channel_id BIGINT NOT NULL,\n"); // The channel the sign up post goes to
	fprintf(stdout, "reminder_channel_id BIGINT NOT NULL,\n"); // The channel the reminder post goes to
	fprintf(stdout, "hosting_channel_id BIGINT NOT NULL,\n"); // The channel the hosting guide post goes to

	fprintf(stdout, "details_id BIGINT NOT NULL DEFAULT 0,\n"); // The message ID of the details post.
	fprintf(stdout, "signups_id BIGINT NOT NULL DEFAULT 0,\n"); // The message ID of the signup post.
	fprintf(stdout, "reminder_id BIGINT NOT NULL DEFAULT 0,\n");

	//fprintf(stdout, "locked BOOLEAN NOT NULL DEFAULT 0,\n"); // Has the draft been locked?
	//fprintf(stdout, "fired BOOLEAN NOT NULL DEFAULT 0,\n");
	//fprintf(stdout, "complete BOOLEAN NOT NULL DEFAULT 0,\n");

	fprintf(stdout, "UNIQUE KEY key_pair(guild_id, draft_code)\n");
	fprintf(stdout, ");");
	fprintf(stdout, "\n\n");


	fprintf(stdout, "CREATE TABLE IF NOT EXISTS draft_signups(\n");
	fprintf(stdout, "guild_id BIGINT NOT NULL,\n");
	fprintf(stdout, "draft_code VARCHAR(%lu) NOT NULL,\n", DRAFT_CODE_LENGTH_MAX);
	fprintf(stdout, "member_id BIGINT NOT NULL,\n");
	fprintf(stdout, "preferred_name VARCHAR(%lu) NOT NULL,\n", DISCORD_NAME_LENGTH_MAX);
	fprintf(stdout, "time BIGINT NOT NULL,\n");
	fprintf(stdout, "status INT NOT NULL DEFAULT 0,\n");
	fprintf(stdout, "FOREIGN KEY (draft_code) REFERENCES draft_events(draft_code) ON DELETE CASCADE,\n");
	fprintf(stdout, "UNIQUE KEY key_triplicate(guild_id, member_id, draft_code)\n");
	fprintf(stdout, ");");
	fprintf(stdout, "\n\n");


	fprintf(stdout, "CREATE TABLE IF NOT EXISTS leaderboards(\n");
	fprintf(stdout, "league VARCHAR(32) NOT NULL,\n");
	fprintf(stdout, "season INT NOT NULL,\n");
	fprintf(stdout, "member_id BIGINT NOT NULL,\n");
	fprintf(stdout, "rank INT NOT NULL,\n");
	fprintf(stdout, "week_01 INT DEFAULT NULL,\n");
	fprintf(stdout, "week_02 INT DEFAULT NULL,\n");
	fprintf(stdout, "week_03 INT DEFAULT NULL,\n");
	fprintf(stdout, "week_04 INT DEFAULT NULL,\n");
	fprintf(stdout, "week_05 INT DEFAULT NULL,\n");
	fprintf(stdout, "week_06 INT DEFAULT NULL,\n");
	fprintf(stdout, "week_07 INT DEFAULT NULL,\n");
	fprintf(stdout, "week_08 INT DEFAULT NULL,\n");
	fprintf(stdout, "week_09 INT DEFAULT NULL,\n");
	fprintf(stdout, "week_10 INT DEFAULT NULL,\n");
	fprintf(stdout, "week_11 INT DEFAULT NULL,\n");
	fprintf(stdout, "week_12 INT DEFAULT NULL,\n");
	fprintf(stdout, "week_13 INT DEFAULT NULL,\n");
	fprintf(stdout, "points INT NOT NULL DEFAULT 0,\n"); // TODO: Calculate this in a query? SUM(week_01, week_02, ...) etc.
	fprintf(stdout, "average FLOAT NOT NULL DEFAULT 0.0,\n"); // TODO: Calculate this in a query.
	fprintf(stdout, "drafts INT NOT NULL DEFAULT 0,\n"); // TODO: This could be TINY
	fprintf(stdout, "trophies INT NOT NULL DEFAULT 0,\n"); // TODO: Calculate this in a query
	fprintf(stdout, "win_rate FLOAT NOT NULL DEFAULT 0.0,\n");
	fprintf(stdout, "UNIQUE KEY key_triplicate(league, season, member_id)\n");
	fprintf(stdout, ");");
	fprintf(stdout, "\n\n");


	fprintf(stdout, "CREATE TABLE IF NOT EXISTS temp_roles(\n");
	fprintf(stdout, "guild_id BIGINT NOT NULL,\n");
	fprintf(stdout, "draft_code VARCHAR(%lu) NOT NULL,\n", DRAFT_CODE_LENGTH_MAX);
	fprintf(stdout, "role_id BIGINT NOT NULL UNIQUE\n");
	fprintf(stdout, ");");
	fprintf(stdout, "\n\n");


#if 0
	fprintf(stdout, "CREATE TABLE IF NOT EXISTS temp_threads(\n");
	fprintf(stdout, "guild_id BIGINT NOT NULL,\n");
	fprintf(stdout, "draft_code VARCHAR(%lu) NOT NULL,\n", DRAFT_CODE_LENGTH_MAX);
	fprintf(stdout, "thread_id BIGINT NOT NULL UNIQUE\n");
	fprintf(stdout, ");");
	fprintf(stdout, "\n\n");
#endif


#if 0
	fprintf(stdout, "CREATE TABLE IF NOT EXISTS temp_members(\n");
	fprintf(stdout, "guild_id BIGINT NOT NULL,\n"); // TODO: Not needed?
	fprintf(stdout, "member_id BIGINT NOT NULL,\n");
	fprintf(stdout, "role_id BIGINT NOT NULL UNIQUE,\n");
	fprintf(stdout, "FOREIGN KEY (role_id) REFERENCES temp_roles(role_id) ON DELETE CASCADE\n");
	fprintf(stdout, ");");
	fprintf(stdout, "\n\n");
#endif

	fprintf(stdout, "CREATE TABLE IF NOT EXISTS noshows(\n");
	fprintf(stdout, "guild_id BIGINT NOT NULL,\n");
	fprintf(stdout, "member_id BIGINT NOT NULL,\n");
	fprintf(stdout, "draft_code VARCHAR(%lu) NOT NULL\n", DRAFT_CODE_LENGTH_MAX);
	fprintf(stdout, ");");
	fprintf(stdout, "\n\n");

	fprintf(stdout, "CREATE TABLE IF NOT EXISTS droppers(\n");
	fprintf(stdout, "guild_id BIGINT NOT NULL,\n");
	fprintf(stdout, "member_id BIGINT NOT NULL,\n");
	fprintf(stdout, "draft_code VARCHAR(%lu) NOT NULL,\n", DRAFT_CODE_LENGTH_MAX);
	fprintf(stdout, "note VARCHAR(%lu)\n", DISCORD_MESSAGE_CHARACTER_LIMIT);
	fprintf(stdout, ");");
	fprintf(stdout, "\n\n");

	fprintf(stdout, "CREATE USER '%s'@localhost IDENTIFIED BY '%s'\n", g_config.mysql_username, g_config.mysql_password);
	fprintf(stdout, "GRANT DELETE, INSERT, SELECT, UPDATE ON %s.* TO '%s'@localhost;\n", g_config.mysql_database, g_config.mysql_username);
	fprintf(stdout, "GRANT DROP ON %s.commands TO '%s'@localhost;\n", g_config.mysql_database, g_config.mysql_username);
	fprintf(stdout, "FLUSH PRIVILEGES;\n");
}

static std::vector<std::string> get_pack_images(const char* format) {
	Set_List list = get_set_list_from_string(format);
	int pack_to_use = 0;
	const char* previous_set_code = NULL;
	std::vector<std::string> result;

	// TODO: If the first set and the third set are the same, but the second is different, use pack art 1 and 2?

	if(list.count == 0) {
		// No match found. Do a reverse lookup and find the art.
		const MTG_Draftable_Set* set = get_set_from_name(format);
		if(set != NULL && set->key_art == true) {
			result.push_back(fmt::format("gfx/pack_art/key/{}.png", set->name));
		}
	} else {
		for(int i = 0; i < list.count; ++i) {
			if(previous_set_code != NULL) {
				if(strcmp(list.set[i]->code, previous_set_code) == 0) {
					// Same code as last loop
					if(list.set[i]->pack_images > pack_to_use) {
						pack_to_use++;
						pack_to_use %= list.set[i]->pack_images;
					} else {
						pack_to_use = 0;
					}
				} else {
					pack_to_use = 0; // Reset
				}
			}

			// Conflux needs a special case as its files are in CON_ - CON is a reserved directory name on Windows.
			if(strcmp(list.set[i]->code, "CON") != 0) {
				result.push_back(fmt::format("gfx/pack_art/crop/{}/{}.png", list.set[i]->code, pack_to_use+1));
			} else {
				result.push_back(fmt::format("gfx/pack_art/crop/CON_/{}.png", pack_to_use+1));
			}

			previous_set_code = list.set[i]->code;
		}
	}

	return result;
}

static void config_file_kv_pair_callback(const char* key, const char* value, size_t value_len) {
	CONFIG_KEY_STR(mysql_host)     else
	CONFIG_KEY_STR(mysql_username) else
	CONFIG_KEY_STR(mysql_password) else
	CONFIG_KEY_STR(mysql_database) else
	CONFIG_KEY_U16(mysql_port)     else
	CONFIG_KEY_STR(logfile_path)   else
	CONFIG_KEY_STR(discord_token)  else
	CONFIG_KEY_STR(xmage_server)   else
	CONFIG_KEY_STR(eventbot_host)  else
	CONFIG_KEY_STR(api_key)        else
	CONFIG_KEY_STR(imgur_client_secret)
}

int main(int argc, char* argv[]) {
	// Load the config file. This file has sensitive information so isn't in version control.
	if(!load_config_file(CONFIG_FILE_NAME, config_file_kv_pair_callback)) {
		return EXIT_FAILURE;
	}

	if(argc > 1) {
		if(strcmp(argv[1], "-sql") == 0) {
			// Dump SQL schema to stdout and exit.
			output_sql();
			return EXIT_SUCCESS;
		}
		if(strcmp(argv[1], "-version") == 0) {
			// Print the BUILD_MODE and exit.
			// Used by the install script to ensure we're running the correct build on the public server.
			fprintf(stdout, "%s", BUILD_MODE);
			return EXIT_SUCCESS;
		}

	}

	// Check the version of EventBot that's running is in the correct place.
	// This is to prevent accidentally running the DEBUG version instead of the RELEASE version.
	{
		char cwd[FILENAME_MAX];
#ifdef DEBUG
		if((getcwd(cwd, FILENAME_MAX) == NULL) || (strcmp(cwd, EXPECTED_WORKING_DIR) == 0)) {
			fprintf(stderr, "Running the DEBUG build of EventBot from '%s' is not supported!\n", EXPECTED_WORKING_DIR);
#endif
#ifdef RELEASE
		if((getcwd(cwd, FILENAME_MAX) == NULL) || (strcmp(cwd, EXPECTED_WORKING_DIR) != 0)) {
			fprintf(stderr, "Running the RELEASE build of EventBot from anywhere other than '%s' is not supported!\n", EXPECTED_WORKING_DIR);
#endif
			return EXIT_FAILURE;
		}
	}

	// Check we're running on the deploy server and not the build server.
	{
		static const size_t HOSTNAME_MAX = 253 + 1; // 253 is the maximum number of ASCII characters allowed for a hostname.
		char hostname[HOSTNAME_MAX];
		if(gethostname(hostname, HOSTNAME_MAX) != 0 || strcmp(hostname, g_config.eventbot_host) != 0) {
			fprintf(stderr, "Running on wrong HOSTNAME. You are on '%s' but '%s' is required.\n", hostname, g_config.eventbot_host);
			return EXIT_FAILURE;
		}
	}

	// Careful not to pipe these somewhere a malicious user could find...
	fprintf(stdout, "eventbot_host	     = '%s'\n", g_config.eventbot_host);
	fprintf(stdout, "mysql_port          = '%d'\n", g_config.mysql_port);
	fprintf(stdout, "mysql_host          = '%s'\n", g_config.mysql_host);
	fprintf(stdout, "mysql_username      = '%s'\n", g_config.mysql_username);
	fprintf(stdout, "mysql_password      = '%s'\n", g_config.mysql_password);
	fprintf(stdout, "mysql_database      = '%s'\n", g_config.mysql_database);
	fprintf(stdout, "logfile_path        = '%s'\n", g_config.logfile_path);
	fprintf(stdout, "discord_token       = '%s'\n", g_config.discord_token);
	fprintf(stdout, "xmage_server        = '%s'\n", g_config.xmage_server);
	fprintf(stdout, "api_key             = '%s'\n", g_config.api_key);
	fprintf(stdout, "imgur_client_secret = '%s'\n", g_config.imgur_client_secret);

	// EventBot runs as a Linux systemd service, so we need to gracefully handle these signals.
	(void)signal(SIGINT,  sig_handler);
	(void)signal(SIGABRT, sig_handler);
	(void)signal(SIGHUP,  sig_handler);
	(void)signal(SIGTERM, sig_handler);
	// NOTE: SIGKILL is uncatchable, for (presumably!) obvious reasons!

	curl_global_init(CURL_GLOBAL_DEFAULT);
	mysql_library_init(0, NULL, NULL);

	srand(time(NULL));

	// Set up logging to an external file.
	log_init(g_config.logfile_path);

	log(LOG_LEVEL_INFO, "====== EventBot starting ======");
	log(LOG_LEVEL_INFO, "Build mode: %s",	         BUILD_MODE);
	log(LOG_LEVEL_INFO, "MariaDB client version: %s", mysql_get_client_info());
	log(LOG_LEVEL_INFO, "libDPP++ version: %s",       dpp::utility::version().c_str());
	log(LOG_LEVEL_INFO, "libcurl version: %s",        curl_version());

	// Download and install the latest IANA time zone database.
	// TODO: Only do this if /tmp/tzdata doesn't exist?
	log(LOG_LEVEL_INFO, "Downloading IANA time zone database.");
	const std::string tz_version = date::remote_version(); // FIXME?: Valgrind says this leaks...
	(void)date::remote_download(tz_version);
	(void)date::remote_install(tz_version);

	// Create the bot and connect to Discord.
	// TODO: We don't need all intents, so just request what we need...
	dpp::cluster bot(g_config.discord_token, dpp::i_all_intents);

	// Override the default DPP logger with ours.
	bot.on_log([](const dpp::log_t& event) {
		LOG_LEVEL level = g_log_level;
		switch(event.severity) {
			case dpp::ll_trace:    level = LOG_LEVEL_DEBUG;   break;
			case dpp::ll_debug:    level = LOG_LEVEL_DEBUG;   break;
			case dpp::ll_info:     level = LOG_LEVEL_INFO;    break;
			case dpp::ll_warning:  level = LOG_LEVEL_WARNING; break;
			case dpp::ll_error:    level = LOG_LEVEL_ERROR;   break;
			case dpp::ll_critical: level = LOG_LEVEL_ERROR;   break;
		}
		log(level, "%s", event.message.c_str());
	});

	// Called for slash command options that have the autocomplete flag set to true.
	bot.on_autocomplete([&bot](const dpp::autocomplete_t& event) {
		const u64 guild_id = event.command.get_guild().id;
		for(auto& opt : event.options) {
			if(opt.focused) {
				if(opt.name == "draft_code") {
					if(event.name == "post_draft") {
						// Gets a list of all drafts that haven't been posted.
						std::string prefix = std::get<std::string>(opt.value); // What the user has typed so far
						auto draft_codes = database_get_draft_codes_for_post_draft_autocomplete(guild_id, prefix, DISCORD_AUTOCOMPLETE_ENTRIES_MAX);
						auto response = dpp::interaction_response(dpp::ir_autocomplete_reply);
						for(auto& draft_code : draft_codes.value) {
							response.add_autocomplete_choice(dpp::command_option_choice(draft_code, draft_code));
						}
						bot.interaction_response_create(event.command.id, event.command.token, response);
					} else
					if(event.name == "edit_draft" || "view_draft") {
						// Gets a list of drafts that have been created, but not necessarily posted yet.
						std::string prefix = std::get<std::string>(opt.value); // What the user has typed so far
						auto draft_codes = database_get_draft_codes_for_edit_draft_autocomplete(guild_id, prefix, DISCORD_AUTOCOMPLETE_ENTRIES_MAX);
						auto response = dpp::interaction_response(dpp::ir_autocomplete_reply);
						for(auto& draft_code : draft_codes.value) {
							response.add_autocomplete_choice(dpp::command_option_choice(draft_code, draft_code));
						}
						bot.interaction_response_create(event.command.id, event.command.token, response);
					} else
					if(event.name == "delete_draft") {
						std::string prefix = std::get<std::string>(opt.value); // What the user has typed so far
						auto draft_codes = database_get_draft_codes_for_delete_draft_autocomplete(guild_id, prefix, DISCORD_AUTOCOMPLETE_ENTRIES_MAX);
						auto response = dpp::interaction_response(dpp::ir_autocomplete_reply);
						for(auto& draft_code : draft_codes.value) {
							response.add_autocomplete_choice(dpp::command_option_choice(draft_code, draft_code));
						}
						bot.interaction_response_create(event.command.id, event.command.token, response);
					}
				} else
				if(opt.name == "message") {
					if(event.name == "help") {
						auto response = dpp::interaction_response(dpp::ir_autocomplete_reply);
						std::string prefix = std::get<std::string>(opt.value);
						int limit = DISCORD_AUTOCOMPLETE_ENTRIES_MAX;
#if 0
						if(prefix.length() == 0) {
							response.add_autocomplete_choice(dpp::command_option_choice("All Commands - Print a list of all commands", "all_commands"));
							--limit;
						}
#endif
						auto commands = database_get_help_messages_for_autocomplete(guild_id, prefix, limit);
						for(auto& command : commands.value) {
							if(strlen(command.summary) > 0) {
								// NOTE: ARGH! Discord trims whitespace on auto complete options so we can't align this list nicely. ;(
								//auto choice = fmt::format("{} - {}", command.name, command.summary);
								response.add_autocomplete_choice(dpp::command_option_choice(command.summary, command.summary));
							} else {
								response.add_autocomplete_choice(dpp::command_option_choice(command.name, command.name));
							}
						}
						bot.interaction_response_create(event.command.id, event.command.token, response);
					}
				} else {
					log(LOG_LEVEL_ERROR, "Unhandled autocomplete for %s", opt.name);
				}
			}
		}
	});

	// Called when Discord has connected the bot to a guild.
	bot.on_guild_create([&bot](const dpp::guild_create_t& event) {

		log(LOG_LEVEL_INFO, "on_guild_create: Guild name:[%s] Guild ID:[%lu]", event.created->name.c_str(), static_cast<u64>(event.created->id));

		// As this is a "private" bot we don't want unknown guilds adding the bot and using the commands.
		// This won't prevent others joining the bot to their guild but it won't install any of the slash
		// commands for them to interact with. We could have the bot delete itself from the guild here,
		// but that would require the MANAGE_GUILD permission, which we don't need for any other reason.
		if(event.created->id != GUILD_ID) {
			log(LOG_LEVEL_INFO, "on_guild_create: Unknown guild %lu attempting to connect.", event.created->id);
			return;
		}

#ifdef DELETE_ALL_GUILD_COMMANDS
		// Running this will delete all guild commands registered with this bot.
		log(LOG_LEVEL_INFO, "Deleting all guild commands for %lu", event.created->id);
		bot.guild_commands_get(event.created->id, [&bot,&event](const dpp::confirmation_callback_t& callback) {
			if(!callback.is_error()) {
				auto commands = std::get<dpp::slashcommand_map>(callback.value);
				for(auto& [key, value] : commands) {
					if(value.application_id == bot.me.id) {
						bot.guild_command_delete(key, event.created->id);
					}
				}
			}
		});
		sleep(5); // Give Discord time to handle the guild_command_delete request(s) above.
#endif

		// We only want to re-create the slash commands when the bot is first started, not when Discord reconnects a guild, so check if we've already created the slash commands on this execution.
		if(g_commands_registered == false) {
			// Create slash commands
#ifdef DEBUG
			{
				dpp::slashcommand cmd("cpu_burner", "Create banner art for every set that has >= 3 images.", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				bot.guild_command_create(cmd, event.created->id);
			}
#endif // DEBUG
			{
				dpp::slashcommand cmd("banner", "Create a banner image for a draft.", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				// Required
				cmd.add_option(dpp::command_option(dpp::co_string, "draft_code", "The draft code for this draft. i.e. 123.4-PC.", true));
				cmd.add_option(dpp::command_option(dpp::co_string, "date", "Date of the draft in YYYY-MM-DD format. i.e. 2023-11-15.", true));
				cmd.add_option(dpp::command_option(dpp::co_string, "format", "The format of the draft. i.e. TSP/PLC/FUT or 'Artifact Chaos'.", true));

				// Optional
				auto draft_type_opt = dpp::command_option(dpp::co_integer, "draft_type", "Deovtion, hero, or community choice draft.", false);
				for(int i = (int)DRAFT_TYPE_DEVOTION_GIANT; i < (int)DRAFT_TYPE_COUNT; ++i) {
					draft_type_opt.add_choice(dpp::command_option_choice(std::string{to_string((DRAFT_TYPE)i)}, (std::int64_t)i));
				}
				cmd.add_option(draft_type_opt);

				cmd.add_option(dpp::command_option(dpp::co_string, "subheading", "Add a subheading. i.e. 'First draft of the season!'", false));
				cmd.add_option(dpp::command_option(dpp::co_attachment, "art", "Art to use as the background. Will be resized to 825x550 pixels.", false));
				bot.guild_command_create(cmd, event.created->id);
			}
			{
				dpp::slashcommand cmd("create_draft", "Create a new draft.", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				// Required
				cmd.add_option(dpp::command_option(dpp::co_string, "draft_code", "The draft code for this draft. i.e. 123.4-PC.", true));
				cmd.add_option(dpp::command_option(dpp::co_string, "format", "The format of the draft. i.e. TSP/PLC/FUT or 'Artifact Chaos'.", true));
				cmd.add_option(dpp::command_option(dpp::co_string, "date", "Date of the draft in YYYY-MM-DD format. i.e. 2023-03-15.", true));
				cmd.add_option(dpp::command_option(dpp::co_attachment, "banner", "Banner image for the draft.", true));

				// Optional
				cmd.add_option(dpp::command_option(dpp::co_string, "start_time", "Override the default start time. i.e. 17:50. Must be in 24 hour format.", false));
				cmd.add_option(dpp::command_option(dpp::co_number, "duration", "Duration of the draft in decimal hours. i.e 3.5 for 3 1/2 hours. Defaults to 3.", false));
				cmd.add_option(dpp::command_option(dpp::co_string, "blurb_1", "A paragraph about the format. i.e. Whose Devotion draft is it or who designed the custom set.", false));
				cmd.add_option(dpp::command_option(dpp::co_string, "blurb_2", "A paragraph about the format. i.e. Whose Devotion draft is it or who designed the custom set.", false));
				cmd.add_option(dpp::command_option(dpp::co_string, "blurb_3", "A paragraph about the format. i.e. Whose Devotion draft is it or who designed the custom set.", false));
				cmd.add_option(dpp::command_option(dpp::co_string, "guide_url", "A link to a draft guide for this format.", false));
				cmd.add_option(dpp::command_option(dpp::co_string, "card_list", "A link to a card list for this draft. i.e. A link to CubeCobra.", false));
				cmd.add_option(dpp::command_option(dpp::co_string, "set_list", "For chaos drafts. A comma separated list of sets in the pool. i.e. IVN,ONS,ZEN,...", false));
				cmd.add_option(dpp::command_option(dpp::co_string, "color", "Override the default color for the league. Must be RGB in hexidecimal. i.e. 8CE700", false));
				cmd.add_option(dpp::command_option(dpp::co_boolean, "draftmancer_draft", "Will the draft potion be run on Draftmancer?", false));
				cmd.add_option(dpp::command_option(dpp::co_string, "xmage_server", "Override the default XMage server. i.e. xmage.today:17172", false));
				cmd.add_option(dpp::command_option(dpp::co_channel, "signup_channel", "Channel to post the sign up. Defaults to #-pre-register.", false));
				cmd.add_option(dpp::command_option(dpp::co_channel, "reminder_channel", "Channel to post the pre-draft reminder message. Defaults to #-in-the-moment-draft.", false));
				cmd.add_option(dpp::command_option(dpp::co_channel, "hosting_channel", "Channel to post the hosting guide. Defaults to #-current-draft-management.", false));

				bot.guild_command_create(cmd, event.created->id);
			}
			{
				dpp::slashcommand cmd("view_draft", "View the details for a draft.", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				cmd.add_option(dpp::command_option(dpp::co_string, "draft_code", "The draft code of the draft to view.", true).set_auto_complete(true));
				bot.guild_command_create(cmd, event.created->id);
			}
			{
				dpp::slashcommand cmd("edit_draft", "Edit the details of a draft", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				// Required
				cmd.add_option(dpp::command_option(dpp::co_string, "draft_code", "The draft code of the draft to edit.", true).set_auto_complete(true));

				// Optional
				cmd.add_option(dpp::command_option(dpp::co_string, "format", "The format of the draft. i.e. TSP/PLC/FUT or 'Artifact Chaos'.", false));
				cmd.add_option(dpp::command_option(dpp::co_attachment, "banner", "Banner image for the draft.", false));
				cmd.add_option(dpp::command_option(dpp::co_string, "date", "Date of the draft in YYYY-MM-DD format. i.e. 2023-03-15.", false));
				cmd.add_option(dpp::command_option(dpp::co_string, "start_time", "Override the default start time. i.e. 17:50. Must be in 24 hour format.", false));
				cmd.add_option(dpp::command_option(dpp::co_number, "duration", "Duration of the draft in decimal hours. i.e 3.5 for 3 1/2 hours. Defaults to 3.", false));
				cmd.add_option(dpp::command_option(dpp::co_string, "blurb_1", "A paragraph about the format. i.e. Whose Devotion draft is it or who designed the custom set.", false));
				cmd.add_option(dpp::command_option(dpp::co_string, "blurb_2", "A paragraph about the format. i.e. Whose Devotion draft is it or who designed the custom set.", false));
				cmd.add_option(dpp::command_option(dpp::co_string, "blurb_3", "A paragraph about the format. i.e. Whose Devotion draft is it or who designed the custom set.", false));
				cmd.add_option(dpp::command_option(dpp::co_string, "guide_url", "A link to a draft guide for this format.", false));
				cmd.add_option(dpp::command_option(dpp::co_string, "card_list", "A link to a card list for this draft. i.e. A link to CubeCobra.", false));
				cmd.add_option(dpp::command_option(dpp::co_string, "set_list", "For chaos drafts. A comma separated list of sets in the pool. i.e. IVN,ONS,ZEN,...", false));
				cmd.add_option(dpp::command_option(dpp::co_string, "color", "Override the default color for the league. Must be RGB in hexidecimal. i.e. 8CE700", false));
				cmd.add_option(dpp::command_option(dpp::co_boolean, "draftmancer_draft", "Will the draft potion be run on Draftmancer?", false));
				cmd.add_option(dpp::command_option(dpp::co_string, "xmage_server", "Override the default XMage server. i.e. xmage.today:17172", false));
				cmd.add_option(dpp::command_option(dpp::co_channel, "signup_channel", "Channel to post the sign up.", false));
				cmd.add_option(dpp::command_option(dpp::co_channel, "reminder_channel", "Channel to post the pre-draft reminder message.", false));
				cmd.add_option(dpp::command_option(dpp::co_channel, "hosting_channel", "Channel to post the hosting guide.", false));

				bot.guild_command_create(cmd, event.created->id);
			}
			{
				dpp::slashcommand cmd("post_draft", "Post a draft.", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				cmd.add_option(dpp::command_option(dpp::co_string, "draft_code", "The code of the draft event to post.", true).set_auto_complete(true));
				bot.guild_command_create(cmd, event.created->id);
			}
			{
				dpp::slashcommand cmd("delete_draft", "Delete a draft post.", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				cmd.add_option(dpp::command_option(dpp::co_string, "draft_code", "The code of the draft to delete.", true).set_auto_complete(true));
				cmd.add_option(dpp::command_option(dpp::co_boolean, "purge", "Purge the draft and delete all sign ups from the EventBot database.", false));

				bot.guild_command_create(cmd, event.created->id);
			}
			{
				dpp::slashcommand cmd("view_allocations", "Print the pod allocations to the #-current-draft-management channel.", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				bot.guild_command_create(cmd, event.created->id);
			}
			{
				dpp::slashcommand cmd("add_player", "Add a member to the Playing column of the sign up sheet.", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				cmd.add_option(dpp::command_option(dpp::co_user, "member", "The member to add too the sign up sheet.", true));
				auto pod_option = dpp::command_option(dpp::co_integer, "pod", "Which pod to add the member to.", true);
				pod_option.add_choice(dpp::command_option_choice("Competitive", (s64) SIGNUP_STATUS_COMPETITIVE));
				pod_option.add_choice(dpp::command_option_choice("Casual", (s64) SIGNUP_STATUS_CASUAL));
				pod_option.add_choice(dpp::command_option_choice("Flexible", (s64) SIGNUP_STATUS_FLEXIBLE));
				cmd.add_option(pod_option);
				bot.guild_command_create(cmd, event.created->id);
			}
			{
				dpp::slashcommand cmd("remove_player", "Remove a player from the sign up sheet and (optionally) record them as a No Show", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				cmd.add_option(dpp::command_option(dpp::co_user, "member", "The member to remove from the sign up sheet.", true));
				cmd.add_option(dpp::command_option(dpp::co_boolean, "noshow", "Record this as a No Show.", true));
				bot.guild_command_create(cmd, event.created->id);
			}
			{
				dpp::slashcommand cmd("post_allocations", "Post the pod allocations to the public channels, create threads and groups.", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				bot.guild_command_create(cmd, event.created->id);
			}
			{
				dpp::slashcommand cmd("fire", "Create a role with all draft participants, and separate roles for each pod.", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				bot.guild_command_create(cmd, event.created->id);
			}
			{
				dpp::slashcommand cmd("timer", "Post the Draftmancer specific reminders and a timer for deck submission.", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				bot.guild_command_create(cmd, event.created->id);
			}
			{
				dpp::slashcommand cmd("dropper", "Add a player to the droppers list.", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				cmd.add_option(dpp::command_option(dpp::co_user, "member", "The member to add to the droppers list.", true));
				cmd.add_option(dpp::command_option(dpp::co_string, "note", "Attach a note to the drop record.", false));
				bot.guild_command_create(cmd, event.created->id);
			}
			{
				dpp::slashcommand cmd("finish", "Post the post draft message.", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				bot.guild_command_create(cmd, event.created->id);
			}
			{
				dpp::slashcommand cmd("stats", "Get your stats via private message.", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				bot.guild_command_create(cmd, event.created->id);
			}
			{
				dpp::slashcommand cmd("help", "Post a pre-written help message.", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				cmd.add_option(dpp::command_option(dpp::co_string, "message", "The pre-written help message to post.", true).set_auto_complete(true));
				bot.guild_command_create(cmd, event.created->id);
			}

			g_commands_registered = true;
		}

	});

	bot.on_slashcommand([&bot](const dpp::slashcommand_t& event) {
		const auto command_name = event.command.get_command_name();
		const auto guild_id = event.command.get_guild().id;

#ifdef DEBUG
		if(command_name == "cpu_burner") {
			event.reply("Here we go!");

			Banner_Opts opts;
			opts.draft_type = DRAFT_TYPE_NOT_APPLICABLE;
			opts.datetime = "DATETIME / DATETIME / DATETIME / DATETIME";
			for(size_t i = 0; i < SET_COUNT; ++i) {
				const XDHS_League* league = &g_xdhs_leagues[rand() % XDHS_LEAGUE_COUNT];
				opts.league_color = league->color & 0x0000FF00;
				opts.league_color |= (league->color & 0xFF) << 16;
				opts.league_color |= (league->color & 0x00FF0000) >> 16;
				const MTG_Draftable_Set* set = &g_draftable_sets[i];
				if(set->pack_images >= 1) {
					std::string format = fmt::format("{}/{}/{}", set->code, set->code, set->code);
					opts.images = get_pack_images(format.c_str());
					opts.title = fmt::format("BANNER TEST / SS.W-LT: {}", format);
					//log(LOG_LEVEL_DEBUG, "Rendering: %s", format.c_str());
					const auto banner = render_banner(&opts);
					if(!is_error(banner)) {
						dpp::message message;
						message.set_type(dpp::message_type::mt_default);
						message.set_guild_id(GUILD_ID);
						message.set_channel_id(1170985661185151017); // #spam
						message.set_allowed_mentions(false, false, false, false, {}, {});
						message.set_content(format);
						message.add_file("banner.png", dpp::utility::read_file(banner.value));
						bot.message_create(message);
					} else {
						event.reply(std::string{global_error_to_string(banner.error)});
					}
					opts.images.clear();
				}
			}
		} else
#endif // DEBUG
		if(command_name == "banner") {
			Banner_Opts opts;
			opts.draft_type = DRAFT_TYPE_NOT_APPLICABLE;

			// Required options
			auto draft_code_str = std::get<std::string>(event.get_parameter("draft_code"));
			const auto draft_code = parse_draft_code(draft_code_str.c_str());
			if(is_error(draft_code)) {
				event.reply(std::string{global_error_to_string(draft_code.error)});
				return;
			}

			const XDHS_League* league = draft_code.value.league;

			// Swap bytes so to the color format used by the blit_ functions.
			opts.league_color = (0xFF << 24) |
								((league->color >> 16) & 0xFF) |
								((league->color & 0xFF) << 16) |
								((league->color & 0x0000FF00));

			auto format = std::get<std::string>(event.get_parameter("format"));
			if(format.length() > FORMAT_STRING_LEN_MAX) {
				event.reply(fmt::format("Format string exceeds maximum allowed length of {} bytes.", FORMAT_STRING_LEN_MAX));
				return;
			}

			Date date;
			auto date_string = std::get<std::string>(event.get_parameter("date"));
			{
				const auto result = parse_date_string(date_string.c_str());
				if(is_error(result)) {
					event.reply(std::string{global_error_to_string(result.error)});
					return;
				}
				date = result.value;
			}

			// Create the default zoned time for this region.
			auto zoned_time = date::make_zoned(league->time_zone,
								date::local_days{date::year{date.year} / date.month / date.day} +
								std::chrono::hours(league->time.hour) +
								 std::chrono::minutes(league->time.minute));

			opts.datetime = date::format("%a %b %d @ %H:%M %Z", zoned_time).c_str();

			switch(league->id) {
				case LEAGUE_ID_AMERICAS_CHRONO:
				case LEAGUE_ID_AMERICAS_BONUS:  {
					opts.datetime += date::format(" | %H:%M %Z", date::make_zoned("America/Los_Angeles", zoned_time));
				} break;

				case LEAGUE_ID_EURO_CHRONO: {
					// Nothing extra to add.
				} break;

				case LEAGUE_ID_ASIA_CHRONO: {
					// Euro and Australia - Date is the same
					opts.datetime += date::format(" | %H:%M %Z", date::make_zoned("Australia/Sydney", zoned_time));
				} break;

				case LEAGUE_ID_PACIFIC_CHRONO: {
					// America and Australia
					opts.datetime += date::format(" | %H:%M %Z", date::make_zoned("America/Los_Angeles", zoned_time));

					opts.datetime += date::format(" || %a %b %d @ %H:%M %Z", date::make_zoned("Australia/Sydney", zoned_time));
				} break;

				case LEAGUE_ID_EURO_BONUS: {
					// Nothing extra to add.
				} break;
			}

			opts.title = fmt::format("{} / {}: {}", to_upper(to_string(league->id)), draft_code_str, format);

			// Optional options
			{
				auto opt = event.get_parameter("draft_type");
				if(std::holds_alternative<int64_t>(opt)) {
					opts.draft_type = (DRAFT_TYPE)std::get<int64_t>(opt);
				}
			}
			{
				auto opt = event.get_parameter("subheading");
				if(std::holds_alternative<std::string>(opt)) {
					opts.subtitle = std::get<std::string>(opt);
				}
			}

			if(opts.subtitle.length() == 0 && opts.draft_type == DRAFT_TYPE_COMMUNITY_CHOICE) {
				opts.subtitle = "Community Choice";
			}

			// As downloading a large image can take some time, and we have 3 seconds to respond, reply with something now and then update the reply as we progress.
			event.reply(":hourglass_flowing_sand: Creating banner...");
			// NOTE: From here to the end of the function event.edit_response() must be used, not event.reply()

			{
				// If an image was provided, download it and write to storage for later use.
				auto opt = event.get_parameter("art");
				if(std::holds_alternative<dpp::snowflake>(event.get_parameter("art"))) {
					auto art_id = std::get<dpp::snowflake>(event.get_parameter("art"));
					auto itr = event.command.resolved.attachments.find(art_id);
					auto art = itr->second;
				event.edit_response(fmt::format(":hourglass_flowing_sand: Downloading background art: {}", art.url));
				auto download = download_file(art.url.c_str());//, &image_full_size, &image_full_data);
					if(is_error(download)) {
						event.edit_response(std::string{global_error_to_string(download.error)});
						return;
					}
					SCOPE_EXIT(free(download.value.data));

					if(download.value.size > DOWNLOAD_BYTES_MAX) {
						event.edit_response(fmt::format("Downloading art image failed: Image exceeds maximum allowed size of {} bytes. Please resize your image to {}x{} pixels and try again.", DOWNLOAD_BYTES_MAX, BANNER_IMAGE_WIDTH, PACK_IMAGE_HEIGHT));
						return;
					}

					std::string temp_file = fmt::format("/tmp/EventBot_Art_{}", random_string(16));
					FILE* file = fopen(temp_file.c_str(), "wb");
					if(file) {
						SCOPE_EXIT(fclose(file));
						event.edit_response(":hourglass_flowing_sand: Saving image");
						size_t wrote = fwrite(download.value.data, 1, download.value.size, file);
						if(wrote == download.value.size) {
							opts.images.push_back(temp_file);
						} else {
							event.edit_response("Saving the provided art image has failed. This is not your fault! Please try again.");
							return;
						}
					} else {
						event.edit_response("Saving the provided art image has failed. This is not your fault! Please try again.");
						return;
					}
				}
			}

			// No art was provided so try find art from the format name.
			if(opts.images.size() == 0) {
				opts.images = get_pack_images(format.c_str());
			}

			if(opts.images.size() == 0) {
				event.edit_response(fmt::format("No art could be found for the format \"{}\" and you didn't provide an art image.", format));
				return;
			}

			event.edit_response(":hourglass_flowing_sand: Rendering banner");
			auto start = std::chrono::high_resolution_clock::now();
			const auto result = render_banner(&opts);
			if(is_error(result)) {
				event.edit_response(std::string{global_error_to_string(result.error)});
				return;
			}
			auto end = std::chrono::high_resolution_clock::now();
			auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

			dpp::message message;
			message.set_content(fmt::format(":hourglass_flowing_sand: {} ms", elapsed.count()));
			message.add_file(fmt::format("{} - {}.png", draft_code_str, format), dpp::utility::read_file(result.value));
			event.edit_response(message);
		} else
		if(command_name == "create_draft") {
			Draft_Event draft_event;

			// Required options
			auto draft_code_str = std::get<std::string>(event.get_parameter("draft_code"));
			// First, check if the draft code is valid and if it is get a copy of the XDHS_League it applies to.
			const auto draft_code = parse_draft_code(draft_code_str.c_str());
			if(is_error(draft_code)) {
				event.reply(dpp::message(draft_code.errstr).set_flags(dpp::m_ephemeral));;
				return;
			}
			strcpy(draft_event.draft_code, draft_code_str.c_str());

			const XDHS_League* league = draft_code.value.league;

			const std::string_view league_name = to_string(league->id);
			memcpy(draft_event.league_name, league_name.data(), league_name.length());

			auto format = std::get<std::string>(event.get_parameter("format"));
			if(format.length() > FORMAT_STRING_LEN_MAX) {
				event.reply(dpp::message(fmt::format("Format string exceeds maximum allowed length of {} bytes.", FORMAT_STRING_LEN_MAX)).set_flags(dpp::m_ephemeral));
				return;
			}
			strcpy(draft_event.format, format.c_str());

			// Get the time zone string
			strcpy(draft_event.time_zone, league->time_zone);

			Date date;
			auto date_string = std::get<std::string>(event.get_parameter("date"));
			{
				const auto result = parse_date_string(date_string.c_str());
				if(is_error(result)) {
					event.reply(dpp::message(result.errstr).set_flags(dpp::m_ephemeral));
					return;
				}
				date = result.value;
			}

			// Is the default start time for this league overridden?
			Start_Time start_time;
			start_time.hour = league->time.hour;
			start_time.minute = league->time.minute;
			{
				auto opt = event.get_parameter("start_time");
				if(std::holds_alternative<std::string>(opt)) {
					std::string start_time_string = std::get<std::string>(opt);
					const auto start_time_override = parse_start_time_string(start_time_string.c_str());
					if(is_error(start_time_override)) {
						event.reply(dpp::message(start_time_override.errstr).set_flags(dpp::m_ephemeral));
						return;
					}
					start_time = start_time_override.value;
				}
			}

			// Get the banner image.
			{
				auto banner_id = std::get<dpp::snowflake>(event.get_parameter("banner"));
				auto itr = event.command.resolved.attachments.find(banner_id);
				auto banner = itr->second;

				// Attachments are treated as 'ephemeral' by Discord and can be deleted after a period of time. To avoid this ever happening we download the attachment and save it to storage and later attach it to the draft sign up post.
				Result<Heap_Buffer> download = download_file(banner.url.c_str());
				SCOPE_EXIT(free(download.value.data));
				if(is_error(download)) {
					event.reply(dpp::message(download.errstr).set_flags(dpp::m_ephemeral));
					return;
				}

				if(download.value.size > DOWNLOAD_BYTES_MAX) {
					event.reply(dpp::message(fmt::format("Downloading art image failed: Image exceeds maximum allowed size of {} bytes. Please resize your image to {}x{} pixels and try again.", DOWNLOAD_BYTES_MAX, BANNER_IMAGE_WIDTH, PACK_IMAGE_HEIGHT)).set_flags(dpp::m_ephemeral));
					return;
				}

				std::string filename = fmt::format("{}/{}.png", HTTP_SERVER_DOC_ROOT, draft_code_str);
				FILE* file = fopen(filename.c_str(), "wb");
				if(file) {
					SCOPE_EXIT(fclose(file));
					size_t wrote = fwrite(download.value.data, 1, download.value.size, file);
					if(wrote == download.value.size) {
						strcpy(draft_event.banner_file, filename.c_str());

						struct stat file_attributes;
						stat(filename.c_str(), &file_attributes); // FIXME: This can fail!
						//draft_event.banner_timestamp = file_attributes.st_mtime;
					} else {
						event.edit_response("Saving the provided art image has failed. This is not your fault! Please try again.");
						return;
					}
				} else {
					event.edit_response("Saving the provided art image has failed. This is not your fault! Please try again.");
					return;
				}
			}

			// blurbs
			for(size_t i = 0; i < BLURB_COUNT; ++i) {
				static const size_t blurb_name_len = strlen("blurb_x") + 1;
				char blurb_name[blurb_name_len];
				snprintf(blurb_name, blurb_name_len, "blurb_%lu", i + 1);
				auto opt = event.get_parameter(blurb_name);
				if(std::holds_alternative<std::string>(opt)) {
					auto blurb = std::get<std::string>(opt);
					if(blurb.length() > DRAFT_BLURB_LENGTH_MAX) {
						event.reply(dpp::message(fmt::format("blurb_{} exceeds maximum length of {} bytes.", i, DRAFT_BLURB_LENGTH_MAX)).set_flags(dpp::m_ephemeral));
						return;
					}
					strcpy(&draft_event.blurbs[i][0], blurb.c_str());
				}
			}

			// Is the draft portion on Draftmancer?
			draft_event.draftmancer_draft = false;
			{
				auto opt = event.get_parameter("draftmancer_draft");
				if(std::holds_alternative<bool>(opt)) {
					draft_event.draftmancer_draft = std::get<bool>(opt);
				}
			}

			// Check if the default league color has been overridden.
			draft_event.color = league->color;
			{
				auto opt = event.get_parameter("color");
				if(std::holds_alternative<std::string>(opt)) {
					auto color_hex = std::get<std::string>(opt);
					if(color_hex.length() != strlen("RRGGBB")) {
						event.reply(dpp::message("Invalid hex string for color. Color should be written as RRGGBB.").set_flags(dpp::m_ephemeral));
						return;
					}
					for(size_t i = 0; i < color_hex.length(); ++i) {
						if(!isxdigit(color_hex[i])) {
							event.reply(dpp::message("Invalid hex digit in color string.").set_flags(dpp::m_ephemeral));
							return;
						}
					}
					draft_event.color = (u32) strtoul(color_hex.c_str(), NULL, 16);
				}
			}

			// Was a link to a draft guide given?
			std::string guide_url;
			{
				auto opt = event.get_parameter("guide_url");
				if(std::holds_alternative<std::string>(opt)) {
					guide_url = std::get<std::string>(opt);
					if(guide_url.length() > URL_LENGTH_MAX) {
						event.reply(dpp::message(fmt::format("guide_url exceeds maximum allowed length of {} bytes.", URL_LENGTH_MAX)).set_flags(dpp::m_ephemeral));
						return;
					}
				}
			}
			strcpy(draft_event.draft_guide_url, guide_url.c_str());

			// Was a link to a card list given?
			std::string card_list;
			{
				auto opt = event.get_parameter("card_list");
				if(std::holds_alternative<std::string>(opt)) {
					card_list = std::get<std::string>(opt);
					if(card_list.length() > URL_LENGTH_MAX) {
						event.reply(dpp::message(fmt::format("card_list exceeds maximum allowed length of {} bytes.", URL_LENGTH_MAX)).set_flags(dpp::m_ephemeral));
						return;
					}
				}
			}
			strcpy(draft_event.card_list_url, card_list.c_str());

			std::string set_list;
			{
				auto opt = event.get_parameter("set_list");
				if(std::holds_alternative<std::string>(opt)) {
					set_list = std::get<std::string>(opt);
					if(set_list.length() > SET_LIST_LENGTH_MAX) {
						event.reply(dpp::message(fmt::format("set_list exceeds maximum allowed length of {} bytes.", SET_LIST_LENGTH_MAX)).set_flags(dpp::m_ephemeral));
						return;
					}
				}
			}
			strcpy(draft_event.set_list, set_list.c_str());

			std::string xmage_server = g_config.xmage_server;
			{
				auto opt = event.get_parameter("xmage_server");
				if(std::holds_alternative<std::string>(opt)) {
					xmage_server = std::get<std::string>(opt);
					if(xmage_server.length() > XMAGE_SERVER_LENGTH_MAX) {
						event.reply(dpp::message(fmt::format("xmage_server string exceeds maximum allowed length of {} bytes.", XMAGE_SERVER_LENGTH_MAX)).set_flags(dpp::m_ephemeral));;
						return;
					}
				}
			}
			strcpy(draft_event.xmage_server, xmage_server.c_str());

			draft_event.signup_channel_id = PRE_REGISTER_CHANNEL_ID;
			{
				auto opt = event.get_parameter("signup_channel");
				if(std::holds_alternative<dpp::snowflake>(opt)) {
					draft_event.signup_channel_id = std::get<dpp::snowflake>(opt);
				}
			}

			draft_event.reminder_channel_id = IN_THE_MOMENT_DRAFT_CHANNEL_ID;
			{
				auto opt = event.get_parameter("reminder_channel");
				if(std::holds_alternative<dpp::snowflake>(opt)) {
					draft_event.reminder_channel_id = std::get<dpp::snowflake>(opt);
				}
			}

			draft_event.hosting_channel_id = CURRENT_DRAFT_MANAGEMENT_ID;
			{
				auto opt = event.get_parameter("hosting_channel");
				if(std::holds_alternative<dpp::snowflake>(opt)) {
					draft_event.hosting_channel_id = std::get<dpp::snowflake>(opt);
				}
			}

			draft_event.duration = DEFAULT_DRAFT_DURATION.hours + DEFAULT_DRAFT_DURATION.minutes;
			{
				auto opt = event.get_parameter("duration");
				if(std::holds_alternative<f64>(opt)) {
					const f64 duration = std::get<f64>(opt);
					if(duration < 0.0) {
						event.reply(dpp::message(fmt::format("Duration must be a positive number.")).set_flags(dpp::m_ephemeral));
						return;
					}
					draft_event.duration = duration;
				}
			}

			draft_event.time = pack_time(date.year, date.month, date.day, start_time.hour, start_time.minute);

			// Iterate over all the roles for this guild and find the IDs of the pingable roles.
			// TODO: Do this at post time?
			char ping_string[PING_STRING_LENGTH_MAX + 1];
			char* ping_string_ptr = ping_string;
			const dpp::guild& guild = event.command.get_guild();
			for(size_t i = 0; i < LEAGUE_PINGS_MAX; ++i) {
				const char* role_name = league->ping[i];
				if(role_name == NULL) break;
				for(auto& role_id : guild.roles) { // A std::vector<dpp::snowflake>
					dpp::role* role = find_role(role_id);
					if(role->name == role_name) {
						ping_string_ptr += snprintf(ping_string_ptr, (ping_string+PING_STRING_LENGTH_MAX)-ping_string_ptr, "<@&%lu> ", (u64)role_id);
						break;
					}
				}
			}

			strcpy(draft_event.pings, ping_string);

			// Add the event to the database.
			auto result = database_add_draft(guild_id, &draft_event);
			if(!is_error(result)) {
				//const dpp::user& issuing_user = event.command.get_issuing_user();
				event.reply(dpp::message(fmt::format("Draft {} created. Use ``/view_draft`` to view the settings, ``/edit_draft`` to make changes and ``/post_draft`` to post it.", draft_code_str)).set_flags(dpp::m_ephemeral));
			} else {
				event.reply(result.errstr);
			}
		} else
		if(command_name == "post_draft") {
			const std::string draft_code = std::get<std::string>(event.get_parameter("draft_code"));
			auto draft = database_get_event(guild_id, draft_code);
			if(!is_error(draft)) {
				post_draft(bot, guild_id, draft.value);
				event.reply(dpp::message(fmt::format("Draft {} posted to <#{}>", draft_code, draft.value->signup_channel_id)).set_flags(dpp::m_ephemeral));
			} else {
				event.reply(draft.errstr);
			}
		} else
		if(command_name == "view_draft") {
			const std::string draft_code = std::get<std::string>(event.get_parameter("draft_code"));
			auto draft = database_get_event(guild_id, draft_code);

			char time_string[TIME_STRING_MAX] = {0};
			make_time_string(draft.value->time, time_string);

			std::string text;
			text += "```";
			text += fmt::format("             status: {}\n", draft_status_to_string(draft.value->status));
			text += fmt::format("         draft_code: {}\n", draft.value->draft_code);
			text += fmt::format("              pings: {}\n", draft.value->pings);
			text += fmt::format("        league_name: {}\n", draft.value->league_name);
			text += fmt::format("             format: {}\n", draft.value->format);
			text += fmt::format("          time_zone: {}\n", draft.value->time_zone);
			text += fmt::format("               time: {}\n", time_string);
			text += fmt::format("           duration: {}\n", draft.value->duration);
			for(size_t i = 0; i < BLURB_COUNT; ++i) {
				text += fmt::format("            blurb_{}: {}\n", i+1, draft.value->blurbs[i]);
			}
			text += fmt::format("    draft_guide_url: {}\n", draft.value->draft_guide_url);
			text += fmt::format("      card_list_url: {}\n", draft.value->card_list_url);
			text += fmt::format("           set_list: {}\n", draft.value->set_list);
			text += fmt::format("              color: {:x}\n", draft.value->color);
			text += fmt::format("       xmage_server: {}\n", draft.value->xmage_server);
			text += fmt::format("  draftmancer_draft: {}\n", draft.value->draftmancer_draft);
			text += fmt::format("        banner_file: {}\n", draft.value->banner_file);
			text += fmt::format("   banner_timestamp: {}\n", draft.value->banner_timestamp);
			text += fmt::format("  signup_channel_id: {}\n", draft.value->signup_channel_id);
			text += fmt::format("reminder_channel_id: {}\n", draft.value->reminder_channel_id);
			text += fmt::format(" hosting_channel_id: {}\n", draft.value->hosting_channel_id);
			text += "```";

			event.reply(text); // TODO: Do we want this to be ephemeral?
		} else
		if(command_name == "edit_draft") {
			const std::string draft_code = std::get<std::string>(event.get_parameter("draft_code"));

			auto draft_event = database_get_event(guild_id, draft_code);

			// format
			{
				auto opt = event.get_parameter("format");
				if(std::holds_alternative<std::string>(opt)) {
					const std::string format = std::get<std::string>(opt);
					strcpy(draft_event.value->format, format.c_str());
				}
			}

			// date
			{
				auto opt = event.get_parameter("date");
				if(std::holds_alternative<std::string>(opt)) {
					const std::string date_string = std::get<std::string>(opt);
					const auto date = parse_date_string(date_string.c_str());
					if(is_error(date)) {
						event.reply(dpp::message(date.errstr).set_flags(dpp::m_ephemeral));
						return;
					}

					int year, month, day, hour, minute;
					unpack_time(draft_event.value->time, &year, &month, &day, &hour, &minute);
					draft_event.value->time = pack_time(date.value.year, date.value.month, date.value.day, hour, minute);
				}
			}

			// start_time
			{
				auto opt = event.get_parameter("start_time");
				if(std::holds_alternative<std::string>(opt)) {
					const std::string start_time = std::get<std::string>(opt);
					const auto st = parse_start_time_string(start_time.c_str());
					if(is_error(st)) {
						event.reply(dpp::message(st.errstr).set_flags(dpp::m_ephemeral));
						return;
					}

					int year, month, day, hour, minute;
					unpack_time(draft_event.value->time, &year, &month, &day, &hour, &minute);
					draft_event.value->time = pack_time(year, month, day, st.value.hour, st.value.minute);
				}
			}

			// duration
			{
				auto opt = event.get_parameter("duration");
				if(std::holds_alternative<f64>(opt)) {
					const f64 duration = std::get<f64>(opt);
					if(duration < 0.0) {
						event.reply(dpp::message("Duration must be a positive number.").set_flags(dpp::m_ephemeral));
						return;
					}
					draft_event.value->duration = duration;
				}
			}

			// banner
			{
				auto opt = event.get_parameter("banner");
				if(std::holds_alternative<dpp::snowflake>(opt)) {
					dpp::snowflake banner_id = std::get<dpp::snowflake>(opt);
					auto itr = event.command.resolved.attachments.find(banner_id);
					auto banner = itr->second;

					auto download = download_file(banner.url.c_str());
					SCOPE_EXIT(free(download.value.data));
					if(is_error(download)) {
						event.reply(dpp::message(download.errstr).set_flags(dpp::m_ephemeral));
						return;
					}

					if(download.value.size > DOWNLOAD_BYTES_MAX) {
						event.reply(dpp::message(fmt::format("Downloading art image failed: Image exceeds maximum allowed size of {} bytes. Please resize your image to {}x{} pixels and try again.", DOWNLOAD_BYTES_MAX, BANNER_IMAGE_WIDTH, PACK_IMAGE_HEIGHT)).set_flags(dpp::m_ephemeral));
						return;
					}

					std::string filename = fmt::format("{}/{}.png", HTTP_SERVER_DOC_ROOT, draft_code);
					FILE* file = fopen(filename.c_str(), "wb");
					if(file) {
						SCOPE_EXIT(fclose(file));
						size_t wrote = fwrite(download.value.data, 1, download.value.size, file);
						if(wrote == download.value.size) {
							strcpy(draft_event.value->banner_file, filename.c_str());

							struct stat file_attributes;
							stat(filename.c_str(), &file_attributes); // FIXME: This can fail!
							//draft_event.value->banner_timestamp = file_attributes.st_mtime;
						} else {
							event.reply(dpp::message("Saving the provided art image has failed. This is not your fault! Please try again.").set_flags(dpp::m_ephemeral));
							return;
						}
					} else {
						event.reply(dpp::message("Saving the provided art image has failed. This is not your fault! Please try again.").set_flags(dpp::m_ephemeral));
						return;
					}
				}
			}

			//blurb_1 ... blurb_3
			for(size_t i = 0; i < BLURB_COUNT; ++i) {
				static const size_t blurb_name_len = strlen("blurb_x") + 1;
				char blurb_name[blurb_name_len];
				snprintf(blurb_name, blurb_name_len, "blurb_%lu", i + 1);
				auto opt = event.get_parameter(blurb_name);
				if(std::holds_alternative<std::string>(opt)) {
					auto blurb = std::get<std::string>(opt);
					if(blurb.length() > DRAFT_BLURB_LENGTH_MAX) {
						event.reply(dpp::message(fmt::format("blurb_{} exceeds maximum length of {} bytes.", i, DRAFT_BLURB_LENGTH_MAX)).set_flags(dpp::m_ephemeral));
						return;
					}
					strcpy(&draft_event.value->blurbs[i][0], blurb.c_str());
				}
			}

			//guild_url
			{
				auto opt = event.get_parameter("guide_url");
				if(std::holds_alternative<std::string>(opt)) {
					const std::string guide_url = std::get<std::string>(opt);
					if(guide_url.length() > URL_LENGTH_MAX) {
						event.reply(dpp::message(fmt::format("guide_url exceeds maximum allowed length of {} bytes.", URL_LENGTH_MAX)).set_flags(dpp::m_ephemeral));
						return;
					}
					strcpy(draft_event.value->draft_guide_url, guide_url.c_str());
				}
			}

			//card_list
			{
				auto opt = event.get_parameter("card_list");
				if(std::holds_alternative<std::string>(opt)) {
					const std::string card_list = std::get<std::string>(opt);
					if(card_list.length() > URL_LENGTH_MAX) {
						event.reply(dpp::message(fmt::format("card_list exceeds maximum allowed length of {} bytes.", URL_LENGTH_MAX)).set_flags(dpp::m_ephemeral));
						return;
					}
					strcpy(draft_event.value->card_list_url, card_list.c_str());
				}
			}

			//set_list
			{
				auto opt = event.get_parameter("set_list");
				if(std::holds_alternative<std::string>(opt)) {
					const std::string set_list = std::get<std::string>(opt);
					if(set_list.length() > SET_LIST_LENGTH_MAX) {
						event.reply(dpp::message(fmt::format("set_list exceeds maximum allowed length of {} bytes.", SET_LIST_LENGTH_MAX)).set_flags(dpp::m_ephemeral));
						return;
					}
					strcpy(draft_event.value->set_list, set_list.c_str());
				}
			}

			//color
			{
				auto opt = event.get_parameter("color");
				if(std::holds_alternative<std::string>(opt)) {
					const auto color_hex = std::get<std::string>(opt);
					if(color_hex.length() != strlen("RRGGBB")) {
						event.reply(dpp::message("Invalid hex string for color. Color should be written as RRGGBB.").set_flags(dpp::m_ephemeral));
						return;
					}
					for(size_t i = 0; i < color_hex.length(); ++i) {
						if(!isxdigit(color_hex[i])) {
							event.reply(dpp::message("Invalid hex digit in color string.").set_flags(dpp::m_ephemeral));
							return;
						}
					}
					draft_event.value->color = (u32) strtoul(color_hex.c_str(), NULL, 16);
				}
			}

			//draftmancer
			{
				auto opt = event.get_parameter("draftmancer_draft");
				if(std::holds_alternative<bool>(opt)) {
					draft_event.value->draftmancer_draft = std::get<bool>(opt);
				}
			}

			//xmage_server
			{
				auto opt = event.get_parameter("xmage_server");
				if(std::holds_alternative<std::string>(opt)) {
					const std::string xmage_server = std::get<std::string>(opt);
					if(xmage_server.length() > XMAGE_SERVER_LENGTH_MAX) {
						event.reply(dpp::message(fmt::format("xmage_server string exceeds maximum allowed length of {} bytes.", XMAGE_SERVER_LENGTH_MAX)).set_flags(dpp::m_ephemeral));
						return;
					}
					strcpy(draft_event.value->xmage_server, xmage_server.c_str());
				}
			}

			{
				auto opt = event.get_parameter("signup_channel");
				if(std::holds_alternative<dpp::snowflake>(opt)) {
					draft_event.value->signup_channel_id = std::get<dpp::snowflake>(opt);
				}
			}

			{
				auto opt = event.get_parameter("reminder_channel");
				if(std::holds_alternative<dpp::snowflake>(opt)) {
					draft_event.value->reminder_channel_id = std::get<dpp::snowflake>(opt);
				}
			}

			{
				auto opt = event.get_parameter("hosting_channel");
				if(std::holds_alternative<dpp::snowflake>(opt)) {
					draft_event.value->hosting_channel_id = std::get<dpp::snowflake>(opt);
				}
			}

			auto result = database_edit_draft(guild_id, draft_event.value);
			if(!is_error(result)) {
				event.reply(dpp::message(fmt::format("Draft {} updated", draft_code)).set_flags(dpp::m_ephemeral));
				edit_draft(bot, guild_id, draft_event.value);
			} else {
				event.reply(dpp::message(result.errstr).set_flags(dpp::m_ephemeral));
			}
		} else
		if(command_name == "delete_draft") {
			const std::string draft_code = std::get<std::string>(event.get_parameter("draft_code"));
			const u64 guild_id = event.command.get_guild().id;

			bool purge = false;
			{
				auto opt = event.get_parameter("purge");
				if(std::holds_alternative<bool>(opt)) {
					purge = std::get<bool>(opt);
				}
			}

			delete_draft_posts(bot, guild_id, draft_code);

			if(purge == false) {
				(void)database_clear_draft_post_ids(guild_id, draft_code);
				event.reply(dpp::message(fmt::format("Draft {} post deleted. Use ``/post_draft`` to repost it.", draft_code)).set_flags(dpp::m_ephemeral));
			} else {
				database_purge_draft_event(guild_id, draft_code);
				event.reply(dpp::message(fmt::format("Draft {} and all sign ups purged.", draft_code)).set_flags(dpp::m_ephemeral));
			}
		} else
		if(command_name == "add_player") {
			const auto guild_id = event.command.get_guild().id;

			const auto draft = database_get_event(guild_id, g_current_draft_code);
#if TESTING
			if(!BIT_SET(draft.value->status, DRAFT_STATUS_LOCKED)) {
				event.reply(dpp::message("This command can only be used once the draft is locked.").set_flags(dpp::m_ephemeral));
				return;
			}
#endif

			const auto member_id = std::get<dpp::snowflake>(event.get_parameter("member"));
			const auto pod = (SIGNUP_STATUS) std::get<std::int64_t>(event.get_parameter("pod"));

			const std::string preferred_name = get_members_preferred_name(guild_id, member_id);

			(void)database_sign_up_to_a_draft(guild_id, g_current_draft_code, member_id, preferred_name, time(NULL), pod);

			// Redraw the sign up sheet in the #-pre-register channel.
			redraw_signup(bot, guild_id, draft.value->signups_id, draft.value->signup_channel_id, draft.value);

			// Redraw the sign up sheet on the reminder message sent to #-in-the-moment-draft
			if(draft.value->reminder_id != 0) {
				redraw_signup(bot, guild_id, draft.value->reminder_id, draft.value->reminder_channel_id, draft.value);
			}

			event.reply(fmt::format("{} added to {} {} pod.", preferred_name, g_current_draft_code, to_string(pod)));
		} else
		if(command_name == "remove_player") {
			const auto guild_id = event.command.get_guild().id;
			const auto member_id = std::get<dpp::snowflake>(event.get_parameter("member"));

			const auto draft = database_get_event(guild_id, g_current_draft_code);
#if TESTING
			if(!BIT_SET(draft.value->status, DRAFT_STATUS_LOCKED)) {
				event.reply(dpp::message("This command can only be used once the draft is locked.").set_flags(dpp::m_ephemeral));
				return;
			}
#endif

			{
				auto opt = event.get_parameter("noshow");
				if(std::holds_alternative<bool>(opt)) {
					bool noshow = std::get<bool>(opt);
					if(noshow == true) database_add_noshow(guild_id, member_id, g_current_draft_code.c_str());
				}
			}

			const std::string preferred_name = get_members_preferred_name(guild_id, member_id);
			auto current_sign_up_status = database_get_members_sign_up_status(guild_id, g_current_draft_code, member_id);
			(void)database_sign_up_to_a_draft(guild_id, g_current_draft_code, member_id, preferred_name, current_sign_up_status.value.timestamp, (SIGNUP_STATUS)(current_sign_up_status.value.status | SIGNUP_STATUS_REMOVED));

			// Redraw the sign up sheet in the #-pre-register channel.
			redraw_signup(bot, guild_id, draft.value->signups_id, draft.value->signup_channel_id, draft.value);

			// Redraw the sign up sheet on the reminder message sent to #-in-the-moment-draft
			if(draft.value->reminder_id != 0) {
				redraw_signup(bot, guild_id, draft.value->reminder_id, draft.value->reminder_channel_id, draft.value);
			}

			event.reply(fmt::format("{} removed from {}.", preferred_name, g_current_draft_code));
		} else
		if(command_name == "post_allocations") {
			if(g_current_draft_code.length() == 0) {
				event.reply(dpp::message{"No active draft"}.set_flags(dpp::m_ephemeral));
				return;
			}

			// TODO: Create a role with the draft_code as it's name and add everyone to it. Do the same for each Pod
			const auto guild_id = event.command.get_guild().id;

			const auto draft = database_get_event(guild_id, g_current_draft_code);
			if(!BIT_SET(draft.value->status, DRAFT_STATUS_LOCKED)) {
				event.reply(dpp::message{"This command can only be used once the draft is locked."}.set_flags(dpp::m_ephemeral));
				return;
			}

			// A bad draft code should be impossible at this point, but just to be safe...
			const auto draft_code = parse_draft_code(g_current_draft_code.c_str());
			if(is_error(draft_code)) {
				event.reply(dpp::message(draft_code.errstr).set_flags(dpp::m_ephemeral));
				return;
			}

			const XDHS_League* league = draft_code.value.league;
			char league_code[3];
			league_code[0] = league->region_code;
			league_code[1] = league->league_type;
			league_code[2] = 0;

			// TESTME: What happens on the first draft of the season and the leaderboard is empty?
			// FIXME: Does this need to be a shared_ptr / on the heap? This function might exit before Discord can finish making all the pod roles and assigning members to them.
			auto sign_ups = database_get_sign_ups(guild_id, g_current_draft_code, league_code, draft_code.value.season);
			if(is_error(sign_ups)) {
				log(LOG_LEVEL_ERROR, "database_get_sign_ups(%lu, %s, %s, %d) failed", guild_id, g_current_draft_code, league_code, draft_code.value.season);
				event.reply(dpp::message(sign_ups.errstr).set_flags(dpp::m_ephemeral));
				return;
			}

			// Remove all players with SIGNUP_STATUS_NOT_PLAYING
			auto erased_count = std::erase_if(sign_ups.value, [](const Draft_Sign_Up x) {
				return x.status & SIGNUP_STATUS_NOT_PLAYING;
			});
			(void)erased_count;

			if(sign_ups.count < POD_SEATS_MIN) {
				event.reply(dpp::message(fmt::format("At least {} players needed. Recruit more players and use /add_player to add them to the sign up sheet.", POD_SEATS_MIN)).set_flags(dpp::m_ephemeral));
				return;
			}

			if((sign_ups.count % 2) == 1) {
				event.reply(dpp::message("An even number of sign ups is required before using this command. Recruit more players and use /add_player to add them to the sign up sheet, or use /remove_player to remove someone.").set_flags(dpp::m_ephemeral));
				return;
			}

			if(sign_ups.count > PLAYERS_MAX) {
				event.reply(dpp::message("Maximum player count of {} exceeded. You're on your own!").set_flags(dpp::m_ephemeral));
				return;
			}

			// TODO: Can this be done by MySQL as part of the query?
			for(auto& member : sign_ups.value) {
				if(member.rank_is_null == true) {
					member.rank = 9999;
				}
				if(member.is_shark_is_null == true) {
					member.is_shark = false;
				}
				if(member.points_is_null == true) {
					member.points = 0;
				}
				if(member.devotion_is_null == true) {
					member.devotion = 0;
				}
				if(member.win_rate_is_null == true) {
					member.win_rate = 0.0f;
				}
			}

			// Mark all potential hosts and mark everyone as unallocated.
			int host_count = 0;
			for(size_t i = 0; i < sign_ups.value.size(); ++i) {
				sign_ups.value[i].reason = POD_ALLOCATION_REASON_UNALLOCATED;
				try {
					const dpp::guild_member member = dpp::find_guild_member(guild_id, sign_ups.value[i].member_id);
					const std::vector<dpp::snowflake> roles = member.get_roles();
					for(const auto role : roles) {
						if((role == XDHS_TEAM_ROLE_ID) || (role == XDHS_HOST_ROLE_ID)) {
							sign_ups.value[i].is_host = true;
							host_count++;
						}
					}
				} catch(dpp::cache_exception& e) {
					log(LOG_LEVEL_ERROR, "Caught exception: %s", e.what());
				}
			}

			for(const auto& player : sign_ups.value) {
				log(LOG_LEVEL_DEBUG, fmt::format("Player:{} status:{} rank:{} is_shark:{} points:{} devotion:{} win_rate:{}",
					player.preferred_name, (int)player.status, player.rank, player.is_shark, player.points, player.devotion, player.win_rate).c_str());
			}

			// FIXME: Does this need to be a shared_ptr / on the heap? This function might exit before Discord can finish making all the pod roles and assigning members to them.
			Draft_Tournament tournament = set_up_pod_count_and_sizes(sign_ups.count);

			if(tournament.pod_count == 0) {
				// Shouldn't be possible with the above checks.
				event.reply(dpp::message("Insufficient players to form a pod.").set_flags(dpp::m_ephemeral));;
				return;
			}

			/* Pod Priority Rules - https://discord.com/channels/528728694680715324/828170430040768523/828197583255765043
				#1: The host of that pod - If the only available hosts (@XDHS Team members and @Hosts) are both in the leaderboard Top 3, the lowest-ranked host will host Pod 2.
				#2: Players who are required to play in that pod via the Rule of 3 or Extended Rule of 3
				For the last draft of the season, any player who is mathematically live for Top 3 after final draft points are scored must play in Pod 1.
				#3: Players with "Shark" status for the current Season (must play in pod 1)
				#4: New XDHS players and Goblins (1-4 drafts played) have priority for Pod 2
				#5: Players who reacted with their preferred emoji ( :Pod1~1:  or :Pod2~1: ) in #-pre-register
				#6: Players who didn't react in #-pre-register (first among these to join the draft table on XMage gets the spot)

				The tiebreaker for #3/4/5 is determined by the order output from the randomizer. NOTE: This is not how it is done here. Instead we use sign up time - first in, first served!
			 */

			/* Assigning hosts

			Allocate everyone to a pod.

			Check that each pod has at least one host assigned.
			If not, for each pod > 1 without a host, look back through the pods looking for a pod with more than 1 host. Sort the hosts in that pod based on their requirements to be in the pod (Ro3, Sharks etc) and swap the host with the lowest priority and win rate with the player with the highest win rate.

			If no suitable host is found, the draft organizer will have to nominate a temporary host.

			We probably want to work out each players players pod placement and then try to allocate the best host from there, right? Like if someone is a shark or has a Ro3 placement the should host pod 1, whereas someone not in contention for a place on the leaderboard would be better in Pod 2, even if that's not their preference...

			*/

			log(LOG_LEVEL_INFO, "%d pods", tournament.pod_count);

			if(tournament.pod_count == 1) {
				// Easiest case - Only a single pod. Just add everyone to pod 1.
				for(auto& player : sign_ups.value) {
					player.reason = POD_ALLOCATION_REASON_SINGLE_POD;
					add_player_to_pod(&tournament.pods[0], player.member_id, POD_ALLOCATION_REASON_SINGLE_POD, player.preferred_name);
				}
			} else {
				static const int RULE_OF_THREE_START_WEEK = 3;
				static const int EXTENDED_RULE_OF_THREE_START_WEEK = 6;
				static const int WEEKS_IN_CURRENT_SEASON = 9; // TODO: Get this from the spreadsheet

				if(draft_code.value.week < RULE_OF_THREE_START_WEEK) {
					// No Rule of Three, nothing to do here.
					log(LOG_LEVEL_INFO, "Week: %d, no Rule of Three", draft_code.value.week);
				} else
				if(draft_code.value.week >= RULE_OF_THREE_START_WEEK && draft_code.value.week < EXTENDED_RULE_OF_THREE_START_WEEK) {
					// Rule of Three
					log(LOG_LEVEL_INFO, "Week: %d, Rule of Three", draft_code.value.week);

					for(auto& player : sign_ups.value) {
						if(player.reason == POD_ALLOCATION_REASON_UNALLOCATED && player.rank <= 3) {
							player.reason = POD_ALLOCATION_REASON_RO3;

							add_player_to_pod(get_next_empty_draft_pod_high(&tournament), player.member_id, player.reason, player.preferred_name);
						}
					}
				} else
				if(draft_code.value.week >= EXTENDED_RULE_OF_THREE_START_WEEK && draft_code.value.week != WEEKS_IN_CURRENT_SEASON) {
					// Extended Rule of Three
					log(LOG_LEVEL_INFO, "Week: %d, Extended Rule of Three", draft_code.value.week);

					// Find the score of the player ranked 3rd.
					int point_threshold = -9999;
					for(auto& player : sign_ups.value) {
						if(player.rank == 3) {
							point_threshold = player.points;
							break;
						}
					}

					for(auto& player : sign_ups.value) {
						if(player.reason == POD_ALLOCATION_REASON_UNALLOCATED && player.points >= point_threshold) {
							player.reason = POD_ALLOCATION_REASON_ERO3;

							add_player_to_pod(get_next_empty_draft_pod_high(&tournament), player.member_id, player.reason, player.preferred_name);
						}
					}
				} else {
					// Last draft of the season. Everyone in contention for a trophy must be in pod 1.
					log(LOG_LEVEL_INFO, "Week: %d, Last draft of the season", draft_code.value.week);

					// Find the points for third place
					int point_threshold = -9999;
					for(auto& player : sign_ups.value) {
						if(player.rank == 3) {
							point_threshold = player.points;
							break;
						}
					}

					for(auto& player : sign_ups.value) {
						// FIXME: This can be compressed to a group of ||'s, but for testing I'm expanding it to individual ifs
						// NOTE: If sign_ups is sorted by rank, as soon as we hit a player where +9 to their score doesn't put them in third place we can stop.
						if(player.reason == POD_ALLOCATION_REASON_UNALLOCATED) {
							if(player.points + 0 >= point_threshold) {
								player.reason = POD_ALLOCATION_REASON_CONTENTION;
								add_player_to_pod(get_next_empty_draft_pod_high(&tournament), player.member_id, player.reason, player.preferred_name);
							} else
							if(player.points + 3 >= point_threshold) {
								player.reason = POD_ALLOCATION_REASON_CONTENTION;
								add_player_to_pod(get_next_empty_draft_pod_high(&tournament), player.member_id, player.reason, player.preferred_name);
							} else
							if(player.points + 6 >= point_threshold) {
								player.reason = POD_ALLOCATION_REASON_CONTENTION;
								add_player_to_pod(get_next_empty_draft_pod_high(&tournament), player.member_id, player.reason, player.preferred_name);
							} else
							if(player.points + 9 >= point_threshold) {
								player.reason = POD_ALLOCATION_REASON_CONTENTION;
								add_player_to_pod(get_next_empty_draft_pod_high(&tournament), player.member_id, player.reason, player.preferred_name);
							}
						}
					}
				}

				// Sharks
				for(auto& player : sign_ups.value) {
					if(player.reason == POD_ALLOCATION_REASON_UNALLOCATED && player.is_shark == true) {
						player.reason = POD_ALLOCATION_REASON_SHARK;
						add_player_to_pod(get_next_empty_draft_pod_high(&tournament), player.member_id, player.reason, player.preferred_name);
					}
				}

				// At this point we have marked all people who should be in Pod 1. Find and assign hosts for all pods.

				// Anyone with devotion <= 4 and status == CASUAL has pod 2+ priority
				for(auto& player : sign_ups.value) {
					if(player.reason == POD_ALLOCATION_REASON_UNALLOCATED && player.devotion <= 4 && player.status == SIGNUP_STATUS_CASUAL) {
						player.reason = POD_ALLOCATION_REASON_NEWBIE;
						add_player_to_pod(get_next_empty_draft_pod_low(&tournament), player.member_id, player.reason, player.preferred_name);
					}
				}

				// Try to give COMPETITIVE and CASUAL players their preferred pod.
				for(auto& player : sign_ups.value) {
					if(player.reason == POD_ALLOCATION_REASON_UNALLOCATED && player.status == SIGNUP_STATUS_COMPETITIVE) {
						player.reason = POD_ALLOCATION_PREFERENCE;
						add_player_to_pod(get_next_empty_draft_pod_high(&tournament), player.member_id, player.reason, player.preferred_name);
					} else
					if(player.reason == POD_ALLOCATION_REASON_UNALLOCATED && player.status == SIGNUP_STATUS_CASUAL) {
						player.reason = POD_ALLOCATION_PREFERENCE;
						add_player_to_pod(get_next_empty_draft_pod_low(&tournament), player.member_id, player.reason, player.preferred_name);
					}
				}

				// Randomly assign FLEXIBLE players to whatever seats are available.
				for(auto& player : sign_ups.value) {
					if(player.reason == POD_ALLOCATION_REASON_UNALLOCATED && player.status == SIGNUP_STATUS_FLEXIBLE) {
						player.reason = POD_ALLOCATION_RANDOM;
						add_player_to_pod(get_random_empty_draft_pod(&tournament), player.member_id, player.reason, player.preferred_name);
					}
				}

			}

			// Check that all players have been allocated somewhere.
			for(auto& player : sign_ups.value) {
				if(player.reason == POD_ALLOCATION_REASON_UNALLOCATED) {
					log(LOG_LEVEL_ERROR, "Player %s has not been allocated to a pod", player.preferred_name);
				} else {
					log(LOG_LEVEL_INFO, "Player %s reason: %s", player.preferred_name, emoji_for_reason(player.reason));
				}
			}

			// Check that each pod has all seats filled
			for(int p = 0; p < tournament.pod_count; ++p) {
				const Draft_Pod *pod = &tournament.pods[p];
				if(pod->seats != pod->count) {
					log(LOG_LEVEL_ERROR, "Pod %d has %d empty seats", p, pod->seats - pod->count);
				} else {
					// Verify each seat has a valid player
					for(int s = 0; s < pod->seats; ++s) {
						if(pod->players[s].reason == POD_ALLOCATION_REASON_UNALLOCATED) {
							log(LOG_LEVEL_ERROR, "Pod %d seat %d is unallocated", p, s);
						}
					}
				}
			}

			std::string pod_allocations[PODS_MAX];
			for(int p = 0; p < tournament.pod_count; ++p) {
				pod_allocations[p] += fmt::format("## Pod {} Allocations:\n", p+1);
				const Draft_Pod* pod = &tournament.pods[p];
				for(int s = 0; s < pod->seats; ++s) {
					pod_allocations[p] += fmt::format("  {} <@{}>\n", emoji_for_reason(pod->players[s].reason), pod->players[s].member_id);
				}
			}

			// TODO: In case this isn't the first invocation of post_allocations we need a way
			// to clear old roles and recreate them...

			dpp::message message;
			message.set_content(fmt::format("# Pod allocations for {}\n", g_current_draft_code.c_str()));
			event.reply(message);

			// Create the "current draft" role and give it to all participants of this draft.
			dpp::role draft_role;
			draft_role.set_guild_id(guild_id);
			draft_role.set_name("current draft");
			draft_role.set_color(league->color);
			bot.role_create(draft_role, [&bot, guild_id, tournament, league, sign_ups, pod_allocations, draft](const dpp::confirmation_callback_t& callback) {
				if(!callback.is_error()) {
					// "current draft" role successfully created. Add it to the database so it can be deleted after the draft has finished.
					dpp::role draft_role = std::get<dpp::role>(callback.value);
					const auto result = database_add_temp_role(guild_id, g_current_draft_code.c_str(), draft_role.id);
					if(!is_error(result)) {
						for(int P = 0; P < tournament.pod_count; ++P) {
							// Create a role for each pod and assign it to all members in this pod.
							dpp::role pod_role;
							pod_role.set_guild_id(guild_id);
							pod_role.set_name(fmt::format("Pod-{}", P+1));
							pod_role.set_color(league->color);
							bot.role_create(pod_role, [&bot, draft_role, guild_id, tournament, P](const dpp::confirmation_callback_t& callback) {
								if(!callback.is_error()) {
									// Pod-X role successfully created.
									dpp::role pod_role = std::get<dpp::role>(callback.value);
									// TODO: Add temp role to database
									const auto result = database_add_temp_role(guild_id, g_current_draft_code.c_str(), pod_role.id);
									if(!is_error(result)) {
										// Give all members in this pod the draft_role and pod_role
										const Draft_Pod* pod = &tournament.pods[P];
										for(int player = 0; player < pod->count; ++player) {
											u64 member_id = pod->players[player].member_id;
											u64 draft_role_id = draft_role.id;
											u64 pod_role_id = pod_role.id;
											try {
												dpp::guild_member member = dpp::find_guild_member(guild_id, member_id);
												std::vector<dpp::snowflake> roles = member.get_roles();
												roles.push_back(draft_role_id);
												roles.push_back(pod_role_id);
												member.set_roles(roles);
												bot.guild_edit_member(member, [&bot, guild_id, member_id, draft_role_id, pod_role_id](const dpp::confirmation_callback_t& callback){
													if(!callback.is_error()) {
													} else {
														// TODO: Log and report error
													}
												});
											} catch(dpp::cache_exception& e) {
												log(LOG_LEVEL_ERROR, "Caught exception: %s", e.what());
											}

										}
									} else {
										// TODO Log and report error
									}
								} else {
									// TODO: Log and report error
								}
							});

							// Create the {Draft Code}-Pod-{X} threads. First we need to create a message to attach the thread to.
							dpp::message post;
							post.set_type(dpp::message_type::mt_default);
							post.set_guild_id(guild_id);
							post.set_channel_id(draft.value->reminder_channel_id);
							post.set_allowed_mentions(true, true, true, false, {}, {});
							post.set_content(pod_allocations[P]);
							// TODO: Need to pass in an array of members in this pod and why there were allocated here
							bot.message_create(post, [&bot, guild_id, P, tournament, draft](const dpp::confirmation_callback_t& callback) {
								if(!callback.is_error()) {
									// Message posted. Create a thread attached to it.
									const dpp::message& message = std::get<dpp::message>(callback.value);
									bot.thread_create_with_message(fmt::format("{} - Pod {}", g_current_draft_code, P + 1), draft.value->reminder_channel_id, message.id, 1440, 0, [&bot, guild_id, P, tournament](const dpp::confirmation_callback_t& callback) {
										if(!callback.is_error()) {
											//  Add the thread to the database with the draft code so when the draft is deleted the threads are archived.
											dpp::thread thread = std::get<dpp::thread>(callback.value);
#if 0
											auto result = database_add_temp_thread(guild_id, thread.id, g_current_draft_code.c_str());
											if(result != true) {
												log(LOG_LEVEL_ERROR, "Failed to add temporary thread %lu to database",(u64)thread.id);
											}
#endif

											// Just in case a member has the bot blocked, add members to the thread so they can see it.
											const Draft_Pod* pod = &tournament.pods[P];
											for(int player = 0; player < pod->count; ++player) {
												bot.thread_member_add(thread.id, pod->players[player].member_id, [&bot](const dpp::confirmation_callback_t& callback) {
													if(callback.is_error()) {
														// TODO: Log and report error
													}
												});
											}

											const auto draft = database_get_event(guild_id, g_current_draft_code);
											if(!is_error(draft)) {
												const bool is_draftmancer = draft.value->draftmancer_draft;
												std::string text;
												if(!is_draftmancer) {
													text += "# The draft table for your pod will be created shortly. Please join ASAP.\n\n";
												} else {
													text += "# The Draftmancer link for your pod will be posted shortly. Please join ASAP and set your name to match your XDHS Discord name.\n\n";
												}
												text += "## While drafting, please remember to:\n";
												text += "* **Keep Discord open, with notifications on.** Pay attention to pings and messages in this thread. We may need to restart the draft if there's a disconnect or other issue.\n";
												text += "* **Double click to actively select your pick before the timer runs out.** This is good etiquette to keep the draft moving at a reasonable pace.\n";

												if(!is_draftmancer) {
													text += "* **Save your deck during deck construction.** Very important if we need to play side matches or host a new tournament.\n";
												} else {
													text += fmt::format("* **Log into the {} XMage server.** Make sure your usernames match everywhere.\n", draft.value->xmage_server);
													text += "* During the draft, you can hold Alt while hovering over a card to show the Oracle text.\n";
												}
												text += "\nGood luck and have fun!";

												// Post a message in the new thread.
												dpp::message post;
												post.set_type(dpp::message_type::mt_default);
												post.set_guild_id(thread.guild_id);
												post.set_channel_id(thread.id);
												post.set_content(text);
												bot.message_create(post, [](const dpp::confirmation_callback_t& callback) {
													if(!callback.is_error()) {
													} else {
														log(LOG_LEVEL_ERROR, callback.get_error().message.c_str());
													}
												});
											}
										} else {
											log(LOG_LEVEL_ERROR, callback.get_error().message.c_str());
										}
									});
								} else {
									log(LOG_LEVEL_ERROR, callback.get_error().message.c_str());
								}
							});
						}
					}
				} else {
					log(LOG_LEVEL_ERROR, "Failed to create role for %s.", g_current_draft_code.c_str());
				}
			});
		} else
#if 0
		if(command_name == "fire") {
			// TODO: Check the draft status is locked, just to be safe
			const auto guild_id = event.command.get_guild().id;

			const auto draft = database_get_event(guild_id, g_current_draft_code);
			if(draft == false) return; // TODO: Error message.

			std::string text;
			text += "## While drafting, please remember to:\n";
			text += "* **Keep Discord open, with notifications on.** Pay attention to pings and messages in this channel. We may need to restart the draft if there's a disconnect or other issue.\n";
			text += "* **Double click to actively select your pick before the timer runs out.** This is good etiquette to keep the draft moving at a reasonable pace.\n";

			if(!draft.value->draftmancer_draft) {
				// XMage
				text += "* **Save your deck during deck construction.** Very important if we need to play side matches or host a new tournament.\n";
			} else {
				// Draftmancer
				text += fmt::format("* Join the link posted by the host for your pod, and _also_ log into the {} XMage server. Make sure your usernames match everywhere.\n", draft.value->xmage_server);
				text += "* During the draft, you can hold Alt while hovering over a card to show the Oracle text.\n";
			}

			event.reply(text);

			// TODO: Need to lock the draft entirely.... what if we have a dropper and they quit the draft?
		} else
#endif
		if(command_name == "timer") {
			// TODO: Ping everyone in this pod? How would we know what pod this is for?
			//const auto guild_id = event.command.get_guild().id;
			time_t now = time(0) + DECK_CONSTRUCTION_MINUTES;
			std::string text = fmt::format("* Now the draft has finished, you can build your deck either on Draftmancer or XMage. Export as MTGA format from Draftmancer, then import to XMage from the clipboard. Don't forget to choose your favorite basic land art!\n* Make sure not to double-click cards while editing your deck in XMage (that will remove the card from your deck rather than moving it to the sideboard, and you'll have to reimport to fix it). Drag and drop instead.\n* Save your deck when done building and join the XMage table for your pod when it goes up.\n\n:alarm_clock: The timer for deck construction expires **<t:{}:R>**", now);
			event.reply(text);
		} else
		if(command_name == "dropper") {
			const auto guild_id = event.command.get_guild().id;
			const auto member_id = std::get<dpp::snowflake>(event.get_parameter("member"));

			std::string note;
			auto opt = event.get_parameter("note");
			if(std::holds_alternative<std::string>(opt)) {
				note = std::get<std::string>(opt);
			}

			auto result = database_add_dropper(guild_id, member_id, g_current_draft_code, note);
			if(!is_error(result)) {
				const std::string preferred_name = get_members_preferred_name(guild_id, member_id);
				event.reply(fmt::format("Incremented drop count for {}.", preferred_name));
			} else {
				event.reply(result.errstr);
			}
		} else
		if(command_name == "finish") {
			std::string text;
			text += "## Thanks everyone for drafting with us!\n";
			text += fmt::format("* You can share a screenshot of your deck in <#{}>.\n", DECK_SCREENSHOTS_CHANNEL_ID);
			text += fmt::format("* If you want feedback on your draft, just ask or give yourself the Civilized Scholar role in <#{}>).\n", ROLE_SELF_ASSIGNMENT_CHANNEL_ID);
			text += fmt::format("* You can also upload your draftlog to <https://magic.flooey.org/draft/upload> and share it in <#{}>.\n", P1P1_AND_DRAFT_LOG_CHANNEL_ID);
			text += fmt::format("* We're happy to hear feedback on how to improve, either in <#{}> or anonymously with the /feedback command.\n", FEEDBACK_CHANNEL_ID);
			text += fmt::format("* Check out <#{}> and sign up to some upcoming events!", CALENDAR_CHANNEL_ID);
			event.reply(text);
		} else
		if(command_name == "stats") {
			const auto guild_id = event.command.get_guild().id;
			const dpp::user& issuing_user = event.command.get_issuing_user();

			const std::string preferred_name = get_members_preferred_name(guild_id, issuing_user.id);

			auto stats = database_get_stats(issuing_user.id);
			if(has_value(stats)) {
				if(stats.count == 1) {
					event.reply(fmt::format("{}, your stats will be delivered via private message.", preferred_name));
					dpp::embed embed;

					embed.set_title(fmt::format("Hello, {}! Here are your stats:", preferred_name));

					embed.add_field("Devotion Badge", stats.value.devotion.name, true);
					embed.add_field("Devotion Points", fmt::format("{}", stats.value.devotion.value), true);
					embed.add_field("Points needed for next badge", fmt::format("{}", stats.value.devotion.next), true);

					embed.add_field("Victory Badge", stats.value.victory.name, true);
					embed.add_field("Victory Points", fmt::format("{}", stats.value.victory.value), true);
					embed.add_field("Points needed for next badge", fmt::format("{}", stats.value.victory.next), true);

					embed.add_field("Trophy Badge", stats.value.trophies.name, true);
					embed.add_field("Trophy Points", fmt::format("{}", stats.value.trophies.value), true);
					embed.add_field("Points needed for next badge", fmt::format("{}", stats.value.trophies.next), true);

					embed.add_field("Shark Badge", stats.value.shark.name, true);
					embed.add_field("Shark Kills", fmt::format("{}", stats.value.shark.value), true);
					embed.add_field("Kills needed for next badge", fmt::format("{}", stats.value.shark.next), true);

					embed.add_field("Draft Hero Badge", stats.value.hero.name, true);
					embed.add_field("Hero Points", fmt::format("{}", stats.value.hero.value), true);
					embed.add_field("Points needed for next badge", fmt::format("{}", stats.value.hero.next), true);

					{
						std::string wins[3];
						wins[0] = stats.value.win_rate_recent.chrono  > 0.0f ? fmt::format("{:.1f}%", stats.value.win_rate_recent.chrono)  : "-";
						wins[1] = stats.value.win_rate_recent.bonus   > 0.0f ? fmt::format("{:.1f}%", stats.value.win_rate_recent.bonus)   : "-";
						wins[2] = stats.value.win_rate_recent.overall > 0.0f ? fmt::format("{:.1f}%", stats.value.win_rate_recent.overall) : "-";
						embed.add_field("Chrono win rate (last 6 seasons)", wins[0], true);
						embed.add_field("Bonus win rate (last 6 seasons)", wins[1], true);
						embed.add_field("Overall win rate (last 6 seasons)", wins[2], true);
					}
					{
						std::string wins[3];
						wins[0] = stats.value.win_rate_all_time.chrono  > 0.0f ? fmt::format("{:.1f}%", stats.value.win_rate_all_time.chrono)  : "-";
						wins[1] = stats.value.win_rate_all_time.bonus   > 0.0f ? fmt::format("{:.1f}%", stats.value.win_rate_all_time.bonus)   : "-";
						wins[2] = stats.value.win_rate_all_time.overall > 0.0f ? fmt::format("{:.1f}%", stats.value.win_rate_all_time.overall) : "-";
						embed.add_field("Chrono win rate (all time)", wins[0], true);
						embed.add_field("Bonus win rate (all time)", wins[1], true);
						embed.add_field("Overall win rate (all time)", wins[2], true);
					}

					embed.set_timestamp(stats.value.timestamp);
					embed.set_footer("Stats last updated", "https://i.imgur.com/NPtgFpC.png");

					dpp::message message;
					message.add_embed(embed);

					if(strlen(stats.value.badge_card.url) > 0) {
						dpp::embed badge_card;
						badge_card.set_title("Your badge card:");
						badge_card.set_image(stats.value.badge_card.url);
						badge_card.set_timestamp(stats.value.badge_card.timestamp);
						badge_card.set_footer("Badge card last updated", "https://i.imgur.com/NPtgFpC.png");
						message.add_embed(badge_card);
					}

					bot.direct_message_create(issuing_user.id, message);
				} else {
					event.reply(fmt::format("No stats found for {}. You must complete at least one XDHS draft first.", preferred_name));
				}
			} else {
				event.reply(fmt::format("Sorry {}, there was an error retrieving your stats. This is not your fault! Please wait a few minutes and try again.", preferred_name));
			}
		} else
		if(command_name == "help") {
			const auto guild_id = event.command.get_guild().id;
			std::string message = std::get<std::string>(event.get_parameter("message"));
			if(message == "all_commands") {
				auto result = database_get_all_help_messages(guild_id);
				if(has_value(result)) {
					std::string content;
					content.reserve(DISCORD_MESSAGE_CHARACTER_LIMIT);
					//content += "```";
					dpp::message msg;
					for(auto& help_message : result.value) {
						//content += fmt::format("{:<{}} - {}\n", help_message.name, 32, help_message.summary);
						content += fmt::format("{}\n", help_message.summary);
					}
					//content += "```";
					msg.set_content(content);
					msg.set_flags(dpp::m_ephemeral);
					event.reply(msg);
				} else {
					// TODO: Log database error and return message
				}
			} else {
				auto result = database_get_help_message(guild_id, message);
				if(has_value(result)) {
					std::string content;
					dpp::message msg;

					if(result.value.host == true) {
						// Check the user has the HOST role.
						const dpp::user& issuing_user = event.command.get_issuing_user();
						dpp::guild_member member = dpp::find_guild_member(guild_id, issuing_user.id);
						std::vector<dpp::snowflake> roles = member.get_roles();
						bool is_host = false;
						for(auto role : roles) {
							if(role == XDHS_HOST_ROLE_ID) {
								is_host = true;
								break;
							}
						}

						if(is_host == true) {
							msg.set_content(result.value.content);
						} else {
							msg.set_content("The Host role is required to post this help message.");
							msg.set_flags(dpp::m_ephemeral);
						}
					} else {
						msg.set_content(result.value.content);
					}

					event.reply(msg);
				} else {
					// TODO: Log database error and return message
				}
			}
		} else {
			log(LOG_LEVEL_ERROR, "No handler for '{}' command.", command_name);
		}
	});

	bot.on_button_click([&bot](const dpp::button_click_t& event) {
		const u64 guild_id = event.command.get_guild().id;
		const u64 member_id = (u64)event.command.usr.id;

		const std::string preferred_name = get_members_preferred_name(guild_id, member_id);

		log(LOG_LEVEL_DEBUG, "preferred_name:'%s' (%lu) from %lu clicked %s", preferred_name.c_str(), (u64)event.command.usr.id, (u64)event.command.guild_id, event.custom_id.c_str());

		const std::string draft_code = event.custom_id.substr(0, event.custom_id.find('_'));

		// Get the current sign up status (if any) for this member.
		auto current_sign_up_status = database_get_members_sign_up_status(guild_id, draft_code, member_id);
		if(is_error(current_sign_up_status)) {
			log(LOG_LEVEL_ERROR, current_sign_up_status.errstr);
			// TODO: Now what? Send an error to #bot-commands or something?
		}

		auto get_sign_up_type = [](const std::string& str) -> SIGNUP_STATUS {
			size_t idx = str.find('_');
			if(idx != std::string::npos) {
				std::string type = str.substr(idx + 1);
				if(type == "competitive")     return SIGNUP_STATUS_COMPETITIVE;
				else if(type == "casual")     return SIGNUP_STATUS_CASUAL;
				else if(type == "flexible")   return SIGNUP_STATUS_FLEXIBLE;
				else if(type == "tentative")  return SIGNUP_STATUS_TENTATIVE;
				else if(type == "minutemage") return SIGNUP_STATUS_MINUTEMAGE;
				else if(type == "decline")    return SIGNUP_STATUS_DECLINE;
			}
			// Should be impossible to ever get here...
			log(LOG_LEVEL_ERROR, "Unknown sign up type: '%s'", str.c_str());
			return SIGNUP_STATUS_INVALID;
		};

		SIGNUP_STATUS new_sign_up_status = get_sign_up_type(event.custom_id);

		time_t now = time(NULL);

		const time_t time_to_use[] = {
			now,
			current_sign_up_status.value.timestamp
		};

		// -1 = Can't happen, 0 = Set the timestamp to now, 1 = Keep the timestamp from the previous button click event.
		static const int time_to_use_index_lookup[7][7] = {
			/*                   none, competitive, casual, flexible, tentative, minutemage, declined */
			/* none */        {  -1,   0,           0,      0,        0,         0,          0},
			/* competitive */ {  -1,   1,           1,      1,        0,         0,          0},
			/* casual */      {  -1,   1,           1,      1,        0,         0,          0},
			/* flexible */    {  -1,   1,           1,      1,        0,         0,          0},
			/* tentative */   {  -1,   0,           0,      0,        1,         0,          0},
			/* minutemage */  {  -1,   0,           0,      0,        0,         1,          0},
			/* declined */    {  -1,   0,           0,      0,        0,         0,          1},
		};

		auto demask = [](SIGNUP_STATUS status) -> int {
			switch(status) {
				case SIGNUP_STATUS_NONE:        return 0;
				case SIGNUP_STATUS_COMPETITIVE: return 1;
				case SIGNUP_STATUS_CASUAL:      return 2;
				case SIGNUP_STATUS_FLEXIBLE:    return 3;
				case SIGNUP_STATUS_TENTATIVE:   return 4;
				case SIGNUP_STATUS_MINUTEMAGE:  return 5;
				case SIGNUP_STATUS_DECLINE:     return 6;
				case SIGNUP_STATUS_INVALID:
				default:                        return -1;
			}
		};

		int index = time_to_use_index_lookup[demask(current_sign_up_status.value.status)][demask(new_sign_up_status)];
		time_t timestamp = time_to_use[index];
		(void)database_sign_up_to_a_draft(guild_id, draft_code, member_id, preferred_name, timestamp, new_sign_up_status);

		const auto draft = database_get_event(guild_id, draft_code);
		if(is_error(draft)) {
			log(LOG_LEVEL_ERROR, "database_get_event(%lu, %s) failed" , guild_id, draft_code.c_str());
		}

		event.reply(); // Acknowledge the interaction, but show nothing to the user.

		// The sign up sheet in the #-pre-register channel.
		redraw_signup(bot, guild_id, draft.value->signups_id, draft.value->signup_channel_id, draft.value);

		// The sign up sheet on the reminder message sent to #-in-the-moment-draft
		if(draft.value->reminder_id != 0) {
			redraw_signup(bot, guild_id, draft.value->reminder_id, draft.value->reminder_channel_id, draft.value);
		}

		// If the draft has been locked, alert the hosts when someone has clicked the minutemage button.
		if(new_sign_up_status == SIGNUP_STATUS_MINUTEMAGE) {
			time_t draft_start = unpack_and_make_timestamp(draft.value->time, draft.value->time_zone);
			if(now >= draft_start) {
				send_message(bot, GUILD_ID, draft.value->hosting_channel_id, fmt::format(":warning: {} signed up as a minutemage. :warning:", preferred_name));
			}
		}
	});

	// Called when a member leaves a server or is kicked.
	bot.on_guild_member_remove([&bot](const dpp::guild_member_remove_t& event) {
		const u64 guild_id = event.guild_id;
		const u64 member_id = event.removed.id;

		if(!is_error(database_delete_member_from_all_sign_ups(guild_id, member_id))) {
			// Remove them from the sign up sheet of any upcoming drafts.
			const auto drafts = database_get_all_events(guild_id);
			if(!is_error(drafts)) {
				for(const auto& D : drafts.value) {
					const std::shared_ptr<Draft_Event> draft = std::make_shared<Draft_Event>(D);
					if(draft->signups_id != 0) {
						redraw_signup(bot, GUILD_ID, draft->signups_id, draft->signup_channel_id, draft);
						if(draft->reminder_id != 0) {
							redraw_signup(bot, GUILD_ID, draft->reminder_id, draft->reminder_channel_id, draft);
						}
					}
				}
			} else {
				log(LOG_LEVEL_ERROR, "database_get_all_events(%lu) failed", guild_id);
			}
		} else {
			log(LOG_LEVEL_ERROR, "database_delete_member_from_all_sign_ups(%lu, %lu) failed", guild_id, member_id);
		}

		send_message(bot, GUILD_ID, BOT_COMMANDS_CHANNEL_ID, fmt::format("Member '{}' (ID:{}) has left the server.", event.removed.username, event.removed.id));
	});

	// Called when the bot has successfully connected to Discord.
	bot.on_ready([&bot](const dpp::ready_t& event) {
		// Discord will now call on_guild_create for each guild this bot is a member of.
	});


	bot.start(true);

	bot.start_timer([&bot](dpp::timer t) {
		set_bot_presence(bot);

		auto draft_code = database_get_next_upcoming_draft(GUILD_ID);
		if(is_error(draft_code)) {
			return;
		}
		if(draft_code.count == 0) return; // No scheduled drafts.

		g_current_draft_code = draft_code.value;

		auto draft = database_get_event(GUILD_ID, draft_code.value);
		if(is_error(draft_code) || draft.count == 0) {
			return;
		}

		time_t now = time(NULL);

		time_t draft_start = unpack_and_make_timestamp(draft.value->time, draft.value->time_zone);

		// Send the pre-draft reminder message if it hasn't already been sent.
		if(!(BIT_SET(draft.value->status, DRAFT_STATUS_REMINDER_SENT)) && (draft_start - now <= SECONDS_BEFORE_DRAFT_TO_SEND_REMINDER)) {
			send_message(bot, GUILD_ID, BOT_COMMANDS_CHANNEL_ID, fmt::format("{} - Sending pre-draft reminder message and unlocking minutemage sign up.", draft_code.value.c_str()));
			// TODO: Remove mentions on this when the draft is fired?
			post_pre_draft_reminder(bot, GUILD_ID, draft_code.value.c_str());
		}

		// Ping the tentatives if they haven't already been pinged.
		if(!(BIT_SET(draft.value->status, DRAFT_STATUS_TENTATIVES_PINGED)) && (draft_start - now <= SECONDS_BEFORE_DRAFT_TO_PING_TENTATIVES)) {
			send_message(bot, GUILD_ID, BOT_COMMANDS_CHANNEL_ID, fmt::format("{} - Sending tentative reminder message, if needed.", draft_code.value.c_str()));
			// Redraw the sign up posts so the Tentative button shows as locked.
			ping_tentatives(bot, GUILD_ID, draft_code.value.c_str());
		}

		// Lock the draft.
		if((draft.value->status < DRAFT_STATUS_LOCKED) && now >= draft_start) {
			send_message(bot, GUILD_ID,BOT_COMMANDS_CHANNEL_ID, fmt::format("{} - Locking sign ups and pinging for a minutemage, if needed.", draft_code.value.c_str()));
			database_set_draft_status(GUILD_ID, draft_code.value, DRAFT_STATUS_LOCKED);

			// Ping minutemages if there is an odd number of confirmed sign ups.
			ping_minutemages(bot, GUILD_ID, draft_code.value.c_str());

			post_host_guide(bot, draft_code.value.c_str());
		}

		// Delete the draft after a few hours.
		if((draft.value->status < DRAFT_STATUS_COMPLETE) && now - draft_start > SECONDS_AFTER_DRAFT_TO_DELETE_POSTS) {
			send_message(bot, GUILD_ID, BOT_COMMANDS_CHANNEL_ID, fmt::format("{} - Deleting completed draft.", draft_code.value.c_str()));
			delete_draft_posts(bot, GUILD_ID, draft_code.value);
			// TODO: Test this is actually working!
			delete_temp_roles(bot, GUILD_ID, draft_code.value);
			database_clear_draft_post_ids(GUILD_ID, draft_code.value);
			database_set_draft_status(GUILD_ID, draft_code.value, DRAFT_STATUS_COMPLETE);

			// TODO: Delete/archive any threads created?
		}

	}, JOB_THREAD_TICK_RATE, [](dpp::timer){});

	while(g_exit_code == 0) {
		sleep(1);
	}

	bot.shutdown();
	mysql_library_end();
	curl_global_cleanup();

	log(LOG_LEVEL_INFO, "Exiting");

	log_close();

	return g_exit_code;
}

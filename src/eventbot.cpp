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

// TODO: Cleanup inconsistent use of char* and std::string in database functions.
// TODO: All the blit_ functions can be rewritten to use SIMD ops
// FIXME: Tried to /remove_player but they were still counted in the player list when I used /post_allocations
// TODO: Alert hosts when a drafter is a first time player
// TODO: Get rid of all asserts - can't have the bot going offline, ever

// C libraries
#include <alloca.h>
#include <assert.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// C++ libraries
#include <cinttypes>
#include <iostream> // TESTING ONLY - REMOVE!

// System libraries
#include <curl/curl.h>
#include <mysql.h>

// User libraries
#include <dpp/dpp.h>
#include <fmt/format.h>

// Local libraries
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_FAILURE_USRMSG
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS (1<<11)
#include "stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "date/tz.h"  // Howard Hinnant's date and timezone library.
#include "log.h"
#include "config.h"
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

#define BIT_SET(value,mask) (((value) & (mask)) == (mask))

// FIXME: This is an awful hack so I don't have to deal with timezone conversion stuff. Add this to timestamps inserted in the database by Badge Bot by this amount. This is the UTC offset of where the server running this code is.
static const int SERVER_TIMEZONE_OFFSET = (60*60*10);
 
// How many seconds before a draft the pre-draft reminder message should be sent.
static const time_t SECONDS_BEFORE_DRAFT_TO_SEND_REMINDER   = (60*60*1);

// How many seconds before a draft to remind tentatives to confirm their status.
static const time_t SECONDS_BEFORE_DRAFT_TO_PING_TENTATIVES = (60*10);

// How long after a draft to wait before deleting the signup posts from the #-pre-register channel.
static const time_t SECONDS_AFTER_DRAFT_TO_DELETE_POSTS     = (60*60*5);

// How often often to spin up the thread that sends the pre-draft reminders, tentatives ping, etc.
static const dpp::timer JOB_THREAD_TICK_RATE                = 15;

// How long we allow for deck construction
static const time_t DECK_CONSTRUCTION_MINUTES               = (10*60);

// The bot is designed to run in two modes, Debug and Release. Debug builds will only run on the XDHS Dev server and Release builds will only run on the public XDHS server.
// In the future we might want to control these values with a bot command, but for now we'll simply hard code them in.
#ifdef DEBUG
// The bot will be running in debug mode on the XDHS Dev server.
static const char* g_build_mode                  = "Debug";
static const u64 GUILD_ID                        = 882164794566791179;
static const u64 PRE_REGISTER_CHANNEL_ID         = 907524659099099178; // Default channel to post the draft signup.
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
static bool g_commands_registered                = false; // Have the bot slash commands been registered for this guild?
#else
// The bot will be running in release mode on the XDHS public server.
static const char* g_build_mode                  = "Release";
static const u64 GUILD_ID                        = 528728694680715324;
static const u64 PRE_REGISTER_CHANNEL_ID         = 753639027428687962; // Default channel to post the draft signup.
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
static bool g_commands_registered                = false; // Have the bot slash commands been registered for this guild?;
#endif

// Some serious errors will ping this person as the error needs attention ASAP.
static const u64 TANDEM_DISCORD_ID               = 767299805348233217;

// The name of the currently active draft or the next upcoming draft.
// NOTE: When the bot starts there is a brief window of time where this has not yet been set.
static std::string g_current_draft_code;

// g_quit will be set to true if we get a signal from the OS to exit.
// This signal handling is extremely basic but it should be all this bot needs.
static bool g_quit     = false;
static int g_exit_code = 0;

static void sig_handler(int signo) {
	// TODO: All database writes need to be done as transactions so a sudden shutdown of the service here won't mess up the database.
    switch(signo) {
        case SIGINT:  // Fall through
        case SIGABRT: // Fall through
        case SIGHUP:  // Fall through
        case SIGTERM:
            log(LOG_LEVEL_INFO, strsignal(signo));
            g_quit = true;
        	break;

        default: log(LOG_LEVEL_INFO, "Caught unhandled signal: %d", signo);
    }
    g_exit_code = signo;
}

static std::string to_upper(const char* src) {
	const size_t len = strlen(src);
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



static const size_t DOWNLOAD_BYTES_MAX = (3 * 1024 * 1024);

struct curl_mem_chunk {
    size_t size;
    u8* data;
};

static size_t curl_write_memory_callback(void* data, size_t size, size_t nmemb, void* usr_ptr) {
    size_t real_size = size * nmemb;
    curl_mem_chunk* mem = (curl_mem_chunk*)usr_ptr;

    // TODO: Keep track of how much we've allocated so far and error out if over some maximum amount to prevent denial of service attacks from giant files being sent.

    u8* ptr = (u8*) realloc(mem->data, mem->size + real_size);
    if(ptr == NULL) {
		log(LOG_LEVEL_ERROR, "%s: out of memeory", __FUNCTION__); // TODO: Now what? On the potato server EventBot is on this could happen...
        return 0;
    }

    mem->data = ptr;
    memcpy(mem->data + mem->size, data, real_size);
    mem->size += real_size;

    return real_size;
}

enum DOWNLOAD_IMAGE_RESULT {
    DOWNLOAD_IMAGE_RESULT_OK,
    DOWNLOAD_IMAGE_RESULT_CURL_INIT_ERROR,
    DOWNLOAD_IMAGE_RESULT_CURL_ERROR
};

static DOWNLOAD_IMAGE_RESULT download_file(const char* url, size_t* size, u8** data) {
    curl_global_init(CURL_GLOBAL_DEFAULT); // TODO: Can this be done once at startup?

    CURL* curl = curl_easy_init();
    if(curl == NULL) {
        return DOWNLOAD_IMAGE_RESULT_CURL_INIT_ERROR;
    }

    // Disable checking SSL certs
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    // Disable checking SSL certs are valid
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    //curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    curl_mem_chunk chunk;
    chunk.size = 0;
    chunk.data = NULL;

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_memory_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);

    CURLcode result = curl_easy_perform(curl);
    if(result != CURLE_OK) {
        // TODO: Need to pass in a **error_str variable to get these error messages from curl?
		log(LOG_LEVEL_ERROR, "curl_easy_perform() failed: %s", curl_easy_strerror(result));
        return DOWNLOAD_IMAGE_RESULT_CURL_ERROR;
    }

    //printf("downloaded %lu bytes\n", chunk.size);

    curl_easy_cleanup(curl);

    curl_global_cleanup(); // TODO: Can this be done once at shutdown?

    *size = chunk.size;
    *data = chunk.data;

    return DOWNLOAD_IMAGE_RESULT_OK;
}



// Send a message to a channel. Mostly used for posting what the bot is currently doing.
static void send_message(dpp::cluster& bot, const u64 channel_id, const std::string& text) {
	dpp::message message;
	message.set_type(dpp::message_type::mt_default);
	message.set_guild_id(GUILD_ID);
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
	{"SOI", "Shadows Over Innistrad",                      3, false},
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
	{"MOM", "March of the Machine",                        1, false},
	{"LTR", "The Lord of the Rings: Tales of Middle-earth",1, false},
	{"WOE", "Wilds of Eldraine",                           1, false},
	{"LCI", "Lost Caverns of Ixalan",                      1, false},
	{"RVR", "Ravnica Remastered",                          1, false},

	{"INVR", "Invasion Remastered",                        0,  true},
	{"KMGR", "Kamigawa Remastered",                        0,  true},
	{"PMMA", "Pre Mirage Masters",                         0, false},
	{"GBMA", "Garbage Masters",                            0, false},
	{"SLCS", "The Sliver Core Set",                        0, false},
	{"USGR", "Urza Block Redeemed",                        0, false},
	{"TWAR", "Total WAR",                                  0, false},
	{"ATQR", "Antiquities Reforged",                       0, false},
	{"ISDR", "Innistrad Remastered ",                      0, false},
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

	char token[128]; // FIXME: A malicious or careless user could overflow this...
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
		while(*end != '/' && *end != '\\' && *end != '|' && *end != '\0') {
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
struct Start_Time {
	int hour;
	int minute;
};

// The maximum number of leagues to be pinged when a draft signup is posted. Increase this if a league ever needs to ping more than two roles.
static const size_t LEAGUE_PINGS_MAX = 2; 

enum LEAGUE_ID {
	LEAGUE_ID_AMERICAS_CHRONO,
	LEAGUE_ID_EURO_CHRONO,
	LEAGUE_ID_ASIA_CHRONO,
	LEAGUE_ID_PACIFIC_CHRONO,
	LEAGUE_ID_ATLANTIC_BONUS,
	LEAGUE_ID_AMERICAS_BONUS,
	LEAGUE_ID_EURO_BONUS
};

static const char* to_cstring(const LEAGUE_ID id) {
	switch(id) {
		case LEAGUE_ID_AMERICAS_CHRONO: return "Americas Chrono";
		case LEAGUE_ID_EURO_CHRONO:     return "Euro Chrono";
		case LEAGUE_ID_ASIA_CHRONO:     return "Asia Chrono";
		case LEAGUE_ID_PACIFIC_CHRONO:  return "Pacific Chrono";
		case LEAGUE_ID_ATLANTIC_BONUS:  return "Atlantic Bonus";
		case LEAGUE_ID_AMERICAS_BONUS:  return "Americas Bonus";
		case LEAGUE_ID_EURO_BONUS:      return "Euro Bonus";
	}

	return NULL;
}

struct XDHS_League {
	LEAGUE_ID id;
	char region_code;                   // (E)uro, (A)mericas, A(S)ia, (P)acific, A(T)lantic
	char league_type;                   // (C)hrono or (B)onus
	u32 color;                          // Color for the league
	const char* time_zone;              // IANA time zone identifier
	Start_Time time;                    // When the draft starts
	const char* ping[LEAGUE_PINGS_MAX]; // Which roles to ping when the signup goes up
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
		LEAGUE_ID_ATLANTIC_BONUS,
		'T','B',
		0x00ed8821,
		"Europe/Berlin",
		{19,50},
		{"Euro", "Americas"},
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
static const size_t LEAGUE_COUNT = sizeof(g_xdhs_leagues) / sizeof(XDHS_League);

// Parse a draft code and find the league it is for. Makes a copy into 'league' and returns true if found, false otherwise.
// TODO: Replace this with parse_league_code
static const XDHS_League* get_league_from_draft_code(const char* draft_code) {
	// SSS.W-RT S=season, W=week, R=region, T=type
	if(draft_code == NULL) return NULL;

	while(isdigit(*draft_code)) {draft_code++;} // Skip the numeric part
	if(*draft_code++ != '.') return NULL;
	while(isdigit(*draft_code)) {draft_code++;} // Skip the numeric part
	if(*draft_code++ != '-') return NULL;
	char region_code = *draft_code++;
	char league_type = *draft_code;

	for(size_t i = 0; i < LEAGUE_COUNT; ++i) {
		const XDHS_League* ptr = &g_xdhs_leagues[i];
		if((ptr->region_code == region_code) && (ptr->league_type == league_type)) {
			// Found it!
			return ptr;
		}
	}

	return NULL;
}

// FIXME: Get rid of this!
static void make_2_digit_league_code(const XDHS_League* league, char out[3]) {
	out[0] = league->region_code;
	out[1] = league->league_type;
	out[2] = 0;
}


// The maximum allowed byte length of a draft code.
static const size_t DRAFT_CODE_LENGTH_MAX = strlen("SSS.GG-LT");

struct Draft_Code {
	u16 season; // max 3 digits
	u8 week; // max 2 digits
	const XDHS_League *league;
};

static bool parse_draft_code(const char* draft_code, Draft_Code* out) {
	if(draft_code == NULL) return false;
	const size_t len = strlen(draft_code);
	if(len > DRAFT_CODE_LENGTH_MAX) return false;
	char str[DRAFT_CODE_LENGTH_MAX]; // Mutable copy
	memcpy(str, draft_code, len);

	char* start = str;
	char* end = str;

	// Season
	while(isdigit(*end)) end++;
	if(*end != '.') return false;
	*end = 0;
	if(strlen(start) > 3) return false;
	out->season = strtol(start, NULL, 10);
	start = end++;

	// Week
	while(isdigit(*end)) end++;
	if(*end != '-') return false;
	*end = 0;
	if(strlen(start) > 2) return false;
	out->week = strtol(start, NULL, 10);
	start = end++;

	const char region_code = *end++;
	const char league_type = *end;

	for(size_t i = 0; i < LEAGUE_COUNT; ++i) {
		if((g_xdhs_leagues[i].region_code == region_code) && (g_xdhs_leagues[i].league_type == league_type)) {
			out->league = &g_xdhs_leagues[i];
			return true;
		}
	}

	return false;
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
	while(isdigit(*str)) str++;       \
	if(*str != '-' && *str != '.' && *str != '\\' && *str != '/' && *str != '\0') { \
		return "Invalid digit separator.";  \
	} \
	*str++ = 0; \
	if(strlen(start) < min_len || strlen(start) > max_len) return "Date should be written as YYYY-MM-DD"; \
	out = strtol(start, NULL, 10); \
}
static const char* parse_date_string(const char* date_string, Date* out) {
	if(strlen(date_string) < strlen("YY-M-D")) return "String is too short to contain a valid date. Date should be written as YYYY-MM-DD.";
	if(strlen(date_string) > strlen("YYYY-MM-DD")) return "String is too long. Date should be written as YYYY-MM-DD.";

	// Make a mutable copy of the date string, including terminator.
	char str[strlen("YYYY-MM-DD")+1];
	memcpy(str, date_string, strlen(date_string)+1);
	char* str_ptr = str;

	split_date(str_ptr, 2, 4, out->year);
	split_date(str_ptr, 1, 2, out->month);
	split_date(str_ptr, 1, 2, out->day);

	if(out->year <= 99) out->year += 2000;
	
	// String parsed - check if this looks like a valid date.
	// TODO: The date library probably could do this, right?

	time_t current_time = time(NULL);
	struct tm t = *localtime(&current_time);
	int current_year = t.tm_year + 1900;

	// TODO: Check the entire date is in the future.

	if(out->year < current_year) {
		return "Date is in the past and time travel does not yet exist.";
	}

	if(out->day < 1) return "Day should be between 01 and 31.";

	if(out->month < 1 || out->month > 12) {
		return "Month should be between 01 and 12.";
	} else
	if(out->month == 1 || out->month == 3 || out->month == 5 || out->month == 7 || out->month == 8 || out->month == 10 || out->month == 12) {
		if(out->day > 31) return "Day should be between 01 and 31 for the specified month.";
	} else
	if (out->month == 4 || out->month == 6 || out->month == 9 || out->month == 11) {
		if(out->day > 30) return "Day should be between 01 and 30 for the specified month.";	
	} else {
		// Febuary
		if(((out->year % 4 == 0) && (out->year % 100 != 0)) || (out->year % 400 == 0)) {
			// Leap year
			if(out->day > 29) return "Day should be between 01 and 29 for the specified month.";
		} else {
			if(out->day > 28) return "Day should be between 01 and 28 for the specified month.";
		}
	}

	return NULL;
}

// Do some rudimentary validation on the start time string sent with create_draft command and parse the provided values. Returns true and fills the 'out' variable if no problem was found, false otherwise.
// TODO: Return error strings like the above function?
static bool parse_start_time_string(const char* start_time_string, Start_Time* out) {
	if(start_time_string == NULL) return false;
	if(strlen(start_time_string) < strlen("H:M")) return false;
	if(strlen(start_time_string) > strlen("HH:MM")) return false;

	// Make a copy of the date string, including terminator.
	char str[strlen("HH:MM")+1];
	memcpy(str, start_time_string, strlen(start_time_string)+1);
	char* str_ptr = str;

	// Parse the hour
	const char* hour = str_ptr;
	while(isdigit(*str_ptr)) {
		str_ptr++;
	}
	if(*str_ptr != ':' && *str_ptr != '-' && *str_ptr != ',' && *str_ptr != '.') {
		return false;
	}
	*str_ptr++ = 0;
	out->hour = (int) strtol(hour, NULL, 10);
	if(out->hour < 0 || out->hour > 23) return false;

	// Parse the minutes
	const char* minute = str_ptr;
	while(isdigit(*str_ptr)) {
		str_ptr++;
	}
	if(*str_ptr != '\0') {
		return false;
	}
	out->minute = (int) strtol(minute, NULL, 10);
	if(out->minute < 1 && out->minute > 59) return false;

	return true;
}

// TODO: Discord has as hard limit on how many characters are allowed in a post so we should take care not to exceed this...
//static const size_t DISCORD_MESSAGE_CHARACTER_LIMIT = 2000;

// The maximum allowed characters in a Discord username or nickname.
static const size_t DISCORD_NAME_LENGTH_MAX = 32;

// The maximum allowed byte length of a draft format string.
static const size_t DRAFT_FORMAT_LENGTH_MAX = 64;

// The maximum allowed byte length of a draft format description string.
static const size_t DRAFT_FORMAT_DESCRIPTION_LENGTH_MAX = 128;

// The maximum allowed byte length for each 'blurb' paragraph in the draft details post.
static const size_t DRAFT_BLURB_LENGTH_MAX = 512;

// URLs can potentially be much longer than this but with Discord message character limits we want to keep things short and sweet.
static const size_t URL_LENGTH_MAX = 512; 

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
};

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

	u32 color; // Color to use for vertical strip on the signup post.
	char xmage_server[XMAGE_SERVER_LENGTH_MAX + 1];
	bool draftmancer_draft; // Will the draft portion take place on Draftmancer?
	char banner_url[URL_LENGTH_MAX + 1]; // URL of the image to use for this draft.

	u64 channel_id; // TODO: This can be hard coded in, right? These events should only to to #-pre-register...
	u64 details_id; // Message ID of the post in #-pre-register describing the format.
	u64 signups_id; // Message ID of the sign up sheet posted in #-pre-register.
	u64 reminder_id; // Message ID of the reminder message sent to all sign ups #-in-the-moment-draft.
	u64 tentatives_pinged_id; // Message ID of the reminder sent to tentatives #-in-the-moment-draft. FIXME: Set but never used.

	// TODO!
	u64 hosts_info_id; // Message ID of the message posted to hosts in #current-draft-management
};
static_assert(std::is_trivially_copyable<Draft_Event>(), "struct Draft_Event is not trivially copyable");


enum POD_ALLOCATION_REASON {
	POD_ALLOCATION_REASON_INVALID,

	POD_ALLOCATION_REASON_HOST,
	POD_ALLOCATION_REASON_RO3,
	POD_ALLOCATION_REASON_ERO3,
	POD_ALLOCATION_REASON_CONTENTION,
	POD_ALLOCATION_REASON_SHARK,
	POD_ALLOCATION_REASON_NEWBIE, // Members with < 4 drafts player get priority for pod 2
	POD_ALLOCATION_PREFERENCE,
	POD_ALLOCATION_RANDOM
};

struct Pod_Player {
	u64 member_id;
	POD_ALLOCATION_REASON reason;
	char preferred_name[DISCORD_NAME_LENGTH_MAX + 1];
};

// Our drafts can have no fewer seats than this.
static const int POD_SEATS_MIN = 6;

// Our drafts can have no more seats than this.
static const int POD_SEATS_MAX = 10;

// The maximum number of pods this bot can handle.
static const int PODS_MAX = 8;

// The maximum number of players this bot can handle in a single tournament.
static const int PLAYERS_MAX = 64;

struct Table {
	int seats; // How many seats at this table. Either 6, 8 or 10
	Pod_Player players[POD_SEATS_MAX];
};

static void add_player_to_pod(Table* pod, const Pod_Player* player) {
	memcpy((void*)&pod->players[pod->seats], player, sizeof(Pod_Player));
	++pod->seats;
}

struct Draft_Pods {
	Draft_Pods() {
		memset(this, 0, sizeof(Draft_Pods));
	}

	int table_count; // Total number of tables.
	Table tables[PODS_MAX];
};

// With player_count players, how many pods should be created?
// Reference: https://i.imgur.com/tpNo13G.png
// TODO: This needs to support a player_count of any size
// FIXME: Brute forcing this is silly, but as I write this I can't work out the function to calculate the correct number of seats...
static Draft_Pods allocate_pod_seats(int player_count) {
	// Round up player count to even number.
	if((player_count % 2) == 1) player_count++;

	// As we only need to consider an even number of players, we can halve the player count and use it as an array index.
	player_count /= 2;
	static const int tables_needed_for_count[] = {0,0,0,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,6,6,6,6,7,7,7,7,8,8,8,8}; 

	static const int seats_per_table[(PLAYERS_MAX/2)+1/*plus 1 for 0 players*/][PODS_MAX] = {
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

	Draft_Pods result;
	result.table_count = tables_needed_for_count[player_count];
	for(int table = 0; table < result.table_count; ++table) {
		result.tables[table].seats = seats_per_table[player_count][table];
	}

	return result;
}

// All database_xxxx functions return this struct. If the member variable success is true value will contain the requested data and count will be the number of rows returned.
template<typename T>
struct Database_Result {
	bool success;
	u64 count;
	T value;

	bool operator==(const bool& rhs) const { return  (success == rhs); }
	bool operator!=(const bool& rhs) const { return !(success == rhs); }
};

// For database_ functions that return no data. We could use template specialization here this is simpler.
struct Database_No_Value {};

static const char* DATABASE_NAME = "XDHS"; // TODO: Move this to options?

// Connect to the MySQL database specified in the bot.ini file and request access to the XDHS table.
#define MYSQL_CONNECT()                                      \
	MYSQL* mysql = mysql_init(NULL);                         \
	if(mysql == NULL) {                                      \
		log(LOG_LEVEL_ERROR, "mysql_init(NULL) failed");     \
		return {false, 0, {}};                               \
	}                                                        \
	MYSQL* connection = mysql_real_connect(mysql,            \
		g_config.mysql_host,                                 \
		g_config.mysql_username,                             \
		g_config.mysql_password,                             \
		DATABASE_NAME,                                       \
		g_config.mysql_port,                                 \
		NULL, 0);                                            \
	if(connection == NULL) {                                 \
		log(LOG_LEVEL_ERROR, "mysql_real_connect() failed"); \
		return {false, 0, {}};                               \
	}                                                        \
	SCOPE_EXIT(mysql_close(connection));


// Prepare a MySQL statement using the provided query.
#define MYSQL_STATEMENT()                                                                       \
	MYSQL_STMT* stmt = mysql_stmt_init(connection);                                             \
	if(stmt == NULL) {                                                                          \
		log(LOG_LEVEL_ERROR, "mysql_stmt_init(connection) failed: %s", mysql_stmt_error(stmt)); \
		return {false, 0, {}};                                                                  \
	}                                                                                           \
	SCOPE_EXIT(mysql_stmt_close(stmt));                                                         \
	if(mysql_stmt_prepare(stmt, query, strlen(query)) != 0) {                                   \
		log(LOG_LEVEL_ERROR, "mysql_stmt_prepare() failed: %s", mysql_stmt_error(stmt));        \
		return {false, 0, {}};                                                                  \
	}


// Create and prepare the input array used to bind variables to the query string.
#define MYSQL_INPUT_INIT(count)      \
	MYSQL_BIND input[(count)];       \
	memset(input, 0, sizeof(input));

// Add an item to the input array. One of these is needed for each input variable.
#define MYSQL_INPUT(index, type, pointer, size) \
	input[(index)].buffer_type = (type);        \
	input[(index)].buffer = (void*) (pointer);  \
	input[(index)].buffer_length = (size);

// Once all input variables have been declared, bind them to the query and execute the statement.
#define MYSQL_INPUT_BIND_AND_EXECUTE()                                                                     \
	if(mysql_stmt_bind_param(stmt, input) != 0) {                                                          \
		log(LOG_LEVEL_ERROR, "mysql_stmt_bind_param() failed: %s", mysql_stmt_error(stmt));                \
		return {false, 0, {}};                                                                             \
	}                                                                                                      \
	if(mysql_stmt_execute(stmt) != 0) {                                                                    \
		log(LOG_LEVEL_ERROR, "%s: mysql_stmt_execute() failed: %s", __FUNCTION__, mysql_stmt_error(stmt)); \
		return {false, 0, {}};                                                                             \
	}

// A query with no input params
#define MYSQL_EXECUTE()                                                                                    \
	if(mysql_stmt_execute(stmt) != 0) {                                                                    \
		log(LOG_LEVEL_ERROR, "%s: mysql_stmt_execute() failed: %s", __FUNCTION__, mysql_stmt_error(stmt)); \
		return {false, 0, {}};                                                                             \
	}

// Prepare an array to hold the output from a query.
#define MYSQL_OUTPUT_INIT(count)       \
	MYSQL_BIND output[(count)];        \
	memset(output, 0, sizeof(output)); \
	unsigned long length[(count)];     \
	my_bool is_null[(count)];          \
	my_bool is_error[(count)];

// Set up an item in the output array.
#define MYSQL_OUTPUT(index, type, pointer, size) \
	output[(index)].buffer_type = (type);        \
	output[(index)].buffer = (void*) (pointer);  \
	output[(index)].buffer_length = (size);      \
	output[(index)].is_null = &is_null[(index)]; \
	output[(index)].length = &length[(index)];   \
	output[(index)].error = &is_error[(index)];

// Bind the output array to the statement.
#define MYSQL_OUTPUT_BIND_AND_STORE()                                                        \
    if(mysql_stmt_bind_result(stmt, output) != 0) {                                          \
		log(LOG_LEVEL_ERROR, "mysql_stmt_bind_result() failed: %s", mysql_stmt_error(stmt)); \
		return {false, 0, {}};                                                               \
    }                                                                                        \
    if(mysql_stmt_store_result(stmt) != 0) {                                                 \
		log(LOG_LEVEL_ERROR, "mysql_stmt_store_result: %s", mysql_stmt_error(stmt));         \
		return {false, 0, {}};                                                               \
    }

// Return for queries that don't return any rows
#define MYSQL_RETURN() \
	return {true, 0, {}}

// Return for queries that are expected to fetch zero or one rows
#define MYSQL_FETCH_AND_RETURN_ZERO_OR_ONE_ROWS()                                                      \
	u64 row_count = 0;                                                                                 \
	while(true) {                                                                                      \
		int status = mysql_stmt_fetch(stmt);                                                           \
		if(status == 1) {                                                                              \
			log(LOG_LEVEL_ERROR, "mysql_stmt_fetch() failed: %s", mysql_stmt_error(stmt));             \
			return {false, row_count, {}};                                                             \
		}                                                                                              \
		if(status == MYSQL_NO_DATA) break;                                                             \
		++row_count;                                                                                   \
	}                                                                                                  \
	if(row_count > 1) {                                                                                \
		log(LOG_LEVEL_ERROR, "Database query returned %lu rows but 0 or 1 was expected.", row_count);  \
		return {false, row_count, {}};                                                                 \
	}                                                                                                  \
	return {true, row_count, result};

// Return for queries that are expected to fetch and return a single row of data.
#define MYSQL_FETCH_AND_RETURN_SINGLE_ROW()                                                            \
	u64 row_count = 0;                                                                                 \
	while(true) {                                                                                      \
		int status = mysql_stmt_fetch(stmt);                                                           \
		if(status == 1) {                                                                              \
			log(LOG_LEVEL_ERROR, "mysql_stmt_fetch() failed: %s", mysql_stmt_error(stmt));             \
			return {false, row_count, {}};                                                             \
		}                                                                                              \
		if(status == MYSQL_NO_DATA) break;                                                             \
		++row_count;                                                                                   \
	}                                                                                                  \
	if(row_count != 1) {                                                                               \
		log(LOG_LEVEL_ERROR, "Database query returned %lu rows but 1 was expected.", row_count);       \
		return {false, row_count, {}};                                                                 \
	}                                                                                                  \
	return {true, 1, result};


// Return for queries that are expected to fetch and return 0 or more rows of data.
#define MYSQL_FETCH_AND_RETURN_MULTIPLE_ROWS()                                             \
	u64 row_count = 0;                                                                     \
	while(true) {                                                                          \
		int status = mysql_stmt_fetch(stmt);                                               \
		if(status == 1) {                                                                  \
			log(LOG_LEVEL_ERROR, "mysql_stmt_fetch() failed: %s", mysql_stmt_error(stmt)); \
			return {false, row_count, {}};                                                 \
		}                                                                                  \
		if(status == MYSQL_NO_DATA) break;                                                 \
		results.push_back(result);                                                         \
		++row_count;                                                                       \
	}                                                                                      \
	return {true, row_count, results};


static Database_Result<Database_No_Value> database_add_draft(const u64 guild_id, const Draft_Event* event) {
	MYSQL_CONNECT();

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
			draftmancer_draft, -- 16
			banner_url,   -- 17
			channel_id    -- 17
		)
		VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
		)";

	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(19);
	MYSQL_INPUT( 0, MYSQL_TYPE_LONGLONG, &guild_id,              sizeof(guild_id));
	MYSQL_INPUT( 1, MYSQL_TYPE_STRING,   event->pings,           strlen(event->pings));
	MYSQL_INPUT( 2, MYSQL_TYPE_STRING,   event->draft_code,      strlen(event->draft_code));
	MYSQL_INPUT( 3, MYSQL_TYPE_STRING,   event->league_name,     strlen(event->league_name));
	MYSQL_INPUT( 4, MYSQL_TYPE_STRING,   event->format,          strlen(event->format));
	MYSQL_INPUT( 5, MYSQL_TYPE_STRING,   event->time_zone,       strlen(event->time_zone));
	MYSQL_INPUT( 6, MYSQL_TYPE_LONGLONG, &event->time,           sizeof(event->time));
	MYSQL_INPUT( 7, MYSQL_TYPE_FLOAT,    &event->duration,       sizeof(event->duration));
	MYSQL_INPUT( 8, MYSQL_TYPE_STRING,   &event->blurbs[0][0],   strlen(&event->blurbs[0][0]));
	MYSQL_INPUT( 9, MYSQL_TYPE_STRING,   &event->blurbs[1][0],   strlen(&event->blurbs[1][0]));
	MYSQL_INPUT(10, MYSQL_TYPE_STRING,   &event->blurbs[2][0],   strlen(&event->blurbs[2][0]));
	MYSQL_INPUT(11, MYSQL_TYPE_STRING,   event->draft_guide_url, strlen(event->draft_guide_url));
	MYSQL_INPUT(12, MYSQL_TYPE_STRING,   event->card_list_url,   strlen(event->card_list_url));
	MYSQL_INPUT(13, MYSQL_TYPE_STRING,   event->set_list,        strlen(event->set_list));
	MYSQL_INPUT(14, MYSQL_TYPE_LONG,     &event->color,          sizeof(event->color));
	MYSQL_INPUT(15, MYSQL_TYPE_STRING,   event->xmage_server,    strlen(event->xmage_server));
	MYSQL_INPUT(16, MYSQL_TYPE_TINY,     &event->draftmancer_draft, sizeof(event->draftmancer_draft));
	MYSQL_INPUT(17, MYSQL_TYPE_STRING,   event->banner_url,      strlen(event->banner_url));
	MYSQL_INPUT(18, MYSQL_TYPE_LONGLONG, &event->channel_id,     sizeof(event->channel_id));

	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<std::shared_ptr<Draft_Event>> database_get_event(const u64 guild_id, const std::string& draft_code) {
	MYSQL_CONNECT();

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
			banner_url,          -- 17
			channel_id,          -- 18
			details_id,          -- 19
			signups_id,          -- 20
			reminder_id,         -- 21
			tentatives_pinged_id -- 22
		FROM draft_events
		WHERE guild_id=? AND draft_code=?
	)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id, sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING, draft_code.c_str(), draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	auto result = std::make_shared<Draft_Event>();

	MYSQL_OUTPUT_INIT(23);
	MYSQL_OUTPUT( 0, MYSQL_TYPE_LONG,     &result->status,         sizeof(result->status));
	MYSQL_OUTPUT( 1, MYSQL_TYPE_STRING,   result->draft_code,      DRAFT_CODE_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 2, MYSQL_TYPE_STRING,   result->pings,           PING_STRING_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 3, MYSQL_TYPE_STRING,   result->league_name,     LEAGUE_NAME_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 4, MYSQL_TYPE_STRING,   result->format,          DRAFT_FORMAT_DESCRIPTION_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 5, MYSQL_TYPE_STRING,   result->time_zone,       IANA_TIME_ZONE_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 6, MYSQL_TYPE_LONGLONG, &result->time,           sizeof(result->time));
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
	MYSQL_OUTPUT(17, MYSQL_TYPE_STRING,   result->banner_url,      URL_LENGTH_MAX + 1);
	MYSQL_OUTPUT(18, MYSQL_TYPE_LONGLONG, &result->channel_id,     sizeof(result->channel_id));
	MYSQL_OUTPUT(19, MYSQL_TYPE_LONGLONG, &result->details_id,     sizeof(result->details_id));
	MYSQL_OUTPUT(20, MYSQL_TYPE_LONGLONG, &result->signups_id,     sizeof(result->signups_id));
	MYSQL_OUTPUT(21, MYSQL_TYPE_LONGLONG, &result->reminder_id,    sizeof(result->reminder_id));
	MYSQL_OUTPUT(22, MYSQL_TYPE_LONGLONG, &result->tentatives_pinged_id, sizeof(result->tentatives_pinged_id));
	MYSQL_OUTPUT_BIND_AND_STORE();

	MYSQL_FETCH_AND_RETURN_ZERO_OR_ONE_ROWS();
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
	SIGNUP_STATUS_REMOVED     = 64, // The host has removed the player (likely a no-show) from the signup sheet.

	SIGNUP_STATUS_INVALID     = -1,

	SIGNUP_STATUS_PLAYING     = SIGNUP_STATUS_COMPETITIVE | SIGNUP_STATUS_CASUAL | SIGNUP_STATUS_FLEXIBLE
};


static const char* to_cstring(SIGNUP_STATUS s) {
	switch(s) {
		case SIGNUP_STATUS_NONE:        return "none";
		case SIGNUP_STATUS_COMPETITIVE: return "competitive";
		case SIGNUP_STATUS_CASUAL:      return "casual";
		case SIGNUP_STATUS_FLEXIBLE:    return "flexible";
		case SIGNUP_STATUS_TENTATIVE:   return "tentative";
		case SIGNUP_STATUS_MINUTEMAGE:  return "minutemage";
		case SIGNUP_STATUS_DECLINE:     return "decline";
		case SIGNUP_STATUS_REMOVED:     return "removed";
		default:                        return NULL;
	}
}


struct Draft_Signup_Status {
	u64 member_id;
	time_t timestamp;
	SIGNUP_STATUS status;

	// Cache the members preferred name so we don't have to look it up every time someone changes their sign up status.
	// NOTE: Doing this creates a bug - If a member were to sign up to a draft and then later change their guild nickname, this nickname change would not be shown on the sign up post until that same member clicked one of the sign up buttons again. I can live with this!
	char preferred_name[DISCORD_NAME_LENGTH_MAX + 1];
};

static Database_Result<Draft_Signup_Status> database_get_members_sign_up_status(const u64 guild_id, const std::string& draft_code, const u64 member_id) {
	MYSQL_CONNECT();
	const char* query = "SELECT status, time, preferred_name FROM draft_signups WHERE guild_id=? AND member_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,          sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_LONGLONG, &member_id,         sizeof(member_id));
	MYSQL_INPUT(2, MYSQL_TYPE_STRING,   draft_code.c_str(), draft_code.length());
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

static Database_Result<Database_No_Value> database_sign_up_to_a_draft(const u64 guild_id, const std::string& draft_code, const u64 member_id, const std::string& preferred_name, const time_t timestamp, const SIGNUP_STATUS status) {
	MYSQL_CONNECT();
	const char* query = "INSERT INTO draft_signups (guild_id, member_id, preferred_name, draft_code, time, status) VALUES(?,?,?,?,?,?) ON DUPLICATE KEY UPDATE preferred_name=?, time=?, status=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(9);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,              sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_LONGLONG, &member_id,             sizeof(member_id));
	MYSQL_INPUT(2, MYSQL_TYPE_STRING,   preferred_name.c_str(), preferred_name.length());
	MYSQL_INPUT(3, MYSQL_TYPE_STRING,   draft_code.c_str(),     draft_code.length());
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
static const Database_Result<std::vector<Draft_Signup_Status>> database_get_draft_sign_ups(const u64 guild_id, const std::string& draft_code) {
	MYSQL_CONNECT();
	const char* query = "SELECT member_id, preferred_name, time, status FROM draft_signups WHERE guild_id=? AND draft_code=? ORDER BY time";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,          sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING,   draft_code.c_str(), draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	Draft_Signup_Status result;

	MYSQL_OUTPUT_INIT(3);
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
	bool allocated; // Player has been allocated to a pod.
};

static const Database_Result<std::vector<Draft_Sign_Up>> database_get_sign_ups(const u64 guild_id, const std::string& draft_code, const char* league) {
	MYSQL_CONNECT();
	// TODO: This needs to know the season!
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
		LEFT JOIN leaderboards ON draft_signups.member_id=leaderboards.member_id AND leaderboards.league=? -- League code from spread: PC, AC, EB etc 
		LEFT JOIN shark ON draft_signups.member_id=shark.id
		LEFT JOIN devotion ON draft_signups.member_id=devotion.id
		LEFT JOIN win_rate_recent ON draft_signups.member_id=win_rate_recent.id
		WHERE
			draft_signups.guild_id=?
		AND
			draft_signups.draft_code=?
		;)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT(0, MYSQL_TYPE_STRING, league, strlen(league));
	MYSQL_INPUT(1, MYSQL_TYPE_LONGLONG, &guild_id, sizeof(guild_id));
	MYSQL_INPUT(2, MYSQL_TYPE_STRING, draft_code.c_str(), draft_code.length());
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

static const Database_Result<std::vector<std::string>> database_get_draft_codes_for_post_draft_autocomplete(const u64 guild_id, std::string& prefix) {
	MYSQL_CONNECT();

	prefix += "%"; // The LIKE operator has to be part of the parameter, not in the query string.
	const char* query = "SELECT draft_code FROM draft_events WHERE guild_id=? AND status=? AND draft_code LIKE ? ORDER BY draft_code LIMIT 25";
	MYSQL_STATEMENT();

	const DRAFT_STATUS status = DRAFT_STATUS_CREATED;

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,      sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_LONG,     &status,        sizeof(status));
	MYSQL_INPUT(2, MYSQL_TYPE_STRING,   prefix.c_str(), prefix.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	char result[DRAFT_CODE_LENGTH_MAX + 1];

	MYSQL_OUTPUT_INIT(1);
	MYSQL_OUTPUT(0, MYSQL_TYPE_STRING, &result[0], DRAFT_CODE_LENGTH_MAX + 1);
	MYSQL_OUTPUT_BIND_AND_STORE();

	std::vector<std::string> results;

	MYSQL_FETCH_AND_RETURN_MULTIPLE_ROWS();
}

static const Database_Result<std::vector<std::string>> database_get_draft_codes_for_delete_draft_autocomplete(const u64 guild_id, std::string& prefix) {
	MYSQL_CONNECT();

	prefix += "%"; // The LIKE operator has to be part of the parameter, not in the query string.
	const char* query = "SELECT draft_code FROM draft_events WHERE guild_id=? AND status>=? AND status<? AND draft_code LIKE ? ORDER BY draft_code LIMIT 25";
	MYSQL_STATEMENT();

	const DRAFT_STATUS status1 = DRAFT_STATUS_CREATED;
	const DRAFT_STATUS status2 = DRAFT_STATUS_COMPLETE;

	MYSQL_INPUT_INIT(4);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,      sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_LONG,     &status1,       sizeof(status1));
	MYSQL_INPUT(2, MYSQL_TYPE_LONG,     &status2,       sizeof(status2));
	MYSQL_INPUT(3, MYSQL_TYPE_STRING,   prefix.c_str(), prefix.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	char result[DRAFT_CODE_LENGTH_MAX + 1];

	MYSQL_OUTPUT_INIT(1);
	MYSQL_OUTPUT(0, MYSQL_TYPE_STRING, &result[0], DRAFT_CODE_LENGTH_MAX + 1);
	MYSQL_OUTPUT_BIND_AND_STORE();

	std::vector<std::string> results;

	MYSQL_FETCH_AND_RETURN_MULTIPLE_ROWS();
}

static Database_Result<Database_No_Value> database_set_details_message_id(const u64 guild_id, const char* draft_code, const u64 message_id) {
	MYSQL_CONNECT();
	static const char* query = "UPDATE draft_events SET details_id=? WHERE guild_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &message_id, sizeof(message_id));
	MYSQL_INPUT(1, MYSQL_TYPE_LONGLONG, &guild_id,   sizeof(guild_id));
	MYSQL_INPUT(2, MYSQL_TYPE_STRING,   draft_code,  strlen(draft_code));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_set_signups_message_id(const u64 guild_id, const char* draft_code, const u64 message_id) {
	MYSQL_CONNECT();
	static const char* query = "UPDATE draft_events SET signups_id=? WHERE guild_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &message_id, sizeof(message_id));
	MYSQL_INPUT(1, MYSQL_TYPE_LONGLONG, &guild_id,   sizeof(guild_id));
	MYSQL_INPUT(2, MYSQL_TYPE_STRING,   draft_code,  strlen(draft_code));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_set_reminder_message_id(const u64 guild_id, const char* draft_code, const u64 message_id) {
	MYSQL_CONNECT();
	static const char* query = "UPDATE draft_events SET reminder_id=? WHERE guild_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &message_id, sizeof(message_id));
	MYSQL_INPUT(1, MYSQL_TYPE_LONGLONG, &guild_id,   sizeof(guild_id));
	MYSQL_INPUT(2, MYSQL_TYPE_STRING,   draft_code,  strlen(draft_code));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<std::string> database_get_next_upcoming_draft(const u64 guild_id) {
	MYSQL_CONNECT();
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

static Database_Result<Database_No_Value> database_clear_draft_post_ids(const u64 guild_id, const std::string& draft_code) {
	MYSQL_CONNECT();
	static const char* query = "UPDATE draft_events SET details_id=0, signups_id=0 WHERE guild_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,          sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING,   draft_code.c_str(), draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_set_draft_status(const u64 guild_id, const std::string& draft_code, const DRAFT_STATUS status) {
	MYSQL_CONNECT();
	static const char* query = "UPDATE draft_events SET status=status|? WHERE guild_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT(0, MYSQL_TYPE_LONG,     &status,            sizeof(status));
	MYSQL_INPUT(1, MYSQL_TYPE_LONGLONG, &guild_id,          sizeof(guild_id));
	MYSQL_INPUT(2, MYSQL_TYPE_STRING,   draft_code.c_str(), draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_purge_draft_event(const u64 guild_id, const std::string& draft_code) {
	MYSQL_CONNECT();
	static const char* query = "DELETE FROM draft_events WHERE guild_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,          sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING,   draft_code.c_str(), draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

struct Draft_Post_IDs {
	u64 channel;
	u64 details;
	u64 signups;
};

// TODO: Don't need this? No function calls this that doesn't call database_get_event
static Database_Result<Draft_Post_IDs> database_get_draft_post_ids(const u64 guild_id, const std::string& draft_code) {
	MYSQL_CONNECT();
	static const char* query = "SELECT channel_id, details_id, signups_id FROM draft_events WHERE guild_id=? AND draft_code=?"; 
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,          sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING,   draft_code.c_str(), draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	Draft_Post_IDs result;

	MYSQL_OUTPUT_INIT(3);
	MYSQL_OUTPUT(0, MYSQL_TYPE_LONGLONG, &result.channel, sizeof(result.channel));
	MYSQL_OUTPUT(1, MYSQL_TYPE_LONGLONG, &result.details, sizeof(result.details));
	MYSQL_OUTPUT(2, MYSQL_TYPE_LONGLONG, &result.signups, sizeof(result.signups));
	MYSQL_OUTPUT_BIND_AND_STORE();

	MYSQL_FETCH_AND_RETURN_SINGLE_ROW();
}

static Database_Result<Database_No_Value> database_set_tentatives_pinged_id(const u64 guild_id, const char* draft_code, const u64 message_id) {
	MYSQL_CONNECT();
	static const char* query = "UPDATE draft_events SET tentatives_pinged_id=? WHERE guild_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &message_id, sizeof(message_id));
	MYSQL_INPUT(1, MYSQL_TYPE_LONGLONG, &guild_id,   sizeof(guild_id));
	MYSQL_INPUT(2, MYSQL_TYPE_STRING,   draft_code,  strlen(draft_code));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_add_temp_role(const u64 guild_id, const char* draft_code, const u64 role_id) {
	MYSQL_CONNECT();
	static const char* query = "REPLACE INTO temp_roles (guild_id, draft_code, role_id) VALUES(?,?,?)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,  sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING,   draft_code, strlen(draft_code));
	MYSQL_INPUT(2, MYSQL_TYPE_LONGLONG, &role_id,   sizeof(role_id));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_del_temp_roles(const u64 guild_id, const char* draft_code) {
	MYSQL_CONNECT();
	static const char *query = "DELETE FROM temp_roles WHERE guild_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,  sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING,   draft_code, strlen(draft_code));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_add_temp_member_role(const u64 guild_id, const u64 member_id, const u64 role_id) {
	MYSQL_CONNECT();
	static const char* query = "REPLACE INTO temp_members (guild_id, member_id, role_id) VALUES(?,?,?)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,  sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_LONGLONG, &member_id, sizeof(member_id));
	MYSQL_INPUT(2, MYSQL_TYPE_LONGLONG, &role_id,   sizeof(role_id));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_add_noshow(const u64 guild_id, const u64 member_id, const char* draft_code) {
	MYSQL_CONNECT();
	static const char* query = "REPLACE INTO noshows (guild_id, member_id, draft_code) VALUES(?,?,?)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,  sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_LONGLONG, &member_id, sizeof(member_id));
	MYSQL_INPUT(2, MYSQL_TYPE_STRING,   draft_code, strlen(draft_code));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_add_dropper(const u64 guild_id, const u64 member_id, const char* draft_code) {
	MYSQL_CONNECT();
	static const char* query = "REPLACE INTO droppers (guild_id, member_id, draft_code) VALUES(?,?,?)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,  sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_LONGLONG, &member_id, sizeof(member_id));
	MYSQL_INPUT(2, MYSQL_TYPE_STRING,   draft_code, strlen(draft_code));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}


static const size_t XMAGE_VERSION_STRING_MAX = 128;

struct XMage_Version {
	char version[XMAGE_VERSION_STRING_MAX + 1];
	u64 timestamp;
};

static Database_Result<XMage_Version> database_get_xmage_version() {
	MYSQL_CONNECT();
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


static const int BANNER_IMAGE_WIDTH = 900;
static const int BANNER_IMAGE_HEIGHT = 600;

// Pack images are scaled to this size
static const int PACK_IMAGE_WIDTH  = 300;
static const int PACK_IMAGE_HEIGHT = 470;

static const int KEY_ART_WIDTH = (PACK_IMAGE_WIDTH * 3);
static const int KEY_ART_HEIGHT = PACK_IMAGE_HEIGHT;


union Pixel {
    struct {
		u8 r, g, b, a; // Little endian
    } components;
    u8 block[4];
    u32 c; // 0xAABBGGRR
};
static_assert(sizeof(Pixel) == 4, "Unexpected size for Pixel union.");

struct Image {
    int w;
    int h;
	int channels;
	void* data;
};

void init_image(Image* img, int width, int height, int channels, u32 color) {
	img->data = malloc(width * height * channels);// sizeof(Pixel));
	if(img->data == NULL) {
		// FIXME: Failing to allocate is possibly a real problem on the virtual server this runs on...
		img->w = 0;
		img->h = 0;
		img->channels = 0;
		fprintf(stdout, "out of memory\n");
		return;
	}

	img->channels = channels;
	img->w = width;
	img->h = height;

	// TODO: Do I need to support 3 channel images here? Probably not...
	if(channels == 4) {
		Pixel* ptr = (Pixel*)img->data;
		for(int i = 0; i < (width * height); ++i) {
			ptr[i].c = color;
		}
	} else
	if(channels == 1) {
		u8* ptr = (u8*)img->data;
		for(int i = 0; i < (width * height); ++i) {
			ptr[i] = (u8)color;	
		}
	}
}

void image_max_alpha(Image* img) {
	if(img->channels == 4) {
		Pixel* ptr = (Pixel*)img->data;
		for(int i = 0; i < (img->w * img->h); ++i) {
			ptr[i].components.a = 0xFF;
		}
	} else
	if(img->channels == 1) {
		u8* ptr = (u8*)img->data;
		for(int i = 0; i < (img->w * img->h); ++i) {
			ptr[i] = (u8)0xFF;	
		}
	}
}

// blit src over dst at position x, y using (SourceColor*SourceAlpha)+(DestColor*(1-SourceAlpha)) as the blend function.
void blit_RGBA_to_RGBA(const Image* src, const Image* dst, int x, int y) {
	assert(src->channels == 4);
	assert(dst->channels == 4);

    for(int row = 0; row < src->h; ++row) {
        int write_offset = (dst->w * y) + (dst->w * row) + x;
        u32* write_ptr = (u32*) ((Pixel*)dst->data + write_offset);

        int read_offset = src->w * row;
        u32* read_ptr = (u32*) ((Pixel*)src->data + read_offset);
        for(int col = 0; col < src->w; ++col) {
            Pixel src_pixel;
            src_pixel.c = *read_ptr++;
            const f32 src_r = (f32)src_pixel.components.r / 255.0f;
            const f32 src_g = (f32)src_pixel.components.g / 255.0f;
            const f32 src_b = (f32)src_pixel.components.b / 255.0f;
            const f32 src_a = (f32)src_pixel.components.a / 255.0f;

            Pixel dst_pixel;
            dst_pixel.c = *write_ptr;
            const f32 dst_r = (f32)dst_pixel.components.r / 255.0f;
            const f32 dst_g = (f32)dst_pixel.components.g / 255.0f;
            const f32 dst_b = (f32)dst_pixel.components.b / 255.0f;
            const f32 dst_a = (f32)dst_pixel.components.a / 255.0f;

			const f32 one_minus_source_alpha = (1.0f - src_a);
            Pixel out;
            out.components.r = 255 * ((src_r * src_a) + (dst_r * (one_minus_source_alpha)));
            out.components.g = 255 * ((src_g * src_a) + (dst_g * (one_minus_source_alpha)));
            out.components.b = 255 * ((src_b * src_a) + (dst_b * (one_minus_source_alpha)));
            out.components.a = 255 * ((src_a * src_a) + (dst_a * (one_minus_source_alpha)));

            *write_ptr++ = out.c;
        }
    }
}

void blit_RGB_to_RGBA(const Image* src, const Image* dst, int x, int y) {
	assert(src->channels == 3);
	assert(dst->channels == 4);
 
    for(int row = 0; row < src->h; ++row) {
        int write_offset = (dst->w * y) + (dst->w * row) + x;
        u32* write_ptr = (u32*) ((Pixel*)dst->data + write_offset);

        const u8* read_ptr = (u8*) ((u8*)src->data + (src->w * row * src->channels));
        for(int col = 0; col < src->w; ++col) {
            Pixel out;
            out.components.r = *read_ptr++;
            out.components.g = *read_ptr++;
            out.components.b = *read_ptr++;
            out.components.a = 255;
            *write_ptr++ = out.c;
        }
    }
}

void blit_A8_to_RGBA(const Image* src, int stride, const Pixel color, const Image* dst, int x, int y) {
	assert(src->channels == 1);
	assert(dst->channels == 4);

	const f32 src_r = (f32)color.components.r / 255.0f;
	const f32 src_g = (f32)color.components.g / 255.0f;
	const f32 src_b = (f32)color.components.b / 255.0f;

    for(int row = 0; row < src->h; ++row) {
        int write_offset = (dst->w * y) + (dst->w * row) + x; // FIXME: Should this be size_t?
        u32* write_ptr = (u32*) ((Pixel*)dst->data + write_offset);

        int read_offset = stride * row;
        u8* read_ptr = (u8*)((u8*)src->data + read_offset);
        for(int col = 0; col < src->w; ++col) {
            const f32 src_a = (f32)*read_ptr / 255.0f;
			read_ptr++;

            Pixel dst_pixel;
            dst_pixel.c = *write_ptr;
            const f32 dst_r = (f32)dst_pixel.components.r / 255.0f;
            const f32 dst_g = (f32)dst_pixel.components.g / 255.0f;
            const f32 dst_b = (f32)dst_pixel.components.b / 255.0f;
            const f32 dst_a = (f32)dst_pixel.components.a / 255.0f;

			const f32 one_minus_source_alpha = 1.0f - src_a;
            Pixel out;
            out.components.r = 255 * ((src_r * src_a) + (dst_r * one_minus_source_alpha));
            out.components.g = 255 * ((src_g * src_a) + (dst_g * one_minus_source_alpha));
            out.components.b = 255 * ((src_b * src_a) + (dst_b * one_minus_source_alpha));
            out.components.a = 255 * ((src_a * src_a) + (dst_a * one_minus_source_alpha));

            *write_ptr++ = out.c;
        }
    }
}

void blit_A8_to_RGBA_no_alpha(const Image* src, int stride, const Pixel color, const Image* dst, int x, int y) {
	assert(src->channels == 1);
	assert(dst->channels == 4);

	const f32 src_r = (f32)color.components.r / 255.0f;
	const f32 src_g = (f32)color.components.g / 255.0f;
	const f32 src_b = (f32)color.components.b / 255.0f;

    for(int row = 0; row < src->h; ++row) {
        int write_offset = (dst->w * y) + (dst->w * row) + x;
        u32* write_ptr = (u32*) ((Pixel*)dst->data + write_offset);

        int read_offset = stride * row;
        u8* read_ptr = (u8*)((u8*)src->data + read_offset);
        for(int col = 0; col < src->w; ++col) {
            const f32 src_a = (f32)*read_ptr / 255.0f;
			read_ptr++;

            Pixel out;
            out.components.r = 255 * ((src_r * src_a));
            out.components.g = 255 * ((src_g * src_a));
            out.components.b = 255 * ((src_b * src_a));
            out.components.a = 255 * ((src_a * src_a));

            *write_ptr++ = out.c;
        }
    }
}

void blit_A8_to_A8(const Image* src, int stride, Image* dst, int x, int y) {
	assert(src->channels == 1);
	assert(dst->channels == 1);

    for(int row = 0; row < src->h; ++row) {
        int write_offset = (dst->w * y) + (dst->w * row) + x;
        u8* write_ptr = (u8*) ((u8*)dst->data + write_offset);

        int read_offset = stride * row;
        u8* read_ptr = (u8*) ((u8*)src->data + read_offset);
        for(int col = 0; col < src->w; ++col) {
            const f32 src_a = (f32)*read_ptr / 255.0f;
			read_ptr++;

            const f32 dst_a = (f32)(*write_ptr) / 255.0f;
			const f32 one_minus_source_alpha = 1.0f - src_a;
            u8 out = 255 * ((src_a * src_a) + (dst_a * one_minus_source_alpha));

            *write_ptr++ = out;
        }
    }
}

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
	while((ch = u8_nextchar(str, &index)) != 0) {
		f32 x_shift = xpos - (f32) floor(xpos);
		int advance, lsb;
		stbtt_GetCodepointHMetrics(font, ch, &advance, &lsb);
		int x0, y0, x1, y1;
      	stbtt_GetCodepointBitmapBoxSubpixel(font, ch, scale, scale, x_shift, 0, &x0, &y0, &x1, &y1);
		xpos += advance * scale;
		if(isutf(str[index+1])) {
			int tmp = index;
			xpos += scale * stbtt_GetCodepointKernAdvance(font, ch, u8_nextchar(str, &tmp));
		}
	}

	dim.w = ceil(xpos);

	return dim;
}

// FIXME: Move color to the end as it's optional
static void render_text_to_image(stbtt_fontinfo* font, const u8* str, const int size, const Pixel color, Image* canvas, int x, int y) {
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
	while((ch = u8_nextchar(str, &index)) != 0) {
		if(!isutf(ch)) continue; // FIXME: Not sure why some std::strings are reading past the end. Should they be double terminiated?
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
			// FIXME: log error
			assert(false);
		}

		xpos += advance * scale;
		if(isutf(str[index+1])) {
			int index_copy = index;
			xpos += scale * stbtt_GetCodepointKernAdvance(font, ch, u8_nextchar(str, &index_copy));
		}
	}
}

void draw_shadowed_text(stbtt_fontinfo* font, int font_size, int max_width, const u8* str, u32 shadow_color, u32 text_color, Image* out, int ypos) {
	// The output of the stbtt_truetype library isn't as nice as Freetype so to generate smoother looking glyphs we render the text larger than we need, then scale it down to the requested size. This produces better aliasing, IMO!
	const int upscale_factor = 2;
	const int upscaled_font_size = (font_size * upscale_factor); // TODO: Rename to font_size_upscaled or something

	Text_Dim dim = get_text_dimensions(font, upscaled_font_size, str);

	// FIXME: There are no bounds checks done here

	Image upscaled;
	init_image(&upscaled, dim.w, dim.h, 1, 0x00000000);
	render_text_to_image(font, str, upscaled_font_size, {.c=shadow_color}, &upscaled, 0, 0);

	Image downscaled;
	if((upscaled.w / upscale_factor) < max_width) {
		init_image(&downscaled, dim.w / upscale_factor, dim.h / upscale_factor, 1, 0x00000000);
	} else {
		// The text is too wide, it will need to be scaled down more than upscale_factor
		f32 ratio = ((f32)max_width / (f32)dim.w);
		int height = ceil(((f32)dim.h * ratio));
		init_image(&downscaled, max_width, height, 1, 0x00000000);
	}
	stbir_resize_uint8_srgb((const u8*)upscaled.data, upscaled.w, upscaled.h, 0,
	                        (u8*)downscaled.data, downscaled.w, downscaled.h, 0, STBIR_1CHANNEL);

	const int xpos = (BANNER_IMAGE_WIDTH / 2) - (downscaled.w / 2);
	ypos -= (downscaled.h / 2);

	blit_A8_to_RGBA(&downscaled, downscaled.w, {.c=shadow_color}, out, xpos-1, ypos-1); // left top
	blit_A8_to_RGBA(&downscaled, downscaled.w, {.c=shadow_color}, out, xpos-1, ypos+1); // left bottom
	blit_A8_to_RGBA(&downscaled, downscaled.w, {.c=shadow_color}, out, xpos-1, ypos);   // left centre
	blit_A8_to_RGBA(&downscaled, downscaled.w, {.c=shadow_color}, out, xpos+1, ypos);   // right centre
	blit_A8_to_RGBA(&downscaled, downscaled.w, {.c=shadow_color}, out, xpos,   ypos-1); // center top
	blit_A8_to_RGBA(&downscaled, downscaled.w, {.c=shadow_color}, out, xpos,   ypos+1); // center bottom
	blit_A8_to_RGBA(&downscaled, downscaled.w, {.c=shadow_color}, out, xpos+1, ypos-1); // right top
	blit_A8_to_RGBA(&downscaled, downscaled.w, {.c=shadow_color}, out, xpos+1, ypos+1); // right bottom

	blit_A8_to_RGBA(&downscaled, downscaled.w, {.c=text_color}, out, xpos, ypos);

	free(upscaled.data);
	free(downscaled.data);
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

static const char* to_cstring(const DRAFT_TYPE dt) {
	switch(dt) {
		case DRAFT_TYPE_DEVOTION_GIANT:  return "Devotion - Giant";
		case DRAFT_TYPE_DEVOTION_SPHINX: return "Devotion - Sphinx";
		case DRAFT_TYPE_DEVOTION_DEMON:  return "Devotion - Demon";
		case DRAFT_TYPE_DEVOTION_DRAGON: return "Devotion - Dragon";
		case DRAFT_TYPE_DEVOTION_TITAN:  return "Devotion - Titan";
		case DRAFT_TYPE_DEVOTION_GOD:    return "Devotion - God";

		case DRAFT_TYPE_COMMUNITY_CHOICE: return "Community Choice";

		case DRAFT_TYPE_HERO_20: return "Hero - 20";
		case DRAFT_TYPE_HERO_40: return "Hero - 40";
		case DRAFT_TYPE_HERO_60: return "Hero - 60";

		case DRAFT_TYPE_NOT_APPLICABLE: // Fall through
		default:
			break;
	}
	
	return NULL;
}

struct Draft_Type {
	DRAFT_TYPE value;
	const char* name;
};

#if 0
// NOTE: 2023-10-24: Due to a bug in DPP++ autocomplete does not work when mapping sting->int
// but when (if!) this bug is fixed I'll want to use this code again.
static std::vector<Draft_Type> get_draft_types_for_autocomplete(const std::string& input) {
	fprintf(stdout, "%s\n", __FUNCTION__);
	std::vector<Draft_Type> result;
	for(int i = (int)DRAFT_TYPE_DEVOTION_GIANT; i < (int)DRAFT_TYPE_COUNT; ++i) {
		const char* name = to_cstring((DRAFT_TYPE)i);
		if(name == NULL) continue;
		if(input.empty() || strncmp(name, input.c_str(), input.length()) == 0) {
			result.push_back({(DRAFT_TYPE)i, name});
		}
	}
	return result;
}
#endif

struct Icon {
	DRAFT_TYPE type;
	const char* file; // Relative path
	int x, y;         // Position on the screen
};

static const Icon g_icons[] = {
	{DRAFT_TYPE_DEVOTION_GIANT,   "gfx/banner/icons/devotion_giant.png",    30, 105},
	{DRAFT_TYPE_DEVOTION_SPHINX,  "gfx/banner/icons/devotion_sphinx.png",   30, 105},
	{DRAFT_TYPE_DEVOTION_DEMON,   "gfx/banner/icons/devotion_demon.png",    30, 105},
	{DRAFT_TYPE_DEVOTION_DRAGON,  "gfx/banner/icons/devotion_dragon.png",   30, 105},
	{DRAFT_TYPE_DEVOTION_TITAN,   "gfx/banner/icons/devotion_titan.png",    30, 105},
	{DRAFT_TYPE_DEVOTION_GOD,     "gfx/banner/icons/devotion_god.png",      30, 105},
	{DRAFT_TYPE_COMMUNITY_CHOICE, "gfx/banner/icons/community_choice.png", 290, 116},
	{DRAFT_TYPE_HERO_20,          "gfx/banner/icons/hero_20.png",           30, 105},
	{DRAFT_TYPE_HERO_40,          "gfx/banner/icons/hero_40.png",           30, 105},
	{DRAFT_TYPE_HERO_60,          "gfx/banner/icons/hero_60.png",           30, 105},
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

struct Render_Banner_Result {
	bool is_error;
	std::string path; // if is_error == false, this is the path to the generated banner. FIXME: This smells bad!
};

const Render_Banner_Result render_banner(Banner_Opts* opts) {
	if(g_banner_font_loaded == false) {
		size_t size = 0;
		const u8* buffer = file_slurp(g_banner_font_file, &size); // NOTE: Intentionally never freed
		int result = stbtt_InitFont(&g_banner_font, buffer, stbtt_GetFontOffsetForIndex(buffer, 0));
		if(result == 0) {
			return {true, fmt::format("Internal error: Failed to load font file \"{}\". This is not your fault! Please try again.", g_banner_font_file)};
		}
		g_banner_font_loaded = true;
	}

	static const char* BANNER_FRAME_TOP_FILE    = "gfx/banner/frame_top.png";
	static const char* BANNER_FRAME_SIDE_FILE   = "gfx/banner/frame_side.png"; // left and right
	static const char* BANNER_FRAME_BOTTOM_FILE = "gfx/banner/frame_bottom.png";
	static const char* BANNER_GRADIENT_FILE = "gfx/banner/gradient.png";
	static const char* BANNER_SUBTITLE_FILE = "gfx/banner/subtitle.png";

	static const int DATETIME_YPOS      = 15;
	static const int DATETIME_FONT_SIZE = 40;

	static const int FORMAT_TEXT_WIDTH_MAX = 800; // Scale down the format string if longer than this.
	static const int FORMAT_TEXT_FONT_SIZE = 44;
	static const int FORMAT_TEXT_YPOS      = 88;

	static const int SUBTITLE_FRAME_YPOS     = 118;
	static const int SUBTITLE_TEXT_WIDTH_MAX = 640;
	static const int SUBTITLE_TEXT_YPOS      = 138;
	static const int SUBTITLE_TEXT_FONT_SIZE = 28;

	static const int PACK_DIVIDER_YPOS = 116; // Starting row to draw the divider between packs

	Image banner;
	init_image(&banner, BANNER_IMAGE_WIDTH, BANNER_IMAGE_HEIGHT, 4, 0xFF000000);
	SCOPE_EXIT(free(banner.data));

	// blit the background image(s)
	if(opts->images.size() == 1) {
		// A single piece of key art is to be used.
		Image scaled;
		init_image(&scaled, KEY_ART_WIDTH, KEY_ART_HEIGHT, 3, 0x00000000);
		SCOPE_EXIT(free(scaled.data));
		
		Image img;
		img.data = (void*) stbi_load(opts->images[0].c_str(), &img.w, &img.h, &img.channels, 3);
		if(img.data == NULL) {
			return {true, fmt::format("Internal error: Failed to load image \"{}\" Reason: {}", opts->images[0], stbi_failure_reason())};
		}
		SCOPE_EXIT(stbi_image_free(img.data));
		stbir_resize_uint8_srgb((const u8*)img.data, img.w, img.h, 0,
				(u8*)scaled.data, scaled.w, scaled.h, 0, STBIR_RGB);
		blit_RGB_to_RGBA(&scaled, &banner, 0, BANNER_IMAGE_HEIGHT - scaled.h);
	} else
	if(opts->images.size() == 3) {
		// Three pack images given.
		Image scaled;
		init_image(&scaled, PACK_IMAGE_WIDTH, PACK_IMAGE_HEIGHT, 3, 0x00000000);
		SCOPE_EXIT(free(scaled.data));
		for(size_t f = 0; f < opts->images.size(); ++f) {
			Image img;
			img.data = (void*) stbi_load(opts->images[f].c_str(), &img.w, &img.h, &img.channels, 3);
			if(img.data == NULL) {
				return {true, fmt::format("Internal error: Failed to load image \"{}\" Reason: {}", opts->images[f], stbi_failure_reason())};
			}
			SCOPE_EXIT(stbi_image_free(img.data));
			stbir_resize_uint8_srgb((const u8*)img.data, img.w, img.h, 0,
					(u8*)scaled.data, scaled.w, scaled.h, 0, STBIR_RGB);
			blit_RGB_to_RGBA(&scaled, &banner, f * PACK_IMAGE_WIDTH, BANNER_IMAGE_HEIGHT - scaled.h);
		}

		// Draw a thin line to separate each pack.
		// FIXME: Replace this with a draw_rect function to avoid unnecessary heap allocations for each line.
		Image line;
		init_image(&line, 3, BANNER_IMAGE_HEIGHT-PACK_DIVIDER_YPOS, 4, 0xFF000000);
		SCOPE_EXIT(free(line.data));
		for(int i = 1; i < 3; ++i) {
			blit_RGBA_to_RGBA(&line, &banner, (i * PACK_IMAGE_WIDTH)-1, PACK_DIVIDER_YPOS);		
		}
	} else {
		return {true, fmt::format("Internal error: Unexpected or unsupported pack count: {}", opts->images.size())};
	}

	// Blit the gradient. TODO: This could be done in code instead of using an image...
	{
		Image grad;
		grad.data = (void*) stbi_load(BANNER_GRADIENT_FILE, &grad.w, &grad.h, &grad.channels, 1);
		if(grad.data == NULL) {
			return {true, fmt::format("Internal error: Failed to load image \"{}\" Reason: {}", BANNER_GRADIENT_FILE, stbi_failure_reason())};
		}
		SCOPE_EXIT(stbi_image_free(grad.data));
		blit_A8_to_RGBA(&grad, grad.w, {.c=0xFF000000}, &banner, 0, 0);
	}

	// Blit the title box frames and color them
	{
		{
			// Top
			Image frame;
			frame.data = (void*) stbi_load(BANNER_FRAME_TOP_FILE, &frame.w, &frame.h, &frame.channels, 1);
			if(frame.data == NULL) {
				return {true, fmt::format("Internal error: Failed to load image \"{}\" Reason: {}", BANNER_FRAME_TOP_FILE, stbi_failure_reason())};
			}
			SCOPE_EXIT(stbi_image_free(frame.data));
			blit_A8_to_RGBA(&frame, frame.w, {.c=opts->league_color}, &banner, 9, 60);
		}
		{
			// Bottom
			Image frame;
			frame.data = (void*) stbi_load(BANNER_FRAME_BOTTOM_FILE, &frame.w, &frame.h, &frame.channels, 1);
			if(frame.data == NULL) {
				return {true, fmt::format("Internal error: Failed to load image \"{}\" Reason: {}", BANNER_FRAME_BOTTOM_FILE, stbi_failure_reason())};
			}
			SCOPE_EXIT(stbi_image_free(frame.data));
			blit_A8_to_RGBA(&frame, frame.w, {.c=opts->league_color}, &banner, 9, 578);
		}
		{
			// Left & right
			Image frame;
			frame.data = (void*) stbi_load(BANNER_FRAME_SIDE_FILE, &frame.w, &frame.h, &frame.channels, 1);
			if(frame.data == NULL) {
				return {true, fmt::format("Internal error: Failed to load image \"{}\". Reason: {}", BANNER_FRAME_SIDE_FILE, stbi_failure_reason())};
			}
			SCOPE_EXIT(stbi_image_free(frame.data));
			blit_A8_to_RGBA(&frame, frame.w, {.c=opts->league_color}, &banner, 9, 116);
			blit_A8_to_RGBA(&frame, frame.w, {.c=opts->league_color}, &banner, 882, 116);
		}
	}

	// Blit the date/time text
	{
		Text_Dim dim = get_text_dimensions(&g_banner_font, DATETIME_FONT_SIZE, (const u8*)opts->datetime.c_str());
		Image img;
		init_image(&img, dim.w, dim.h, 1, 0x00000000);
		SCOPE_EXIT(free(img.data));
		render_text_to_image(&g_banner_font, (const u8*)opts->datetime.c_str(), DATETIME_FONT_SIZE, {.c=0xFFFFFFFF}, &img, 0, 0);
		if(img.w < (BANNER_IMAGE_WIDTH - 10)) {
			blit_A8_to_RGBA(&img, img.w, {.c=0xFFFFFFFF}, &banner, (BANNER_IMAGE_WIDTH/2)-(img.w/2), DATETIME_YPOS);
		} else {
			// Scale it to fit.
			f32 ratio = ((f32)(BANNER_IMAGE_WIDTH-10) / dim.w);
			int height = ceil(((f32)dim.h * ratio));
			Image scaled;
			init_image(&scaled, (BANNER_IMAGE_WIDTH-10), height, 1, 0x00000000);
			SCOPE_EXIT(free(scaled.data));
			stbir_resize_uint8_srgb((const u8*)img.data, img.w, img.h, 0,
			                        (u8*)scaled.data, scaled.w, scaled.h, 0, STBIR_1CHANNEL);
			blit_A8_to_RGBA(&scaled, scaled.w, {.c=0xFFFFFFFF}, &banner, (BANNER_IMAGE_WIDTH/2)-(scaled.w/2), DATETIME_YPOS);
		}
	}
	
	// Blit the title text
	draw_shadowed_text(&g_banner_font, FORMAT_TEXT_FONT_SIZE, FORMAT_TEXT_WIDTH_MAX, (const u8*)opts->title.c_str(), 0xFF000000, 0xFFFFFFFF, &banner, FORMAT_TEXT_YPOS);

	if(opts->subtitle.length() > 0) {
		// Blit the subtitle box
		Image sub;
		sub.data = (void*) stbi_load(BANNER_SUBTITLE_FILE, &sub.w, &sub.h, &sub.channels, 1);
		if(sub.data == NULL) {
			return {true, fmt::format("Internal error: Failed to load image \"{}\" Reason: {}", BANNER_SUBTITLE_FILE, stbi_failure_reason())};
		}
		SCOPE_EXIT(stbi_image_free(sub.data));
		blit_A8_to_RGBA_no_alpha(&sub, sub.w, {.c=opts->league_color}, &banner, (BANNER_IMAGE_WIDTH/2)-(sub.w/2), SUBTITLE_FRAME_YPOS);
		// Blit the devotion/hero/etc. icon
		const Icon* icon = get_icon(opts->draft_type);
		if(icon != NULL) {
			Image icon_image;
			icon_image.data = (void*) stbi_load(icon->file, &icon_image.w, &icon_image.h, &icon_image.channels, 4);
			if(icon_image.data == NULL) {
				return {true, fmt::format("Internal error: Failed to load image \"{}\" Reason: {}", icon->file, stbi_failure_reason())}; 
			}
			SCOPE_EXIT(stbi_image_free(icon_image.data));
			blit_RGBA_to_RGBA(&icon_image, &banner, icon->x, icon->y);
			//stbi_image_free(icon_image.data);
		}

		// Draw the subtitle text
		draw_shadowed_text(&g_banner_font, SUBTITLE_TEXT_FONT_SIZE, SUBTITLE_TEXT_WIDTH_MAX, (const u8*)opts->subtitle.c_str(), 0xFF000000, 0xFF04CDFF, &banner, SUBTITLE_TEXT_YPOS);
	}

	// Save the file
	// TODO: Only need to save RGB, this saves having to clear the alpha channel, but does stbii_write support this?
	stbi_write_png_compression_level = 9; // TODO: What's the highest stbi supports?
	image_max_alpha(&banner); // TODO: How do I write only the RGB channels?
	std::string file_path = fmt::format("/tmp/EventBot_Banner_{}.png", random_string(16));
	if(stbi_write_png(file_path.c_str(), banner.w, banner.h, 4, (u8*)banner.data, banner.w*4) == 0) {
		return {true, "Internal error: Failed to save generated banner to storage. Please try again."};
	}

	return {false, file_path};
}


static void delete_draft_posts(dpp::cluster& bot, const u64 guild_id, const std::string& draft_code) {
	auto ids = database_get_draft_post_ids(guild_id, draft_code);
	if(ids == true) {
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
		log(LOG_LEVEL_ERROR, "database_get_draft_post_ids() failed");
	}
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

	// FIXME: This works, but it should probably check the draft status instead, right?
	if(draft_event->time - now <= SECONDS_BEFORE_DRAFT_TO_SEND_REMINDER) {
		minutemage_locked = false;
	}

	if(draft_event->time - now <= SECONDS_BEFORE_DRAFT_TO_PING_TENTATIVES) {
		tentative_locked = true;
	}

	if(now >= draft_event->time) {
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
	button6.set_style(dpp::cos_danger);
	button6.set_label("Decline");
	button6.set_emoji("⛔");
	button6.set_id(draft_event->draft_code + std::string("_decline"));

	dpp::component button_row_two;
	button_row_two.set_type(dpp::cot_action_row);
	button_row_two.add_component(button4);
	button_row_two.add_component(button5);
	button_row_two.add_component(button6);
	message.add_component(button_row_two);
}

static dpp::embed make_sign_up_embed(const u64 guild_id, std::shared_ptr<Draft_Event> draft_event) {
	dpp::embed embed;

	embed.set_color(draft_event->color);

	std::string embed_title = fmt::format(":hourglass_flowing_sand: Draft starts: <t:{}:R>", draft_event->time);
	embed.set_title(embed_title);

	if(draft_event->draftmancer_draft == false) {
		embed.set_description(fmt::format("The draft and games will take place on **{}**.", draft_event->xmage_server));
	} else {
		embed.set_description(fmt::format("The draft will take place on **https://www.draftmancer.com**, games on **{}**.", draft_event->xmage_server));
	}

	const auto sign_ups = database_get_draft_sign_ups(guild_id, draft_event->draft_code);

	// Create the three embed fields (Playing, Tentative, Minutemage) and the list of players for each.
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

		embed.add_field(fmt::format(g_draft_sign_up_columns[i].header, count), names, true);
	}

	embed.set_image(draft_event->banner_url);

	time_t now = time(NULL);
	if(now >= draft_event->time) {
		dpp::embed_footer footer;
		footer.set_text("Sign up for this draft is now locked.");
		footer.set_icon("https://em-content.zobj.net/source/skype/289/locked_1f512.png"); // TODO: Link to somewhere more reliable...
		embed.set_footer(footer);
	}

	return embed;
}

static bool post_draft(dpp::cluster& bot, const u64 guild_id, const std::string& draft_code) {
	auto draft_event = database_get_event(guild_id, draft_code);
	if(draft_event != true) {
		return false;
	}

	char description[1024]; // FIXME: This can overflow...
	expand_format_string(draft_event.value->format, strlen(draft_event.value->format), description, 1024);

	// Timestamp for when the draft ends
	Draft_Duration duration = {(int)draft_event.value->duration, (int)(60.0f * (draft_event.value->duration - (int)draft_event.value->duration))};
	time_t end_timestamp = draft_event.value->time + ((60*60*duration.hours) + (60*duration.minutes));

	// The entire event post content is contained in this string.
	std::string draft_details; 

	// Title line
	draft_details += "~~　　　　　　　　　　　　　　　　　　~~\n"; // NOTE: These are ideographic spaces.
	draft_details += fmt::format("{} The signup for the {} **{}** draft ({}: {}) is now up!\n\nThis draft will take place on **<t:{}:F> - <t:{}:t>**", draft_event.value->pings, draft_event.value->league_name, description, draft_code, draft_event.value->format, draft_event.value->time, end_timestamp);

	// Blurbs
	for(size_t i = 0; i < BLURB_COUNT; ++i) {
		if(strlen(&draft_event.value->blurbs[i][0]) > 0) {
			draft_details += fmt::format("\n\n{}", &draft_event.value->blurbs[i][0]);
		}
	}

	// Set list
	if(strlen(draft_event.value->set_list) > 0) {
		char buffer[1024]; // FIXME: Magic number
		expand_set_list(draft_event.value->set_list, strlen(draft_event.value->set_list), buffer, 1024);
		draft_details += fmt::format("\n{}", buffer); // NOTE: This single newline is intentional.
	}

	// Card list
	if(strlen(draft_event.value->card_list_url) > 0) {
		draft_details += fmt::format("\n\nView the card list here: {}", draft_event.value->card_list_url);
	}

	// Draft Guide
	if(strlen(draft_event.value->draft_guide_url) > 0) {
		draft_details += fmt::format("\n\nView the draft guide here: {}", draft_event.value->draft_guide_url);
	}

	dpp::message details_post;
	details_post.set_type(dpp::message_type::mt_default);
	details_post.set_guild_id(guild_id);
	details_post.set_channel_id(draft_event.value->channel_id);
	details_post.set_content(draft_details);
	details_post.set_allowed_mentions(true, true, true, false, {}, {});

	// Post the details post. If the message is successful we then post the sign up post.
	bot.message_create(details_post, [&bot, guild_id, draft_code, draft_event](const dpp::confirmation_callback_t& callback) {
		if(!callback.is_error()) {
			const dpp::message& message = std::get<dpp::message>(callback.value);
			log(LOG_LEVEL_DEBUG, "Created details post: %lu", (u64)message.id);

			(void)database_set_details_message_id(guild_id, draft_event.value->draft_code, message.id);

			// The entire sign up post is in here.
			dpp::message signups_post;
			signups_post.set_type(dpp::message_type::mt_default);
			signups_post.set_guild_id(guild_id);
			signups_post.set_channel_id(draft_event.value->channel_id);

			dpp::embed embed = make_sign_up_embed(guild_id, draft_event.value);
			signups_post.add_embed(embed);

			add_sign_up_buttons_to_message(signups_post, draft_event.value);

			bot.message_create(signups_post, [&bot, guild_id, draft_event](const dpp::confirmation_callback_t& callback) {
				if(!callback.is_error()) {
					const dpp::message& message = std::get<dpp::message>(callback.value);
					log(LOG_LEVEL_DEBUG, "Created sign ups post: %lu", (u64)message.id);

					(void)database_set_signups_message_id(guild_id, draft_event.value->draft_code, message.id);
					database_set_draft_status(guild_id, draft_event.value->draft_code, DRAFT_STATUS_POSTED);
				} else {
					// TODO: What now???
					log(LOG_LEVEL_ERROR, callback.get_error().message.c_str());
				}
			});
		} else {
			// TODO: What now???
			log(LOG_LEVEL_ERROR, callback.get_error().message.c_str());
		}
	});

	return true;
}

// Called whenever a name is added to the signup list or when the state of the buttons needs to be updated.
static void redraw_signup(dpp::cluster& bot, const u64 guild_id, const u64 message_id, const u64 channel_id, std::shared_ptr<Draft_Event> draft) {
	// Clear the current embeds so we can create a new one.
	bot.message_get(message_id, channel_id, [&bot, guild_id, message_id, channel_id, draft](const dpp::confirmation_callback_t& callback) {
		if(!callback.is_error()) {
			dpp::message message = std::get<dpp::message>(callback.value);

			message.embeds.clear();
			dpp::embed embed = make_sign_up_embed(guild_id, draft);
			message.add_embed(embed);

			add_sign_up_buttons_to_message(message, draft);

			bot.message_edit(message, [&bot, message_id, channel_id](const dpp::confirmation_callback_t& callback) {
				if(!callback.is_error()) {
				} else {
					log(LOG_LEVEL_ERROR, "message_edit(%lu, %lu) failed: %s", message_id, channel_id, callback.get_error().message.c_str());
				}
			});
		} else {
		}

	});
}

static void post_pre_draft_reminder(dpp::cluster& bot, const u64 guild_id, const char* draft_code) {
	auto draft_event = database_get_event(guild_id, draft_code);
	if(draft_event != true) {
		log(LOG_LEVEL_ERROR, "database_get_event() error");
		return;
	};		

	const auto sign_ups = database_get_draft_sign_ups(guild_id, draft_code);

	dpp::message message;
	message.set_type(dpp::message_type::mt_default);
	message.set_guild_id(guild_id);
	message.set_channel_id(IN_THE_MOMENT_DRAFT_CHANNEL_ID);
	message.set_allowed_mentions(true, true, true, true, {}, {});

	std::string text;
	text.reserve(512);
	for(const auto& sign_up : sign_ups.value) {
		if(sign_up.status == SIGNUP_STATUS_DECLINE) continue;
		text += fmt::format("<@{}> ", sign_up.member_id);
	}

	text += "\n\n";
	text += fmt::format(":bell: This is the pre-draft reminder for {}: {} :bell:**\n\n", draft_event.value->draft_code, draft_event.value->format);
	text += "Please confirm your status on the signup sheet below.\n\n";
	text += "Minutemage sign ups are now open. If needed, a minutemage will be selected at random to fill an empty seat.\n\n";
	text += fmt::format("If playing, check your XMage install is up-to-date by starting the launcher, updating if necessary, and connecting to {}.", draft_event.value->xmage_server);

	const auto xmage_version = database_get_xmage_version();
	if(xmage_version == true) {
		u64 timestamp = xmage_version.value.timestamp + SERVER_TIMEZONE_OFFSET;
		// Note: The leading space is intentional as this joins with the previous line.
		text += fmt::format(" The latest XMage release is {}, released <t:{}:R>.", xmage_version.value.version, timestamp);
	}

	message.set_content(text);

	bot.message_create(message, [&bot, guild_id, draft_event = draft_event.value](const dpp::confirmation_callback_t& callback) {
		if(!callback.is_error()) {
			const dpp::message& message = std::get<dpp::message>(callback.value);
			log(LOG_LEVEL_DEBUG, "Created reminder post: %lu", (u64)message.id);
			//(void)database_set_reminder_message_id(guild_id, draft_event->draft_code, message.id);

			// Create the sign ups part.
			dpp::message signup;
			signup.set_type(dpp::message_type::mt_default);
			signup.set_guild_id(guild_id);
			signup.set_channel_id(IN_THE_MOMENT_DRAFT_CHANNEL_ID);
			signup.add_embed(make_sign_up_embed(guild_id, draft_event));
			add_sign_up_buttons_to_message(signup, draft_event);
			bot.message_create(signup, [&bot, guild_id, draft_event](const dpp::confirmation_callback_t& callback) {
				if(!callback.is_error()) {
					const dpp::message& message = std::get<dpp::message>(callback.value);
					log(LOG_LEVEL_DEBUG, "Create reminder signup post: %lu", (u64)message.id);
					(void)database_set_reminder_message_id(guild_id, draft_event->draft_code, message.id);
				}
			});

		} else {
			log(LOG_LEVEL_DEBUG, callback.get_error().message.c_str());
		}
	});
}

static void ping_tentatives(dpp::cluster& bot, const u64 guild_id, const char* draft_code) {
	const auto draft_event = database_get_event(guild_id, draft_code);
	if(draft_event == false) return;

	const auto sign_ups = database_get_draft_sign_ups(guild_id, draft_code);
	if(sign_ups == false) return;

	int tentative_count = 0;
	for(const auto& sign_up : sign_ups.value) {
		if(sign_up.status == SIGNUP_STATUS_TENTATIVE) tentative_count++;
	}

	if(tentative_count > 0) {
		dpp::message message;
		message.set_type(dpp::message_type::mt_default);
		message.set_guild_id(guild_id);
		message.set_channel_id(IN_THE_MOMENT_DRAFT_CHANNEL_ID);
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
		text += fmt::format("Please confirm whether you are joining the imminent draft by clicking your desired pod role or Decline if you are not drafting today: https://discord.com/channels/{}/{}/{}", guild_id, IN_THE_MOMENT_DRAFT_CHANNEL_ID, draft_event.value->reminder_id);

		message.set_content(text);

		bot.message_create(message, [&bot, guild_id, draft = draft_event.value](const dpp::confirmation_callback_t& callback) {
			if(!callback.is_error()) {
				const dpp::message& message = std::get<dpp::message>(callback.value);
				database_set_tentatives_pinged_id(guild_id, draft->draft_code, message.id);
			} else {
				log(LOG_LEVEL_DEBUG, callback.get_error().message.c_str());
			}
		});
	}
}

static void ping_minutemages(dpp::cluster& bot, const u64 guild_id, const char* draft_code) {
	const auto draft_event = database_get_event(guild_id, draft_code);
	if(draft_event == false) return;

	const auto sign_ups = database_get_draft_sign_ups(guild_id, draft_code);
	if(sign_ups == false) return;

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
		message.set_channel_id(IN_THE_MOMENT_DRAFT_CHANNEL_ID);
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
		if(minutemages.size() > 0) {
			if(minutemages.size() <= heroes_needed) {
				// Easiest case - All minutemages (any maybe more!) are needed to fire.
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
					minutemages.erase(minutemages.begin() + r);
				}

				text += fmt::format("! You are needed on {} for {}.", 
					draft_event.value->draftmancer_draft == true ? "Draftmancer" : draft_event.value->xmage_server,
					draft_event.value->format);

				heroes_needed = 0;
			}
		}

		log(LOG_LEVEL_INFO, "%lu still needed", heroes_needed);
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
}

// Post a message to the hosts-only #-current-draft-management channel outlining the procedures for correctly managing the firing of the draft.
static void post_host_guide(dpp::cluster& bot, const char* draft_code) {
	std::string text = fmt::format(":alarm_clock: **Attention hosts! Draft {} has now been partially locked.** :alarm_clock:\n\n", draft_code);

	text += "NOTE: Not everything here is implemented yet!\n\n";

	text += "Use the following commands to manage the signup list before pod allocations are posted:\n";
	text += "	**/add_player** - Add a player to the Playing list. Use this for adding Minutemages.\n";
	text += "	**/remove_player** - Remove a player from the Playing list. Use this for no-shows or members volunteering to drop to make an even number of players.\n";
	text += "\n";

	text += "Nominate a host for each pod:\n";
	text += "   **/add_host**\n";
	text += "   **/remove_host**\n";
	text += "\n";

	text += "Once the Playing list shows all available players and they are confirmed to be playing:\n";
	text += "   **/show_allocations** - Show the pod allocations here in this team-only channel.\n";
	// TODO: On the next line, #in-the-moment-draft should be a link
	text += "   **/post_allocations** - Create threads for each pod and post the pod allocations to #-in-the-moment-draft channel.\n";
	text += "\n";

	text += "Once all pods have been filled, either on XMage or Draftmancer:\n";
	text += "   **/fire** - Post the pre-draft reminders and fully lock the draft.\n";
	text += "\n";

	text += "The follow commands are available once the draft has been fired:\n";
	text += "   **/timer**   - Start a timer for deck submission. Use this when the draft was on Draftmancer.\n";
	text += "	**/dropper** - Increment the drop counter for a member.\n";
	text += "\n";

	text += "After the draft has completed:\n";
	text += "   **/finish** - Post the post-draft reminder message to #in-the-moment draft (close the threads now?).\n";

	send_message(bot, CURRENT_DRAFT_MANAGEMENT_ID, text);
}

// Users on Discord have two names per guild: Their global name or an optional per-guild nickname.
static std::string get_members_preferred_name(const u64 guild_id, const u64 member_id) {
	std::string preferred_name;
	const dpp::guild_member member = dpp::find_guild_member(guild_id, member_id);
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

static void set_bot_presence(dpp::cluster& bot) {
	dpp::presence_status status;
	dpp::activity_type type;
	std::string description;

	auto draft_code = database_get_next_upcoming_draft(GUILD_ID);
	if(draft_code == true) {
		if(draft_code.value.length() > 0) {
			auto draft = database_get_event(GUILD_ID, draft_code.value);
			if(draft == true) {
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
	fprintf(stdout, "CREATE DATABASE IF NOT EXISTS XDHS; USE XDHS\n\n");

	// NOTE: BadgeBot tables not shown here.

	fprintf(stdout, "CREATE TABLE IF NOT EXISTS draft_events(\n");
	// Draft details
	fprintf(stdout, "status INT NOT NULL DEFAULT %d,\n", DRAFT_STATUS_CREATED);
	fprintf(stdout, "guild_id BIGINT NOT NULL,\n");
	fprintf(stdout, "pings VARCHAR(%lu) NOT NULL,\n", PING_STRING_LENGTH_MAX);
	fprintf(stdout, "draft_code VARCHAR(%lu) NOT NULL PRIMARY KEY,\n", DRAFT_CODE_LENGTH_MAX);
	fprintf(stdout, "league_name VARCHAR(%lu) NOT NULL,\n", LEAGUE_NAME_LENGTH_MAX);
	fprintf(stdout, "format VARCHAR(%lu) NOT NULL,\n", DRAFT_FORMAT_LENGTH_MAX);
	fprintf(stdout, "time_zone VARCHAR(%lu) NOT NULL,\n", IANA_TIME_ZONE_LENGTH_MAX);
	fprintf(stdout, "time BIGINT NOT NULL,\n");
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
	fprintf(stdout, "banner_url VARCHAR(%lu) NOT NULL,\n", URL_LENGTH_MAX);

	//fprintf(stdout, "deleted BOOLEAN NOT NULL DEFAULT 0,\n"); // Has the event been deleted?
	fprintf(stdout, "channel_id BIGINT NOT NULL,\n"); // The channel the post is going to.
	fprintf(stdout, "details_id BIGINT NOT NULL DEFAULT 0,\n"); // The message ID of the details post.
	fprintf(stdout, "signups_id BIGINT NOT NULL DEFAULT 0,\n"); // The message ID of the signup post.
	fprintf(stdout, "reminder_id BIGINT NOT NULL DEFAULT 0,\n");
	fprintf(stdout, "tentatives_pinged_id BIGINT NOT NULL DEFAULT 0,\n"); // Has the 10 minute reminder been sent to tentatives?

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
	fprintf(stdout, "drafts INT NOT NULL DEFAULT 0,\n");
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


	fprintf(stdout, "CREATE TABLE IF NOT EXISTS temp_members(\n");
	fprintf(stdout, "guild_id BIGINT NOT NULL,\n"); // TODO: Not needed?
	fprintf(stdout, "member_id BIGINT NOT NULL,\n");
	fprintf(stdout, "role_id BIGINT NOT NULL UNIQUE,\n");
	fprintf(stdout, "FOREIGN KEY (role_id) REFERENCES temp_roles(role_id) ON DELETE CASCADE\n");
	fprintf(stdout, ");");
	fprintf(stdout, "\n\n");


	fprintf(stdout, "CREATE TABLE IF NOT EXISTS noshows(\n");
	fprintf(stdout, "guild_id BIGINT NOT NULL,\n");
	fprintf(stdout, "member_id BIGINT NOT NULL,\n");
	fprintf(stdout, "draft_code VARCHAR(%lu) NOT NULL\n", DRAFT_CODE_LENGTH_MAX);
	fprintf(stdout, ");");
	fprintf(stdout, "\n\n");

	fprintf(stdout, "CREATE TABLE IF NOT EXISTS droppers(\n");
	fprintf(stdout, "guild_id BIGINT NOT NULL,\n");
	fprintf(stdout, "member_id BIGINT NOT NULL,\n");
	fprintf(stdout, "draft_code VARCHAR(%lu) NOT NULL\n", DRAFT_CODE_LENGTH_MAX);
	fprintf(stdout, ");");
	fprintf(stdout, "\n\n");

}

static std::vector<std::string> get_pack_images(const char* format) {
	Set_List list = get_set_list_from_string(format);
	int pack_to_use = 0;
	const char* previous_set_code = NULL;
	std::vector<std::string> result;

	// TODO: If the first set and the third set are the same, but the second is different, use pack art 1 and 2
	if(list.count == 0) {
		// No match found. Do a reverse lookup and find the art.
		const MTG_Draftable_Set* set = get_set_from_name(format);
		if(set != NULL && set->key_art == true) {
			result.push_back(fmt::format("gfx/pack_art/key/{}.png", set->code));
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

			// Conflux needs a special case as its files are in CON_.
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


int main(int argc, char* argv[]) {
#if 0
	// Verify the allocate_pod_seats() function is doing the right thing.
	for(int i = 6; i < 66; i+=2) {
		Draft_Pods pods = allocate_pod_seats(i);
		int sum = 0;
		for(int j = 0; j < pods.table_count; ++j) {
			sum += pods.tables[j].seats;
		}
		if(sum != i) fprintf(stdout, "ERROR: %d has %d seats\n", i, sum);
	}
#endif

	if(argc > 1) {
		if(strcmp(argv[1], "-sql") == 0) {
			// Dump SQL schema to stdout and return.
			output_sql();
			return EXIT_SUCCESS;
		}

	}

	// Load the bot.ini config file. This file has sensitive information and so isn't versioned.
	if(!load_config_file(CONFIG_FILE_PATH, config_file_kv_pair_callback)) {
		return EXIT_FAILURE;
	}

#ifdef DEBUG
    // Careful not to pipe these somewhere a malicious user could find...
    fprintf(stdout, "mysql_host     = '%s'\n", g_config.mysql_host);
    fprintf(stdout, "mysql_username = '%s'\n", g_config.mysql_username);
    fprintf(stdout, "mysql_password = '%s'\n", g_config.mysql_password);
    fprintf(stdout, "mysql_port     = '%d'\n", g_config.mysql_port);
    fprintf(stdout, "logfile_path   = '%s'\n", g_config.logfile_path);
    fprintf(stdout, "discord_token  = '%s'\n", g_config.discord_token);
    fprintf(stdout, "xmage_server   = '%s'\n", g_config.xmage_server);
#endif

	// EventBot runs as a Linux systemd service, so we need to gracefully handle these signals.
    (void)signal(SIGINT,  sig_handler);
    (void)signal(SIGABRT, sig_handler);
    (void)signal(SIGHUP,  sig_handler);
    (void)signal(SIGTERM, sig_handler);
    (void)signal(SIGKILL, sig_handler);

	srand(time(NULL));

	// Set up logging to an external file.
	log_init(g_config.logfile_path);

    log(LOG_LEVEL_INFO, "====== EventBot starting ======");
	log(LOG_LEVEL_INFO, "Build mode: %s",             g_build_mode);
    log(LOG_LEVEL_INFO, "MariaDB client version: %s", mysql_get_client_info());
	log(LOG_LEVEL_INFO, "libDPP++ version: %s",       dpp::utility::version().c_str());
	log(LOG_LEVEL_INFO, "libcurl version: %s",        curl_version());

	// Download and install the latest IANA time zone database.
	// TODO: Only do this if /tmp/tzdata doesn't exist?
	log(LOG_LEVEL_INFO, "Downloading IANA time zone database.");
	const std::string tz_version = date::remote_version();
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
		for(auto& opt : event.options) {
			if(opt.focused) {
				if(opt.name == "draft_code") {
					if(event.name == "post_draft") {
						// Gets a list of all drafts that haven't been posted.
						std::string prefix = std::get<std::string>(opt.value); // What the user has typed so far
						u64 guild_id = event.command.get_guild().id;
						auto draft_codes = database_get_draft_codes_for_post_draft_autocomplete(guild_id, prefix);
						auto response = dpp::interaction_response(dpp::ir_autocomplete_reply);
						for(auto& draft_code : draft_codes.value) {
							response.add_autocomplete_choice(dpp::command_option_choice(draft_code, draft_code));
						}
						bot.interaction_response_create(event.command.id, event.command.token, response);
					} else
					if(event.name == "delete_draft") {
						std::string prefix = std::get<std::string>(opt.value); // What the user has typed so far
						u64 guild_id = event.command.get_guild().id;
						auto draft_codes = database_get_draft_codes_for_delete_draft_autocomplete(guild_id, prefix);
						auto response = dpp::interaction_response(dpp::ir_autocomplete_reply);
						for(auto& draft_code : draft_codes.value) {
							response.add_autocomplete_choice(dpp::command_option_choice(draft_code, draft_code));
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

		// As this is a "private" bot we don't want unknown guilds adding the bot and using the commands. This won't prevent them joining the bot to their guild but it won't install any of the slash commands for them to interact with.
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
			{
				dpp::slashcommand cmd("cpu_burner", "Create banner art for every set that has >= 3 images.", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				bot.guild_command_create(cmd, event.created->id);
			}
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
					draft_type_opt.add_choice(dpp::command_option_choice(to_cstring((DRAFT_TYPE)i), (std::int64_t)i));
				}
				cmd.add_option(draft_type_opt);

				cmd.add_option(dpp::command_option(dpp::co_string, "subheading", "Add a subheading. i.e. 'First draft of the season!'", false));
				cmd.add_option(dpp::command_option(dpp::co_attachment, "art", "Art to use as the background. Will be resized to 900x470 pixels.", false));
				bot.guild_command_create(cmd, event.created->id);
			}
			{
				dpp::slashcommand cmd("create_draft", "Create a new draft.", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				// Required
				cmd.add_option(dpp::command_option(dpp::co_string, "draft_code", "The draft code for this draft. i.e. 123.4-PC.", true));
				cmd.add_option(dpp::command_option(dpp::co_string, "format", "The format of the draft. i.e. TSP/PLC/FUT or 'Artifact Chaos'.", true));
				//cmd.add_option(dpp::command_option(dpp::co_string, "description", "A short description of the draft format. i.e. Time Spiral/Planar Chaos/Future Sight.", true));
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
				cmd.add_option(dpp::command_option(dpp::co_channel, "channel", "Channel to post the signup. Defaults to #-pre-register.", false));
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
				dpp::slashcommand cmd("add_player", "Add a member to the signup list.", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				cmd.add_option(dpp::command_option(dpp::co_user, "member", "The member to add too the signup list.", true));
				auto pod_option = dpp::command_option(dpp::co_integer, "pod", "Which pod to add the member to.", true);
				pod_option.add_choice(dpp::command_option_choice("Competitive", (s64) SIGNUP_STATUS_COMPETITIVE));
				pod_option.add_choice(dpp::command_option_choice("Casual", (s64) SIGNUP_STATUS_CASUAL));
				pod_option.add_choice(dpp::command_option_choice("Flexible", (s64) SIGNUP_STATUS_FLEXIBLE));
				cmd.add_option(pod_option);
				bot.guild_command_create(cmd, event.created->id);
			}
			{
				dpp::slashcommand cmd("remove_player", "Remove a player from the signup list and (optionally) record them as a No Show", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				cmd.add_option(dpp::command_option(dpp::co_user, "member", "The member to remove from the signup list.", true));
				cmd.add_option(dpp::command_option(dpp::co_boolean, "noshow", "Record this as a No Show.", true));
				bot.guild_command_create(cmd, event.created->id);
			}
			{
			}
				//dpp::slashcommand cmd("add_host", "Specify a host for a pod");
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
				bot.guild_command_create(cmd, event.created->id);
			}
			{
				dpp::slashcommand cmd("finish", "Post the post draft message.", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				bot.guild_command_create(cmd, event.created->id);
			}

			g_commands_registered = true;
		}

    });

	bot.on_slashcommand([&bot](const dpp::slashcommand_t& event) {
		const auto command_name = event.command.get_command_name();
		const auto guild_id = event.command.get_guild().id;

		if(command_name == "cpu_burner") {
			event.reply("Here we go!");
			Banner_Opts opts;
			opts.draft_type = DRAFT_TYPE_NOT_APPLICABLE;
			opts.league_color = 0xFFD500FF;
			opts.datetime = "DATETIME / DATETIME / DATETIME / DATETIME";
			for(size_t i = 0; i < SET_COUNT; ++i) {
				const MTG_Draftable_Set* set = &g_draftable_sets[i];
				if(set->pack_images == 1) {
					std::string format = fmt::format("{}/{}/{}", set->code, set->code, set->code);
					opts.images = get_pack_images(format.c_str());
					opts.title = fmt::format("BANNER TEST / SS.W-LT: {}", format);
					log(LOG_LEVEL_DEBUG, "Rendering: %s", format.c_str());
					const Render_Banner_Result banner = render_banner(&opts);
					if(banner.is_error != true) {
						dpp::message message;
						message.set_type(dpp::message_type::mt_default);
						message.set_guild_id(GUILD_ID);
						message.set_channel_id(885048614190190593); // #bot-commands
						message.set_allowed_mentions(false, false, false, false, {}, {});
						message.set_content(format);
						message.add_file("banner.png", dpp::utility::read_file(banner.path));
						bot.message_create(message);
					}
					opts.images.clear();
				}
			}
		} else
		if(command_name == "banner") {
			Banner_Opts opts;
			opts.draft_type = DRAFT_TYPE_NOT_APPLICABLE;

			// Required options
			auto draft_code = std::get<std::string>(event.get_parameter("draft_code"));
			const XDHS_League* league = get_league_from_draft_code(draft_code.c_str());
			if(league == NULL) {
				event.reply("**Invalid draft code.** Draft codes should look like SS.W-RT, where:\n\t**SS** is the season\n\t**W** is the week in the season\n\t**R** is the region code: (E)uro, (A)mericas, (P)acific, A(S)ia or A(T)lantic\n\t**T** is the league type: (C)hrono or (B)onus.");
				return;
			}

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
				const char* result = parse_date_string(date_string.c_str(), &date);
				if(result != NULL) {
					event.reply(fmt::format("Error parsing date: {}", result));
					return;
				}
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

				case LEAGUE_ID_ATLANTIC_BONUS: {
					// Euro and American - Date is the same
					opts.datetime += date::format(" | %H:%M %Z", date::make_zoned("America/New_York", zoned_time));
					opts.datetime += date::format(" | %H:%M %Z", date::make_zoned("America/Los_Angeles", zoned_time));
				} break;

				case LEAGUE_ID_EURO_BONUS: {
					// Nothing extra to add.
				} break;
			}

			opts.title = fmt::format("{} / {}: {}", to_upper(to_cstring(league->id)), draft_code, format);
			
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
		            size_t image_full_size = 0;
		            u8* image_full_data = NULL;
		            SCOPE_EXIT(free(image_full_data));
		            auto download_result = download_file(art.url.c_str(), &image_full_size, &image_full_data);
		            if(download_result != DOWNLOAD_IMAGE_RESULT_OK) {
		                event.edit_response("Internal error: Downloading art image failed. This is not your fault! Please try again.");
		                return;
        		    }

					if(image_full_size > DOWNLOAD_BYTES_MAX) {
						event.edit_response(fmt::format("Downloading art image failed: Image exceeds maximum allowed size of {} bytes. Please resize your image to {}x{} pixels and try again.", DOWNLOAD_BYTES_MAX, BANNER_IMAGE_WIDTH, PACK_IMAGE_HEIGHT));
						return;
					}

					std::string temp_file = fmt::format("/tmp/EventBot_Art_{}", random_string(16));
					FILE* file = fopen(temp_file.c_str(), "wb");
					if(file) {
						event.edit_response(":hourglass_flowing_sand: Saving image");
						SCOPE_EXIT(fclose(file));
						size_t wrote = fwrite(image_full_data, 1, image_full_size, file);
						if(wrote == image_full_size) {
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
			const Render_Banner_Result result = render_banner(&opts);
			if(result.is_error == true) {
				event.edit_response(result.path);
				return;
			}
			auto end = std::chrono::high_resolution_clock::now();
			auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

			dpp::message message;
			message.set_content(fmt::format(":hourglass_flowing_sand: {} ms", elapsed.count()));
			message.add_file(fmt::format("{} - {}.png", draft_code, format), dpp::utility::read_file(result.path));
			event.edit_response(message);
		} else
		if(command_name == "create_draft") {
			Draft_Event draft_event;

			// Required options
			auto draft_code = std::get<std::string>(event.get_parameter("draft_code"));
			// First, check if the draft code is valid and if it is get a copy of the XDHS_League it applies to.
			const XDHS_League* league = get_league_from_draft_code(draft_code.c_str());
			if(league == NULL) {
				event.reply("**Invalid draft code.** Draft codes should look like SS.W-RT, where:\n\tSS is the season\n\tW is the week in the season\n\tR is the region code: (E)uro, (A)mericas, (P)acific, A(S)ia or A(T)lantic\n\tT is the league type: (C)hrono or (B)onus.");
				return;
			}
			strcpy(draft_event.draft_code, draft_code.c_str());

			strcpy(draft_event.league_name, to_cstring(league->id));

			auto format = std::get<std::string>(event.get_parameter("format"));
			if(format.length() > FORMAT_STRING_LEN_MAX) {
				event.reply(fmt::format("Format string exceeds maximum allowed length of {} bytes.", FORMAT_STRING_LEN_MAX));
				return;
			}
			strcpy(draft_event.format, format.c_str());

			// Get the time zone string
			strcpy(draft_event.time_zone, league->time_zone);

			Date date;
			auto date_string = std::get<std::string>(event.get_parameter("date"));
			{
				const char* result = parse_date_string(date_string.c_str(), &date);
				if(result != NULL) {
					event.reply(fmt::format("Error parsing date: {}", result));
					return;
				}
			}

			// Is the default start time for this league overridden?
			Start_Time start_time;
			start_time.hour = league->time.hour;
			start_time.minute = league->time.minute;
			{
				auto opt = event.get_parameter("start_time");
				if(std::holds_alternative<std::string>(opt)) {
					std::string start_time_string = std::get<std::string>(opt);
					Start_Time start_time;
					if(!parse_start_time_string(start_time_string.c_str(), &start_time)) {
						event.reply("Invalid start time. Start time should be entered as HH:MM, in 24 hour time.");
						return;
					}
				}
			}

			// Get the banner image.
			auto banner_id = std::get<dpp::snowflake>(event.get_parameter("banner"));
			auto itr = event.command.resolved.attachments.find(banner_id);
			auto banner = itr->second;
			strcpy(draft_event.banner_url, banner.url.c_str());

			for(size_t i = 0; i < BLURB_COUNT; ++i) {
				static const size_t blurb_name_len = strlen("blurb_x") + 1;
				char blurb_name[blurb_name_len];
				snprintf(blurb_name, blurb_name_len, "blurb_%lu", i + 1);
				auto opt = event.get_parameter(blurb_name);
				if(std::holds_alternative<std::string>(opt)) {
					auto blurb = std::get<std::string>(opt);
					if(blurb.length() > DRAFT_BLURB_LENGTH_MAX) {
						event.reply(fmt::format("blurb_{} exceeds maximum length of {} bytes.", i, DRAFT_BLURB_LENGTH_MAX));
						return;
					}
					strcpy(&draft_event.blurbs[i][0], blurb.c_str()); 
				}
			}

			// Is the draft portion using the MTGA Draft site?
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
						event.reply("Invalid hex string for color. Color should be written as RRGGBB.");
						return;
					}
					for(size_t i = 0; i < color_hex.length(); ++i) {
						if(!isxdigit(color_hex[i])) {
							event.reply("Invalid hex digit in color string.");
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
						event.reply(fmt::format("guide_url exceeds maximum allowed length of {} bytes.", URL_LENGTH_MAX));
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
						event.reply(fmt::format("card_list exceeds maximum allowed length of {} bytes.", URL_LENGTH_MAX));
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
						event.reply(fmt::format("set_list exceeds maximum allowed length of {} bytes.", SET_LIST_LENGTH_MAX));
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
						event.reply(fmt::format("xmage_server string exceeds maximum allowed length of {} bytes.", XMAGE_SERVER_LENGTH_MAX));
						return;
					}
				}
			}
			strcpy(draft_event.xmage_server, xmage_server.c_str());

			draft_event.channel_id = PRE_REGISTER_CHANNEL_ID;
			{
				auto opt = event.get_parameter("channel");
				if(std::holds_alternative<dpp::snowflake>(opt)) {
					draft_event.channel_id = std::get<dpp::snowflake>(opt);
				}
			}

			draft_event.duration = DEFAULT_DRAFT_DURATION.hours + DEFAULT_DRAFT_DURATION.minutes;
			{
				auto opt = event.get_parameter("duration");
				if(std::holds_alternative<double>(opt)) {
					double duration = std::get<double>(opt);
					if(duration < 0.0) {
						event.reply(fmt::format("Duration must be a positive number."));
						return;
					}
					draft_event.duration = duration;
				}
			}

    		auto zoned_time = date::make_zoned(draft_event.time_zone,
				date::local_days{date::year{date.year}/date.month/date.day} +
				std::chrono::hours(start_time.hour) +
				std::chrono::minutes(start_time.minute));
			draft_event.time = std::chrono::system_clock::to_time_t(zoned_time.get_sys_time());
			
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
			if(database_add_draft(guild_id, &draft_event) == true) {
				const dpp::user& issuing_user = event.command.get_issuing_user();
				event.reply(fmt::format("{} created {}. Use ``/post_draft`` to post it.", issuing_user.global_name, draft_code));
			} else {
				event.reply(fmt::format("⚠️ There was an error saving the details of the draft to the database. This is not your fault! Please try again, but in the meantime <@{}> has been alerted.", TANDEM_DISCORD_ID));
				log(LOG_LEVEL_ERROR, "Adding draft to database failed.");
			}
		} else
		if(command_name == "post_draft") {
			// TODO: If the event is already posted, update it.
			const std::string draft_code = std::get<std::string>(event.get_parameter("draft_code"));

			bool result = post_draft(bot, guild_id, draft_code);
			std::string text;
			if(result == true) {
				text = fmt::format("Draft {} posted.", draft_code);
			} else {
				text = fmt::format("⚠️ There was an error posting the draft. This is not your fault! Please try again, but in the meantime <@{}> has been alerted.", TANDEM_DISCORD_ID);
			}
			event.reply(text);
		} else
		if(command_name == "edit_draft") {
			// NOTE: May not need this if we're going to get the draft the data from the spreadsheet...
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
				event.reply(fmt::format("Draft {} post deleted. Use ``/post_draft`` to repost it.", draft_code));
			} else {
				database_purge_draft_event(guild_id, draft_code);
				event.reply(fmt::format("Draft {} and all sign ups purged.", draft_code));
			}
		} else
		if(command_name == "add_player") {
			const auto guild_id = event.command.get_guild().id;

			const auto draft = database_get_event(guild_id, g_current_draft_code);
#if TESTING // Disabled so I can use the command any time.
			if(draft.value->status != DRAFT_STATUS_LOCKED) {
				dpp::message message;
				message.set_flags(dpp::m_ephemeral);
				message.set_content("This command can only be used once the draft is locked.");
				event.reply(message);
				return;
			}
#endif

			const auto member_id = std::get<dpp::snowflake>(event.get_parameter("member"));
			const auto pod = (SIGNUP_STATUS) std::get<std::int64_t>(event.get_parameter("pod"));

			const std::string preferred_name = get_members_preferred_name(guild_id, member_id);
			
			(void)database_sign_up_to_a_draft(guild_id, g_current_draft_code, member_id, preferred_name, time(NULL), pod);

			// Redraw the signup sheet in the #-pre-register channel.
			redraw_signup(bot, guild_id, draft.value->signups_id, draft.value->channel_id, draft.value);

			// Redraw the signup sheet on the reminder message sent to #-in-the-moment-draft
			if(draft.value->reminder_id != 0) {
				redraw_signup(bot, guild_id, draft.value->reminder_id, IN_THE_MOMENT_DRAFT_CHANNEL_ID, draft.value);
			}

			event.reply(fmt::format("{} added to {} {} pod.", preferred_name, g_current_draft_code, to_cstring(pod)));
		} else
		if(command_name == "remove_player") {
			const auto guild_id = event.command.get_guild().id;
			const auto member_id = std::get<dpp::snowflake>(event.get_parameter("member"));

			const auto draft = database_get_event(guild_id, g_current_draft_code);
#if TESTING
			if(draft.value->status != DRAFT_STATUS_LOCKED) {
				dpp::message message;
				message.set_flags(dpp::m_ephemeral);
				message.set_content("This command can only be used once the draft is locked.");
				event.reply(message);
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

			// Redraw the signup sheet in the #-pre-register channel.
			redraw_signup(bot, guild_id, draft.value->signups_id, draft.value->channel_id, draft.value);

			// Redraw the signup sheet on the reminder message sent to #-in-the-moment-draft
			if(draft.value->reminder_id != 0) {
				redraw_signup(bot, guild_id, draft.value->reminder_id, IN_THE_MOMENT_DRAFT_CHANNEL_ID, draft.value);
			}

			event.reply(fmt::format("{} removed from {}.", preferred_name, g_current_draft_code));
		} else
		if(command_name == "post_allocations") {
			// TODO: Create a role with the draft_code as it's name and add everyone to it. Do the same for each Pod
			const auto guild_id = event.command.get_guild().id;

			// TODO: Use new parse_draft_code
			const XDHS_League* league = get_league_from_draft_code(g_current_draft_code.c_str());
			char league_code[3];
			make_2_digit_league_code(league, league_code);
			auto sign_ups = database_get_sign_ups(guild_id, g_current_draft_code, league_code);
			if(sign_ups != true) {
				log(LOG_LEVEL_ERROR, "database_get_sign_ups failed");
				return;
			}	

			if(sign_ups.count < POD_SEATS_MIN) {
				event.reply(fmt::format("At least {} players needed. Use /add_player.", POD_SEATS_MIN));
				return;
			}

			if((sign_ups.count % 2) == 1) {
				event.reply("Odd number of sign ups. Use /add_player or /remove_player.");
				return;
			}

			if(sign_ups.count > PLAYERS_MAX) {
				event.reply("Maximum player count of {} exceeded. You're on your own!");
				return;
			}

			Draft_Pods pods = allocate_pod_seats(sign_ups.count);

			if(pods.table_count == 0) {
				// Shouldn't be possible with the above checks.
				event.reply("Insufficient players to form a pod.");
				return;
			}


			std::string all_sign_ups;
			all_sign_ups += "#,id,name,status,time,rank,is_shark,points,devotion,win_rate\n";
			for(u64 player = 0; player < sign_ups.count; ++player) {
				all_sign_ups += fmt::format("{},<@{}>,{},{},{},{},{},{},{},{}\n",
					(player+1),
					sign_ups.value[player].member_id,
					sign_ups.value[player].preferred_name,
					(int)sign_ups.value[player].status,
					sign_ups.value[player].time,
					(sign_ups.value[player].rank_is_null == true ? 0 : sign_ups.value[player].rank),
					(sign_ups.value[player].is_shark_is_null == true ? 0 : sign_ups.value[player].is_shark),
					(sign_ups.value[player].points_is_null == true ? -1 : sign_ups.value[player].points),
					(sign_ups.value[player].devotion_is_null == true ? 0 : sign_ups.value[player].devotion),
					(sign_ups.value[player].win_rate_is_null == true ? 0.0f : sign_ups.value[player].win_rate)
					);

			}
			log(LOG_LEVEL_INFO, all_sign_ups.c_str());
			event.reply(all_sign_ups);

			// Create a role that contains everyone in this pod.
			// TODO: In case this isn't the first invocation of post_allocations we need a way
			// to clear old roles and recreate them...
			// Create a role with everyone in this draft.
			dpp::role role;
			role.set_guild_id(guild_id);
			role.set_name(g_current_draft_code);
			role.set_color(league->color);
			bot.role_create(role, [&bot, guild_id, sign_ups](const dpp::confirmation_callback_t& callback) {
				if(!callback.is_error()) {
					dpp::role role = std::get<dpp::role>(callback.value);
					const auto result = database_add_temp_role(guild_id, g_current_draft_code.c_str(), role.id);
					if(result == true) {
						for(u64 i = 0; i < sign_ups.count; ++i) {
							// Give each sign up this role.
							u64 member_id = sign_ups.value[i].member_id;
							u64 role_id = role.id;
							dpp::guild_member member = dpp::find_guild_member(guild_id, member_id); // TODO: This can throw exceptions!
							std::vector<dpp::snowflake> roles = member.get_roles();
							roles.push_back(role_id);
							member.set_roles(roles);
							bot.guild_edit_member(member, [&bot, guild_id, member_id, role_id](const dpp::confirmation_callback_t& callback){
								if(!callback.is_error()) {
									auto result = database_add_temp_member_role(guild_id, member_id, role_id);
									if(result == true) {
										log(LOG_LEVEL_INFO, "Added %lu to %lu", member_id, role_id);
									}
								} else {
									log(LOG_LEVEL_ERROR, "%s: guild_id:%lu member_id:%lu role_id:%lu",
										callback.get_error().message.c_str(),
										guild_id,
										member_id,
										role_id
										);
								}
							});
						}
					}
				} else {
					log(LOG_LEVEL_ERROR, "Failed to create role for %s.", g_current_draft_code.c_str());
				}
			});

			// Flag all potential hosts and flag everyone as not allocated.
#if 0
			int host_indexes[64];
			int host_count = 0;

			for(size_t i = 0; i < sign_ups.value.size(); ++i) {
				sign_ups.value[i].is_host = false;
				sign_ups.value[i].allocated = false;
				const dpp::guild_member member = dpp::find_guild_member(guild_id, sign_ups.value[i].member_id);
				for(auto role : member.roles) {
					if((role == XDHS_TEAM_ROLE_ID) || (role == XDHS_HOST_ROLE_ID)) {
						sign_ups.value[i].is_host = true;
						host_indexes[host_count] = i;
						++host_count;
					}
				}
			}

			if(host_count < pods.table_count) {
				// TODO: Now what?
			}

			std::vector<Draft_Sign_Up*> hosts;
			for(int i = 0; i < host_count; ++i) {
				hosts.push_back(&sign_ups.value[host_indexes[i]]);
			}
#endif

/* Pod Priority Rules
#1: The host of that pod - If the only available hosts (@XDHS Team members and @Hosts) are both in the leaderboard Top 3, the lowest-ranked host will host Pod 2.
#2: Players who are required to play in that pod via the Rule of 3
#3: Players with "Shark" status for the current Season (must play in pod 1)
#4: New XDHS players and Goblins (1-4 drafts played) have priority for Pod 2
#5: Players who reacted with their preferred emoji ( :Pod1~1:  or :Pod2~1: ) in #-pre-register
#6: Players who didn't react in #-pre-register (first among these to join the draft table on XMage gets the spot)
The tiebreaker for #3/4/5 is determined by the order output from the randomizer.
*/

			// Create a message to tell everyone their allocation, and then create threads for each pod.
			// Reference: bot.thread_create_with_message("123P-C", IN_THE_MOMENT_DRAFT_CHANNEL_ID, 1090316907871211561, 1440, 0, [&bot](const dpp::confirmation_callback_t& event) {
			log(LOG_LEVEL_INFO, "Creating %d pod(s)", pods.table_count);
			for(int pod = 0; pod < pods.table_count; ++pod) { // TODO: Create in reverse order?
				dpp::message post;
				post.set_type(dpp::message_type::mt_default);
				post.set_guild_id(guild_id);
				post.set_channel_id(IN_THE_MOMENT_DRAFT_CHANNEL_ID);
				post.set_allowed_mentions(true, true, true, false, {}, {});
				post.set_content(fmt::format("Pod {} Allocations: **WIP!**", pod+1));
				// TODO: Need to pass in an array of members in this pod and why there were allocated here
				bot.message_create(post, [&bot, guild_id, pod](const dpp::confirmation_callback_t& callback) {
					if(!callback.is_error()) {
						// Message posted. Create a thread attached to it.
						const dpp::message& message = std::get<dpp::message>(callback.value);
						bot.thread_create_with_message(fmt::format("{} - Pod {}", g_current_draft_code, pod + 1), IN_THE_MOMENT_DRAFT_CHANNEL_ID, message.id, 1440, 0, [&bot, pod](const dpp::confirmation_callback_t& callback) {
							if(!callback.is_error()) {
								dpp::thread thread = std::get<dpp::thread>(callback.value);
								// TODO: Add the thread ID to the database with the draft code so when the draft is deleted the threads are archived.
								// TODO: thread_member_add() for each member allocated to this pod.

								// TESTING: Try post a message in the thread.
								dpp::message post;
								post.set_type(dpp::message_type::mt_default);
								post.set_guild_id(thread.guild_id);
								post.set_channel_id(thread.id);
								post.set_content(fmt::format("Thread for Pod {}", pod+1));
								bot.message_create(post, [](const dpp::confirmation_callback_t& callback) {
									if(!callback.is_error()) {
									} else {
										log(LOG_LEVEL_ERROR, callback.get_error().message.c_str());
									}
								});
							} else {
								log(LOG_LEVEL_ERROR, callback.get_error().message.c_str());
							}
						});
					} else {
						log(LOG_LEVEL_ERROR, callback.get_error().message.c_str());
					}
				});
			}


#if 0
			std::string text;
			for(auto& sign_up : sign_ups.value) {
				sign_up.is_host = false;
				const dpp::guild_member member = dpp::find_guild_member(guild_id, sign_up.member_id);
				for(auto role : member.roles) {
					if((role == XDHS_TEAM_ROLE_ID) || (role == XDHS_HOST_ROLE_ID)) {
						sign_up.is_host = true;
					}
				}

				if(sign_up.is_host == true) text += ":cowboy:";
				if(sign_up.is_shark_is_null == false && sign_up.is_shark == true) text += ":shark:";
				if(sign_up.status == 1) text += ":one:";
				if(sign_up.status == 2) text += ":two:";
				if(sign_up.status == 3) text += ":person_shrugging:";

				text += sign_up.preferred_name;
				
				text += "\n";
			}
			event.reply(text);
#endif
		} else
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
		if(command_name == "timer") {
			// TODO: Ping everyone in this pod? How would we know what pod this is for?
			const auto guild_id = event.command.get_guild().id;
			time_t now = time(0) + DECK_CONSTRUCTION_MINUTES;
			std::string text = fmt::format("* Now the draft has finished, you can build your deck either on Draftmancer or XMage. Export as MTGA format from Draftmancer, then import to XMage from the clipboard. Don't forget to choose your favorite basic land art!\n* Make sure not to double-click cards while editing your deck in XMage (that will remove the card from your deck rather than moving it to the sideboard, and you'll have to reimport to fix it). Drag and drop instead.\n* Save your deck when done building and join the XMage table for your pod when it goes up.\n\nYour timer for deck construction expires <t:{}:R>", now);
			event.reply(text);
		} else
		if(command_name == "dropper") {
			const auto guild_id = event.command.get_guild().id;
			const auto member_id = std::get<dpp::snowflake>(event.get_parameter("member"));

			database_add_dropper(guild_id, member_id, g_current_draft_code.c_str());
			const std::string preferred_name = get_members_preferred_name(guild_id, member_id);
			event.reply(fmt::format("{} added to droppers list.", preferred_name));
		} else
		if(command_name == "finish") {
			std::string text;
			text += "## Thanks everyone for drafting with us!\n";
			text += fmt::format("* You can share a screenshot of your deck in <#{}>.\n", DECK_SCREENSHOTS_CHANNEL_ID);
			text += fmt::format("* If you want feedback on your draft, just ask or give yourself the Civilized Scholar role in <#{}>).\n", ROLE_SELF_ASSIGNMENT_CHANNEL_ID);
			text += fmt::format("* You can also upload your draftlog to <https://magic.flooey.org/draft/upload> and share it in <#{}>.\n", P1P1_AND_DRAFT_LOG_CHANNEL_ID);
			text += fmt::format("* We're happy to hear feedback on how to improve, either in <#{}> or anonymously with the /feedback command.\n", FEEDBACK_CHANNEL_ID);
			text += fmt::format("* Check out <#{}> to see upcoming events, and sign up in <#{}>!", CALENDAR_CHANNEL_ID, PRE_REGISTER_CHANNEL_ID);
			event.reply(text);
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
		if(current_sign_up_status != true) {
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
		if(draft != true) {
			log(LOG_LEVEL_ERROR, "database_get_event(%lu, %s) failed" , guild_id, draft_code.c_str());
		}

		event.reply(); // Acknowledge the interaction, but show nothing to the user.

		// The signup sheet in the #-pre-register channel.
		redraw_signup(bot, guild_id, draft.value->signups_id, draft.value->channel_id, draft.value);

		// The signup sheet on the reminder message sent to #-in-the-moment-draft
		if(draft.value->reminder_id != 0) {
			redraw_signup(bot, guild_id, draft.value->reminder_id, IN_THE_MOMENT_DRAFT_CHANNEL_ID, draft.value);
		}

		// If the draft has been locked, alert the hosts when someone has clicked the minutemage button.
		if(new_sign_up_status == SIGNUP_STATUS_MINUTEMAGE) {
			if(now >= draft.value->time) {
				send_message(bot, IN_THE_MOMENT_DRAFT_CHANNEL_ID, fmt::format(":warning: {} signed up as a minutemage. :warning:", preferred_name));
			}
		}
	});

	// Called when the bot has successfully connected to Discord.
	bot.on_ready([&bot](const dpp::ready_t& event) {
		//log(LOG_LEVEL_INFO, "on_ready received");
		// Discord will now call on_guild_create for each guild this bot is a member of.
	});

    bot.start(true);

	bot.start_timer([&bot](dpp::timer t) {
		set_bot_presence(bot);

		auto draft_code = database_get_next_upcoming_draft(GUILD_ID);	
		if(draft_code != true) {
			return;
		}
		if(draft_code.count == 0) return; // No scheduled drafts.

		g_current_draft_code = draft_code.value;

		auto draft = database_get_event(GUILD_ID, draft_code.value);
		if(draft.count == 0) {
			return;
		}

		time_t now = time(NULL);

		// Send the pre-draft reminder message if it hasn't already been sent.
		if(!(BIT_SET(draft.value->status, DRAFT_STATUS_REMINDER_SENT)) && (draft.value->time - now <= SECONDS_BEFORE_DRAFT_TO_SEND_REMINDER)) {
			send_message(bot, BOT_COMMANDS_CHANNEL_ID, fmt::format("{} - Sending pre-draft reminder message and unlocking minutemage sign up.", draft_code.value.c_str()));
			// TODO: Remove mentions on this when the draft is fired?
			// TODO: Unlock the minute mage signups
			post_pre_draft_reminder(bot, GUILD_ID, draft_code.value.c_str());
			database_set_draft_status(GUILD_ID, draft_code.value, DRAFT_STATUS_REMINDER_SENT);
		}

		// Ping the tentatives if they haven't already been pinged.
		if(!(BIT_SET(draft.value->status, DRAFT_STATUS_TENTATIVES_PINGED)) && (draft.value->time - now <= SECONDS_BEFORE_DRAFT_TO_PING_TENTATIVES)) {
			send_message(bot, BOT_COMMANDS_CHANNEL_ID, fmt::format("{} - Sending tentative reminder message, if needed.", draft_code.value.c_str()));
			// Redraw the sign up posts so the Tentative button shows as locked.
			redraw_signup(bot, GUILD_ID, draft.value->signups_id, draft.value->channel_id, draft.value);
			redraw_signup(bot, GUILD_ID, draft.value->reminder_id, IN_THE_MOMENT_DRAFT_CHANNEL_ID, draft.value);
			ping_tentatives(bot, GUILD_ID, draft_code.value.c_str());
			database_set_draft_status(GUILD_ID, draft_code.value, DRAFT_STATUS_TENTATIVES_PINGED);
		}

		// Lock the draft.
		if((draft.value->status < DRAFT_STATUS_LOCKED) && now >= draft.value->time) {
			send_message(bot, BOT_COMMANDS_CHANNEL_ID, fmt::format("{} - Locking sign ups and pinging for a minutemage, if needed.", draft_code.value.c_str()));
			database_set_draft_status(GUILD_ID, draft_code.value, DRAFT_STATUS_LOCKED);
			post_host_guide(bot, draft_code.value.c_str());
			// Redraw the signup buttons so they all (except Minutemage) appear locked.
			redraw_signup(bot, GUILD_ID, draft.value->signups_id, draft.value->channel_id, draft.value);
			redraw_signup(bot, GUILD_ID, draft.value->reminder_id, IN_THE_MOMENT_DRAFT_CHANNEL_ID, draft.value);
			// Ping minutemages if there is an odd number of confirmed sign ups.
			ping_minutemages(bot, GUILD_ID, draft_code.value.c_str());
		}

		// Delete the draft after a few hours.
		if((draft.value->status < DRAFT_STATUS_COMPLETE) && now - draft.value->time > SECONDS_AFTER_DRAFT_TO_DELETE_POSTS) {
			send_message(bot, BOT_COMMANDS_CHANNEL_ID, fmt::format("{} - Deleting completed draft.", draft_code.value.c_str()));
			delete_draft_posts(bot, GUILD_ID, draft_code.value);
			database_clear_draft_post_ids(GUILD_ID, draft_code.value);
			database_set_draft_status(GUILD_ID, draft_code.value, DRAFT_STATUS_COMPLETE);

			// TODO: Delete any roles created
			// TODO: Delete/archive any threads created.
		}

	}, JOB_THREAD_TICK_RATE, [](dpp::timer){});

    while(g_quit == false) {
        sleep(1);
    }

    return g_exit_code;
}

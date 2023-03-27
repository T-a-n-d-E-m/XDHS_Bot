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

// C libraries
#include <alloca.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>

// C++ libraries
#include <cinttypes>

// System libraries
#include <mysql.h>

// User libraries
#include <dpp/dpp.h>
#include <fmt/format.h>

// Local libraries
#include "date/tz.h"                   // Howard Hinnant's date and timezone library.
#include "../../BadgeBot/log.h"        // Not versioned - shared from a private repo
#include "../../BadgeBot/config.h"     // Not versioned - shared from a private repo
#include "../../BadgeBot/scope_exit.h" // Not versioned - shared from a private repo

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

// How many seconds before a draft the pre-draft reminder message should be sent.
static const time_t SECONDS_BEFORE_DRAFT_TO_SEND_REMINDER   = (60*60*1);

// How many seconds before a draft to remind tentatives to confirm their status.
static const time_t SECONDS_BEFORE_DRAFT_TO_PING_TENTATIVES = (60*10);

// How long after a draft to wait before deleting the signup posts from the #-pre-register channel.
static const time_t SECONDS_AFTER_DRAFT_TO_DELETE_POSTS     = (60*60*5);

// How often often to spin up the thread that sends the pre-draft reminders, tentatives ping, etc.
static const dpp::timer JOB_THREAD_TICK_RATE = 15;

// The bot is designed to run in two modes, Debug and Release. Debug builds will only run on the XDHS Dev server and Release builds will only run on the public XDHS server.
// In the future we might want to control these values with a bot command, but for now we'll simply hard code them in.
#ifdef DEBUG
// The bot will be running in debug mode on the XDHS Dev server.
static const char* g_build_mode                 = "Debug";
static const u64 GUILD_ID                       = 882164794566791179;
static const u64 PRE_REGISTER_CHANNEL_ID        = 907524659099099178; // Default channel to post the draft signup.
static const u64 CURRENT_DRAFT_MANAGEMENT_ID    = 1087299085612109844;
static const u64 IN_THE_MOMENT_DRAFT_CHANNEL_ID = 1075355794507305001;
static const u64 BOT_COMMANDS_CHANNEL_ID        = 885048614190190593;
static bool g_commands_registered               = false; // Have the bot slash commands been registered for this guild?
#else
// The bot will be running in release mode on the XDHS public server.
static const char* g_build_mode                 = "Release";
static const u64 GUILD_ID                       = 528728694680715324;
static const u64 PRE_REGISTER_CHANNEL_ID        = 753639027428687962; // Default channel to post the draft signup.
static const u64 CURRENT_DRAFT_MANAGEMENT_ID    = 921027014822068234;
static const u64 IN_THE_MOMENT_DRAFT_CHANNEL_ID = 535127333401657354;
static const u64 BOT_COMMANDS_CHANNEL_ID        = 753637350877429842;
static bool g_commands_registered               = false // Have the bot slash commands been registered for this guild?;
#endif

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

struct MTG_Draftable_Sets {
	const char* code;
	const char* name;
};

// Not an complete list - just the sets we draft (or explicitly DON'T) at XDHS.
// This is probably not the best way to handle this as new sets (official or not) need to keep this updated.
// Perhaps move it to the database and use a /add_set command?
static const MTG_Draftable_Sets g_draftable_sets[] = {
	{"LEB", "Limited Edition Beta"},
	{"MIR", "Mirage"},
	{"VIS", "Visions"},
	{"WTH", "Weatherlight"},
	{"POR", "Portal"},
	{"P02", "Portal Second Age"},
	{"TMP", "Tempest"},
	{"STH", "Stronghold"},
	{"EXO", "Exodus"},
	{"USG", "Urza's Saga"},
	{"ULG", "Urza's Legacy"},
	{"UDS", "Urza's Destiny"},
	{"PTK", "Portal Three Kingdoms"},
	{"MMQ", "Mercadian Masques"},
	{"NEM", "Nemesis"},
	{"PCY", "Prophecy"},
	{"INV", "Invasion"},
	{"PLS", "Planeshift"},
	{"APC", "Apocalypse"},
	{"ODY", "Odyssey"},
	{"TOR", "Torment"},
	{"JUD", "Judgment"},
	{"ONS", "Onslaught"},
	{"LGN", "Legions"},
	{"SCG", "Scourge"},
	{"MRD", "Mirrodin"},
	{"DST", "Darksteel"},
	{"5DN", "Fifth Dawn"},
	{"CHK", "Champions of Kamigawa"},
	{"BOK", "Betrayers of Kamigawa"},
	{"SOK", "Saviors of Kamigawa"},
	{"RAV", "Ravnida: City of Guilds"},
	{"GPT", "Guildpact"},
	{"DIS", "Dissension"},
	{"CSP", "Coldsnap"},
	{"TSP", "Time Spiral"},
	{"PLC", "Planar Choas"},
	{"FUT", "Future Sight"},
	{"ME1", "Masters Edition"},
	{"LRW", "Lorwyn"},
	{"MOR", "Morningtide"},
	{"SHM", "Shadowmoor"},
	{"EVE", "Eventide"},
	{"ME2", "Masters Edition II"},
	{"ALA", "Shards of Alara"},
	{"CON", "Conflux"},
	{"ARB", "Alara Reborn"},
	{"M10", "Magic 2010"},
	{"ME3", "Masters Edition III"},
	{"ZEN", "Zendikar"},
	{"WWK", "Worldwake"},
	{"ROE", "Rise of the Eldrazi"},
	{"M11", "Magic 2011"},
	{"ME4", "Masters Edition IV"},
	{"SOM", "Scars of Mirrodin"},
	{"MBS", "Mirrodin Besieged"},
	{"NPH", "New Phyrexia"},
	{"M12", "Magic 2012"},
	{"ISB", "Innistrad"},
	{"DKA", "Dark Ascension"},
	{"AVR", "Avacyn Restored"},
	{"M13", "Magic 2013"},
	{"RTR", "Return to Ravnica"},
	{"GTC", "Gatecrash"},
	{"DGM", "Dragon's Maze"},
	{"MMA", "Modern Masters"},
	{"M14", "Magic 2014"},
	{"THS", "Theros"},
	{"BNG", "Born of the Gods"},
	{"JOU", "Journey into Nyx"},
	{"CNS", "Conspiracy"},
	{"VMA", "Vintage Masters"},
	{"M15", "Magic 2015"},
	{"KTK", "Khans of Tarkir"},
	{"FRF", "Fate Reforged"},
	{"DTK", "Dragons of Tarkir"},
	{"TPR", "Tempest Remastered"},
	{"MM2", "Modern Masters 2015"},
	{"ORI", "Magic Origins"},
	{"BFZ", "Battle for Zendikar"},
	{"OGW", "Oath of the Gatewatch"},
	{"SOI", "Shadows Over Innistrad"},
	{"EMA", "Eternal Masters"},
	{"EMN", "Eldritch Moon"},
	{"CN2", "Conspiracy: Take the Crown"},
	{"KLD", "Kaladesh"},
	{"AER", "Aether Revolt"},
	{"MM3", "Modern Masters 2017"},
	{"AKH", "Amonkhet"},
	{"HOU", "Hour of Devastation"},
	{"XLN", "Ixalan"},
	{"IMA", "Iconic Masters"},
	{"RIX", "Rivals of Ixalan"},
	{"A25", "Masters 25"},
	{"DOM", "Dominaria"},
	{"M19", "Core Set 2019"},
	{"GRN", "Guilds of Ravnica"},
	{"UMA", "Ultimate Masters"},
	{"RNA", "Ravnica Allegiance"},
	{"WAR", "War of the Spark"},
	{"MH1", "Modern Horizons"},
	{"M20", "Core Set 2020"},
	{"ELD", "Throne of Eldraine"},
	{"MB1", "Mystery Booster"},
	{"THB", "Theros Beyond Death"},
	{"IKO", "Ikoria: Lair of Behemoths"},
	{"M21", "Core Set 2021"},
	{"2XM", "Double Masters"},
	{"AKR", "Amonkhet Remastered"},
	{"ZNR", "Zendikar Rising"},
	{"KLR", "Kaladesh Remastered"},
	{"CMD", "Commander Legends"},
	{"KHM", "Kaldheim"},
	{"TSR", "Time Spiral Remastered"},
	{"STX", "Strixhaven: School of Mages"},
	{"MH2", "Modern Horizons 2"},
	{"AFR", "Adventures in the Forgotten Realms"},
	{"MID", "Innistrad: Midnight Hunt"},
	{"VOW", "Innistrad: Crimson Vow"},
	{"NEO", "Kamigawa: Neon Dynasty"},
	{"SNC", "Streets of New Capenna"},
	{"CLB", "Commander Legends: Battle for Baldur's Gate"},
	{"2X2", "Double Masters 2022"},
	{"DMU", "Dominaria United"},
	{"BRO", "The Brothers' War"},
	{"DMR", "Dominaria Remastered"},
	{"ONE", "Phyrexia: All Will Be One"},
	{"MOM", "March of the Machine"},

	// XDHS team custom and remastered sets
	{"INVR", "Invasion Remastered"},
	{"KMGR", "Kamigawa Remastered"},
	{"PMMA", "Pre Mirage Masters"},
	{"GBMA", "Garbage Masters"},

	// Member custom and remastered sets
	{"SLCS", "The Sliver Core Set"},
	{"USGR", "Urza Block Redeemed"},
	{"TWAR", "Total WAR"},
	{"ATQR", "Antiquities Reforged"},
	{"ISDR", "Innistrad Remastered "},
	{"CRSN", "Core Resonance"},
	{"DOMR", "Dominaria Revised"},
	{"10LE", "10 Life Edition"},
	{"GLNT", "Ravnica Revolution: GLINT"},
};
static const size_t SET_COUNT = sizeof(g_draftable_sets) / sizeof(MTG_Draftable_Sets);

// Hardly the most efficient way to search, but until the set list has thousands of entries this will be fast enough.
static const char* get_set_name_from_code(const char* code) {
	for(size_t i = 0; i < SET_COUNT; ++i) {
		if(strcasecmp(g_draftable_sets[i].code, code) == 0) {
			return g_draftable_sets[i].name;
		}
	}
	return NULL;
}

// Parse the set_codes string (e.g. TMP,EXO,ELD,ISD,...) and expand them into full set names (e.g. Tempest, Exodus, Eldraine, Innistrad, ...) writing the results into the out variable.
static const void make_set_list(const char* set_codes, const size_t len, char* out, size_t out_len) {
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
			if(set_name == NULL) set_name = format; // Use the set name passed in
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

struct XDHS_League {
	const char* name;                   // Full name of the league.
	char region_code;                   // (E)uro, (A)mericas, A(S)ia, (P)acific, A(T)lantic
	char league_type;                   // (C)hrono or (B)onus
	u32 color;                          // Color for the league
	const char* time_zone;              // IANA time zone identifier
	Start_Time time;                    // When the draft starts
	const char* ping[LEAGUE_PINGS_MAX]; // Which roles to ping when the signup goes up
	const char* icon;                   // League icon file name. Flag images from https://flagpedia.net/download/images
};

// Lookup table for each of our leagues. The order these are listed doesn't matter. In the future we may want bot commands to create, edit and delete leagues but to keep things simple for now we'll hard code these into the bot.
static const XDHS_League g_xdhs_leagues[] = {
	{
		"Atlantic Bonus",
		'T','B',
		0x00ed8821,
		"Europe/Berlin",
		{19,50},
		{"Euro","Americas"},
		"https://i.imgur.com/VMN7obw.png"
	},
	{
		"Americas Bonus",
		'A','B',
		0x006aa84f,
		"America/New_York",
		{20,50},
		{"Americas",NULL},
		"https://i.imgur.com/9q8TUXj.png"
	},
	{
		"Euro Bonus",
		'E','B',
		0x0061a0da,
		"Europe/Berlin",
		{19,50},
		{"Euro",NULL},
		"https://i.imgur.com/D0LOKkV.png"
	},
	{
		"Americas Chrono",
		'A','C',
		0x002c7652,
		"America/New_York",
		{20,50},
		{"Americas",NULL},
		"https://i.imgur.com/9q8TUXj.png"
	},
	{
		"Euro Chrono",
		'E','C',
		0x000d5ba1,
		"Europe/Berlin",
		{19,50},
		{"Euro",NULL},
		"https://i.imgur.com/D0LOKkV.png"
	},
	{
		"Asia Chrono",
		'S','C',
		0x00793fab,
		"Europe/Berlin",
		{10,50},
		{"Asia",NULL},
		"https://i.imgur.com/EA0TAtu.png"
	},
	{
		"Pacific Chrono",
		'P','C',
		0x00b82f4b,
		"America/New_York",
		{20,50},
		{"Pacific",NULL},
		"https://i.imgur.com/hfF5AdW.png"
	}
};
static const size_t LEAGUE_COUNT = sizeof(g_xdhs_leagues) / sizeof(XDHS_League);

// Parse a draft code and find the league it is for. Makes a copy into 'league' and returns true if found, false otherwise.
static bool get_league_from_draft_code(const char* draft_code, XDHS_League* league) {
	if(draft_code == NULL) return false;

	while(isdigit(*draft_code)) {draft_code++;} // Skip the numeric part
	if(strlen(draft_code) != 3) return false;
	char region_code = *draft_code++;
	draft_code++; // Skip the hyphen
	char league_type = *draft_code;

	for(size_t i = 0; i < LEAGUE_COUNT; ++i) {
		const XDHS_League* ptr = &g_xdhs_leagues[i];
		if((ptr->region_code == region_code) && (ptr->league_type == league_type)) {
			// Found it!
			memcpy(league, ptr, sizeof(XDHS_League));
			return true;
		}
	}

	return false;
}


// With player_count players, how many pods should be created?
// Reference: https://i.imgur.com/tpNo13G.png
// TODO: This needs to support a player_count of any size
int pod_count(int player_count) {
	if(player_count >= 1 && player_count <= 10) {
		return 1;
	} else
	if(player_count >= 11 && player_count <= 18) {
		return 2;
	} else
	if(player_count >= 19 && player_count <= 26) {
		return 3;
	} else
	if(player_count >= 27 && player_count <= 36) {
		return 4;
	} else
	
	return 5;
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
// Despite the harsh sounding error strings, this tries to be quite generous and forgiving.
// For example, it will accept a date written as YY/M/D
static const char* parse_date_string(const char* date_string, Date* out) {
	if(strlen(date_string) < strlen("YY-M-D")) return "String is too short to contain a valid date. Date should be written as YYYY-MM-DD.";
	if(strlen(date_string) > strlen("YYYY-MM-DD")) return "String is too long. Date should be written as YYYY-MM-DD.";

	// Make a mutable copy of the date string, including terminator.
	char str[strlen("YYYY-MM-DD")+1];
	memcpy(str, date_string, strlen(date_string)+1);
	char* str_ptr = str;

	// Parse the year
	const char* year = str_ptr;
	while(isdigit(*str_ptr)) {
		str_ptr++;
	}
	if(*str_ptr != '-' && *str_ptr != '\\' && *str_ptr != '/') {
		return "Unsupported separator used. Use - between digits.";
	}
	*str_ptr++ = 0;
	if(strlen(year) < 2) return "Date should be written as YYYY-MM-DD.";
	out->year = (int) strtol(year, NULL, 0);
	if(strlen(year) == 2) out->year += 2000; // Two digits given, make it four.

	// Parse the month
	const char* month = str_ptr;
	while(isdigit(*str_ptr)) {
		str_ptr++;
	}
	if(*str_ptr != '-' && *str_ptr != '\\' && *str_ptr != '/') {
		return "Unsupported separator used. Use - between digits.";
	}
	*str_ptr++ = 0;
	if(strlen(month) < 1) return "Date should be written as YYYY-MM-DD.";
	out->month = (int) strtol(month, NULL, 0);

	// Parse the day
	const char* day = str_ptr;
	while(isdigit(*str_ptr)) {
		str_ptr++;
	}
	if(*str_ptr != '\0') {
		// TODO: With the previous checks, this should be impossible... right?
		return "String longer than expected.";
	}
	if(strlen(day) < 1) return "Date should be written as YYYY-MM-DD.";
	out->day = (int) strtol(day, NULL, 0);

	// String parsed - check if this looks like a valid date.

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
	out->hour = (int) strtol(hour, NULL, 0);
	if(out->hour < 0 || out->hour > 23) return false;

	// Parse the minutes
	const char* minute = str_ptr;
	while(isdigit(*str_ptr)) {
		str_ptr++;
	}
	if(*str_ptr != '\0') {
		return false;
	}
	out->minute = (int) strtol(minute, NULL, 0);
	if(out->minute < 1 && out->minute > 59) return false;

	return true;
}

// TODO: Discord has as hard limit on how many characters are allowed in a post so we should take care not to exceed this...
//static const size_t DISCORD_MESSAGE_CHARACTER_LIMIT = 2000;

// The maximum allowed characters in a Discord username or nickname.
static const size_t DISCORD_NAME_LENGTH_MAX = 32;

// The maximum allowed byte length of a draft code. 5 digits should be enough!
static const size_t DRAFT_CODE_LENGTH_MAX = strlen("XXXXXL-T");

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

// The maximum number of bytes needed for a ping string. The space on the end is intentional!
static const size_t PING_STRING_LENGTH_MAX = LEAGUE_PINGS_MAX * strlen("<@&18446744073709551616> ");

// The maximum allowed byte length for a IANA time zone string. See: https://en.wikipedia.org/wiki/List_of_tz_database_time_zones
static const size_t IANA_TIME_ZONE_LENGTH_MAX = 64;

// How many blurbs a post can have. NOTE: If we instead end up getting this data from our master spreadsheet we won't need more than one here.
static const size_t BLURB_COUNT = 3;

// The maximum allowed byte length of a XDHS league name.
static const size_t LEAGUE_NAME_LENGTH_MAX = 32;

// All data needed for a #-pre-register post is available in this structure.
struct Draft_Event {
	Draft_Event() {
		memset(this, 0, sizeof(Draft_Event));
	}

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
	bool mtga_draft; // Will the draft portion take place on https://mtgadraft.tk/?
	//u64 banner_id; // TODO: How long do attachments live for?
	char icon_url[URL_LENGTH_MAX + 1]; // URL of the league icon. TODO: Not used - can be removed.
	char banner_url[URL_LENGTH_MAX + 1]; // URL of the image to use for this draft.

	bool locked;
	bool deleted;

	u64 channel_id; // TODO: This can be hard coded in, right? These events should only to to #-pre-register...
	u64 details_id; // Message ID of the post in #-pre-register describing the format.
	u64 signups_id; // Message ID of the sign up sheet posted in #-pre-register.
	u64 reminder_id; // Message ID of the reminder message sent to all sign ups #-in-the-moment-draft.
	u64 tentatives_pinged_id; // Message ID of the reminder sent to tentatives #-in-the-moment-draft.
};
static_assert(std::is_trivially_copyable<Draft_Event>(), "struct Draft_Event is not trivially copyable");

// All database_xxxx functions return this struct. If the member variable success is true value will contain the requested data and count will be the number of rows returned.
template<typename T>
struct Database_Result {
	bool success;
	u64 count;
	T value;

	bool operator==(const bool& rhs) const { return  (success == rhs); }
	bool operator!=(const bool& rhs) const { return !(success == rhs); }
};

// For database_ functions that return no data. We could use template specialization here... but I don't want to!
struct Database_No_Value {};

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
		"XDHS",                                              \
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
			mtga_draft,   -- 16
			icon_url,     -- 17
			banner_url,   -- 18
			channel_id    -- 19
		)
		VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
		)";

	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(20);
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
	MYSQL_INPUT(16, MYSQL_TYPE_TINY,     &event->mtga_draft,     sizeof(event->mtga_draft));
	MYSQL_INPUT(17, MYSQL_TYPE_STRING,   event->icon_url,        strlen(event->icon_url));
	MYSQL_INPUT(18, MYSQL_TYPE_STRING,   event->banner_url,      strlen(event->banner_url));
	MYSQL_INPUT(19, MYSQL_TYPE_LONGLONG, &event->channel_id,     sizeof(event->channel_id));

	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<std::shared_ptr<Draft_Event>> database_get_event(const u64 guild_id, const std::string& draft_code) {
	MYSQL_CONNECT();

	const char* query = R"(
		SELECT
			draft_code,   -- 0
			pings,        -- 1
			league_name,  -- 2
			format,       -- 3
			time_zone,    -- 4
			time,         -- 5
			duration,     -- 6
			blurb_1,      -- 7
			blurb_2,      -- 8
			blurb_3,      -- 9
			draft_guide,  -- 10
			card_list,    -- 11
			set_list,     -- 12
			color,        -- 13
			xmage_server, -- 14
			mtga_draft,   -- 15
			icon_url,     -- 16
			banner_url,   -- 17
			locked,       -- 18
			deleted,      -- 19
			channel_id,   -- 20
			details_id,   -- 21
			signups_id,   -- 22
			reminder_id,  -- 23
			tentatives_pinged_id -- 24
		FROM draft_events
		WHERE guild_id=? AND draft_code=?
	)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id, sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING, draft_code.c_str(), draft_code.length());
	MYSQL_INPUT_BIND_AND_EXECUTE();

	auto result = std::make_shared<Draft_Event>();

	MYSQL_OUTPUT_INIT(25);
	MYSQL_OUTPUT( 0, MYSQL_TYPE_STRING,   result->draft_code,      DRAFT_CODE_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 1, MYSQL_TYPE_STRING,   result->pings,           PING_STRING_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 2, MYSQL_TYPE_STRING,   result->league_name,     LEAGUE_NAME_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 3, MYSQL_TYPE_STRING,   result->format,          DRAFT_FORMAT_DESCRIPTION_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 4, MYSQL_TYPE_STRING,   result->time_zone,       IANA_TIME_ZONE_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 5, MYSQL_TYPE_LONGLONG, &result->time,           sizeof(result->time));
	MYSQL_OUTPUT( 6, MYSQL_TYPE_FLOAT,    &result->duration,       sizeof(result->duration));
	MYSQL_OUTPUT( 7, MYSQL_TYPE_STRING,   &result->blurbs[0][0],   DRAFT_BLURB_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 8, MYSQL_TYPE_STRING,   &result->blurbs[1][0],   DRAFT_BLURB_LENGTH_MAX + 1);
	MYSQL_OUTPUT( 9, MYSQL_TYPE_STRING,   &result->blurbs[2][0],   DRAFT_BLURB_LENGTH_MAX + 1);
	MYSQL_OUTPUT(10, MYSQL_TYPE_STRING,   result->draft_guide_url, URL_LENGTH_MAX + 1);
	MYSQL_OUTPUT(11, MYSQL_TYPE_STRING,   result->card_list_url,   URL_LENGTH_MAX + 1);
	MYSQL_OUTPUT(12, MYSQL_TYPE_STRING,   result->set_list,        SET_LIST_LENGTH_MAX + 1);
	MYSQL_OUTPUT(13, MYSQL_TYPE_LONG,     &result->color,          sizeof(result->color)); 
	MYSQL_OUTPUT(14, MYSQL_TYPE_STRING,   result->xmage_server,    XMAGE_SERVER_LENGTH_MAX + 1);
	MYSQL_OUTPUT(15, MYSQL_TYPE_LONG,     &result->mtga_draft,     sizeof(result->mtga_draft));
	MYSQL_OUTPUT(16, MYSQL_TYPE_STRING,   result->icon_url,        URL_LENGTH_MAX + 1);
	MYSQL_OUTPUT(17, MYSQL_TYPE_STRING,   result->banner_url,      URL_LENGTH_MAX + 1);
	MYSQL_OUTPUT(18, MYSQL_TYPE_LONG,     &result->locked,         sizeof(result->locked));
	MYSQL_OUTPUT(19, MYSQL_TYPE_LONG,     &result->deleted,        sizeof(result->deleted));
	MYSQL_OUTPUT(20, MYSQL_TYPE_LONGLONG, &result->channel_id,     sizeof(result->channel_id));
	MYSQL_OUTPUT(21, MYSQL_TYPE_LONGLONG, &result->details_id,     sizeof(result->details_id));
	MYSQL_OUTPUT(22, MYSQL_TYPE_LONGLONG, &result->signups_id,     sizeof(result->signups_id));
	MYSQL_OUTPUT(23, MYSQL_TYPE_LONGLONG, &result->reminder_id,    sizeof(result->reminder_id));
	MYSQL_OUTPUT(24, MYSQL_TYPE_LONGLONG, &result->tentatives_pinged_id, sizeof(result->tentatives_pinged_id));
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
	SIGNUP_STATUS_DECLINE     = 32,
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
		case SIGNUP_STATUS_REMOVED:     return "decline";
		default:
			return NULL;
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


static const Database_Result<std::vector<std::string>> database_get_draft_codes_for_post_draft_autocomplete(const u64 guild_id, std::string& prefix) {
	MYSQL_CONNECT();

	prefix += "%"; // The LIKE operator has to be part of the parameter, not in the query string.
	const char* query = "SELECT draft_code FROM draft_events WHERE guild_id=? AND details_id=0 AND deleted=0 AND draft_code LIKE ? ORDER BY draft_code LIMIT 25";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,      sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING,   prefix.c_str(), prefix.length());
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
	const char* query = "SELECT draft_code FROM draft_events WHERE guild_id=? AND details_id != 0 AND deleted=0 AND draft_code LIKE ? ORDER BY draft_code LIMIT 25";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,      sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING,   prefix.c_str(), prefix.length());
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
	static const char* query = "SELECT draft_code FROM draft_events WHERE signups_id != 0 AND deleted != 1 AND guild_id=? ORDER BY time ASC LIMIT 1";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(1)
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id, sizeof(guild_id));
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

static Database_Result<Database_No_Value> database_flag_draft_as_deleted(const u64 guild_id, const char* draft_code) {
	MYSQL_CONNECT();
	static const char* query = "UPDATE draft_events SET deleted=1 WHERE guild_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,  sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING,   draft_code, strlen(draft_code));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_flag_draft_as_locked(const u64 guild_id, const std::string& draft_code) {
	MYSQL_CONNECT();
	static const char* query = "UPDATE draft_events SET locked=1 WHERE guild_id=? AND draft_code=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT(0, MYSQL_TYPE_LONGLONG, &guild_id,          sizeof(guild_id));
	MYSQL_INPUT(1, MYSQL_TYPE_STRING,   draft_code.c_str(), draft_code.length());
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

static Database_Result<Database_No_Value> database_add_nowshow(const u64 guild_id, const u64 member_id, const char* draft_code) {
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
	bool minutemage_locked = false; // TODO: Do we ever want to lock this?

	time_t now = time(NULL);

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

	if(draft_event->mtga_draft == false) {
		embed.set_description(fmt::format("The draft and games will take place on **{}**.", draft_event->xmage_server));
	} else {
		embed.set_description(fmt::format("The draft will take place on **mtgadraft.tk**, games on **{}**.", draft_event->xmage_server));
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
	};		

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
		char buffer[1024];
		make_set_list(draft_event.value->set_list, strlen(draft_event.value->set_list), buffer, 1024);
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
	text += fmt::format("**:bell: This is the pre-draft reminder for {}: {} :bell:**\n\n", draft_event.value->draft_code, draft_event.value->format);
	text += "Please confirm your status on the signup sheet below.\n";
	text += fmt::format("If playing, check your XMage install is up-to-date by starting the launcher, updating if necessary, and connecting to {}.", draft_event.value->xmage_server);

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
	const auto sign_ups = database_get_draft_sign_ups(guild_id, draft_code);

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

// Post a message to the hosts-only #-current-draft-management channel outlining the procedures for correctly managing the firing of the draft.
static void post_host_guide(dpp::cluster& bot, const char* draft_code) {
	std::string text = fmt::format(":alarm_clock: **Attention hosts! Draft {} has now been locked.** :alarm_clock:\n\n", draft_code);

	text += "NOTE: Not everything here is implemented yet...\n\n";

	text += "Use the following commands to manage the signup list before pod allocations are posted:\n";
	text += "	**/add_player** - Add a player to the Playing list. Use this for adding Minutemages.\n";
	text += "	**/remove_player** - Remove a player from the Playing list. Use this for no-shows or members volunteering to drop to make an even number of players.\n";

	text += "\n";

	text += "Once the Playing list shows all available players:\n";
	text += "   **/show_allocations** - Show the pod allocations here in this team-only channel.\n";
	text += "   **/post_allocations** - Post the pod allocations to #-in-the-moment-draft for all to see.\n";
	text += "   **/fire** - Post the pre-draft reminder text and prevent further minutemage sign ups.\n";

	text += "\n";
	text += "The follow commands are available once the draft has been fired:\n";
	text += "	**/dropper** - Add a player to the droppers list.\n";

	send_message(bot, CURRENT_DRAFT_MANAGEMENT_ID, text);
}

// Users on Discord have two possible names per guild: Their global username or a per-guild nickname.
static std::string get_members_preferred_name(const u64 guild_id, const u64 member_id) {
	std::string preferred_name;
	const dpp::guild_member member = dpp::find_guild_member(guild_id, member_id);
	if(member.nickname.length() > 0) {
		preferred_name = member.nickname;
	} else {
		const dpp::user* user = dpp::find_user(member_id);
		if(user != nullptr) {
			preferred_name = user->username;	
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
				if(draft.value->locked == true) {
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
	fprintf(stdout, "mtga_draft BOOLEAN NOT NULL DEFAULT 0,\n");
	fprintf(stdout, "icon_url VARCHAR(%lu) NOT NULL,\n", URL_LENGTH_MAX);
	fprintf(stdout, "banner_url VARCHAR(%lu) NOT NULL,\n", URL_LENGTH_MAX);

	fprintf(stdout, "deleted BOOLEAN NOT NULL DEFAULT 0,\n"); // Has the event been deleted?
	fprintf(stdout, "channel_id BIGINT NOT NULL,\n"); // The channel the post is going to.
	fprintf(stdout, "details_id BIGINT NOT NULL DEFAULT 0,\n"); // The message ID of the details post.
	fprintf(stdout, "signups_id BIGINT NOT NULL DEFAULT 0,\n"); // The message ID of the signup post.
	fprintf(stdout, "reminder_id BIGINT NOT NULL DEFAULT 0,\n");
	fprintf(stdout, "tentatives_pinged_id BIGINT NOT NULL DEFAULT 0,\n"); // Has the 10 minute reminder been sent to tentatives?

	fprintf(stdout, "locked BOOLEAN NOT NULL DEFAULT 0,\n"); // Has the draft been locked?
	fprintf(stdout, "fired BOOLEAN NOT NULL DEFAULT 0,\n");
	fprintf(stdout, "complete BOOLEAN NOT NULL DEFAULT 0,\n");

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


int main(int argc, char* argv[]) {
	if(argc > 1) {
		if(strcmp(argv[1], "-sql") == 0) {
			// Dump SQL schema to stdout and return.
			output_sql();
		}

		return EXIT_SUCCESS;
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

	// Set up logging to an external file.
	log_init(g_config.logfile_path);

    log(LOG_LEVEL_INFO, "====== EventBot starting ======");
	log(LOG_LEVEL_INFO, "Build mode: %s",             g_build_mode);
    log(LOG_LEVEL_INFO, "MariaDB client version: %s", mysql_get_client_info());
	log(LOG_LEVEL_INFO, "libDPP++ version: %s",       dpp::utility::version().c_str());

	// Create the bot and connect to Discord.
	// TODO: We don't need all intents, just request what we need...
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
				dpp::slashcommand cmd("create_draft", "Create a new draft.", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				// Required
				cmd.add_option(dpp::command_option(dpp::co_string, "draft_code", "The draft code for this draft. i.e. 123P-C.", true));
				cmd.add_option(dpp::command_option(dpp::co_string, "format", "The format of the draft. i.e. TSP/PLC/FUT or 'Artifact Chaos'", true));
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
				cmd.add_option(dpp::command_option(dpp::co_boolean, "mtga_draft", "Will the draft potion be run on the MTGA Draft website?", false));
				cmd.add_option(dpp::command_option(dpp::co_string, "xmage_server", "Override the default XMage server. i.e. xmage.today:17172", false));
				cmd.add_option(dpp::command_option(dpp::co_attachment, "icon", "Icon image to override the default icon. 320x168 approx. resolution preferred.", false));
				cmd.add_option(dpp::command_option(dpp::co_channel, "channel", "Channel to post the signup. Defaults to #-pre-register.", false));
#if 0
				cmd.add_option(dpp::command_option(dpp::co_integer, "signup_type", "Override the default signup buttons.", false));
#endif
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
				dpp::slashcommand cmd("post_allocations", "Post the pod allocations to the public channels, create threads and groups.", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				bot.guild_command_create(cmd, event.created->id);
			}
			{
				//dpp::slashcommand cmd("fire_draft", "Fire a draft.", bot.me.id);
			}
			{
				dpp::slashcommand cmd("dropper", "Add a player to the droppers list.", bot.me.id);
				cmd.default_member_permissions = dpp::p_use_application_commands;
				cmd.add_option(dpp::command_option(dpp::co_user, "member", "The member to add to the droppers list.", true));
				bot.guild_command_create(cmd, event.created->id);
			}

			g_commands_registered = true;
		}

    });

	bot.on_slashcommand([&bot](const dpp::slashcommand_t& event) {
		const auto command_name = event.command.get_command_name();
		const auto guild_id = event.command.get_guild().id;

		if(command_name == "create_draft") {
			Draft_Event draft_event;

			// Required options
			auto draft_code = std::get<std::string>(event.get_parameter("draft_code"));
			// First, check if the draft code is valid and if it is get a copy of the XDHS_League it applies to.
			XDHS_League league;
			if(get_league_from_draft_code(draft_code.c_str(), &league) == false) {
				event.reply("**Invalid draft code.** Draft codes should look like XXXL-T, where:\n\tXXX is any number\n\tL is the uppercase league code: (E)uro, (A)mericas, (P)acific, A(S)ia or A(T)lantic\n\tT is the uppercase league type: (C)hrono or (B)onus.");
				return;
			}
			strcpy(draft_event.draft_code, draft_code.c_str());

			strcpy(draft_event.league_name, league.name);

			auto format = std::get<std::string>(event.get_parameter("format"));
			if(format.length() > FORMAT_STRING_LEN_MAX) {
				event.reply(fmt::format("Format string exceeds maximum allowed length of {} bytes.", FORMAT_STRING_LEN_MAX));
				return;
			}
			strcpy(draft_event.format, format.c_str());

			// Get the time zone string
			strcpy(draft_event.time_zone, league.time_zone);

			Date date;
			auto date_string = std::get<std::string>(event.get_parameter("date"));
			{
				const char* result = parse_date_string(date_string.c_str(), &date);
				if(result != NULL) {
					char errstr[128];
					snprintf(errstr, 128, "Error parsing date: %s", result);
					event.reply(errstr);
					return;
				}
			}

			// Is the default start time for this league overridden?
			//draft_event.time.hour = league.time.hour;
			//draft_event.time.minute = league.time.minute;
			Start_Time start_time;
			start_time.hour = league.time.hour;
			start_time.minute = league.time.minute;
			{
				auto opt = event.get_parameter("start_time");
				if(std::holds_alternative<std::string>(opt)) {
					std::string start_time_string = std::get<std::string>(opt);
					Start_Time start_time;
					if(!parse_start_time_string(start_time_string.c_str(), &start_time)) {
						event.reply("Invalid start time. Start time should be entered as HH:MM, in 24 hour time.");
						return;
					}
					//draft_event.time.hour = start_time.hour;
					//draft_event.time.minute = start_time.minute;
				}
			}

			// Get the banner image.
			auto banner_id = std::get<dpp::snowflake>(event.get_parameter("banner"));
			auto itr = event.command.resolved.attachments.find(banner_id);
			auto banner = itr->second;
			strcpy(draft_event.banner_url, banner.url.c_str());

			//std::string blurbs[BLURB_COUNT];
			for(size_t i = 0; i < BLURB_COUNT; ++i) {
				static const size_t blurb_name_len = strlen("blurb_x") + 1;
				char blurb_name[blurb_name_len];
				snprintf(blurb_name, blurb_name_len, "blurb_%lu", i + 1);
				auto opt = event.get_parameter(blurb_name);
				if(std::holds_alternative<std::string>(opt)) {
					auto blurb = std::get<std::string>(opt);
					if(blurb.length() > DRAFT_BLURB_LENGTH_MAX) {
						char errstr[128];
						snprintf(errstr, 128, "blurb_%lu exceeds maximum length of %lu bytes.", i, DRAFT_BLURB_LENGTH_MAX);
					}
					strcpy(&draft_event.blurbs[i][0], blurb.c_str()); 
				}
			}

			// Is the draft portion using the MTGA Draft site?
			draft_event.mtga_draft = false;
			{
				auto opt = event.get_parameter("mtga_draft");
				if(std::holds_alternative<bool>(opt)) {
					draft_event.mtga_draft = std::get<bool>(opt);
				}
			}

			// Check if the default league color has been overridden.
			draft_event.color = league.color;
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
						char errstr[128];
						snprintf(errstr, 128, "guide_url exceeds maximum allowed length of %lu bytes.", URL_LENGTH_MAX);
						event.reply(errstr);
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
						char errstr[128];
						snprintf(errstr, 128, "card_list exceeds maximum allowed length of %lu bytes.", URL_LENGTH_MAX);
						event.reply(errstr);
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
						char errstr[128];
						snprintf(errstr, 128, "set_list exceeds maximum allowed length of %lu bytes.", SET_LIST_LENGTH_MAX);
						event.reply(errstr);
						return;
					}
				}
			}
			strcpy(draft_event.set_list, set_list.c_str());

			// Was an icon override attached?
			//std::string icon_url;
			{
				auto opt = event.get_parameter("icon");
				if(std::holds_alternative<dpp::snowflake>(opt)) {
					dpp::snowflake icon_id = std::get<dpp::snowflake>(opt);
					auto itr = event.command.resolved.attachments.find(icon_id);
					//draft_event.league.icon = itr->second.url.c_str();
					strcpy(draft_event.icon_url, itr->second.url.c_str());
				} else {
					strcpy(draft_event.icon_url, league.icon);
				}
			}

			std::string xmage_server = g_config.xmage_server;
			{
				auto opt = event.get_parameter("xmage_server");
				if(std::holds_alternative<std::string>(opt)) {
					xmage_server = std::get<std::string>(opt);
					if(xmage_server.length() > XMAGE_SERVER_LENGTH_MAX) {
						char errstr[128];
						snprintf(errstr, 128, "xmage_server string exceeds maximum allowed length of %lu bytes.", XMAGE_SERVER_LENGTH_MAX);
						event.reply(errstr);
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
						char errstr[128];
						snprintf(errstr, 128, "Duration must be a positive number.");
						event.reply(errstr);
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
				const char* role_name = league.ping[i];
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
			if(database_add_draft(guild_id, &draft_event) != true) {
				log(LOG_LEVEL_ERROR, "Adding draft to database failed");
			}

			char reply_string[128];
			const dpp::user& issuing_user = event.command.get_issuing_user();
			snprintf(reply_string, 128, "%s created %s. Use ``/post_draft`` to post it.", issuing_user.username.c_str(), draft_code.c_str());
			event.reply(reply_string);

		} else
		if(command_name == "post_draft") {
			// TODO: If the event is already posted, update it.
			const std::string draft_code = std::get<std::string>(event.get_parameter("draft_code"));

			bool result = post_draft(bot, guild_id, draft_code);
			if(result == true) {
				event.reply(fmt::format("Draft {} posted.", draft_code));
			} else {
				event.reply(fmt::format("Paging @TandEm: There was an error posting draft {}!", draft_code));
			}
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
#if TESTING
			if(draft.value->locked == false) {
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
			if(draft.value->locked == false) {
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
					if(noshow == true) database_add_nowshow(guild_id, member_id, g_current_draft_code.c_str());
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
		if(command_name == "dropper") {
			const auto guild_id = event.command.get_guild().id;
			const auto member_id = std::get<dpp::snowflake>(event.get_parameter("member"));

			database_add_dropper(guild_id, member_id, g_current_draft_code.c_str());
			const std::string preferred_name = get_members_preferred_name(guild_id, member_id);
			event.reply(fmt::format("{} added to droppers list.", preferred_name));
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
		if((draft.value->reminder_id == 0) && (draft.value->time - now <= SECONDS_BEFORE_DRAFT_TO_SEND_REMINDER)) {
			send_message(bot, BOT_COMMANDS_CHANNEL_ID, fmt::format("Sending pre-draft reminder message for {}.", draft_code.value.c_str()));
			// TODO: Remove mentions on this when the draft is fired?
			post_pre_draft_reminder(bot, GUILD_ID, draft_code.value.c_str());
		}

		// Ping the tentatives if they haven't already been pinged.
		if((draft.value->tentatives_pinged_id == 0) && (draft.value->time - now <= SECONDS_BEFORE_DRAFT_TO_PING_TENTATIVES)) {
			send_message(bot, BOT_COMMANDS_CHANNEL_ID, fmt::format("Sending tentative reminder message for {}.", draft_code.value.c_str()));
			// Redraw the sign up posts so the Tentative button shows as locked.
			redraw_signup(bot, GUILD_ID, draft.value->signups_id, draft.value->channel_id, draft.value);
			redraw_signup(bot, GUILD_ID, draft.value->reminder_id, IN_THE_MOMENT_DRAFT_CHANNEL_ID, draft.value);
			ping_tentatives(bot, GUILD_ID, draft_code.value.c_str());
		}

		// Lock the draft.
		if((draft.value->locked == false) && now >= draft.value->time) {
			send_message(bot, BOT_COMMANDS_CHANNEL_ID, fmt::format("Locking draft: {}.", draft_code.value.c_str()));
			post_host_guide(bot, draft_code.value.c_str());
			database_flag_draft_as_locked(GUILD_ID, draft_code.value);
			// Redraw the signup buttons so they all (except Minutemage) appear locked.
			redraw_signup(bot, GUILD_ID, draft.value->signups_id, draft.value->channel_id, draft.value);
			redraw_signup(bot, GUILD_ID, draft.value->reminder_id, IN_THE_MOMENT_DRAFT_CHANNEL_ID, draft.value);
		}

		// Delete the draft after a few hours.
		if(now - draft.value->time > SECONDS_AFTER_DRAFT_TO_DELETE_POSTS) {
			send_message(bot, BOT_COMMANDS_CHANNEL_ID, fmt::format("Deleting completed draft: {}", draft_code.value.c_str()));
			database_flag_draft_as_deleted(GUILD_ID, draft_code.value.c_str());
			delete_draft_posts(bot, GUILD_ID, draft_code.value);
			database_clear_draft_post_ids(GUILD_ID, draft_code.value);
		}

	}, JOB_THREAD_TICK_RATE, [](dpp::timer){});


    while(g_quit == false) {
        sleep(1);
    }

    return g_exit_code;
}
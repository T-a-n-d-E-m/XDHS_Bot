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
// TODO: Need a /minutemage command that either pings a random 'mage or the role 

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
static const u64 DECK_SCREENSHOTS_CHANNEL_ID    = 1155769474520199279;
static const u64 ROLE_SELF_ASSIGNMENT_CHANNEL_ID = 1155771897225674752;
static const u64 P1P1_AND_DRAFT_LOG_CHANNEL_ID  = 1155772743485235200;
static const u64 FEEDBACK_CHANNEL_ID            = 1155773361104887880;
static const u64 CALENDAR_CHANNEL_ID			= 1155774664732323952;
static const u64 XDHS_TEAM_ROLE_ID              = 885054778978234408;
static const u64 XDHS_HOST_ROLE_ID              = 1091275398639267881;
static const u64 MINUTEMAGE_ROLE_ID             = 1156767797192437891;
static bool g_commands_registered               = false; // Have the bot slash commands been registered for this guild?
#else
// The bot will be running in release mode on the XDHS public server.
static const char* g_build_mode                 = "Release";
static const u64 GUILD_ID                       = 528728694680715324;
static const u64 PRE_REGISTER_CHANNEL_ID        = 753639027428687962; // Default channel to post the draft signup.
static const u64 CURRENT_DRAFT_MANAGEMENT_ID    = 921027014822068234;
static const u64 IN_THE_MOMENT_DRAFT_CHANNEL_ID = 535127333401657354;
static const u64 BOT_COMMANDS_CHANNEL_ID        = 753637350877429842;
static const u64 DECK_SCREENSHOTS_CHANNEL_ID    = 647073844649000962;
static const u64 ROLE_SELF_ASSIGNMENT_CHANNEL_ID = 663422413891174400;
static const u64 P1P1_AND_DRAFT_LOG_CHANNEL_ID  = 796861143594958868; 
static const u64 FEEDBACK_CHANNEL_ID            = 822015209756950528;
static const u64 CALENDAR_CHANNEL_ID			= 794227134892998666;
static const u64 XDHS_TEAM_ROLE_ID              = 639451893399027722;
static const u64 XDHS_HOST_ROLE_ID              = 1051631435506794657;
static const u64 MINUTEMAGE_ROLE_ID             = 843796946984370176;
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
	{"LTR", "The Lord of the Rings: Tales of Middle-earth"},
	{"WOE", "Wilds of Eldraine"},
	{"RVR", "Ravnica Remastered"},

	// XDHS custom and remastered sets
	{"INVR", "Invasion Remastered"},
	{"KMGR", "Kamigawa Remastered"},
	{"PMMA", "Pre Mirage Masters"},
	{"GBMA", "Garbage Masters"},
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
			if(set_name == NULL) set_name = format; // Unknown set - Use the set name passed in
			offset += snprintf(out + offset, out_len - offset, "%s", set_name);
			if(i != token_count-1) { // If not the last in the list, add a separator.
				offset += snprintf(out + offset, out_len - offset, "%s", "/");
			}
		}
	}
}

#if 0
struct Draft_Code {
	int season;
	int week;
	char region_code;
	char league_type;
};

static void parse_draft_code(const char* draft_code, Draft_Code* out) {

}
#endif


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
};

// Lookup table for each of our leagues. The order these are listed doesn't matter. In the future we may want bot commands to create, edit and delete leagues but to keep things simple for now we'll hard code these into the bot.
static const XDHS_League g_xdhs_leagues[] = {
	{
		"Americas Chrono",
		'A','C',
		0x002c7652,
		"America/New_York",
		{20,50},
		{"Americas", NULL},
	},
	{
		"Euro Chrono",
		'E','C',
		0x000d5ba1,
		"Europe/Berlin",
		{19,50},
		{"Euro", NULL},
	},
	{
		"Asia Chrono",
		'S','C',
		0x00793fab,
		"Europe/Berlin",
		{10,50},
		{"Asia", NULL},
	},
	{
		"Pacific Chrono",
		'P','C',
		0x00b82f4b,
		"America/New_York",
		{20,50},
		{"Pacific", NULL},
	},
	{
		"Atlantic Bonus",
		'T','B',
		0x00ed8821,
		"Europe/Berlin",
		{19,50},
		{"Euro", "Americas"},
	},
	{
		"Americas Bonus",
		'A','B',
		0x006aa84f,
		"America/New_York",
		{20,50},
		{"Americas", NULL},
	},
	{
		"Euro Bonus",
		'E','B',
		0x0061a0da,
		"Europe/Berlin",
		{19,50},
		{"Euro", NULL},
	}
};
static const size_t LEAGUE_COUNT = sizeof(g_xdhs_leagues) / sizeof(XDHS_League);

// Parse a draft code and find the league it is for. Makes a copy into 'league' and returns true if found, false otherwise.
static bool get_league_from_draft_code(const char* draft_code, XDHS_League* league) {
	// SSS.W-RT S=season, W=week, R=region, T=type
	if(draft_code == NULL) return false;

	while(isdigit(*draft_code)) {draft_code++;} // Skip the numeric part
	if(*draft_code++ != '.') return false;
	while(isdigit(*draft_code)) {draft_code++;} // Skip the numeric part
	if(*draft_code++ != '-') return false;
	char region_code = *draft_code++;
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

static void make_2_digit_league_code(const XDHS_League* league, char out[3]) {
	out[0] = league->region_code;
	out[1] = league->league_type;
	out[2] = 0;
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

// The maximum allowed byte length of a draft code.
static const size_t DRAFT_CODE_LENGTH_MAX = strlen("SSS.GG-LT");

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
	text += fmt::format("If playing, **check your XMage install is up-to-date** by starting the launcher, updating if necessary, and connecting to {}.", draft_event.value->xmage_server);

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
			confirmed_count++;
		}
	}

	if((confirmed_count % 2) == 1) {
		// Odd number of players.

		dpp::message message;
		message.set_type(dpp::message_type::mt_default);
		message.set_guild_id(guild_id);
		message.set_channel_id(IN_THE_MOMENT_DRAFT_CHANNEL_ID);
		message.set_allowed_mentions(true, true, true, true, {}, {});
		std::string text;

		std::vector<const Draft_Signup_Status*> minutemages;
		for(const auto& sign_up : sign_ups.value) {
			if(sign_up.status == SIGNUP_STATUS_MINUTEMAGE) {
				minutemages.push_back(&sign_up);
			}
		}
		if(minutemages.size() > 0) {
			u64 member_id = 0;
			if(minutemages.size() == 1) {
				member_id = minutemages[0]->member_id;
			} else {
				// Select a minutemage at random.
				int index = rand() % minutemages.size();
				member_id = minutemages[index]->member_id;
			}

			text = fmt::format(":superhero: Paging minutemage <@{}>! You are needed on {} for {}.",
				member_id,
				draft_event.value->draftmancer_draft == true ? "Draftmancer" : draft_event.value->xmage_server,
				draft_event.value->format);
		} else {
			// Ping the @Minutemage role.
			text = fmt::format(":superhero: <@&{}> One more needed on {} for {}.",
				MINUTEMAGE_ROLE_ID,
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
	text += "   **/post_allocations** - Create threads for each pod and post the pod allocations to #-in-the-moment-draft channel.\n";
	text += "\n";

	text += "Once all pods have been filled:\n";
	text += "   **/fire** - Post the pre-draft reminder message and fully lock the draft.\n";
	text += "\n";

	text += "The follow commands are available once the draft has been fired:\n";
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
	if(member.nickname.length() > 0) {
		preferred_name = member.nickname;
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
#ifdef DEBUG
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

	srand(time(NULL));

	// Set up logging to an external file.
	log_init(g_config.logfile_path);

    log(LOG_LEVEL_INFO, "====== EventBot starting ======");
	log(LOG_LEVEL_INFO, "Build mode: %s",             g_build_mode);
    log(LOG_LEVEL_INFO, "MariaDB client version: %s", mysql_get_client_info());
	log(LOG_LEVEL_INFO, "libDPP++ version: %s",       dpp::utility::version().c_str());

	// Download and install the latest IANA time zone database.
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
				cmd.add_option(dpp::command_option(dpp::co_string, "draft_code", "The draft code for this draft. i.e. 123.4-PC.", true));
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
				//dpp::slashcommand cmd("add_host", "Specify a 
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

		if(command_name == "create_draft") {
			Draft_Event draft_event;

			// Required options
			auto draft_code = std::get<std::string>(event.get_parameter("draft_code"));
			// First, check if the draft code is valid and if it is get a copy of the XDHS_League it applies to.
			XDHS_League league;
			if(get_league_from_draft_code(draft_code.c_str(), &league) == false) {
				event.reply("**Invalid draft code.** Draft codes should look like SS.W-RT, where:\n\tSS is the season\n\tW is the week in the season\n\tR is the region code: (E)uro, (A)mericas, (P)acific, A(S)ia or A(T)lantic\n\tT is the league type: (C)hrono or (B)onus.");
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
			draft_event.draftmancer_draft = false;
			{
				auto opt = event.get_parameter("draftmancer_draft");
				if(std::holds_alternative<bool>(opt)) {
					draft_event.draftmancer_draft = std::get<bool>(opt);
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
			std::string text;
			if(result == true) {
				text = fmt::format("Draft {} posted.", draft_code);
			} else {
				text = fmt::format("Paging @TandEm: There was an error posting draft {}!", draft_code);
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
			const auto guild_id = event.command.get_guild().id;

			XDHS_League league;
			get_league_from_draft_code(g_current_draft_code.c_str(), &league);
			char league_code[3];
			make_2_digit_league_code(&league, league_code);
			auto sign_ups = database_get_sign_ups(guild_id, g_current_draft_code, league_code);

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

			// Flag all potential hosts and flag everyone as not allocated.
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
			text += "### Thanks everyone for drafting with us!\n";
			text += fmt::format("* You can share a screenshot of your deck in <#{}>.\n", DECK_SCREENSHOTS_CHANNEL_ID);
			text += fmt::format("* If you want feedback on your draft, just ask or give yourself the Civilized Scholar role in <#{}>).\n", ROLE_SELF_ASSIGNMENT_CHANNEL_ID);
			text += fmt::format("* You can also upload your draftlog to <https://magic.flooey.org/draft/upload> and share it in <#{}>.\n", P1P1_AND_DRAFT_LOG_CHANNEL_ID);
			text += fmt::format("* We're happy to hear feedback on how to improve, either in <#{}> or anonymously with FeedbackBot ""/feedback"".\n", FEEDBACK_CHANNEL_ID);
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

#if 0
	{
		bot.thread_create_with_message("123P-C", IN_THE_MOMENT_DRAFT_CHANNEL_ID, 1090316907871211561, 1440, 0, [&bot](const dpp::confirmation_callback_t& event) {

		});
	}
#endif

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
			send_message(bot, BOT_COMMANDS_CHANNEL_ID, fmt::format("Sending pre-draft reminder message for {}.", draft_code.value.c_str()));
			// TODO: Remove mentions on this when the draft is fired?
			// TODO: Unlock the minute mage signups
			post_pre_draft_reminder(bot, GUILD_ID, draft_code.value.c_str());
			database_set_draft_status(GUILD_ID, draft_code.value, DRAFT_STATUS_REMINDER_SENT);
		}

		// Ping the tentatives if they haven't already been pinged.
		if(!(BIT_SET(draft.value->status, DRAFT_STATUS_TENTATIVES_PINGED)) && (draft.value->time - now <= SECONDS_BEFORE_DRAFT_TO_PING_TENTATIVES)) {
			send_message(bot, BOT_COMMANDS_CHANNEL_ID, fmt::format("Sending tentative reminder message for: {}", draft_code.value.c_str()));
			// Redraw the sign up posts so the Tentative button shows as locked.
			redraw_signup(bot, GUILD_ID, draft.value->signups_id, draft.value->channel_id, draft.value);
			redraw_signup(bot, GUILD_ID, draft.value->reminder_id, IN_THE_MOMENT_DRAFT_CHANNEL_ID, draft.value);
			ping_tentatives(bot, GUILD_ID, draft_code.value.c_str());
			database_set_draft_status(GUILD_ID, draft_code.value, DRAFT_STATUS_TENTATIVES_PINGED);
		}

		// Lock the draft.
		if((draft.value->status < DRAFT_STATUS_LOCKED) && now >= draft.value->time) {
			send_message(bot, BOT_COMMANDS_CHANNEL_ID, fmt::format("Locking draft: {}", draft_code.value.c_str()));
			post_host_guide(bot, draft_code.value.c_str());
			database_set_draft_status(GUILD_ID, draft_code.value, DRAFT_STATUS_LOCKED);
			// Ping minutemages if there is an odd number of confirmed sign ups.
			ping_minutemages(bot, GUILD_ID, draft_code.value.c_str());
			// Redraw the signup buttons so they all (except Minutemage) appear locked.
			redraw_signup(bot, GUILD_ID, draft.value->signups_id, draft.value->channel_id, draft.value);
			redraw_signup(bot, GUILD_ID, draft.value->reminder_id, IN_THE_MOMENT_DRAFT_CHANNEL_ID, draft.value);
		}

		// Delete the draft after a few hours.
		if((draft.value->status < DRAFT_STATUS_COMPLETE) && now - draft.value->time > SECONDS_AFTER_DRAFT_TO_DELETE_POSTS) {
			send_message(bot, BOT_COMMANDS_CHANNEL_ID, fmt::format("Deleting completed draft: {}", draft_code.value.c_str()));
			delete_draft_posts(bot, GUILD_ID, draft_code.value);
			database_clear_draft_post_ids(GUILD_ID, draft_code.value);
			database_set_draft_status(GUILD_ID, draft_code.value, DRAFT_STATUS_COMPLETE);
		}

	}, JOB_THREAD_TICK_RATE, [](dpp::timer){});

    while(g_quit == false) {
        sleep(1);
    }

    return g_exit_code;
}

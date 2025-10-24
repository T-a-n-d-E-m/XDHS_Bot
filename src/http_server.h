#ifndef HTTP_SERVER_H_INCLUDED
#define HTTP_SERVER_H_INCLUDED
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#include <vector>

#define FMT_HEADER_ONLY
#include <fmt/format.h>

// TODO: Update to version 7.14? Seems to have a lot of arbitrary breaking
// changes that would have little to no benefit here...
#include "mongoose.h"

#include "constants.h"
#include "result.h"
#include "log.h"
#include "database.h"
#include "curl.h"
#include "defer.h"
#include "slurp.h"
#include "image.h"
#include "font.h"

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
#pragma GCC diagnostic ignored "-Wunused-function"
#define STBIR_DEFAULT_FILTER_DOWNSAMPLE STBIR_FILTER_CUBICSPLINE // TODO: Investigate which looks best.
#include "stb_image_resize2.h"
#pragma GCC diagnostic pop
#endif // #ifndef

#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "stb_image_write.h"
#pragma GCC diagnostic pop
#endif // #ifndef

#include "poppler/cpp/poppler-document.h"
#include "poppler/cpp/poppler-page.h"
#include "poppler/cpp/poppler-page-renderer.h"

static const int THUMBNAIL_SIZE = 50;

#if MG_ENABLE_CUSTOM_LOG
// Currently not used...
void mg_log_prefix(int level, const char* file, int line, const char* fname) {
	(void)level; (void)file; (void)line; (void)fname;
}

void mg_log(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	log(g_log_level, fmt, args);
	va_end(args);
}
#endif // MG_ENABLE_CUSTOM_LOG

// Direct mongoose to write to our log file
void log_write_char(char c, void*) {
	fputc(c, g_log_descriptor);
}

static void start_thread(void *(*f)(void *), void *p) {
	pthread_t thread_id = (pthread_t) 0;
	pthread_attr_t attr;
	(void) pthread_attr_init(&attr);
	(void) pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_create(&thread_id, &attr, f, p);
	pthread_attr_destroy(&attr);
}

struct thread_data {
	unsigned long conn_id;
	mg_mgr *mgr;

	mg_str content_type;
	mg_str api_key;
	mg_str uri;
	mg_str body;
};

struct http_response {
	int result;
	const char* str; // Must point to heap memory. Freed in MG_EV_WAKEUP handler.

	// TODO: It appears as if mg_wakeup sends the contents of this struct, but then
	// later we call mg_http_reply with the same data to send it again?
};


#define STR_OR_NULL(ptr) ((ptr != NULL ? ptr : "(NULL)"))

struct Stats_In {
	uint64_t member_id; // Called user_id when sent from sheet
	struct {
		char* name;
		int value;
		int next;
	} devotion;

	struct {
		char* name;
		int value;
		int next;
	} victory;

	struct {
		char* name;
		int value;
		int next;
	} trophies;

	struct {
		char* name;
		int value;
		int next;
	} hero;

	struct {
		char* name;
		int value;
		int next;
		bool is_shark;
	} shark;

	struct {
		double chrono;
		double bonus;
		double overall;
	} win_rate_recent;

	struct {
		double chrono;
		double bonus;
		double overall;
	} win_rate_all_time;
};

static Database_Result<Database_No_Value> database_touch_stats(const Stats_In* stats) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "REPLACE INTO stats (id, timestamp) VALUES (?,?)";
	MYSQL_STATEMENT();

	time_t timestamp = time(NULL);

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT_I64(&stats->member_id);
	MYSQL_INPUT_I64(&timestamp);
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_upsert_devotion(const Stats_In* stats) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "REPLACE INTO devotion (id, name, value, next) VALUES (?,?,?,?)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(4);
	MYSQL_INPUT_I64(&stats->member_id);
	MYSQL_INPUT_STR(stats->devotion.name, strlen(stats->devotion.name));
	MYSQL_INPUT_I32(&stats->devotion.value);
	MYSQL_INPUT_I32(&stats->devotion.next);
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_upsert_victory(const Stats_In* stats) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "REPLACE INTO victory (id, name, value, next) VALUES (?,?,?,?)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(4);
	MYSQL_INPUT_I64(&stats->member_id);
	MYSQL_INPUT_STR(stats->victory.name, strlen(stats->victory.name));
	MYSQL_INPUT_I32(&stats->victory.value);
	MYSQL_INPUT_I32(&stats->victory.next);
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_upsert_trophies(const Stats_In* stats) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "REPLACE INTO trophies (id, name, value, next) VALUES (?,?,?,?)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(4);
	MYSQL_INPUT_I64(&stats->member_id);
	MYSQL_INPUT_STR(stats->trophies.name, strlen(stats->trophies.name));
	MYSQL_INPUT_I32(&stats->trophies.value);
	MYSQL_INPUT_I32(&stats->trophies.next);
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_upsert_hero(const Stats_In* stats) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "REPLACE INTO hero (id, name, value, next) VALUES (?,?,?,?)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(4);
	MYSQL_INPUT_I64(&stats->member_id);
	MYSQL_INPUT_STR(stats->hero.name, strlen(stats->hero.name));
	MYSQL_INPUT_I32(&stats->hero.value);
	MYSQL_INPUT_I32(&stats->hero.next);
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_upsert_shark(const Stats_In* stats) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "REPLACE INTO shark (id, name, value, next, is_shark) VALUES (?,?,?,?,?)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(5);
	MYSQL_INPUT_I64(&stats->member_id);
	MYSQL_INPUT_STR(stats->shark.name, strlen(stats->shark.name));
	MYSQL_INPUT_I32(&stats->shark.value);
	MYSQL_INPUT_I32(&stats->shark.next);
	MYSQL_INPUT_I32(&stats->shark.is_shark);
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_upsert_win_rate_all_time(const Stats_In* stats) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "REPLACE INTO win_rate_all_time (id, league, bonus, overall) VALUES (?,?,?,?)";
	MYSQL_STATEMENT();

	float chrono  = (float) stats->win_rate_all_time.chrono;
	float bonus   = (float) stats->win_rate_all_time.bonus;
	float overall = (float) stats->win_rate_all_time.overall;

	MYSQL_INPUT_INIT(4);
	MYSQL_INPUT_I64(&stats->member_id);
	MYSQL_INPUT_F32(&chrono);
	MYSQL_INPUT_F32(&bonus);
	MYSQL_INPUT_F32(&overall);
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_upsert_win_rate_recent(const Stats_In* stats) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "REPLACE INTO win_rate_recent (id, league, bonus, overall) VALUES (?,?,?,?)";
	MYSQL_STATEMENT();

	float chrono  = (float) stats->win_rate_recent.chrono;
	float bonus   = (float) stats->win_rate_recent.bonus;
	float overall = (float) stats->win_rate_recent.overall;

	MYSQL_INPUT_INIT(4);
	MYSQL_INPUT_I64(&stats->member_id);
	MYSQL_INPUT_F32(&chrono);
	MYSQL_INPUT_F32(&bonus);
	MYSQL_INPUT_F32(&overall);
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}


void print_stats(const Stats_In* s) {
	log(LOG_LEVEL_DEBUG, "member_id : %lu", s->member_id);
	log(LOG_LEVEL_DEBUG, "    devotion.name  : %s", s->devotion.name);
	log(LOG_LEVEL_DEBUG, "    devotion.value : %d", s->devotion.value);
	log(LOG_LEVEL_DEBUG, "    devotion.next  : %d", s->devotion.next);

	log(LOG_LEVEL_DEBUG, "    victory.name  : %s", s->victory.name);
	log(LOG_LEVEL_DEBUG, "    victory.value : %d", s->victory.value);
	log(LOG_LEVEL_DEBUG, "    victory.next  : %d", s->victory.next);

	log(LOG_LEVEL_DEBUG, "    trophies.name  : %s", s->trophies.name);
	log(LOG_LEVEL_DEBUG, "    trophies.value : %d", s->trophies.value);
	log(LOG_LEVEL_DEBUG, "    trophies.next  : %d", s->trophies.next);

	log(LOG_LEVEL_DEBUG, "    hero.name  : %s", s->hero.name);
	log(LOG_LEVEL_DEBUG, "    hero.value : %d", s->hero.value);
	log(LOG_LEVEL_DEBUG, "    hero.next  : %d", s->hero.next);

	log(LOG_LEVEL_DEBUG, "    shark.name  : %s", s->shark.name);
	log(LOG_LEVEL_DEBUG, "    shark.value : %d", s->shark.value);
	log(LOG_LEVEL_DEBUG, "    shark.next  : %d", s->shark.next);
	log(LOG_LEVEL_DEBUG, "    shark.shark : %d", s->shark.is_shark);

	log(LOG_LEVEL_DEBUG, "    win_rate_recent.chrono : %f", s->win_rate_recent.chrono);
	log(LOG_LEVEL_DEBUG, "    win_rate_recent.bonus : %f", s->win_rate_recent.bonus);
	log(LOG_LEVEL_DEBUG, "    win_rate_recent.overall : %f", s->win_rate_recent.overall);

	log(LOG_LEVEL_DEBUG, "    win_rate_all_time.chrono : %f", s->win_rate_all_time.chrono);
	log(LOG_LEVEL_DEBUG, "    win_rate_all_time.bonus : %f", s->win_rate_all_time.bonus);
	log(LOG_LEVEL_DEBUG, "    win_rate_all_time.overall : %f", s->win_rate_all_time.overall);
}

http_response parse_stats(const mg_str json) {
	Stats_In stats;
	memset(&stats, 0, sizeof(Stats_In));

	{
		char* value = mg_json_get_str(json, "$.member_id");
		if(value != NULL) {
			stats.member_id = strtoull(value, NULL, 10);
			free(value);
		} else {
			return {400, mg_mprintf(R"({"result":"'member_id' key not found"})")};
		}
	}

	stats.devotion.name = mg_json_get_str(json, "$.devotion.name");
	if(stats.devotion.name == NULL) {
		return {400, mg_mprintf(R"({"result":"'devotion.name' key not found"})")};
	}
	defer { free(stats.devotion.name); };
	if(strlen(stats.devotion.name) > DEVOTION_BADGE_NAME_LENGTH_MAX) {
		return {400, mg_mprintf(R"({"result":"'devotion.name' > %d characters"})", DEVOTION_BADGE_NAME_LENGTH_MAX)};
	}

	stats.devotion.value = mg_json_get_long(json, "$.devotion.value", -1);
	if(stats.devotion.value == -1) {
		return {400, mg_mprintf(R"({"result":"'devotion.value' key not found"})")};
	}

	stats.devotion.next = mg_json_get_long(json, "$.devotion.next", -1);
	if(stats.devotion.next == -1) {
		return {400, mg_mprintf(R"({"result":"'devotion.next' key not found"})")};
	}

	stats.victory.name = mg_json_get_str(json, "$.victory.name");
	if(stats.victory.name == NULL) {
		return {400, mg_mprintf(R"({"result":"'victory.name' key not found"})")};
	}
	defer{ free(stats.victory.name); };
	if(strlen(stats.victory.name) > VICTORY_BADGE_NAME_LENGTH_MAX) {
		return {400, mg_mprintf(R"({"result":"'victory.name' > %d characters"})", VICTORY_BADGE_NAME_LENGTH_MAX)};
	}

	stats.victory.value = mg_json_get_long(json, "$.victory.value", -1);
	if(stats.victory.value == -1) {
		return {400, mg_mprintf(R"({"result":"'victory.value' key not found"})")};
	}

	stats.victory.next = mg_json_get_long(json, "$.victory.next", -1);
	if(stats.victory.next == -1) {
		return {400, mg_mprintf(R"({"result":"'victory.next' key not found"})")};
	}

	stats.trophies.name = mg_json_get_str(json, "$.trophies.name");
	if(stats.trophies.name == NULL) {
		return {400, mg_mprintf(R"({"result":"'trophies.name' key not found"})")};
	}
	defer{ free(stats.trophies.name); };
	if(strlen(stats.trophies.name) > TROPHIES_BADGE_NAME_LENGTH_MAX) {
		return {400, mg_mprintf(R"({"result":"'trophies.name' > %d characters"})", TROPHIES_BADGE_NAME_LENGTH_MAX)};
	}

	stats.trophies.value = mg_json_get_long(json, "$.trophies.value", -1);
	if(stats.trophies.value == -1) {
		return {400, mg_mprintf(R"({"result":"'trophies.value' key not found"})")};
	}

	stats.trophies.next = mg_json_get_long(json, "$.trophies.next", -1);
	if(stats.trophies.next == -1) {
		return {400, mg_mprintf(R"({"result":"'trophies.next' key not found"})")};
	}

	stats.hero.name = mg_json_get_str(json, "$.hero.name");
	if(stats.hero.name == NULL) {
		return {400, mg_mprintf(R"({"result":"'hero.name' key not found"})")};
	}
	defer { free(stats.hero.name); };
	if(strlen(stats.hero.name) > SHARK_BADGE_NAME_LENGTH_MAX) {
		return {400, mg_mprintf(R"({"result":"'hero.name' > %d characters"})", SHARK_BADGE_NAME_LENGTH_MAX)};
	}

	stats.hero.value = mg_json_get_long(json, "$.hero.value", -1);
	if(stats.hero.value == -1) {
		return {400, mg_mprintf(R"({"result":"'hero.value' key not found"})")};
	}

	stats.hero.next = mg_json_get_long(json, "$.hero.next", -1);
	if(stats.hero.next == -1) {
		return {400, mg_mprintf(R"({"result":"'hero.next' key not found"})")};
	}

	stats.shark.name = mg_json_get_str(json, "$.shark.name");
	if(stats.shark.name == NULL) {
		return {400, mg_mprintf(R"({"result":"'shark.name' key not found"})")};
	}
	defer { free(stats.shark.name); };
	if(strlen(stats.shark.name) > SHARK_BADGE_NAME_LENGTH_MAX) {
		return {400, mg_mprintf(R"({"result":"'shark.name' > %d characters"})", SHARK_BADGE_NAME_LENGTH_MAX)};
	}

	stats.shark.value = mg_json_get_long(json, "$.shark.value", -1);
	if(stats.shark.value == -1) {
		return {400, mg_mprintf(R"({"result":"'shark.value' key not found"})")};
	}

	stats.shark.next = mg_json_get_long(json, "$.shark.next", -1);
	if(stats.shark.next == -1) {
		return {400, mg_mprintf(R"({"result":"'shark.next' key not found"})")};
	}

	bool get_shark = mg_json_get_bool(json, "$.shark.is_shark", &stats.shark.is_shark);
	if(get_shark == false) {
		return {400, mg_mprintf(R"({"result":"'shark.is_shark' key not found"})")};
	}

	if(mg_json_get_num(json, "$.win_rate_recent.chrono", &stats.win_rate_recent.chrono) == false) {
		return {400, mg_mprintf(R"({"result":"'win_rate_recent.chrono' key not found"})")};
	}

	if(mg_json_get_num(json, "$.win_rate_recent.bonus", &stats.win_rate_recent.bonus) == false) {
		return {400, mg_mprintf(R"({"result":"'win_rate_recent.bonus' key not found"})")};
	}

	if(mg_json_get_num(json, "$.win_rate_recent.overall", &stats.win_rate_recent.overall) == false) {
		return {400, mg_mprintf(R"({"result":"'win_rate_recent.overall' key not found"})")};
	}

	if(mg_json_get_num(json, "$.win_rate_all_time.chrono", &stats.win_rate_all_time.chrono) == false) {
		return {400, mg_mprintf(R"({"result":"'win_rate_all_time.chrono' key not found"})")};
	}

	if(mg_json_get_num(json, "$.win_rate_all_time.bonus", &stats.win_rate_all_time.bonus) == false) {
		return {400, mg_mprintf(R"({"result":"'win_rate_all_time.bonus' key not found"})")};
	}

	if(mg_json_get_num(json, "$.win_rate_all_time.overall", &stats.win_rate_all_time.overall) == false) {
		return {400, mg_mprintf(R"({"result":"'win_rate_all_time.overall' key not found"})")};
	}


	if(is_error(database_touch_stats(&stats))) {
		return {500, mg_mprintf(R"({"result":"database_touch_stats failed"})")};
	}
	if(is_error(database_upsert_devotion(&stats))) {
		return {500, mg_mprintf(R"({"result":"database_upsert_devotion failed"})")};
	}
	if(is_error(database_upsert_victory(&stats))) {
		return {500, mg_mprintf(R"({"result":"database_upsert_victory failed"})")};
	}
	if(is_error(database_upsert_trophies(&stats))) {
		return {500, mg_mprintf(R"({"result":"database_upsert_trophies failed"})")};
	}
	if(is_error(database_upsert_hero(&stats))) {
		return {500, mg_mprintf(R"({"result":"database_upsert_hero failed"})")};
	}
	if(is_error(database_upsert_shark(&stats))) {
		return {500, mg_mprintf(R"({"result":"database_upsert_hero failed"})")};
	}
	if(is_error(database_upsert_win_rate_recent(&stats))) {
		return {500, mg_mprintf(R"({"result":"database_upsert_win_rate_recent failed"})")};
	}
	if(is_error(database_upsert_win_rate_all_time(&stats))) {
		return {500, mg_mprintf(R"({"result":"database_upsert_win_rate_all_time failed"})")};
	}

	//print_stats(&stats);

	return {200, mg_mprintf(R"({"result":"ok"})")};
}

static const size_t MAX_LEADERBOARD_ROWS = 100;
static const size_t MAX_WEEKS_PER_SEASON = 13;

struct Leaderboard {
   char* league;
   int season;
   struct Leaderboard_Row {
	   std::uint64_t member_id; // member_id will be 0 on first invalid row
	   int rank;
	   float average;
	   int drafts;
	   int trophies;
	   float win_rate;
	   int points[MAX_WEEKS_PER_SEASON];
   } rows[MAX_LEADERBOARD_ROWS];

   int row_count;
};

void print_leaderboard(const Leaderboard* l) {
	log(LOG_LEVEL_DEBUG, "league: %s\n", l->league);
	log(LOG_LEVEL_DEBUG, "season: %d\n", l->season);
	log(LOG_LEVEL_DEBUG, "rows : [\n");
	for(size_t row = 0; row < MAX_LEADERBOARD_ROWS; ++row) {
		if(l->rows[row].member_id != 0) {
			log(LOG_LEVEL_DEBUG, "{\n");
			log(LOG_LEVEL_DEBUG, "	member_id: %lu\n", l->rows[row].member_id);
			log(LOG_LEVEL_DEBUG, "	rank     : %d\n", l->rows[row].rank);
			log(LOG_LEVEL_DEBUG, "	average  : %f\n", l->rows[row].average);
			log(LOG_LEVEL_DEBUG, "	drafts   : %d\n", l->rows[row].drafts);
			log(LOG_LEVEL_DEBUG, "	trophies : %d\n", l->rows[row].trophies);
			log(LOG_LEVEL_DEBUG, "	win_rate : %f\n", l->rows[row].win_rate);
			log(LOG_LEVEL_DEBUG, "	points   : [ ");
			for(size_t week = 0; week < MAX_WEEKS_PER_SEASON; ++week) {
				log(LOG_LEVEL_DEBUG, "%d, ", l->rows[row].points[week]);
			}
			log(LOG_LEVEL_DEBUG, "]\n");
			log(LOG_LEVEL_DEBUG, "}\n");
		}
	}
	log(LOG_LEVEL_DEBUG, "]\n");
}

static Database_Result<Database_No_Value> database_upsert_leaderboard(const Leaderboard* leaderboard) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = R"(
		REPLACE INTO leaderboards(
			league,    -- 0
			season,    -- 1
			member_id, -- 2
			rank,      -- 3
			week_01,   -- 4
			week_02,   -- 5
			week_03,   -- 6
			week_04,   -- 7
			week_05,   -- 8
			week_06,   -- 9
			week_07,   -- 10
			week_08,   -- 11
			week_09,   -- 12
			week_10,   -- 13
			week_11,   -- 14
			week_12,   -- 15
			week_13,   -- 16
			points,    -- 17
			average,   -- 18
			drafts,    -- 19
			trophies,  -- 20
			win_rate)  -- 21
		VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
		;)";

	for(int row = 0; row < leaderboard->row_count; ++row) {
		MYSQL_STATEMENT();

		MYSQL_INPUT_INIT(22);
		MYSQL_INPUT_STR(leaderboard->league, strlen(leaderboard->league));
		MYSQL_INPUT_I32(&leaderboard->season);
		MYSQL_INPUT_I64(&leaderboard->rows[row].member_id);
		MYSQL_INPUT_I32(&leaderboard->rows[row].rank);

		MYSQL_INPUT_I32(&leaderboard->rows[row].points[ 0]);
		MYSQL_INPUT_I32(&leaderboard->rows[row].points[ 1]);
		MYSQL_INPUT_I32(&leaderboard->rows[row].points[ 2]);
		MYSQL_INPUT_I32(&leaderboard->rows[row].points[ 3]);
		MYSQL_INPUT_I32(&leaderboard->rows[row].points[ 4]);
		MYSQL_INPUT_I32(&leaderboard->rows[row].points[ 5]);
		MYSQL_INPUT_I32(&leaderboard->rows[row].points[ 6]);
		MYSQL_INPUT_I32(&leaderboard->rows[row].points[ 7]);
		MYSQL_INPUT_I32(&leaderboard->rows[row].points[ 8]);
		MYSQL_INPUT_I32(&leaderboard->rows[row].points[ 9]);
		MYSQL_INPUT_I32(&leaderboard->rows[row].points[10]);
		MYSQL_INPUT_I32(&leaderboard->rows[row].points[11]);
		MYSQL_INPUT_I32(&leaderboard->rows[row].points[12]);

		MYSQL_INPUT_I32(&leaderboard->rows[row].points);
		MYSQL_INPUT_F32(&leaderboard->rows[row].average);
		MYSQL_INPUT_I32(&leaderboard->rows[row].drafts);
		MYSQL_INPUT_I32(&leaderboard->rows[row].trophies);
		MYSQL_INPUT_F32(&leaderboard->rows[row].win_rate);
		MYSQL_INPUT_BIND_AND_EXECUTE();
	}

	MYSQL_RETURN();
}

http_response parse_leaderboards(const mg_str json) {
	Leaderboard leaderboard;
	memset(&leaderboard, 0, sizeof(Leaderboard));

	leaderboard.league = mg_json_get_str(json, "$.league");
	if(leaderboard.league == NULL) {
		return {400, mg_mprintf(R"({"result":"'league' key not found"})")};
	}
	defer { free(leaderboard.league); };

	leaderboard.season = mg_json_get_long(json, "$.season", -1);
	if(leaderboard.season == -1) {
		return {400, mg_mprintf(R"({"result":"'season' key not found"})")};
	}

	while(true) {
		char key[12];
		snprintf(key, 12, "$.rows[%d]", leaderboard.row_count);

		int length;
		int offset = mg_json_get(json, key, &length);
		if(offset < 0) {
			if(offset == MG_JSON_NOT_FOUND) {
				break;
			} else {
				return {400, mg_mprintf(R"({"result":"JSON parse error"})")};
			}
		}

		const mg_str row = {json.ptr + offset, (size_t) length};

		{
			const char* value = mg_json_get_str(row, "$.member_id");
			if(value != NULL) {
				leaderboard.rows[leaderboard.row_count].member_id = strtoull(value, NULL, 10);
				free((void*)value);
			} else {
				return {400, mg_mprintf(R"({"result":"'%s.member_id' key not found"})", key)};
			}
		}

		leaderboard.rows[leaderboard.row_count].rank = mg_json_get_long(row, "$.rank", -1);
		if(leaderboard.rows[leaderboard.row_count].rank == -1) {
			return {400, mg_mprintf(R"({"result":"'%s.rank' key not found"})", key)};
		}

		leaderboard.rows[leaderboard.row_count].drafts = mg_json_get_long(row, "$.drafts", -1);
		if(leaderboard.rows[leaderboard.row_count].drafts == -1) {
			return {400, mg_mprintf(R"({"result":"'%s.drafts' key not found"})", key)};
		}

		leaderboard.rows[leaderboard.row_count].trophies = mg_json_get_long(row, "$.trophies", -1);
		if(leaderboard.rows[leaderboard.row_count].trophies == -1) {
			return {400, mg_mprintf(R"({"result":"'%s.trophies' key not found"})", key)};
		}

		{
			double value;
			if(mg_json_get_num(row, "$.average", &value) == false) {
				return {400, mg_mprintf(R"({"result":"'%s.average' key not found"})", key)};
			}
			leaderboard.rows[leaderboard.row_count].average = (float) value;
		}

		{
			double value;
			if(mg_json_get_num(row, "$.win_rate", &value) == false) {
				return {400, mg_mprintf(R"({"result":"'%s.win_rate' key not found"})", key)};
			}
			leaderboard.rows[leaderboard.row_count].win_rate = (float) value;
		}

		for(size_t week = 0; week < MAX_WEEKS_PER_SEASON; ++week) {
			char key[32];
			snprintf(key, 32, "$.points[%lu]", week);

			leaderboard.rows[leaderboard.row_count].points[week] = mg_json_get_long(row, key, -1);
		}

		leaderboard.row_count++;
	}

	//print_leaderboard(&leaderboard);

	if(is_error(database_upsert_leaderboard(&leaderboard))) {
		return {200, mg_mprintf(R"({"result":"database_upsert_leaderboard() failed"})")};
	}

	return {200, mg_mprintf(R"({"result":"ok"})")};
}

http_response make_thumbnail(const mg_str json) {

	const char* url = mg_json_get_str(json, "$.url");
	if(url == NULL) {
		return {400, strdup("{\"result\":\"malformed JSON\"}")};
	}
	defer { free((void*)url); };

	const char* filename = NULL;
	for(size_t i = strlen(url)-1; i > 0; --i) {
		if(url[i] == '/') {
			filename = &url[i] + 1;
			break;
		}
	}

	char local_file_path[FILENAME_MAX];
	snprintf(local_file_path, FILENAME_MAX, "%s/static/badge_thumbnails/%s", HTTP_SERVER_DOC_ROOT, filename);
	if(access(local_file_path, F_OK) == 0) {
		return {200, mg_mprintf(R"({"result":"%s:%d/static/badge_thumbnails/%s"})", g_config.server_fqdn, g_config.bind_port, filename)};
	} else {
		log(LOG_LEVEL_DEBUG, "%s: downloadfile(%s)", __FUNCTION__, url);
		auto buffer = download_file(url);
		if(has_value(buffer)) {
			defer { free(buffer.value.data); };

			int width, height, channels;
			uint8_t* img = stbi_load_from_memory(buffer.value.data, buffer.value.size, &width, &height, &channels, 4);
			if(img != NULL) {
				defer{ stbi_image_free(img); };

				uint8_t* resized = (uint8_t*)alloca(THUMBNAIL_SIZE*THUMBNAIL_SIZE*4);
				stbir_resize_uint8_srgb(img, width, height, 0, resized, THUMBNAIL_SIZE, THUMBNAIL_SIZE, 0, STBIR_RGBA);

				snprintf(local_file_path, FILENAME_MAX, "%s/static/badge_thumbnails/%s", HTTP_SERVER_DOC_ROOT, filename);
				stbi_write_png_compression_level = 9;
				if(stbi_write_png(local_file_path, THUMBNAIL_SIZE, THUMBNAIL_SIZE, 4, resized, THUMBNAIL_SIZE*4) != 0) {
					return {201, mg_mprintf(R"({"result":"%s:%d/static/badge_thumbnails/%s"})", g_config.server_fqdn, g_config.bind_port, filename)};
				} else {
					return {500, mg_mprintf(R"({"result":"%s"})", "saving file failed")};
				}
			} else {
				return {400, mg_mprintf(R"({"result":"%s"})", stbi_failure_reason())};
			}
		} else {
			return {400, mg_mprintf(R"({"result":"downloading url failed"})")};
		}
	}
}

static Database_Result<Database_No_Value> database_upsert_badge_card(const uint64_t member_id, const char* url) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "REPLACE INTO badges (id, url, timestamp) VALUES (?,?,?)";
	MYSQL_STATEMENT();

	time_t timestamp = time(NULL);

	MYSQL_INPUT_INIT(3);
	MYSQL_INPUT_I64(&member_id);
	MYSQL_INPUT_STR(url, strlen(url));
	MYSQL_INPUT_I64(&timestamp);
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

http_response pdf_to_png(const mg_str json) {
	int width;
	{
		double value;
		if(mg_json_get_num(json, "$.width", &value) == true) {
			width = (int) value;
		} else {
			return {400, mg_mprintf(R"({"result":"'width' key not found"})")};
		}
	}

	int height;
	{
		double value;
		if(mg_json_get_num(json, "$.height", &value) == true) {
			height = (int) value;
		} else {
			return {400, mg_mprintf(R"({"result":"'height' key not found"})")};
		}
	}

	int dpi;
	{
		double value;
		if(mg_json_get_num(json, "$.dpi", &value) == true) {
			dpi = (int) value;
		} else {
			return {400, mg_mprintf(R"({"result":"'dpi' key not found"})")};
		}
	}

	uint64_t member_id;
	{
		char* value = mg_json_get_str(json, "$.member_id");
		if(value != NULL) {
			member_id = strtoull(value, NULL, 10);
			free(value);
		} else {
			return {400, mg_mprintf(R"({"result":"'member_id' key not found"})")};
		}
	}

	int mem_len = 0;
	char* mem = mg_json_get_b64(json, "$.bytes", &mem_len);
	if(mem == NULL) {
		return {400, mg_mprintf(R"({"result":"'member_id' key not found"})")};
	}
	defer{ free(mem); };

	poppler::document *pdf = poppler::document::load_from_raw_data(mem, mem_len);
	if(pdf == NULL) {
		return {400, mg_mprintf(R"({"result":"could not open PDF"})")};
	}
	defer{ delete pdf; };
	int page_count = pdf->pages();
	if(page_count == 0) {
		return {400, mg_mprintf(R"({"result":"no pages"})")};
	}
	poppler::page *page = pdf->create_page(0);
	defer{ delete page; };
	poppler::page_renderer renderer;
	renderer.set_render_hints(poppler::page_renderer::text_antialiasing | poppler::page_renderer::text_hinting);
	renderer.set_image_format(poppler::image::format_enum::format_rgb24);
	poppler::image img = renderer.render_page(page, dpi, dpi, 0, 0, width, height);

	int size;
	unsigned char* png = stbi_write_png_to_mem((const unsigned char*)img.data(), img.bytes_per_row(), img.width(), img.height(), 3, &size);
	if(png == NULL) {
		return {500, mg_mprintf(R"({"result":"Error decoding PDF to PNG"})")};
	}
	defer{ STBIW_FREE(png); };

	auto upload = upload_img_to_imgur((const char*)png, size, g_config.imgur_client_secret);
	if(is_error(upload)) {
		return {500, mg_mprintf(R"({"result":"Error uploading to Imgur: %s"})", upload.errstr)};
	}
	defer{ free(upload.value.data); };

	mg_str result_json = {(const char*)upload.value.data, upload.value.size};
	char* url = mg_json_get_str(result_json, "$.data.link");
	if(url == NULL) {
		return {500, mg_mprintf(R"({"result":"JSON parse error"})")};
	}
	//defer{ free(url); }; // FIXME: Probably leaking memory here!

	auto db_result = database_upsert_badge_card(member_id, url);
	if(is_error(db_result)) {
		// NOTE: This is an error, but not treated as fatal.
		log(LOG_LEVEL_ERROR, db_result.errstr);
	}

	return {201, mg_mprintf(R"({"result":"%s"})", url)};
}

static Database_Result<Database_No_Value> database_clear_commands() {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "TRUNCATE TABLE commands";
	MYSQL_STATEMENT();
	MYSQL_EXECUTE();
	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_insert_command(const char* name, const char team, const char hidden, const char* content, const char* summary) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "INSERT INTO commands (name, team, hidden, content, summary) VALUES (?,?,?,?,?)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(5);
	MYSQL_INPUT_STR(name, strlen(name));
	MYSQL_INPUT_I8(&team);
	MYSQL_INPUT_I8(&hidden);
	MYSQL_INPUT_STR(content, strlen(content));
	MYSQL_INPUT_STR(summary, strlen(summary));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

http_response parse_commands(const mg_str json) {
	struct Command {
		char* name;
		bool team;
		bool hidden;
		char* text;
		char* summary;
	};

	std::vector<Command> commands;
	commands.reserve(100);

	auto cleanup = [&commands]() {
		for(auto& c : commands) {
			free(c.name);
			free(c.text);
			free(c.summary);
		}
	};

	defer{ cleanup(); };

	int index = 0;
	while(true) {
		char key[32];
		snprintf(key, 32, "$[%d]", index);

		int length;
		int offset = mg_json_get(json, key, &length);
		if(offset < 0) {
			if(offset == MG_JSON_NOT_FOUND) {
				break;
			} else {
				return {400, mg_mprintf(R"({"result":"JSON parse error"})")};
			}
		}
		const mg_str row = {json.ptr + offset, (size_t) length};

		commands.push_back({NULL, false, false, NULL, NULL});

		commands.back().name = mg_json_get_str(row, "$.name");
		if(commands.back().name == NULL) {
			return {400, mg_mprintf(R"({"result":"'name' key not found"})")};
		}

		commands.back().text = mg_json_get_str(row, "$.text");
		if(commands.back().text == NULL) {
			return {400, mg_mprintf(R"({"result":"'text' key not found"})")};
		}

		commands.back().summary = mg_json_get_str(row, "$.summary");
		if(commands.back().summary == NULL) {
			return {400, mg_mprintf(R"({"result":"'summary' key not found"})")};
		}

		if(mg_json_get_bool(row, "$.team", &commands.back().team) == false) {
			return {400, mg_mprintf(R"({"result":"'team' key not found"})")};
		}

		if(mg_json_get_bool(row, "$.hide", &commands.back().hidden) == false) {
			return {400, mg_mprintf(R"({"result":"'hide' key not found"})")};
		}

		//log(LOG_LEVEL_DEBUG, "%s: command %d: %s", __FUNCTION__, index, commands.back().name);

		index++;
	}

	// NOTE: This can hang the thread if MariaDB is stuck waiting for a lock.
	// In the MariaDB terminal type `show full processlist` to see a list of connect clients
	// and `kill [id]` the client that is stuck holding the lock.
	// TODO: This should be start a transaction so on failure we can roll back to a valid state.
	if(is_error(database_clear_commands())) {
		return {500, mg_mprintf(R"({"result":"Internal server error: database_clear_commands() failed"})")};
	}

	for(auto& c : commands) {
		if(is_error(database_insert_command(c.name, c.team, c.hidden, c.text, c.summary))) {
			return {500, mg_mprintf(R"({"result":"Internal server error: database_insert_command() failed"})")};
		} else {
			log(LOG_LEVEL_INFO, "%s: Added command: %s", __FUNCTION__, c.name);
		}
	}

	return {200, mg_mprintf(R"({"result":"ok"})")};
}

Database_Result<Database_No_Value> database_update_xmage_version(const char* version) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "REPLACE INTO xmage_version (version, timestamp) VALUES (?,?)";
	MYSQL_STATEMENT();

	time_t timestamp = time(NULL);

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT_STR(version, strlen(version));
	MYSQL_INPUT_I64(&timestamp);
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

http_response parse_xmage_version(const mg_str json) {
	char* xmage_version = mg_json_get_str(json, "$.version");
	if(xmage_version == NULL) {
		return {400, mg_mprintf(R"({"result":"'version' key not found"})")};
	}
	defer{ free(xmage_version); };

	log(LOG_LEVEL_DEBUG, "XMage version: %s\n", xmage_version);

	database_update_xmage_version(xmage_version);

	return {200, mg_mprintf(R"({"result":"ok"})")};
}

Database_Result<Database_No_Value> database_add_role_command(uint64_t guild_id, uint64_t member_id, int action, char* role) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "INSERT INTO role_commands (guild_id, member_id, action, role) VALUES (?,?,?,?)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(4);
	MYSQL_INPUT_I64(&guild_id);
	MYSQL_INPUT_I64(&member_id);
	MYSQL_INPUT_I32(&action);
	MYSQL_INPUT_STR(role, strlen(role));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

http_response role_command(const mg_str json) {
	uint64_t member_id = 0;
	{
		char* value = mg_json_get_str(json, "$.member_id");
		if(value != NULL) {
			member_id = strtoull(value, NULL, 10);
			free(value);
		} else {
			return {400, mg_mprintf(R"({"result":"'member_id' key not found"})")};
		}
	}

	uint64_t guild_id = 0;
	{
		char* value = mg_json_get_str(json, "$.guild_id");
		if(value != NULL) {
			guild_id = strtoull(value, NULL, 10);
			free(value);
		} else {
			return {400, mg_mprintf(R"({"result":"'guild_id' key not found"})")};
		}
	}
	if(guild_id == 0) {
		return {400, mg_mprintf(R"({"result":"Invalid value for 'guild_id' key"})")};
	}

	int action = mg_json_get_long(json, "$.action", -1);
	if(action == -1) {
		return {400, mg_mprintf(R"({"result":"'action' key not found"})")};
	}

	if((action < 0) || (action > 1)) {
		return {400, mg_mprintf(R"({"result":"Invalid value '%d' for 'action' key"})", action)};
	}

	char* role_name = mg_json_get_str(json, "$.role_name");
	if(role_name == NULL) {
			return {400, mg_mprintf(R"({"result":"'role_name' key not found"})")};
	}
	defer { free(role_name); };

	auto result = database_add_role_command(guild_id, member_id, action, role_name);
	if(is_error(result)) {
		return {500, mg_mprintf(R"({"result":"database_add_role_command failed"})")};
	}
	return {200, mg_mprintf(R"({"result":"ok"})")};
}

// --- Badge card generation ---

static Database_Result<Database_No_Value> database_clear_badge_images() {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "TRUNCATE TABLE badge_images";
	MYSQL_STATEMENT();
	MYSQL_EXECUTE();
	MYSQL_RETURN();
}

static Database_Result<Database_No_Value> database_insert_badge_image(const char* category, const char* name, const char* display, const char* url) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "INSERT INTO badge_images (category, name, display, url) VALUES (?,?,?,?)";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(4);
	MYSQL_INPUT_STR(category, strlen(category));
	MYSQL_INPUT_STR(name,     strlen(name));
	MYSQL_INPUT_STR(display,  strlen(display));
	MYSQL_INPUT_STR(url,      strlen(url));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

http_response upload_badge_images(const mg_str json) {
	struct Badge {
		char* category;
		char* name;
		char* display;
		char *url;
	};

	std::vector<Badge> badges;
	badges.reserve(2000);

	auto cleanup = [&badges]() {
		for(auto& b: badges) {
			free(b.category);
			free(b.name);
			free(b.display);
			free(b.url);
		}
	};

	defer{ cleanup(); };

	int index = 0;
	while(true) {
		char key[32];
		snprintf(key, 32, "$[%d]", index);

		int length;
		int offset = mg_json_get(json, key, &length);
		if(offset < 0) {
			if(offset == MG_JSON_NOT_FOUND) {
				break;
			} else {
				return {400, mg_mprintf(R"({"result":"JSON parse error"})")};
			}
		}
		const mg_str row = {json.ptr + offset, (size_t) length};

		badges.push_back({NULL, NULL, NULL, NULL});

		badges.back().category = mg_json_get_str(row, "$.category");
		if(badges.back().category == NULL) {
			return {400, mg_mprintf(R"({"result":"'category' key not found on row %d"})", index)};
		}

		badges.back().name = mg_json_get_str(row, "$.name");
		if(badges.back().name == NULL) {
			return {400, mg_mprintf(R"({"result":"'name' key not found on row %d"})", index)};
		}

		badges.back().display = mg_json_get_str(row, "$.display");
		if(badges.back().display == NULL) {
			badges.back().display = strdup(badges.back().name);
		}

		badges.back().url = mg_json_get_str(row, "$.url");
		if(badges.back().url == NULL) {
			return {400, mg_mprintf(R"({"result":"'url' key not found on row %d"})", index)};
		}

		index++;
	}

	// NOTE: This can hang the thread if MariaDB is stuck waiting for a lock.
	// In the MariaDB terminal type `show full processlist` to see a list of connect clients
	// and `kill [id]` the client that is stuck holding the lock.
	// TODO: This should be start a transaction so on failure we can roll back to a valid state.
	if(is_error(database_clear_badge_images())) {
		return {500, mg_mprintf(R"({"result":"Internal server error: database_clear_badge_images() failed"})")};
	}

	for(auto& b : badges) {
		if(is_error(database_insert_badge_image(b.category, b.name, b.display, b.url))) {
			return {500, mg_mprintf(R"({"result":"Internal server error: database_insert_badge_image() failed"})")};
		} else {
			log(LOG_LEVEL_INFO, "%s: Added badge: %s - %s", __FUNCTION__, b.category, b.name);
		}
	}

	return {200, mg_mprintf(R"({"result":"ok"})")};
}

struct Badge {
	char category[BADGE_CATEGORY_LENGTH_MAX];
	char name[BADGE_NAME_LENGTH_MAX];
	char display[BADGE_DISPLAY_NAME_LENGTH_MAX];
	char url[URL_LENGTH_MAX];
};

static Database_Result<Badge> database_get_badge_image(const char* category, const char* name) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "SELECT category, name, display, url FROM badge_images WHERE category=? AND name=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT_STR(category, strlen(category));
	MYSQL_INPUT_STR(name, strlen(name));
	MYSQL_INPUT_BIND_AND_EXECUTE()

	Badge result;

	MYSQL_OUTPUT_INIT(4);
	MYSQL_OUTPUT_STR(result.category, BADGE_CATEGORY_LENGTH_MAX);
	MYSQL_OUTPUT_STR(result.name, BADGE_NAME_LENGTH_MAX);
	MYSQL_OUTPUT_STR(result.display, BADGE_DISPLAY_NAME_LENGTH_MAX);
	MYSQL_OUTPUT_STR(result.url, URL_LENGTH_MAX);
	MYSQL_OUTPUT_BIND_AND_STORE();

	MYSQL_FETCH_AND_RETURN_ZERO_OR_ONE_ROWS();
}

static Database_Result<std::vector<Badge>> database_get_unchecked_badges(int how_many) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "SELECT category, name, display, url FROM badge_images WHERE checked=0 LIMIT ?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(1);
	MYSQL_INPUT_I32(&how_many);
	MYSQL_INPUT_BIND_AND_EXECUTE();

	Badge result;
	MYSQL_OUTPUT_INIT(4);
	MYSQL_OUTPUT_STR(result.category, BADGE_CATEGORY_LENGTH_MAX);
	MYSQL_OUTPUT_STR(result.name, BADGE_NAME_LENGTH_MAX);
	MYSQL_OUTPUT_STR(result.display, BADGE_DISPLAY_NAME_LENGTH_MAX);
	MYSQL_OUTPUT_STR(result.url, URL_LENGTH_MAX);
	MYSQL_OUTPUT_BIND_AND_STORE();

	std::vector<Badge> results;
	results.reserve(how_many);

	MYSQL_FETCH_AND_RETURN_MULTIPLE_ROWS();
}

/*

Take JSON input like this...

{
member_name : "Some Discord Username",
badges :[{category="badge category", name="badge name", ...}]
}

.. and render it to a badge card.

*/
http_response make_badge_card(const mg_str json) {
	char* member_name = mg_json_get_str(json, "$.member_name");
	if(member_name == NULL) {
		return {400, mg_mprintf(R"({"result":"'member_name' key not found"})")};
	}
	defer { free(member_name); };

	uint64_t member_id;
	{
		char* value = mg_json_get_str(json, "$.member_id");
		if(value != NULL) {
			member_id = strtoull(value, NULL, 10);
			free(value);
		} else {
			return {400, mg_mprintf(R"({"result":"'member_id' key not found"})")};
		}
	}

	struct Badge_ID {
		char* category;
		char* name;
	};

	std::vector<Badge_ID> badge_ids;
	badge_ids.reserve(100);

	auto cleanup = [&badge_ids]() {
		for(auto& b : badge_ids) {
			free(b.category);
			free(b.name);
		}
	};

	defer{ cleanup(); };

	int index = 0;
	while(true) {
		char key[22];
		snprintf(key, 22, "$.badges[%d]", index);

		int length;
		int offset = mg_json_get(json, key, &length);
		if(offset < 0) {
			if(offset == MG_JSON_NOT_FOUND) {
				break;
			} else {
				return {400, mg_mprintf(R"({"result":"JSON parse error"})")};
			}
		}

		const mg_str row = {json.ptr + offset, (size_t) length};

		badge_ids.push_back({NULL, NULL});

		badge_ids.back().category = mg_json_get_str(row, "$.category");
		if(badge_ids.back().category == NULL) {
			return {400, mg_mprintf(R"({"result":"'category' key not found"})")};
		}

		badge_ids.back().name = mg_json_get_str(row, "$.name");
		if(badge_ids.back().name == NULL) {
			return {400, mg_mprintf(R"({"result":"'name' key not found"})")};
		}

		index++;
	}

	// Construct the card. Unless otherwise stated, units are pixels.
	struct Badge_Card_Design {
		float aspect_ratio;
		int minimum_columns;
		int maximum_columns;

		Pixel background_color;

		// Border around the entire badge card.
		int border_thickness;
		Pixel border_color;

		// Frame around the badges
		int frame_thickness;
		Pixel frame_top_color;
		Pixel frame_bottom_color;
		int frame_padding; // Padding between frame and badges

		// Member name
		int header_height;
		int header_font_size;
		const char* header_font_name;
		//Pixel header_font_color;

		// Badge names
		int badge_name_height;
		int badge_name_font_size;
		const char* badge_name_font_name;
		//Pixel badge_name_font_color;

		int badge_size;
		int badge_padding; // Vertical Padding between badges.
	} design;

	// We eventually might want to support multiple designs...
	design.aspect_ratio = 4.0f / 3.0f;
	design.minimum_columns = 4;
	design.maximum_columns = 17;
	design.background_color.c = 0xFF101010;
	design.border_thickness = 2;
	design.border_color.c = 0xFF000000;
	design.frame_thickness = 20;
	design.frame_top_color.c = 0xFF0D4373;
	design.frame_bottom_color.c = 0xFF1C7FBD;
	design.frame_padding = 4;
	design.header_font_size = 26;
	design.header_height = design.header_font_size + (design.header_font_size * 0.4);
	design.header_font_name = "gfx/badge_card/Beleren2016-Bold.ttf";
	//design.header_font_color = 0xFFFFFFFF;
	design.badge_name_height = 20;
	design.badge_name_font_size = 12;
	design.badge_name_font_name = "gfx/badge_card/calibri.ttf";
	//design.badge_name_font_color.c = 0xFFFFFFFF;
	design.badge_size = 100;
	design.badge_padding = 4;

	// Load the header font
	size_t header_font_buffer_size;
	u8* header_font_buffer = file_slurp(design.header_font_name, &header_font_buffer_size);
	if(header_font_buffer == NULL) {
		return {500, mg_mprintf(R"({"result":"failed to read header font file"})")};
	}
	defer{ free(header_font_buffer); };
	stbtt_fontinfo header_font;
	int header_font_result = stbtt_InitFont(&header_font, header_font_buffer, stbtt_GetFontOffsetForIndex(header_font_buffer, 0));
	if(header_font_result == 0) {
		return {500, mg_mprintf(R"({"result":"failed to init header font file"})")};
	}

	// Load the badge name font
	size_t badge_font_buffer_size;
	u8* badge_font_buffer = file_slurp(design.badge_name_font_name, &badge_font_buffer_size);
	if(badge_font_buffer == NULL) {
		return {500, mg_mprintf(R"({"result":"failed to read badge font file"})")};
	}
	defer{ free(badge_font_buffer); };
	stbtt_fontinfo badge_font;
	int badge_font_result = stbtt_InitFont(&badge_font, badge_font_buffer, stbtt_GetFontOffsetForIndex(badge_font_buffer, 0));
	if(badge_font_result == 0) {
		return {500, mg_mprintf(R"({"result":"failed to init badge font file"})")};
	}

	const int total_badges = (int)badge_ids.size();
	//int badge_columns_needed = (int) floor(((sqrt(total_badges)+1) * design.aspect_ratio));
	int badge_columns_needed = (int) floor(((sqrt(total_badges)) * design.aspect_ratio));
	if(badge_columns_needed < design.minimum_columns) badge_columns_needed = design.minimum_columns;
	if(badge_columns_needed > design.maximum_columns) badge_columns_needed = design.maximum_columns;
	int badge_rows_needed = 1;
	while(badge_rows_needed * badge_columns_needed < total_badges) badge_rows_needed++;

	Image canvas;
	canvas.w = ((design.border_thickness + design.frame_thickness + design.frame_padding)*2) + (badge_columns_needed*design.badge_size) + ((badge_columns_needed - 1) * design.badge_padding);
	canvas.h = ((design.border_thickness + design.frame_thickness + design.frame_padding)*2) + (design.header_height) + (badge_rows_needed*(design.badge_size+design.badge_name_height));
	canvas.channels = 4;
	canvas.data = malloc(canvas.w * canvas.h * canvas.channels);
	if(canvas.data == NULL) {
		return {500, mg_mprintf(R"({"result":"memory allocation for badge card failed"})")};
	}
	defer{ free(canvas.data); };

	auto draw_filled_rect = [](Image* canvas, int x, int y, int w, int h, Pixel color) {
		Pixel* ptr = (Pixel*)canvas->data + (y*canvas->w) + x;
		for(int row = 0; row < h; ++row) {
			for(int col = 0; col < w; ++col) {
				ptr++->c = color.c;
			}
			ptr += canvas->w - w;
		}
	};

	// background
	draw_filled_rect(
			&canvas,
			design.border_thickness + design.frame_thickness,
			design.border_thickness + design.frame_thickness,
			canvas.w - (2 * (design.border_thickness + design.frame_thickness)),
			canvas.h - (2 * (design.border_thickness + design.frame_thickness)),
			design.background_color
			);

	// top border
	draw_filled_rect(
			&canvas,
			0,
			0,
			canvas.w,
			design.border_thickness,
			design.border_color
			);

	// bottom border
	draw_filled_rect(
			&canvas,
			0,
			canvas.h-design.border_thickness,
			canvas.w,
			design.border_thickness,
			design.border_color
			);

	// left border
	draw_filled_rect(
			&canvas,
			0,
			design.border_thickness,
			design.border_thickness,
			canvas.h - (2 * design.border_thickness),
			design.border_color
			);

	// right border
	draw_filled_rect(
			&canvas,
			canvas.w - design.border_thickness,
			design.border_thickness,
			design.border_thickness,
			canvas.h - (2 * design.border_thickness),
			design.border_color
			);

	// Top frame
	draw_filled_rect(
			&canvas,
			design.border_thickness,
			design.border_thickness,
			canvas.w - (2 * design.border_thickness),
			design.frame_thickness,
			design.frame_top_color
			);

	// Bottom frame
	draw_filled_rect(
			&canvas,
			design.border_thickness,
			canvas.h - design.border_thickness - design.frame_thickness,
			canvas.w - (2 * design.border_thickness),
			design.frame_thickness,
			design.frame_bottom_color
			);

	// Function to draw the left and right side frame gradients
	auto draw_gradient_rect = [](Image* canvas, int x, int y, int w, int h, Pixel begin, Pixel end) {
		Pixel* ptr = (Pixel*)canvas->data + (y*canvas->w) + x;
		float r_step = (float)((end.components.r - begin.components.r) / (float)h);
		float g_step = (float)((end.components.g - begin.components.g) / (float)h);
		float b_step = (float)((end.components.b - begin.components.b) / (float)h);
		for(int row = 0; row < h; ++row) {
			for(int col = 0; col < w; ++col) {
				ptr->components.a = 0xFF;
				ptr->components.r = begin.components.r + (row * r_step);
				ptr->components.g = begin.components.g + (row * g_step);
				ptr->components.b = begin.components.b + (row * b_step);
				ptr++;
			}
			ptr += canvas->w - w;
		}
	};

	draw_gradient_rect(
			&canvas,
			design.border_thickness,
			design.border_thickness + design.frame_thickness,
			design.frame_thickness,
			canvas.h - (2 * (design.border_thickness + design.frame_thickness)),
			design.frame_top_color,
			design.frame_bottom_color
			);

	draw_gradient_rect(
			&canvas,
			canvas.w - design.border_thickness - design.frame_thickness,
			design.border_thickness + design.frame_thickness,
			design.frame_thickness,
			canvas.h - (2 * (design.border_thickness + design.frame_thickness)),
			design.frame_top_color,
			design.frame_bottom_color
			);

	// Write the member name
	{
		char header_string[64];
		snprintf(header_string, 64, "%s's badges", member_name);
		int scale = 4;
		Text_Dim dim = get_text_dimensions(&header_font, design.header_font_size*scale, (const u8*)header_string);
		Result<Image> name = make_image(dim.w, dim.h, 1, 0x00000000);
		if(is_error(name)) {
			return {500, mg_mprintf(R"({"result":"Error creating header canvas"})")};
		}
		defer { free(name.value.data); };

		render_text_to_image(
				&header_font,
				(const u8*)header_string,
				design.header_font_size*scale,
				&name.value,
				0,
				0,
				{.c=0xFFFFFFFF}
				);

		Image resized;
		resized.w = dim.w / scale;
		resized.h = dim.h / scale;
		resized.channels = 1;
		resized.data = stbir_resize_uint8_srgb((const unsigned char*)name.value.data, dim.w, dim.h, 0, NULL, resized.w, resized.h, 0, STBIR_1CHANNEL);
		if(resized.data == NULL) {
			return {500, mg_mprintf(R"({"result":"%s: out of memory})", __FUNCTION__)};
		}
		defer{ free(resized.data); };

		if(resized.w < canvas.w - (2 * (design.border_thickness + design.frame_thickness))) {
			// Rendered text fits the width, blit it as is.
			blit_A8_to_RGBA(
					&resized,
					resized.w,
					{.c=0xFFFFFFFF},
					&canvas,
					(canvas.w / 2) - (resized.w/2),
					design.border_thickness + design.frame_thickness + design.frame_padding + (design.header_height/2) - (resized.h/2));
		} else {
			// Rendered text is too wide. Resize it
			// TODO... is this even possible here?
		}
	}

	// Draw the badge images and labels
	int badge_start_x = design.border_thickness + design.frame_thickness + design.frame_padding;
	int badge_start_y = design.border_thickness + design.frame_thickness + design.frame_padding + design.header_height;
	int badge_x = badge_start_x;
	int badge_y = badge_start_y;
	int badge_col = 0;
	for(const auto id : badge_ids) {
		auto badge = database_get_badge_image(id.category, id.name);
		if(has_value(badge)) {
			// Get the filename part from the URL
			const char* filename = NULL;
			for(size_t i = strlen(badge.value.url)-1; i > 0; --i) {
				if(badge.value.url[i] == '/') {
					filename = &badge.value.url[i] + 1;
					break;
				}
			}
			if(filename == NULL) {
				log(LOG_LEVEL_ERROR, "%s: Could not get filename part from url: %s", __FUNCTION__, badge.value.url);
				continue;
			}

			char local_file_path[FILENAME_MAX];
			snprintf(local_file_path, FILENAME_MAX, "gfx/badge_card/images/%s", filename);

			if(access(local_file_path, F_OK) == 0) {
				auto img = load_image(local_file_path, 4);
				if(has_value(img)) {
					defer{ stbi_image_free(img.value.data); };

					{
						// Resize the image to the desired size.
						Image resized;
						resized.w = design.badge_size;
						resized.h = design.badge_size;
						resized.channels = img.value.channels;
						resized.data = stbir_resize_uint8_srgb((const unsigned char*)img.value.data, img.value.w, img.value.h, 0, NULL, resized.w, resized.h, 0, STBIR_RGBA);
						if(resized.data == NULL) {
							return {500, mg_mprintf(R"({"result":"%s: out of memory})", __FUNCTION__)};
						}
						defer { free(resized.data); };

						blit_RGBA_to_RGBA(&resized, &canvas, badge_x, badge_y);
					}

					{
						// Badge name
						int scale = 4; // Render the text larger, then size it down so it looks nicer.
						Text_Dim dim = get_text_dimensions(&badge_font, design.badge_name_font_size*scale, (const u8*)badge.value.display);
						Result<Image> name = make_image(dim.w, dim.h, 1, 0x00000000);
						if(is_error(name)) {
							return {500, mg_mprintf(R"({"result":"Error creating badge name canvas})")};
						}
						defer { free(name.value.data); };
						render_text_to_image(
								&badge_font,
								(const u8*)badge.value.display,
								design.badge_name_font_size*scale,
								&name.value,
								0,
								0,
								{.c=0xFFFFFFFF}
								);

						// Scale it back to the correct size;
						Image resized;
						resized.w = dim.w / scale;
						resized.h = dim.h / scale;
						resized.channels = 1;
						resized.data = stbir_resize_uint8_srgb((const unsigned char*)name.value.data, dim.w, dim.h, 0, NULL, resized.w, resized.h, 0, STBIR_1CHANNEL);
						if(resized.data == NULL) {
							return {500, mg_mprintf(R"({"result":"%s: out of memory})", __FUNCTION__)};
						}
						defer{ free(resized.data); };

						int name_x = badge_x + ((design.badge_size / 2) - (resized.w / 2));
						int name_y = badge_y + design.badge_size + (design.badge_name_height / 2) - (resized.h/2);

						blit_A8_to_RGBA(&resized, resized.w, {.c=0xFFFFFFFF}, &canvas, name_x, name_y);
					}

					badge_col += 1;
					if(badge_col == badge_columns_needed) {
						badge_col = 0;
						badge_x = badge_start_x;
						badge_y += design.badge_size + design.badge_name_height;
					} else {
						badge_x += design.badge_size + design.badge_padding;
					}
				} else {
					log(LOG_LEVEL_ERROR, "Failed to load file %s", local_file_path);
				}
			} else {
				// TODO: Download it!
				log(LOG_LEVEL_ERROR, "Missing badge image for {category=\"%s\", name=\"%s\", url=\"%s\"}", badge.value.category, badge.value.name, badge.value.url);
			}
		} else {
			log(LOG_LEVEL_ERROR, "badge {category=\"%s\", name=\"%s\"} not found", id.category, id.name);
		}
	}

	// Convert the canvas to an in-memory .png
	int png_size;
	unsigned char* png = stbi_write_png_to_mem((const unsigned char*)canvas.data, canvas.w * canvas.channels, canvas.w, canvas.h, canvas.channels, &png_size);
	if(png == NULL) {
		return {500, mg_mprintf(R"({"result":"Error writing canvas to in-memory png"})")};
	}
	defer{ STBIW_FREE(png); };

	auto upload = upload_img_to_imgur((const char*)png, png_size, g_config.imgur_client_secret);
	if(is_error(upload)) {
		return {500, mg_mprintf(R"({"result":"Error uploading to Imgur: %s"})", upload.errstr)};
	}
	defer{ free(upload.value.data); };

	mg_str result_json = {(const char*)upload.value.data, upload.value.size};
	char* url = mg_json_get_str(result_json, "$.data.link");
	if(url == NULL) {
		return {500, mg_mprintf(R"({"result":"JSON parse error"})")};
	}
	defer { free(url); }; // TODO: Check the pdf_to_png function isn't leaking this too

	auto db_result = database_upsert_badge_card(member_id, url);
	if(is_error(db_result)) {
		// NOTE: This is an error, but not treated as fatal.
		log(LOG_LEVEL_ERROR, db_result.errstr);
	}

	return {200, mg_mprintf(R"({"result":"%s"})", url)};
}

Database_Result<Database_No_Value> database_mark_badge_as_checked(const char* category, const char* name) {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	const char* query = "UPDATE badge_images SET checked=1 WHERE category=? AND name=?";
	MYSQL_STATEMENT();

	MYSQL_INPUT_INIT(2);
	MYSQL_INPUT_STR(category, strlen(category));
	MYSQL_INPUT_STR(name, strlen(name));
	MYSQL_INPUT_BIND_AND_EXECUTE();

	MYSQL_RETURN();
}

// TODO: This thread should use a paged arena and just free all pages at the end of the main loop.
static bool g_image_download_thread_should_close = false;
void badge_card_image_downloader_thread() {
	static const int MAX_BADGES_TO_CHECK = 20; // Limit the number of badges to check per iteration to keep memory usage down.
	while(!g_image_download_thread_should_close) {
		const auto badges = database_get_unchecked_badges(MAX_BADGES_TO_CHECK); // TODO: To lower memory usage, just get 10 at a time?
		if(!is_error(badges)) {
			// Iterate over the unchecked badges, download a local copy and resize to the desired size.
			for(const auto& badge : badges.value) {
				// Stop if the close signal has been sent
				if(g_image_download_thread_should_close) break;

				// Get the filename part from the URL
				const char* filename = NULL;
				for(size_t i = strlen(badge.url)-1; i > 0; --i) {
					if(badge.url[i] == '/') {
						filename = &badge.url[i] + 1;
						break;
					}
				}
				if(filename == NULL) {
					log(LOG_LEVEL_ERROR, "%s: Could not get filename part from url: %s", __FUNCTION__, badge.url);
					continue;
				}

				char local_file_path[FILENAME_MAX];
				snprintf(local_file_path, FILENAME_MAX, "gfx/badge_card/images/%s", filename);

				// Check if a local copy already exists. This will happen every time the Badge Images sheet is synced with the bot.
				if(access(local_file_path, F_OK) == 0) {
					database_mark_badge_as_checked(badge.category, badge.name);
				} else {
					auto buffer = download_file(badge.url);
					if(has_value(buffer)) {
						defer { free(buffer.value.data); };

						{
							// Save to storage
							FILE* out = fopen(local_file_path, "wb");
							if(out != NULL) {
								defer { fclose(out); };
								size_t wrote = fwrite(buffer.value.data, 1, buffer.value.size, out);
								if(wrote == buffer.value.size) {
									database_mark_badge_as_checked(badge.category, badge.name);
									log(LOG_LEVEL_DEBUG, "Badge %s downloaded", local_file_path);
								} else {
									log(LOG_LEVEL_ERROR, "%s: fwrite failed", __FUNCTION__);
								}
							} else {
								log(LOG_LEVEL_ERROR, "%s: fopen(%s) failed", __FUNCTION__, local_file_path);
							}
						}
						{
							// Generate a thumbnail too.
							int width, height, channels;
							uint8_t* img = stbi_load_from_memory(buffer.value.data, buffer.value.size, &width, &height, &channels, 4);
							if(img != NULL) {
								defer{ stbi_image_free(img); };

								uint8_t* resized = stbir_resize_uint8_srgb(img, width, height, 0, NULL, THUMBNAIL_SIZE, THUMBNAIL_SIZE, 0, STBIR_RGBA);
								if(resized == NULL) {
									log(LOG_LEVEL_ERROR, "%s: out of memory", __FUNCTION__);
								}
								defer{ free(resized); };

								char thumbnail_file_path[FILENAME_MAX];
								snprintf(thumbnail_file_path, FILENAME_MAX, "%s/static/badge_thumbnails/%s", HTTP_SERVER_DOC_ROOT, filename);

								stbi_write_png_compression_level = 9;
								if(stbi_write_png(thumbnail_file_path, THUMBNAIL_SIZE, THUMBNAIL_SIZE, 4, resized, THUMBNAIL_SIZE * 4) !=0) {
									log(LOG_LEVEL_DEBUG, "Thumbnail %s created", thumbnail_file_path);
								} else {
									log(LOG_LEVEL_ERROR, "Failed to save file: %s", thumbnail_file_path);
								}
							} else {
								log(LOG_LEVEL_ERROR, "stbi_load_from_memory_failed"); // TODO: Get stbi_error
							}
						}
					} else {
						log(LOG_LEVEL_ERROR, "Downloading %s failed", badge.url);
					}
				}
			}
		} else {
			log(LOG_LEVEL_ERROR, "database_get_unchecked_badges() failed");
		}

		sleep(1);
	}
}

Database_Result<int> database_get_badges_count() {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "SELECT COUNT(*) FROM badge_images";
	MYSQL_STATEMENT();

	MYSQL_EXECUTE();

	int result;

	MYSQL_OUTPUT_INIT(1);
	MYSQL_OUTPUT_I32(&result);
	MYSQL_OUTPUT_BIND_AND_STORE();

	MYSQL_FETCH_AND_RETURN_SINGLE_ROW();
}

Database_Result<int> database_get_checked_badges_count() {
	MYSQL_CONNECT(g_config.mysql_host, g_config.mysql_username, g_config.mysql_password, g_config.mysql_database, g_config.mysql_port);
	static const char* query = "SELECT COUNT(*) FROM badge_images WHERE checked=1";
	MYSQL_STATEMENT();

	MYSQL_EXECUTE();

	int result;

	MYSQL_OUTPUT_INIT(1);
	MYSQL_OUTPUT_I32(&result);
	MYSQL_OUTPUT_BIND_AND_STORE();

	MYSQL_FETCH_AND_RETURN_SINGLE_ROW();

}

// --- End of badge card stuff ---
http_response get_badge_image_status(const mg_str json) {
	auto total_badges = database_get_badges_count();
	if(is_error(total_badges)) {
		return {500, mg_mprintf(R"({"result":"database_get_badges_count failed"})")};
	}

	auto checked_badges = database_get_checked_badges_count();
	if(is_error(checked_badges)) {
		return {500, mg_mprintf(R"({"result":"database_get_checked_badges_count failed"})")};
	}

	return {200, mg_mprintf(R"({"total": %d, "have": %d})", total_badges.value, checked_badges.value)};
}

// Handles POST requests
static void *post_thread_function(void *param) {
	thread_data *p = (thread_data*) param;

	// Free all resources that were passed in param
	defer{ free((void*) p->content_type.ptr); };
	defer{ free((void*) p->api_key.ptr); };
	defer{ free((void*) p->uri.ptr); };
	defer{ free((void*) p->body.ptr); };
	defer{ free(p); };

#if 0
	MG_DEBUG(("Content-Type: %s\n", STR_OR_NULL(p->content_type.ptr)));
	MG_DEBUG(("API Key     : %s\n", STR_OR_NULL(p->api_key.ptr)));
	MG_DEBUG(("URI         : %s\n", STR_OR_NULL(p->uri.ptr)));
	MG_DEBUG(("Body        : %s\n", STR_OR_NULL(p->body.ptr)));
#endif

	http_response response;

	if(p->content_type.ptr == NULL || mg_strcmp(p->content_type, mg_str("application/json")) != 0) {
		response = {400, strdup(R"({"result":"JSON payload required"})")};
	} else
	if(p->api_key.ptr == NULL || mg_strcmp(p->api_key, mg_str(g_config.api_key)) != 0) {
		response = {401, strdup(R"({"result":"Invalid API key"})")};
	} else {
		if(mg_match(p->uri, mg_str("/api/v1/upload_badge_images"), NULL)) {
			response = upload_badge_images(p->body);
		} else
		if(mg_match(p->uri, mg_str("/api/v1/get_badge_image_status"), NULL)) {
			response = get_badge_image_status(p->body);
		} else
		if(mg_match(p->uri, mg_str("/api/v1/make_badge_card"), NULL)) {
			response = make_badge_card(p->body);
		} else
		if(mg_match(p->uri, mg_str("/api/v1/upload_stats"), NULL)) {
			response = parse_stats(p->body);
		} else
		if(mg_match(p->uri, mg_str("/api/v1/upload_leaderboard"), NULL)) {
			response = parse_leaderboards(p->body);
		} else
		if(mg_match(p->uri, mg_str("/api/v1/upload_commands"), NULL)) {
			response = parse_commands(p->body);
		} else
		if(mg_match(p->uri, mg_str("/api/v1/make_thumbnail"), NULL)) {
			response = make_thumbnail(p->body);
		} else
		if(mg_match(p->uri, mg_str("/api/v1/update_xmage_version"), NULL)) {
			response = parse_xmage_version(p->body);
		} else
		if(mg_match(p->uri, mg_str("/api/v1/pdf2png"), NULL)) {
			response = pdf_to_png(p->body);
		} else
		if(mg_match(p->uri, mg_str("/api/v1/role_command"), NULL)) {
			response = role_command(p->body);
		} else {
			response = {400, strdup(R"({"result":"Invalid API endpoint"})")};
		}
	}

	if (response.result != 200) {
		log(LOG_LEVEL_DEBUG, response.str);
	}

	mg_wakeup(p->mgr, p->conn_id, &response, sizeof(http_response));

	return NULL;
}

static void http_server_func(mg_connection *con, int event, void *event_data) {
	if (event == MG_EV_HTTP_MSG) {
		mg_http_message *message = (mg_http_message *) event_data;

		log(LOG_LEVEL_DEBUG, "%s: Method:%.*s URI:%.*s, Proto:%.*s Body:\"%.*s\"",
			__FUNCTION__,
			message->method.len, STR_OR_NULL(message->method.ptr),
			message->uri.len, STR_OR_NULL(message->uri.ptr),
			message->proto.len, STR_OR_NULL(message->proto.ptr),
			message->body.len, STR_OR_NULL(message->body.ptr)
		);

		if(mg_match(message->method, mg_str("GET"), NULL)) {
			// TODO: Serve from a thread too?
			mg_http_serve_opts opts;
			memset(&opts, 0, sizeof(mg_http_serve_opts));
			opts.root_dir = HTTP_SERVER_DOC_ROOT;
			opts.extra_headers = "Cache-Control: public, max-age: 31536000\r\n";
			mg_http_serve_dir(con, message, &opts);
		} else
		if(mg_match(message->method, mg_str("POST"), NULL)) {
			thread_data *data = (thread_data*) calloc(1, sizeof(*data)); // Freed in worker thread
			if(data != NULL) {
				for(int i = 0; i < MG_MAX_HTTP_HEADERS && message->headers[i].name.len > 0; ++i) {
					// Get the Content-Type and API_KEY from the HTTP headers.
					MG_DEBUG(("header[%d]->%.*s:%.*s", i, message->headers[i].name.len, message->headers[i].name.ptr, message->headers[i].value.len, message->headers[i].value.ptr));
					if(mg_strcmp(message->headers[i].name, mg_str("Content-Type")) == 0) {
						data->content_type = mg_strdup(message->headers[i].value);
					} else
					if(mg_strcmp(message->headers[i].name, mg_str("API_KEY")) == 0) {
						data->api_key = mg_strdup(message->headers[i].value);
					}
				}

				// Early out bad requests.
				if(data->content_type.ptr == NULL || data->api_key.ptr == NULL) {
					if(data->content_type.ptr != NULL) free((void*)data->content_type.ptr);
					if(data->api_key.ptr != NULL) free((void*)data->api_key.ptr);
					free(data);

					mg_http_reply(con, 500, NULL, "");
				} else {
					data->conn_id = con->id;
					data->mgr     = con->mgr;
					data->uri     = mg_strdup(message->uri);
					data->body    = mg_strdup(message->body);

					start_thread(post_thread_function, data);
				}
			} else {
				mg_http_reply(con, 500, NULL, "");
			}
		} else {
			mg_http_reply(con, 400, NULL, "");
		}
	} else
	if (event == MG_EV_WAKEUP) {
		// Back from the handler thread. Send the response.
		http_response* response = (http_response*) ((mg_str*)event_data)->ptr;
		mg_http_reply(con, response->result, "", "%s\n", response->str);

		free((void*)response->str);
	}
}

static mg_mgr g_mgr;
static std::thread downloader_thread; // TODO: mongoose already uses pthreads, so just use that.

static void http_server_start() {
	mg_log_set(MG_LL_INFO);
	mg_log_set_fn(log_write_char, NULL);
	mg_mgr_init(&g_mgr);
	char listen[64];
	snprintf(listen, 64, "%s:%d", g_config.bind_address, g_config.bind_port);
	mg_http_listen(&g_mgr, listen, http_server_func, NULL);
	mg_wakeup_init(&g_mgr);

	// Start the badge image downloader thread
	downloader_thread = std::thread{badge_card_image_downloader_thread};
}

static void http_server_poll() {
	mg_mgr_poll(&g_mgr, 1000);
}


static void http_server_end() {
	mg_mgr_free(&g_mgr);

	g_image_download_thread_should_close = true;
	downloader_thread.join();
}

#endif // HTTP_SERVER_H_INCLUDED

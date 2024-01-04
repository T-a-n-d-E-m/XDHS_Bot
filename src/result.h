#ifndef RESULT_HPP_INCLUDED
#define RESULT_HPP_INCLUDED

#include <string>

enum GLOBAL_ERROR {
	ERROR_NONE,

	ERROR_OUT_OF_MEMORY,

	ERROR_INVALID_FUNCTION_PARAMETER,

	// database
	ERROR_MYSQL_INIT_FAILED,
	ERROR_MYSQL_REAL_CONNECT_FAILED,
	ERROR_MYSQL_STMT_INIT_FAILED,
	ERROR_MYSQL_STMT_PREPARE_FAILED,
	ERROR_MYSQL_BIND_PARAM_FAILED,
	ERROR_MYSQL_STMT_EXECUTE_FAILED,
	ERROR_MYSQL_STMT_BIND_RESULT_FAILED,
	ERROR_MYSQL_STMT_STORE_RESULT_FAILED,
	ERROR_MYSQL_STMT_FETCH_FAILED,

	ERROR_DATABASE_TOO_MANY_RESULTS,
	ERROR_DATABASE_UNEXPECTED_ROW_COUNT,

	// parse_date_string
	ERROR_MALFORMED_DATE_STRING,
	ERROR_DATE_IS_IN_PAST,
	ERROR_INVALID_MONTH,
	ERROR_INVALID_DAY_28,
	ERROR_INVALID_DAY_29,
	ERROR_INVALID_DAY_30,
	ERROR_INVALID_DAY_31,

	// parse_draft_code
	ERROR_MALFORMED_DRAFT_CODE,
	ERROR_LEAGUE_NOT_FOUND,

	// parse_time_string
	ERROR_MALFORMED_START_TIME_STRING,
	ERROR_INVALID_HOUR,
	ERROR_INVALID_MINUTE,

	// render_banner
	ERROR_LOAD_FONT_FAILED,
	ERROR_LOAD_ART_FAILED,
	ERROR_INVALID_PACK_COUNT,
	ERROR_FAILED_TO_SAVE_BANNER,

	// download_file
	ERROR_CURL_INIT,
	ERROR_DOWNLOAD_FAILED,
};

static const char* to_cstring(const GLOBAL_ERROR e) {
	switch(e) {
		case ERROR_NONE: return "no error";

		case ERROR_OUT_OF_MEMORY: return "Internal EventBot error: The server is out of memory.";

		case ERROR_INVALID_FUNCTION_PARAMETER: return "Internal EventBot error: Invalid function parameter.";

		case ERROR_MYSQL_INIT_FAILED:              return "Internal EventBot Error: mysql_init() failed.";
		case ERROR_MYSQL_REAL_CONNECT_FAILED:      return "Internal EventBot Error: mysql_real_connect() failed: {}";
		case ERROR_MYSQL_STMT_INIT_FAILED:         return "Internal EventBot Error: mysql_stmt_init() failed: {}";
		case ERROR_MYSQL_STMT_PREPARE_FAILED:      return "Internal EventBot Error: mysql_stmt_prepare() failed: {}";
		case ERROR_MYSQL_BIND_PARAM_FAILED:        return "Internal EventBot Error: mysql_bind_param() failed: {}";
		case ERROR_MYSQL_STMT_EXECUTE_FAILED:      return "Internal EventBot Error: mysql_stmt_execute() failed: {}";
		case ERROR_MYSQL_STMT_BIND_RESULT_FAILED:  return "Internal EventBot Error: mysql_stmt_bind_result() failed: {}";
		case ERROR_MYSQL_STMT_STORE_RESULT_FAILED: return "Internal EventBot Error: mysql_stmt_store_result() failed: {}";
		case ERROR_MYSQL_STMT_FETCH_FAILED:        return "Internal EventBot Error: mysql_stmt_fetch() failed: {}";
		case ERROR_DATABASE_TOO_MANY_RESULTS:      return "Internal EventBot Error: Database query returned {} rows, but 0 or 1 was expected.";
		case ERROR_DATABASE_UNEXPECTED_ROW_COUNT:  return "Internal EventBot Error: Database query returned unexpected row count of {} rows.";

		case ERROR_MALFORMED_DATE_STRING: return "Malformed date string. The date should be written as YYYY-MM-DD."; 
		case ERROR_DATE_IS_IN_PAST:       return "The date is in the past and time travel does not yet exist.";
		case ERROR_INVALID_MONTH:         return "Month should be between 01 and 12.";
		case ERROR_INVALID_DAY_28:        return "Day should be between 01 and 28 for the specified month.";
		case ERROR_INVALID_DAY_29:        return "Day should be between 01 and 29 for the specified month.";
		case ERROR_INVALID_DAY_30:        return "Day should be between 01 and 30 for the specified month.";
		case ERROR_INVALID_DAY_31:        return "Day should be between 01 and 31 for the specified month.";

		case ERROR_MALFORMED_DRAFT_CODE: return "**Malformed draft code.** Draft codes should look like SS.W-RT, where:\n\t**SS** is the season\n\t**W** is the week in the season\n\t**R** is the region code: (E)uro, (A)mericas, (P)acific, A(S)ia or A(T)lantic\n\t**T** is the league type: (C)hrono or (B)onus.";
		case ERROR_LEAGUE_NOT_FOUND:     return "No matching league found for draft code.";

		case ERROR_MALFORMED_START_TIME_STRING: return "Malformed start time string. Start time should be written as HH:MM in 24 hour time.";
		case ERROR_INVALID_HOUR:                return "Hour should be between 0 and 23.";
		case ERROR_INVALID_MINUTE:              return "Minute should be between 1 and 59.";

		case ERROR_LOAD_FONT_FAILED:      return "Internal EventBot error: stbtt_InitFont() failed to load banner font.";
		case ERROR_LOAD_ART_FAILED:       return "Internal EventBot error: stbi_load() failed to load art file. file: '{}' reason: {}";
		case ERROR_INVALID_PACK_COUNT:    return "Internal EventBot error: Unexpected background image count of {}.";
		case ERROR_FAILED_TO_SAVE_BANNER: return "Internal EventBot error: Failed to save generated banner to storage. If this is the first instance of seeing this error, please try again.";

		case ERROR_CURL_INIT:       return "Internal EventBot error: curl_easy_init() failed.";
		case ERROR_DOWNLOAD_FAILED: return "Internal EventBot error: Downloading file '{}' failed: {}";
	}

	return "unknown error";
}

template<typename Value_Type, typename Error_String_Type = std::string>
struct Result {
	GLOBAL_ERROR error;
	union {
		Value_Type value;
		Error_String_Type errstr;
	};

	Result() {}

	Result(const Value_Type& val)
		: error(ERROR_NONE)
		, value(val)
	{}

	Result& operator=(const Result& other) {
		error = other.error;
		if(error == ERROR_NONE) {
			value = other.value;
		} else {
			errstr = other.errstr;
		}
		return *this;
	}

	Result(GLOBAL_ERROR err, const Error_String_Type& str)
		: error(err)
		, errstr(str)
	{}

	Result(const Result& other) {
		error = other.error;
		if(error == ERROR_NONE) {
			value = other.value;
		} else {
			errstr = other.errstr;
		}
	}

	// Both Value_Type and Error_String_Type have destructors.
	~Result() requires (std::is_destructible<Value_Type>::value == true) && (std::is_destructible<Error_String_Type>::value == true) {
		if(error == ERROR_NONE) {
			this->value.~Value_Type();
		} else {
			this->errstr.~Error_String_Type();
		}
	}

	// Value_Type has no destructor, Error_String_Type has a destructor.
	~Result() requires (std::is_destructible<Value_Type>::value == false) && (std::is_destructible<Error_String_Type>::value == true) {
		if(error != ERROR_NONE) {
			this->errstr.~Error_String_Type();
		}
	}

	// Value_Type has a destructor, Error_String_Type has no destructor.
	~Result() requires (std::is_destructible<Value_Type>::value == true) && (std::is_destructible<Error_String_Type>::value == false) {
		if(error == ERROR_NONE) {
			this->value.~Value_Type();
		}
	}

	// Neither Value_Type or Error_String_Type have a destructor.
	~Result() requires (std::is_destructible<Value_Type>::value == false) && (std::is_destructible<Error_String_Type>::value == false) = delete;
};

template<typename T>
static inline bool has_value(const Result<T>& result) {
	return result.error == ERROR_NONE;
}

template<typename T>
static inline bool is_error(const Result<T>& result) {
	return result.error != ERROR_NONE;
}

#define MAKE_ERROR_RESULT(code, ...) {.error=code, .errstr={fmt::format(to_cstring(code) __VA_OPT__(,) __VA_ARGS__)}}


#endif // RESULT_HPP_INCLUDED

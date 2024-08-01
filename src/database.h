#ifndef DATABASE_H_INCLUDED
#define DATABASE_H_INCLUDED

#include "log.h"
#include "result.h"
#include "scope_exit.h"

#include <cinttypes>

#include <mysql.h>
#include <fmt/format.h>

template<typename Value_Type, typename Error_String_Type = std::string>
struct Database_Result : Result<Value_Type, Error_String_Type> {
	std::uint64_t count;

	Database_Result(const Value_Type& value, std::uint64_t count)
		: Result<Value_Type, Error_String_Type>(value)
		, count(count)
	{}

	Database_Result(GLOBAL_ERROR error, const Error_String_Type& value, std::uint64_t count)
		: Result<Value_Type, Error_String_Type>(error, value)
		, count(count)
	{}
};

#define MAKE_DATABASE_VALUE_RESULT(result, cnt) {.error=(ERROR_NONE), .value={result}, .count=(cnt)}
#define MAKE_DATABASE_ERROR_RESULT(code, cnt, ...) {.error=(code), .errstr={fmt::format(global_error_to_string(code) __VA_OPT__(,) __VA_ARGS__)}, .count=(cnt)}

// For database_ functions that return no data. We could use template specialization here but this is simpler.
struct Database_No_Value {};

#define MYSQL_CONNECT(host, username, password, database, port)                     \
	MYSQL* mysql = mysql_init(NULL);                                                \
	if(mysql == NULL) {                                                             \
		log(LOG_LEVEL_ERROR, "mysql_init(NULL) failed");                            \
		return MAKE_DATABASE_ERROR_RESULT(ERROR_MYSQL_INIT_FAILED, 0);              \
	}                                                                               \
	SCOPE_EXIT(mysql_close(mysql));                                                 \
	MYSQL* connection = mysql_real_connect(mysql,                                   \
		host,                                                                       \
		username,                                                                   \
		password,                                                                   \
		database,                                                                   \
		port,                                                                       \
		NULL, 0);                                                                   \
	if(connection == NULL) {                                                        \
		log(LOG_LEVEL_ERROR, "mysql_real_connect() failed");                        \
		return MAKE_DATABASE_ERROR_RESULT(ERROR_MYSQL_REAL_CONNECT_FAILED, 0, mysql_error(mysql)); \
	}

// Prepare a MySQL statement using the provided query.
#define MYSQL_STATEMENT()                                                                              \
	MYSQL_STMT* stmt = mysql_stmt_init(connection);                                                    \
	if(stmt == NULL) {                                                                                 \
		log(LOG_LEVEL_ERROR, "mysql_stmt_init(connection) failed: %s", mysql_stmt_error(stmt));        \
		return MAKE_DATABASE_ERROR_RESULT(ERROR_MYSQL_STMT_INIT_FAILED, 0, mysql_stmt_error(stmt));    \
	}                                                                                                  \
	SCOPE_EXIT(mysql_stmt_close(stmt));                                                                \
	if(mysql_stmt_prepare(stmt, query, strlen(query)) != 0) {                                          \
		log(LOG_LEVEL_ERROR, "mysql_stmt_prepare() failed: %s", mysql_stmt_error(stmt));               \
		return MAKE_DATABASE_ERROR_RESULT(ERROR_MYSQL_STMT_PREPARE_FAILED, 0, mysql_stmt_error(stmt)); \
	}

/*
// Create and prepare the input array used to bind variables to the query string.
#define MYSQL_INPUT_INIT(count)      \
	MYSQL_BIND input[(count)];       \
	memset(input, 0, sizeof(input));

// Add an item to the input array. One of these is needed for each input variable.
#define MYSQL_INPUT(index, type, pointer, size) \
	input[(index)].buffer_type = (type);        \
	input[(index)].buffer = (void*) (pointer);  \
	input[(index)].buffer_length = (size);
*/

// Create and prepare the input array used to bind variables to the query string.
#define MYSQL_INPUT_INIT(count)    \
	MYSQL_BIND input[(count)];       \
	memset(input, 0, sizeof(input)); \
	size_t input_index = 0;

// Add an item to the input array. One of these is needed for each input variable.
#define MYSQL_INPUT_STR(pointer, size)                  \
	input[input_index].buffer_type = MYSQL_TYPE_STRING; \
	input[input_index].buffer = (void*) (pointer);      \
	input[input_index].buffer_length = (size);          \
	input_index++;

// Add an item to the input array. One of these is needed for each input variable.
#define MYSQL_INPUT_BIN(pointer, size)                \
	input[input_index].buffer_type = MYSQL_TYPE_BLOB; \
	input[input_index].buffer = (void*) (pointer);    \
	input[input_index].buffer_length = (size);        \
	input_index++;

// Add an item to the input array. One of these is needed for each input variable.
#define MYSQL_INPUT_I8(pointer)                        \
	input[input_index].buffer_type = MYSQL_TYPE_TINY;  \
	input[input_index].buffer = (void*) (pointer);     \
	input[input_index].buffer_length = sizeof(int8_t); \
	input_index++;

// Add an item to the input array. One of these is needed for each input variable.
#define MYSQL_INPUT_I16(pointer)                        \
	input[input_index].buffer_type = MYSQL_TYPE_SHORT;  \
	input[input_index].buffer = (void*) (pointer);      \
	input[input_index].buffer_length = sizeof(int16_t); \
	input_index++;

// Add an item to the input array. One of these is needed for each input variable.
#define MYSQL_INPUT_I32(pointer)                        \
	input[input_index].buffer_type = MYSQL_TYPE_LONG;   \
	input[input_index].buffer = (void*) (pointer);      \
	input[input_index].buffer_length = sizeof(int32_t); \
	input_index++;

// Add an item to the input array. One of these is needed for each input variable.
#define MYSQL_INPUT_I64(pointer)                          \
	input[input_index].buffer_type = MYSQL_TYPE_LONGLONG; \
	input[input_index].buffer = (void*) (pointer);        \
	input[input_index].buffer_length = sizeof(int64_t);   \
	input_index++;

// Add an item to the input array. One of these is needed for each input variable.
#define MYSQL_INPUT_F32(pointer)                       \
	input[input_index].buffer_type = MYSQL_TYPE_FLOAT; \
	input[input_index].buffer = (void*) (pointer);     \
	input[input_index].buffer_length = sizeof(float);  \
	input_index++;

// Add an item to the input array. One of these is needed for each input variable.
#define MYSQL_INPUT_F64(pointer)                        \
	input[input_index].buffer_type = MYSQL_TYPE_DOUBLE; \
	input[input_index].buffer = (void*) (pointer);      \
	input[input_index].buffer_length = sizeof(double);  \
	input_index++;

// Once all input variables have been declared, bind them to the query and execute the statement.
#define MYSQL_INPUT_BIND_AND_EXECUTE()                                                                     \
	if(mysql_stmt_bind_param(stmt, input) != 0) {                                                          \
		log(LOG_LEVEL_ERROR, "mysql_stmt_bind_param() failed: %s", mysql_stmt_error(stmt));                \
		return MAKE_DATABASE_ERROR_RESULT(ERROR_MYSQL_BIND_PARAM_FAILED, 0, mysql_stmt_error(stmt));       \
	}                                                                                                      \
	if(mysql_stmt_execute(stmt) != 0) {                                                                    \
		log(LOG_LEVEL_ERROR, "%s: mysql_stmt_execute() failed: %s", __FUNCTION__, mysql_stmt_error(stmt)); \
		return MAKE_DATABASE_ERROR_RESULT(ERROR_MYSQL_STMT_EXECUTE_FAILED, 0, mysql_stmt_error(stmt));     \
	}

// A query with no input values.
#define MYSQL_EXECUTE()                                                                                    \
	if(mysql_stmt_execute(stmt) != 0) {                                                                    \
		log(LOG_LEVEL_ERROR, "%s: mysql_stmt_execute() failed: %s", __FUNCTION__, mysql_stmt_error(stmt)); \
		return MAKE_DATABASE_ERROR_RESULT(ERROR_MYSQL_STMT_EXECUTE_FAILED, 0, mysql_stmt_error(stmt));     \
	}

/*
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
*/

// Prepare an array to hold the output from a query.
#define MYSQL_OUTPUT_INIT(count)     \
	MYSQL_BIND output[(count)];        \
	memset(output, 0, sizeof(output)); \
	unsigned long length[(count)];     \
	my_bool is_null[(count)];          \
	my_bool is_error[(count)];         \
	size_t output_index = 0;

// Set up an item in the output array.
#define MYSQL_OUTPUT_STR(pointer, size)                    \
	output[output_index].buffer_type = MYSQL_TYPE_STRING;  \
	output[output_index].buffer = (void*) (pointer);       \
	output[output_index].buffer_length = (size);           \
	output[output_index].is_null = &is_null[output_index]; \
	output[output_index].length = &length[output_index];   \
	output[output_index].error = &is_error[output_index];  \
	output_index++;

// Set up an item in the output array.
#define MYSQL_OUTPUT_I8(pointer)                           \
	output[output_index].buffer_type = MYSQL_TYPE_TINY;    \
	output[output_index].buffer = (void*) (pointer);       \
	output[output_index].buffer_length = sizeof(int8_t);   \
	output[output_index].is_null = &is_null[output_index]; \
	output[output_index].length = &length[output_index];   \
	output[output_index].error = &is_error[output_index];  \
	output_index++;

// Set up an item in the output array.
#define MYSQL_OUTPUT_I16(pointer)                          \
	output[output_index].buffer_type = MYSQL_TYPE_SHORT;   \
	output[output_index].buffer = (void*) (pointer);       \
	output[output_index].buffer_length = sizeof(int16_t);  \
	output[output_index].is_null = &is_null[output_index]; \
	output[output_index].length = &length[output_index];   \
	output[output_index].error = &is_error[output_index];  \
	output_index++;

// Set up an item in the output array.
#define MYSQL_OUTPUT_I32(pointer)                          \
	output[output_index].buffer_type = MYSQL_TYPE_LONG;    \
	output[output_index].buffer = (void*) (pointer);       \
	output[output_index].buffer_length = sizeof(int32_t);  \
	output[output_index].is_null = &is_null[output_index]; \
	output[output_index].length = &length[output_index];   \
	output[output_index].error = &is_error[output_index];  \
	output_index++;

// Set up an item in the output array.
#define MYSQL_OUTPUT_I64(pointer)                          \
	output[output_index].buffer_type = MYSQL_TYPE_LONGLONG;    \
	output[output_index].buffer = (void*) (pointer);       \
	output[output_index].buffer_length = sizeof(int64_t);  \
	output[output_index].is_null = &is_null[output_index]; \
	output[output_index].length = &length[output_index];   \
	output[output_index].error = &is_error[output_index];  \
	output_index++;

// Set up an item in the output array.
#define MYSQL_OUTPUT_F32(pointer)                          \
	output[output_index].buffer_type = MYSQL_TYPE_FLOAT;    \
	output[output_index].buffer = (void*) (pointer);       \
	output[output_index].buffer_length = sizeof(float);  \
	output[output_index].is_null = &is_null[output_index]; \
	output[output_index].length = &length[output_index];   \
	output[output_index].error = &is_error[output_index];  \
	output_index++;

// Bind the output array to the statement.
#define MYSQL_OUTPUT_BIND_AND_STORE()                                                                       \
    if(mysql_stmt_bind_result(stmt, output) != 0) {                                                         \
		log(LOG_LEVEL_ERROR, "mysql_stmt_bind_result() failed: %s", mysql_stmt_error(stmt));                \
		return MAKE_DATABASE_ERROR_RESULT(ERROR_MYSQL_STMT_BIND_RESULT_FAILED, 0, mysql_stmt_error(stmt));  \
    }                                                                                                       \
    if(mysql_stmt_store_result(stmt) != 0) {                                                                \
		log(LOG_LEVEL_ERROR, "mysql_stmt_store_result: %s", mysql_stmt_error(stmt));                        \
		return MAKE_DATABASE_ERROR_RESULT(ERROR_MYSQL_STMT_STORE_RESULT_FAILED, 0, mysql_stmt_error(stmt)); \
    }

// Return for queries that don't return any rows
#define MYSQL_RETURN() \
	return {{}, 0};

// Return for queries that are expected to fetch zero or one rows
#define MYSQL_FETCH_AND_RETURN_ZERO_OR_ONE_ROWS()                                                                \
	u64 row_count = 0;                                                                                           \
	while(true) {                                                                                                \
		int status = mysql_stmt_fetch(stmt);                                                                     \
		if(status == 1) {                                                                                        \
			log(LOG_LEVEL_ERROR, "mysql_stmt_fetch() failed: %s", mysql_stmt_error(stmt));                       \
			return MAKE_DATABASE_ERROR_RESULT(ERROR_MYSQL_STMT_FETCH_FAILED, row_count, mysql_stmt_error(stmt)); \
		}                                                                                                        \
		if(status == MYSQL_NO_DATA) break;                                                                       \
		++row_count;                                                                                             \
	}                                                                                                            \
	if(row_count > 1) {                                                                                          \
		log(LOG_LEVEL_ERROR, "Database query returned %lu rows but 0 or 1 was expected.", row_count);            \
		return MAKE_DATABASE_ERROR_RESULT(ERROR_DATABASE_TOO_MANY_RESULTS, row_count, row_count);                \
	}                                                                                                            \
	return {{result}, row_count};

// Return for queries that are expected to fetch and return a single row of data.
#define MYSQL_FETCH_AND_RETURN_SINGLE_ROW()                                                                      \
	u64 row_count = 0;                                                                                           \
	while(true) {                                                                                                \
		int status = mysql_stmt_fetch(stmt);                                                                     \
		if(status == 1) {                                                                                        \
			log(LOG_LEVEL_ERROR, "mysql_stmt_fetch() failed: %s", mysql_stmt_error(stmt));                       \
			return MAKE_DATABASE_ERROR_RESULT(ERROR_MYSQL_STMT_FETCH_FAILED, row_count, mysql_stmt_error(stmt)); \
		}                                                                                                        \
		if(status == MYSQL_NO_DATA) break;                                                                       \
		++row_count;                                                                                             \
	}                                                                                                            \
	if(row_count != 1) {                                                                                         \
		log(LOG_LEVEL_ERROR, "Database query returned %lu rows but 1 was expected.", row_count);                 \
		return MAKE_DATABASE_ERROR_RESULT(ERROR_DATABASE_UNEXPECTED_ROW_COUNT, row_count, row_count);            \
	}                                                                                                            \
	return {{result}, 1};

// Return for queries that are expected to fetch and return 0 or more rows of data.
#define MYSQL_FETCH_AND_RETURN_MULTIPLE_ROWS()                                                                   \
	u64 row_count = 0;                                                                                           \
	while(true) {                                                                                                \
		int status = mysql_stmt_fetch(stmt);                                                                     \
		if(status == 1) {                                                                                        \
			log(LOG_LEVEL_ERROR, "mysql_stmt_fetch() failed: %s", mysql_stmt_error(stmt));                       \
			return MAKE_DATABASE_ERROR_RESULT(ERROR_MYSQL_STMT_FETCH_FAILED, row_count, mysql_stmt_error(stmt)); \
		}                                                                                                        \
		if(status == MYSQL_NO_DATA) break;                                                                       \
		results.push_back(result);                                                                               \
		++row_count;                                                                                             \
	}                                                                                                            \
	return {results, row_count};


#endif // DATABASE_H_INCLUDED

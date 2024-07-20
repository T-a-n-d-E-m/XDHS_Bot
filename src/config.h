#ifndef CONFIG_H_INCLUDED
#define CONFIG_H_INCLUDED

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>

#define CONFIG_KEY_STR(name) if(strcmp(key, (#name)) == 0) { g_config.name = strndup(value, value_len); }
#define CONFIG_KEY_U16(name) if(strcmp(key, (#name)) == 0) { g_config.name = (unsigned short) strtoul(value, NULL, 0); }

typedef void (*config_key_value_callback)(const char*, const char*, size_t len);

// Returns 1 on success, 0 otherwise.
static int load_config_file(const char* path, config_key_value_callback callback) {
	static const size_t KEY_LENGTH_MAX = 64;
	static const size_t VAL_LENGTH_MAX = 256;
	FILE* f = fopen(path, "r");
	if(f == NULL) {
		fprintf(stderr, "Error loading config file: %s\n", strerror(errno));
		return 0;
	}

	char* line = NULL;
	size_t length = 0;
	ssize_t read = 0;
	while((read = getline(&line, &length, f)) != -1) {
		if(line[0] == '#') {
			continue;
		}

		char* read_ptr = line;

		char key[KEY_LENGTH_MAX];
		{
			char* write_ptr = key;
			// Skip any leading whitespace
			while(*read_ptr == ' ' || *read_ptr == '\t') {
				read_ptr++;
			}
			// Read until whitespace or = is hit
			while(*read_ptr != '=' && *read_ptr != ' ' && *read_ptr != '\t') {
				*write_ptr++ = *read_ptr++;
			}
			*write_ptr = '\0';
		}

		char value[VAL_LENGTH_MAX];
		{
			char* write_ptr = value;
			// Skip any joing whitespace or =
			while(*read_ptr == '=' || *read_ptr == ' ' || *read_ptr == '\t') {
				read_ptr++;
			}
			while(*read_ptr != '\0' && *read_ptr != ' ' && *read_ptr != '\t' && *read_ptr != '\n') {
				*write_ptr++ = *read_ptr++;
			}
			*(write_ptr) = '\0';
		}

		callback(key, value, strlen(value));
	}

	fclose(f);
	return 1;
}

#endif // CONFIG_H_INCLUDE

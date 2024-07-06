#ifndef CONFIG_H_INCLUDED
#define CONFIG_H_INCLUDED

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <math.h>

static const char* CONFIG_FILE_PATH = "bot.ini";

struct config {
    char* mysql_host;
    char* mysql_username;
    char* mysql_password;
    unsigned short   mysql_port;
#if defined(BADGEBOT_BOT) || defined(EVENTBOT)
    char* logfile_path;
    char* discord_token;
#endif // BADGEBOT_BOT || EVENTBOT
#if defined(EVENTBOT)
	char* xmage_server;
	char* eventbot_host;
    char* api_key;
    char* imgur_client_secret;
#endif

	// There's no real need to ever free this structure as the OS will clean it up for us on program exit, but
	// leak testing with Valgrind is easier if we free it ourselves.
	~config() {
		if(mysql_host != NULL)     free(mysql_host);
		if(mysql_username != NULL) free(mysql_username);
		if(mysql_password != NULL) free(mysql_password);
		if(logfile_path != NULL)   free(logfile_path);
		if(discord_token != NULL)  free(discord_token);
		if(imgur_client_secret != NULL) free(imgur_client_secret);
		if(xmage_server != NULL)   free(xmage_server);
		if(eventbot_host != NULL)  free(eventbot_host);
        if(api_key != NULL)        free(api_key);
	}
} g_config;


static void config_file_kv_pair_callback(const char* key, const char* value) {
    const size_t value_len = strlen(value);

    if(strcmp(key, "mysql_host") == 0) {
        g_config.mysql_host = strndup(value, value_len);
    } else
    if(strcmp(key, "mysql_username") == 0) {
        g_config.mysql_username = strndup(value, value_len);
    } else
    if(strcmp(key, "mysql_password") == 0) {
        g_config.mysql_password = strndup(value, value_len);
    } else
    if(strcmp(key, "mysql_port") == 0) {
        g_config.mysql_port = (unsigned short) atoi(value);
	}
#if defined(BADGEBOT_BOT) || defined(EVENTBOT)
    else
    if(strcmp(key, "logfile_path") == 0) {
        g_config.logfile_path = strndup(value, value_len);
    } else
    if(strcmp(key, "discord_token") == 0) {
        g_config.discord_token = strndup(value, value_len);
    }
#endif // BADGEBOT_BOT || EVENTBOT
#if defined(EVENTBOT)
	else
	if(strcmp(key, "xmage_server") == 0) {
		g_config.xmage_server = strndup(value, value_len);
	} else
	if(strcmp(key, "eventbot_host") == 0) {
		g_config.eventbot_host = strndup(value, value_len);
	} else
    if(strcmp(key, "api_key") == 0) {
		g_config.api_key = strndup(value, value_len);
    } else
    if(strcmp(key, "imgur_client_secret") == 0) {
        g_config.imgur_client_secret = strndup(value, value_len);
    }
#endif // EVENTBOT
}

////////////////////////////////////////////////////////////////////////////////
// Generic works-for-everything code below

typedef void (*config_key_value_callback)(const char*, const char*);

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

        callback(key, value);
    }

    fclose(f);
    return 1;
}

#endif // CONFIG_H_INCLUDE

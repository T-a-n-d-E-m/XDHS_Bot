#ifndef CURL_H_INCLUDED
#define CURL_H_INCLUDED

// NOTE: Call curl_global_init(CURL_GLOBAL_DEFAULT) before using download_file()
// NOTE: Call curl_global_cleanup() at program exit.

#include <cinttypes>

#include <curl/curl.h>

#include "result.h"
#include "scope_exit.h"
#include "config.h"
#include "log.h"

struct Heap_Buffer {
	size_t size;
    std::uint8_t* data;
};

static size_t curl_write_memory_callback(void* data, size_t size, size_t nmemb, void* usr_ptr) {
    size_t real_size = size * nmemb;
    Heap_Buffer* mem = (Heap_Buffer*)usr_ptr;

    // TODO: Keep track of how much we've allocated so far and error out if over some maximum amount to prevent denial of service attacks from giant files being sent.

    uint8_t* ptr = (uint8_t*) realloc(mem->data, mem->size + real_size);
    if(ptr == NULL) {
        if(mem->data != NULL) {
            free(mem->data);
        }
		//log(LOG_LEVEL_ERROR, "%s: out of memeory", __FUNCTION__); // TODO: Now what? On the potato server EventBot is on this could happen...
        return 0;
    }

    mem->data = ptr;
    memcpy(mem->data + mem->size, data, real_size);
    mem->size += real_size;

    return real_size;
}

static Result<Heap_Buffer> download_file(const char* url) {
    CURL* curl = curl_easy_init();
    if(curl == NULL) {
        return MAKE_ERROR_RESULT(ERROR_CURL_INIT);
    }
    SCOPE_EXIT(curl_easy_cleanup(curl));

    // Disable checking SSL certs
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    // Disable checking SSL certs are valid
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    //curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

	Heap_Buffer buffer = {0, NULL};

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_memory_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

    CURLcode result = curl_easy_perform(curl);
    if(result != CURLE_OK) {
        return MAKE_ERROR_RESULT(ERROR_DOWNLOAD_FAILED, url, curl_easy_strerror(result));
    }

    return {buffer};
}

// Returns URL of uploaded image in a zero terminated string on the heap.
static Result<Heap_Buffer> upload_img_to_imgur(const char* bytes, const size_t len) {
	static const char* IMGUR_API_URL = "https://api.imgur.com/3/image";

	CURL* curl = curl_easy_init();
	if(curl == NULL) {
		return MAKE_ERROR_RESULT(ERROR_CURL_INIT);
	}
	SCOPE_EXIT(curl_easy_cleanup(curl));

    // Disable checking SSL certs
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    // Disable checking SSL certs are valid
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    //curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
	curl_easy_setopt(curl, CURLOPT_URL, IMGUR_API_URL);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_DEFAULT_PROTOCOL, "https");

	char header_string[64];
	snprintf(header_string, 64, "Authorization: Client-ID %s", g_config.imgur_client_secret);

	curl_slist* headers = NULL;
	headers = curl_slist_append(headers, header_string);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	curl_mime* mime;
	curl_mimepart* part;
	mime = curl_mime_init(curl);
	SCOPE_EXIT(curl_mime_free(mime));

	part = curl_mime_addpart(mime);
	curl_mime_name(part, "image");
	curl_mime_data(part, bytes, len);

	part = curl_mime_addpart(mime);
	curl_mime_name(part, "type");
	curl_mime_data(part, "raw", CURL_ZERO_TERMINATED);

	//part = curl_mime_addpart(mime);
	//curl_mime_name(part, "title");
	//curl_mime_data(part, "Simple upload", CURL_ZERO_TERMINATED);

	//part = curl_mime_addpart(mime);
	//curl_mime_name(part, "description");
	//curl_mime_data(part, "This is a simple image upload in Imgur", CURL_ZERO_TERMINATED);

	Heap_Buffer buffer = {0, NULL};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_memory_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

	curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
	CURLcode result = curl_easy_perform(curl);
	if(result != CURLE_OK) {
        return MAKE_ERROR_RESULT(ERROR_UPLOAD_FAILED, curl_easy_strerror(result));
	}

	long http_code;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	if(http_code != 200) {
		std::string errmsg((char*)buffer.data, buffer.size);
		return MAKE_ERROR_RESULT(ERROR_UPLOAD_TO_IMGUR_FAILED, errmsg.c_str());
	}

	return {buffer};
}

#endif // CURL_H_INCLUDED

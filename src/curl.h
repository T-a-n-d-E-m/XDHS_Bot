#ifndef CURL_H_INCLUDED
#define CURL_H_INCLUDED

// NOTE: Call curl_global_init(CURL_GLOBAL_DEFAULT) before using download_file()
// NOTE: Call curl_global_cleanup() at program exit.

#include <cinttypes>

#include <curl/curl.h>

#include "result.h"

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

    curl_easy_cleanup(curl);

    return {buffer};
}


#endif // CURL_H_INCLUDED

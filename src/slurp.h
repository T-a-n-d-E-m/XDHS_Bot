#ifndef SLURP_H_INCLUDED
#define SLURP_H_INCLUDED

#include <stdio.h>
#include <inttypes.h>

#include "defer.h"

// NOTE: Don't forget to free the returned buffer!
static u8* file_slurp(const char* path, size_t* size) {
	u8* file_contents = NULL;
	FILE* f = fopen(path, "rb");
	if(f != NULL) {
		defer { fclose(f); };
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



#endif // SLURP_H_INCLUDED

#ifndef UTF8_HPP_INLUCDED
#define UTF8_HPP_INLUCDED

// https://github.com/JeffBezanson/cutef8/blob/master/utf8.c
#define isutf(c) (((c)&0xC0)!=0x80)

static const uint32_t offsetsFromUTF8[6] = {
	0x00000000UL, 0x00003080UL, 0x000E2080UL,
	0x03C82080UL, 0xFA082080UL, 0x82082080UL
};

static uint32_t u8_nextchar(const unsigned char* s, int* i) {
	u_int32_t ch = 0;
	int sz = 0;

	do {
		ch <<= 6;
		ch += (unsigned char)s[(*i)++];
		sz++;
	} while (s[*i] && !isutf(s[*i]));
	ch -= offsetsFromUTF8[sz-1];

	return ch;
}

#endif // UTF8_HPP_INLUCDED

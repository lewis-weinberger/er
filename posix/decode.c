#include "er.h"

char ch[5];

/* read a byte stream until a complete multibyte character is found */
int
decode(char first, int (*m1)(size_t*), int (*m2)(void), size_t *i, wchar_t *wc)
{
	int n, r;
	mbstate_t ps;

	for(n = 1, ch[0] = first; n < 5; n++){
		memset(&ps, 0, sizeof(ps));
		switch (mbrtowc(wc, ch, n, &ps)){
		case (size_t)-2:
			if((r = (m1 != NULL) ? m1(i) : m2()) == -1)
				return -1;
			ch[n] = r;
			break;
		case (size_t)-1:
			return -1;
		default:
			return first;
		}
	}
	return -1;
}

/* glob.c - Redis-style glob matching; see glob.h. */
#include "ds/glob.h"

int ddup_glob_match(const char *pat, size_t plen, const char *str,
                    size_t slen)
{
    while (plen > 0) {
        switch (pat[0]) {
        case '*':
            /* collapse consecutive stars; a trailing star matches all */
            while (plen > 0 && pat[0] == '*') {
                pat++;
                plen--;
            }
            if (plen == 0)
                return 1;
            /* try the rest of the pattern at every suffix */
            for (;;) {
                if (ddup_glob_match(pat, plen, str, slen))
                    return 1;
                if (slen == 0)
                    return 0;
                str++;
                slen--;
            }
        case '?':
            if (slen == 0)
                return 0;
            str++;
            slen--;
            pat++;
            plen--;
            break;
        case '[': {
            const char *cls = pat + 1;
            size_t clen = plen - 1;
            int not = 0;
            int match = 0;
            int closed = 0;
            if (clen > 0 && cls[0] == '^') {
                not = 1;
                cls++;
                clen--;
            }
            while (clen > 0) {
                if (cls[0] == '\\' && clen >= 2) {
                    cls++;
                    clen--;
                    if (slen > 0 && cls[0] == str[0])
                        match = 1;
                    cls++;
                    clen--;
                } else if (cls[0] == ']') {
                    cls++;
                    clen--;
                    closed = 1;
                    break;
                } else if (clen >= 3 && cls[1] == '-') {
                    /* range (a reversed range matches nothing, like Redis) */
                    if (slen > 0 &&
                        (unsigned char)str[0] >= (unsigned char)cls[0] &&
                        (unsigned char)str[0] <= (unsigned char)cls[2])
                        match = 1;
                    cls += 3;
                    clen -= 3;
                } else {
                    if (slen > 0 && cls[0] == str[0])
                        match = 1;
                    cls++;
                    clen--;
                }
            }
            if (!closed) {
                /* unterminated class: '[' is a literal */
                if (slen == 0 || str[0] != '[')
                    return 0;
                str++;
                slen--;
                pat++;
                plen--;
                break;
            }
            if (not)
                match = !match;
            if (!match)
                return 0;
            str++;
            slen--;
            pat = cls;
            plen = clen;
            break;
        }
        default:
            /* '\' escapes the next char; a trailing '\' stays literal */
            if (pat[0] == '\\' && plen >= 2) {
                pat++;
                plen--;
            }
            if (slen == 0 || pat[0] != str[0])
                return 0;
            str++;
            slen--;
            pat++;
            plen--;
            break;
        }
    }
    return slen == 0;
}

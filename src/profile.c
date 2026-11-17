/* Настройки из профиля игры: разбор текста options.set. Без состояния и без Windows. */
#include <string.h>
#include "profile.h"

/* значение ключа "{name ...}": указатель на первый символ после пробелов; NULL — ключа нет.
 * "{name_other" ключом name не считается. */
static const char *value(const char *text, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = text; p && (p = strstr(p, name)); p += n) {
        if (p == text || p[-1] != '{') continue;
        char c = p[n];
        if (c != ' ' && c != '\t' && c != '}') continue;
        p += n;
        while (*p == ' ' || *p == '\t') p++;
        return p;
    }
    return NULL;
}

int profile_hdr(const char *text)
{
    const char *p = text ? value(text, "hdr") : NULL;
    if (!p) return -1;
    return *p == '0' ? 0 : 1;   /* {hdr} или {hdr 1} — включён */
}

int profile_aa(const char *text)
{
    const char *p = text ? value(text, "antialiasing") : NULL;
    if (!p || *p == '}' || !*p) return -1;
    if (*p == '"') p++;
    if (!strncmp(p, "none", 4) || !strncmp(p, "off", 3)) return 0;
    if (*p >= '0' && *p <= '9') {
        int v = 0;
        while (*p >= '0' && *p <= '9' && v < 1000) v = v * 10 + (*p++ - '0');
        return v <= 1 ? 0 : v;
    }
    return 1;   /* nonmaskable (Supersampling) и всё незнакомое: считаем включённым */
}

int profile_mode(const char *text, unsigned *w, unsigned *h)
{
    const char *p = text ? value(text, "mode") : NULL;
    if (!p) return 0;
    if (*p == '"') p++;
    unsigned a = 0, b = 0;
    while (*p >= '0' && *p <= '9' && a < 100000) a = a * 10 + (unsigned)(*p++ - '0');
    if (*p != 'x' && *p != 'X') return 0;
    p++;
    while (*p >= '0' && *p <= '9' && b < 100000) b = b * 10 + (unsigned)(*p++ - '0');
    if (!a || !b) return 0;
    *w = a; *h = b;
    return 1;
}

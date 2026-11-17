/* Разбор настроек профиля игры (src/profile.c, чистые функции). Последняя строка PASS/FAIL. */
#include <stdio.h>
#include "profile.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    /* как в настоящем options.set */
    const char *real = "{options\n\t{video\n\t\t{mode \"3840x2160\"}\n\t\t{window_mode fullscreen}\n\t\t{hdr 0}\n"
                       "\t\t{antialiasing none}\n\t\t{fxaa 0}\n\t\t{alphaToCoverage}\n\t}\n}\n";
    CHECK(profile_hdr(real) == 0, "hdr 0 -> %d", profile_hdr(real));
    CHECK(profile_aa(real) == 0, "antialiasing none -> %d", profile_aa(real));

    CHECK(profile_hdr("{video {hdr}}") == 1 && profile_hdr("{video {hdr 1}}") == 1, "hdr on");
    CHECK(profile_hdr("{video {mode \"1x1\"}}") == -1, "no hdr line");
    CHECK(profile_hdr("{video {hdr_extra 0}}") == -1, "hdr_extra is another key");

    CHECK(profile_aa("{video {antialiasing 2x}}") == 2, "2x -> %d", profile_aa("{video {antialiasing 2x}}"));
    CHECK(profile_aa("{video {antialiasing 4x}}") == 4 && profile_aa("{video {antialiasing 8x}}") == 8 &&
          profile_aa("{video {antialiasing 16x}}") == 16, "4x, 8x, 16x");
    CHECK(profile_aa("{video {antialiasing\t\"4x\"}}") == 4, "quoted value, tab");
    CHECK(profile_aa("{video {antialiasing nonmaskable}}") == 1, "Supersampling counts as on");
    CHECK(profile_aa("{video {antialiasing something_new}}") == 1, "unknown word counts as on");
    CHECK(profile_aa("{video {hdr 0}}") == -1, "no antialiasing line");
    CHECK(profile_aa("{video {antialiasing}}") == -1, "empty value: unknown");
    /* соседний ключ с тем же началом не мешает, в любом порядке */
    CHECK(profile_aa("{video {antialiasing_quality 2} {antialiasing 4x}}") == 4, "quality before");
    CHECK(profile_aa("{video {antialiasing none} {antialiasing_quality 2}}") == 0, "quality after");
    CHECK(profile_aa("{video {antialiasing_quality 2}}") == -1, "only quality");
    CHECK(profile_aa("{video {antialiasing 0}}") == 0 && profile_aa("{video {antialiasing 1x}}") == 0, "0 and 1x are off");
    CHECK(profile_aa(NULL) == -1 && profile_hdr(NULL) == -1 && profile_aa("") == -1, "empty input");

    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}

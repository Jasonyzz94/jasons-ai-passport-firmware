// tests/test_icons.c —— 校验所有像素图标:行数、行宽、每个非透明符号都在调色板里。
// 行宽错误会让渲染越界读,只能在主机上提前拦住。
#include "icons.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    const pixel_sprite_t *sprite;
} entry_t;

#define E(x) { #x, &x }
static const entry_t k_all[] = {
    E(ICON_TOMATO), E(ICON_WEATHER), E(ICON_REMOTE), E(ICON_SOUND), E(ICON_AI),
    E(ICON_LINK), E(ICON_HOME), E(ICON_BOARD), E(ICON_WIFI), E(ICON_TIMER),
    E(ICON_DICE), E(ICON_GEAR),
    E(WX_SUN), E(WX_PARTLY), E(WX_CLOUD), E(WX_RAIN), E(WX_SNOW), E(WX_STORM),
    E(WX_FOG), E(WX_MOON),
    E(IC_BULB), E(IC_PLUG), E(IC_SCENE), E(IC_SENSOR), E(IC_MIC), E(IC_WAVE),
    E(IC_TUNER), E(IC_METRO), E(IC_SLIDES), E(IC_MUSIC), E(IC_CAMERA), E(IC_CHAT),
    E(IC_HEART), E(IC_COIN), E(IC_CLOUD_OFF), E(IC_CHECK), E(IC_BELL), E(IC_STAR),
};

static int check(const entry_t *e)
{
    const pixel_sprite_t *s = e->sprite;
    int failures = 0;
    for (int y = 0; y < s->height; y++) {
        const char *row = s->rows[y];
        if (!row || (int)strlen(row) != s->width) {
            printf("%s: row %d has width %d, expected %d\n", e->name, y,
                   row ? (int)strlen(row) : -1, s->width);
            failures++;
            continue;
        }
        for (int x = 0; x < s->width; x++) {
            const char k = row[x];
            if (k == '.') continue;
            int found = 0;
            for (const pixel_color_t *c = s->colors; c->key; c++) {
                if (c->key == k) found = 1;
            }
            if (!found) {
                printf("%s: row %d col %d uses '%c' missing from palette\n", e->name, y, x, k);
                failures++;
            }
        }
    }
    return failures;
}

int main(void)
{
    int failures = 0;
    for (size_t i = 0; i < sizeof(k_all) / sizeof(k_all[0]); i++) failures += check(&k_all[i]);
    if (failures) {
        printf("test_icons: FAIL (%d)\n", failures);
        return 1;
    }
    printf("test_icons: PASS (%zu sprites)\n", sizeof(k_all) / sizeof(k_all[0]));
    return 0;
}

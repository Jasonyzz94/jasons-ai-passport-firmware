// main/app_pet.c —— 像素宠物"团子":喂食、玩耍、洗澡,照顾它就会长大。
// 规则在 pet.c(有主机测试);这里只负责界面、存档和动画。目前独立运行,没有和其他功能联动。
#include "apps.h"
#include "icons.h"
#include "pet.h"
#include "svc.h"
#include "sys.h"
#include "ui.h"

#include <stdio.h>
#include <time.h>

#define C_PET 0xFF9F5A

// ---------------------------------------------------------------------------
// 像素画:四个成长阶段
// ---------------------------------------------------------------------------
static const pixel_color_t PAL_PET[] = {
    {'O', 0xFF9F5A}, {'o', 0xD9653B}, {'W', 0xFFFFFF}, {'K', 0x1F2430}, {'P', 0xFF7FA0},
    {'E', 0xFFF1D6}, {'e', 0xE8C99A}, {'S', 0xF59E0B}, {'G', 0x4ADE80}, {'g', 0x16A34A},
    {'Y', 0xFACC15}, {'R', 0xF87171}, {'h', 0x1E2531}, {0, 0},
};
static const char *R_EGG[16] = {
    "................", "......EEEE......", ".....EEEEEE.....", "....EWEEEEEE....",
    "....WEEEESEE....", "...EEEEEESSEE...", "...EEEEEEEEEE...", "...ESSEEEEEEE...",
    "...ESSEEEEEEE...", "...EEEEEEESEE...", "...EEEEEEEEEE...", "....EEEEEEEE....",
    "....eEEEEEEe....", ".....eeeeee.....", "................", "...hhhhhhhhhh...",
};
static const char *R_BABY[16] = {
    "................", "................", "................", "................",
    "......OOOO......", "....OOOOOOOO....", "...OOWOOOOOOO...", "..OOWOOOOOOOOO..",
    "..OOKKOOOOKKOO..", "..OOKWOOOOKWOO..", "..OPOOOooOOOPO..", "..OOOOOOOOOOOO..",
    "...oOOOOOOOOo...", "....oooooooo....", "................", "...hhhhhhhhhh...",
};
static const char *R_TEEN[16] = {
    ".......GG.......", "......GgG.......", ".......g........", ".....OOOOOO.....",
    "...OOOOOOOOOO...", "..OOWOOOOOOOOO..", ".OOWOOOOOOOOOOO.", ".OOOKKOOOOOKKOO.",
    ".OOOKWOOOOOKWOO.", ".OPPOOOOOOOOPPO.", ".OOOOOoooOOOOOO.", ".OOOOOOOOOOOOOO.",
    "..oOOOOOOOOOOo..", "...oOo....oOo...", "................", "..hhhhhhhhhhhh..",
};
static const char *R_ADULT[16] = {
    "....Y..Y..Y.....", "....YYYYYYY.....", "....YRYYYRY.....", "...OOOOOOOOOO...",
    "..OOOOOOOOOOOO..", ".OOWOOOOOOOOOOO.", "OOWOOOOOOOOOOOOO", "OOOOKKOOOOOKKOOO",
    "OOOOKWOOOOOKWOOO", "OOPPOOOOOOOOOPPO", "OOOOOOoOOOoOOOOO", "OOOOOOOoooOOOOOO",
    ".OOOOOOOOOOOOOO.", "..oOOOOOOOOOOo..", "...oOo....oOo...", "..hhhhhhhhhhhh..",
};
static const pixel_sprite_t SPR_PET[4] = {
    {16, 16, R_EGG, PAL_PET}, {16, 16, R_BABY, PAL_PET}, {16, 16, R_TEEN, PAL_PET}, {16, 16, R_ADULT, PAL_PET},
};

// ---------------------------------------------------------------------------
enum { ACT_FEED = 0, ACT_PLAY, ACT_WASH, ACT_COUNT };
static const char *const k_act_names[ACT_COUNT] = {"喂食", "玩耍", "洗澡"};

static pet_t s_pet;
static bool s_loaded;
static int s_sel;
static lv_obj_t *s_root, *s_name, *s_level, *s_sprite, *s_bubble, *s_bubble_text, *s_age;
static lv_obj_t *s_bars[3], *s_acts[ACT_COUNT];
static int64_t s_jump_until, s_bubble_until, s_last_sync;
static int s_sprite_y;

static bool now_minutes(int32_t *min, int *hour)
{
    struct tm tm;
    if (!time_local(&tm)) return false;   // 没对时就不结算,免得开机时间不准把它饿坏
    *min = (int32_t)(time(NULL) / 60);
    *hour = tm.tm_hour;
    return true;
}

static void save(void) { sys_kv_set_blob("pet", &s_pet, sizeof(s_pet)); }

static void load(void)
{
    if (s_loaded) return;
    int32_t min = 0;
    int hour;
    now_minutes(&min, &hour);
    if (!sys_kv_get_blob("pet", &s_pet, sizeof(s_pet)) || !pet_valid(&s_pet)) {
        pet_init(&s_pet, min);
        save();
    }
    s_loaded = true;
}

static void say(const char *text, int ms)
{
    lv_label_set_text(s_bubble_text, text);
    lv_obj_remove_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    s_bubble_until = ms ? sys_now_ms() + ms : 0;
}

static const char *status_line(int hour)
{
    if (pet_stage(&s_pet) == PET_EGG) return "蛋在轻轻晃…多照顾它";
    switch (pet_status(&s_pet, hour)) {
    case PET_HUNGRY: return "肚子咕咕叫…";
    case PET_DIRTY: return "身上脏脏的…";
    case PET_SAD: return "陪我玩一会儿吧";
    case PET_SLEEPY: return "Zz… 好困";
    case PET_HAPPY: return "今天超开心!";
    default: return "嗨~";
    }
}

static void render(void)
{
    const pet_stage_t st = pet_stage(&s_pet);
    ui_pix_set(s_sprite, &SPR_PET[st]);
    lv_label_set_text_fmt(s_name, "团子 · %s", pet_stage_name(st));
    lv_label_set_text_fmt(s_level, "Lv %d", pet_level(&s_pet));
    int32_t min = 0;
    int hour = 12;
    const bool timed = now_minutes(&min, &hour);
    const int next = pet_xp_to_next(&s_pet);
    char age[64];
    if (next > 0) snprintf(age, sizeof(age), "再照顾 %d 次左右就长大", (next + 1) / 2);
    else snprintf(age, sizeof(age), "已经长大啦 · 陪伴第 %d 天", timed ? pet_age_days(&s_pet, min) + 1 : 1);
    lv_label_set_text(s_age, age);
    ui_bar_set(s_bars[0], s_pet.hunger);
    ui_bar_set(s_bars[1], s_pet.mood);
    ui_bar_set(s_bars[2], s_pet.clean);
    for (int i = 0; i < ACT_COUNT; i++) {
        const bool on = i == s_sel;
        lv_obj_set_style_bg_color(s_acts[i], ui_c(on ? C_PET : C_SURFACE), 0);
        lv_obj_set_style_text_color(s_acts[i], ui_c(on ? C_BG : C_TEXT2), 0);
    }
    if (!s_bubble_until) say(status_line(hour), 0);
}

static void sync_time(void)
{
    int32_t min;
    int hour;
    if (now_minutes(&min, &hour)) pet_tick(&s_pet, min);
}

static void create(lv_obj_t *root)
{
    s_root = root;
    load();
    sync_time();
    s_jump_until = s_bubble_until = 0;
    s_last_sync = sys_now_ms();

    s_name = ui_label(root, &ui_font_20b, C_TEXT, "");
    lv_obj_set_pos(s_name, UI_PAD + 4, 4);
    s_level = ui_pill(root, "", C_BG, C_PET);
    lv_obj_align(s_level, LV_ALIGN_TOP_RIGHT, -UI_PAD - 4, 6);

    s_sprite_y = 66;
    s_sprite = ui_pix(root, &SPR_PET[0], 5);
    lv_obj_set_pos(s_sprite, (UI_W - 80) / 2, s_sprite_y);

    s_bubble = ui_card(root, 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT, C_SURFACE2, 12);
    lv_obj_set_style_pad_hor(s_bubble, 10, 0);
    lv_obj_set_style_pad_ver(s_bubble, 4, 0);
    s_bubble_text = ui_label(s_bubble, &ui_font_14, C_TEXT, "");
    lv_obj_align(s_bubble, LV_ALIGN_TOP_MID, 0, 36);

    s_age = ui_label_w(root, &ui_font_14, C_TEXT3, UI_W, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_set_pos(s_age, 0, 148);

    static const char *const names[3] = {"饱腹", "心情", "清洁"};
    static const uint32_t colors[3] = {0xFBBF24, 0xF472B6, 0x38BDF8};
    for (int i = 0; i < 3; i++) {
        const int y = 174 + i * 18;
        lv_obj_t *l = ui_label(root, &ui_font_14, C_TEXT2, names[i]);
        lv_obj_set_pos(l, UI_PAD + 8, y - 4);
        s_bars[i] = ui_bar(root, UI_PAD + 50, y + 2, UI_W - 2 * UI_PAD - 66, 8, colors[i]);
    }
    for (int i = 0; i < ACT_COUNT; i++) {
        s_acts[i] = ui_pill(root, k_act_names[i], C_TEXT2, C_SURFACE);
        lv_obj_set_style_pad_hor(s_acts[i], 14, 0);
        lv_obj_set_style_pad_ver(s_acts[i], 5, 0);
        lv_obj_align(s_acts[i], LV_ALIGN_TOP_MID, (i - 1) * 74, 228);
    }
    shell_hints("▲▼:选择|OK:照顾|长▲:返回");
    render();
}

static void destroy(void)
{
    if (s_loaded) save();
    s_root = NULL;
}

static void act(void)
{
    static const char *const ok_text[ACT_COUNT] = {"好吃!谢谢~", "哈哈,再来!", "香喷喷~"};
    const pet_stage_t before = pet_stage(&s_pet);
    pet_result_t r;
    switch (s_sel) {
    case ACT_FEED: r = pet_feed(&s_pet); break;
    case ACT_PLAY: r = pet_play(&s_pet); break;
    default: r = pet_wash(&s_pet); break;
    }
    const char *text = ok_text[s_sel];
    if (r == PET_FULL) text = "吃不下了…";
    else if (r == PET_TOO_HUNGRY) text = "饿得没力气玩…先喂我";
    else if (r == PET_ALREADY_CLEAN) text = "已经很干净啦";
    if (pet_stage(&s_pet) != before) {
        text = before == PET_EGG ? "破壳啦!你好呀~" : "我长大了!";
        audio_beep(BEEP_WORK_DONE);
    } else {
        audio_beep(r == PET_OK ? BEEP_OK : BEEP_ERR);
    }
    if (r == PET_OK) s_jump_until = sys_now_ms() + 600;
    save();
    say(text, 2500);
    render();
}

static bool key(sys_key_t k, sys_key_ev_t ev)
{
    if (ev != SYS_EV_CLICK) return false;
    if (k == SYS_KEY_OK) act();
    else {
        s_sel = (s_sel + (k == SYS_KEY_DOWN ? 1 : -1) + ACT_COUNT) % ACT_COUNT;
        render();
    }
    return true;
}

static void tick(int64_t now)
{
    // 待机时轻轻上下浮动;照顾成功时跳一下。
    int dy;
    if (now < s_jump_until) {
        const int t = (int)(600 - (s_jump_until - now));   // 0..600
        dy = -(t * (600 - t)) / 9000;                      // 抛物线,最高约 10px
    } else {
        dy = ((now / 500) % 2) ? -2 : 0;
    }
    lv_obj_set_y(s_sprite, s_sprite_y + dy);
    if (s_bubble_until && now > s_bubble_until) {
        s_bubble_until = 0;
        render();
    }
    if (now - s_last_sync > 60000) {   // 每分钟结算一次
        s_last_sync = now;
        sync_time();
        render();
    }
}

const app_t APP_PET = {
    .name = "像素宠物",
    .desc = "喂食 · 玩耍 · 长大",
    .icon = &SPR_PET[1],
    .color = C_PET,
    .create = create,
    .destroy = destroy,
    .key = key,
    .tick = tick,
};

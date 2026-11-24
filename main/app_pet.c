// main/app_pet.c —— 像素宠物:喂食、玩耍、洗澡,照顾它就会长大;4 个物种、闪亮形态、长老和代际传承。
// 规则在 pet.c(有主机测试),字符画在 pet_art.c;这里只负责界面、存档和动画。
// 番茄、冥想和语音助手完成时会通过 pet_give() 给它奖励。
#include "apps.h"
#include "icons.h"
#include "pet.h"
#include "pet_art.h"
#include "svc.h"
#include "sys.h"
#include "ui.h"

#include <stdio.h>
#include <time.h>

#define C_PET 0xFF9F5A

// ---------------------------------------------------------------------------
enum { ACT_FEED = 0, ACT_PLAY, ACT_WASH, ACT_COUNT };
static const char *const k_act_names[ACT_COUNT] = {"喂食", "玩耍", "洗澡"};

static pet_t s_pet;
static bool s_loaded;
static bool s_grew_away;   // 不在宠物页时(收到奖励)长大了:下次打开时庆祝
static int s_sel;
static bool s_dex_open;
static lv_obj_t *s_root, *s_main, *s_dex;
static lv_obj_t *s_name, *s_level, *s_sprite, *s_bubble, *s_bubble_text, *s_age;
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
    pet_v1_t old;
    if (sys_kv_get_blob("pet", &s_pet, sizeof(s_pet)) && pet_valid(&s_pet)) {
        // 当前版本
    } else if (sys_kv_get_blob("pet", &old, sizeof(old)) && pet_migrate_v1(&old, min, &s_pet)) {
        save();   // 第 1 版存档:还是原来那只团子,升级成新格式
    } else {
        pet_init(&s_pet, min, PET_SP_DANGO);   // 第一只是团子
        save();
    }
    s_loaded = true;
}

// 结算时间并检查进化。没对过时就只处理不需要等时间的孵化。返回 true 表示长大了。
static bool sync_time(void)
{
    int32_t min;
    int hour;
    if (now_minutes(&min, &hour)) {
        if (s_pet.last_min == 0) {   // 第一次拿到准确时间(之前一直没对时):从现在开始算
            s_pet.last_min = s_pet.born_min = s_pet.stage_min = min;
        }
        pet_tick(&s_pet, min);
        return pet_evolve(&s_pet, min);
    }
    return s_pet.stage == PET_EGG && pet_evolve(&s_pet, s_pet.stage_min);
}

void pet_give(pet_gift_t gift)
{
    load();
    pet_gift(&s_pet, gift);
    if (sync_time()) s_grew_away = true;
    save();
}

static const char *grew_text(pet_stage_t st)
{
    switch (st) {
    case PET_BABY: return "破壳啦!你好呀~";
    case PET_ADULT: return s_pet.form == PET_FORM_SHINY ? "我长大了,还闪闪发光!" : "我长大了!";
    case PET_ELDER: return "我变成长老啦";
    default: return "我长大了!";
    }
}

static void say(const char *text, int ms)
{
    lv_label_set_text(s_bubble_text, text);
    lv_obj_remove_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    s_bubble_until = ms ? sys_now_ms() + ms : 0;
}

static const char *status_line(int hour, int32_t min, bool timed)
{
    if (s_pet.stage == PET_EGG) return "蛋在轻轻晃…多照顾它";
    if (timed && pet_can_pass_on(&s_pet, min)) return "想去远方看看…";
    switch (pet_status(&s_pet, hour)) {
    case PET_HUNGRY: return "肚子咕咕叫…";
    case PET_DIRTY: return "身上脏脏的…";
    case PET_SAD: return "陪我玩一会儿吧";
    case PET_SLEEPY: return "Zz… 好困";
    case PET_HAPPY: return "今天超开心!";
    default: return "嗨~";
    }
}

static void fmt_wait(char *out, size_t cap, int32_t min_left)
{
    if (min_left >= 24 * 60) snprintf(out, cap, "%d 天", (int)((min_left + 24 * 60 - 1) / (24 * 60)));
    else if (min_left >= 60) snprintf(out, cap, "%d 小时", (int)((min_left + 59) / 60));
    else snprintf(out, cap, "%d 分钟", (int)min_left);
}

static void render(void)
{
    const pet_stage_t st = pet_stage(&s_pet);
    ui_pix_set(s_sprite, pet_art(s_pet.species, st, s_pet.form));
    const bool shiny = st >= PET_ADULT && s_pet.form == PET_FORM_SHINY;
    lv_label_set_text_fmt(s_name, "%s%s · %s", shiny ? "闪亮" : "", st == PET_EGG ? "神秘的蛋" : pet_species_name(s_pet.species),
                          pet_stage_name(st));
    lv_label_set_text_fmt(s_level, "Lv %d", pet_level(&s_pet));
    int32_t min = 0;
    int hour = 12;
    const bool timed = now_minutes(&min, &hour);
    char line[80], wait[16];
    int xp_left;
    int32_t min_left;
    if (pet_next_need(&s_pet, timed ? min : s_pet.stage_min, &xp_left, &min_left)) {
        fmt_wait(wait, sizeof(wait), min_left);
        if (xp_left > 0 && min_left > 0) snprintf(line, sizeof(line), "再照顾约 %d 次,%s后长大", (xp_left + 1) / 2, wait);
        else if (xp_left > 0) snprintf(line, sizeof(line), "再照顾约 %d 次就长大", (xp_left + 1) / 2);
        else if (!timed) snprintf(line, sizeof(line), "联网对时后才能继续长大");
        else snprintf(line, sizeof(line), "%s后长大", wait);
    } else if (timed && pet_can_pass_on(&s_pet, min)) {
        snprintf(line, sizeof(line), "长按OK:送它去旅行,留下新蛋");
    } else {
        snprintf(line, sizeof(line), "第 %d 代 · 陪伴第 %d 天", (int)s_pet.generation,
                 timed ? pet_age_days(&s_pet, min) + 1 : 1);
    }
    lv_label_set_text(s_age, line);
    ui_bar_set(s_bars[0], s_pet.hunger);
    ui_bar_set(s_bars[1], s_pet.mood);
    ui_bar_set(s_bars[2], s_pet.clean);
    for (int i = 0; i < ACT_COUNT; i++) {
        const bool on = i == s_sel;
        lv_obj_set_style_bg_color(s_acts[i], ui_c(on ? C_PET : C_SURFACE), 0);
        lv_obj_set_style_text_color(s_acts[i], ui_c(on ? C_BG : C_TEXT2), 0);
    }
    if (!s_bubble_until) say(status_line(hour, min, timed), 0);
}

// ---------------------------------------------------------------------------
// 图鉴:4 个物种 × 普通 / 闪亮,没见过的显示剪影
// ---------------------------------------------------------------------------
static void dex_close(void)
{
    if (s_dex) lv_obj_delete(s_dex);
    s_dex = NULL;
    s_dex_open = false;
    lv_obj_remove_flag(s_main, LV_OBJ_FLAG_HIDDEN);
    shell_hints("▲▼:选择|OK:照顾|长▼:图鉴");
}

static void dex_open(void)
{
    s_dex_open = true;
    lv_obj_add_flag(s_main, LV_OBJ_FLAG_HIDDEN);
    s_dex = ui_box(s_root, 0, 0, UI_W, UI_CONTENT_H);
    lv_obj_t *t = ui_label(s_dex, &ui_font_20b, C_TEXT, "图鉴");
    lv_obj_set_pos(t, UI_PAD + 4, 4);
    lv_obj_t *n = ui_pill(s_dex, "", C_BG, C_PET);
    lv_label_set_text_fmt(n, "%d / %d", pet_dex_count(s_pet.dex), PET_SPECIES_COUNT * PET_FORM_COUNT);
    lv_obj_align(n, LV_ALIGN_TOP_RIGHT, -UI_PAD - 4, 6);
    const int cell = (UI_W - 2 * UI_PAD) / PET_SPECIES_COUNT;   // 54
    for (int sp = 0; sp < PET_SPECIES_COUNT; sp++) {
        const int x = UI_PAD + sp * cell;
        for (int f = 0; f < PET_FORM_COUNT; f++) {
            const bool has = pet_dex_has(s_pet.dex, sp, f);
            const pixel_sprite_t *img = has ? pet_art(sp, PET_ADULT, f) : pet_art_silhouette(sp);
            lv_obj_t *frame = ui_card(s_dex, x + 2, 40 + f * 74, cell - 4, 66, f ? 0x2A2416 : C_SURFACE, 10);
            lv_obj_t *pix = ui_pix(frame, img, 3);
            lv_obj_align(pix, LV_ALIGN_CENTER, 0, 0);
        }
        lv_obj_t *nm = ui_label_w(s_dex, &ui_font_14, C_TEXT2, cell, LV_TEXT_ALIGN_CENTER, pet_species_name(sp));
        lv_obj_set_pos(nm, x, 192);
    }
    lv_obj_t *foot = ui_label_w(s_dex, &ui_font_14, C_TEXT3, UI_W, LV_TEXT_ALIGN_CENTER,
                                "上排普通 · 下排闪亮(照顾得好)");
    lv_obj_set_pos(foot, 0, 214);
    lv_obj_t *gen = ui_label_w(s_dex, &ui_font_14, C_TEXT3, UI_W, LV_TEXT_ALIGN_CENTER, "");
    lv_label_set_text_fmt(gen, "现在是第 %d 代", (int)s_pet.generation);
    lv_obj_set_pos(gen, 0, 234);
    shell_hints("任意键:返回");
}

// ---------------------------------------------------------------------------
static void pass_on_answer(bool yes)
{
    if (!yes || !s_root) return;
    int32_t min;
    int hour;
    if (!now_minutes(&min, &hour) || !pet_can_pass_on(&s_pet, min)) return;
    pet_pass_on(&s_pet, min, sys_random());
    save();
    audio_beep(BEEP_WORK_DONE);
    say("它留下了一颗新蛋,出发去旅行了", 3500);
    render();
}

static void create(lv_obj_t *root)
{
    s_root = root;
    s_dex = NULL;
    s_dex_open = false;
    load();
    const bool grew = sync_time() || s_grew_away;
    s_grew_away = false;
    s_jump_until = s_bubble_until = 0;
    s_last_sync = sys_now_ms();

    s_main = ui_box(root, 0, 0, UI_W, UI_CONTENT_H);
    s_name = ui_label(s_main, &ui_font_20b, C_TEXT, "");
    lv_obj_set_pos(s_name, UI_PAD + 4, 4);
    s_level = ui_pill(s_main, "", C_BG, C_PET);
    lv_obj_align(s_level, LV_ALIGN_TOP_RIGHT, -UI_PAD - 4, 6);

    s_sprite_y = 66;
    s_sprite = ui_pix(s_main, pet_art(s_pet.species, s_pet.stage, s_pet.form), 5);
    lv_obj_set_pos(s_sprite, (UI_W - 80) / 2, s_sprite_y);

    s_bubble = ui_card(s_main, 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT, C_SURFACE2, 12);
    lv_obj_set_style_pad_hor(s_bubble, 10, 0);
    lv_obj_set_style_pad_ver(s_bubble, 4, 0);
    s_bubble_text = ui_label(s_bubble, &ui_font_14, C_TEXT, "");
    lv_obj_align(s_bubble, LV_ALIGN_TOP_MID, 0, 36);

    s_age = ui_label_w(s_main, &ui_font_14, C_TEXT3, UI_W, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_set_pos(s_age, 0, 148);

    static const char *const names[3] = {"饱腹", "心情", "清洁"};
    static const uint32_t colors[3] = {0xFBBF24, 0xF472B6, 0x38BDF8};
    for (int i = 0; i < 3; i++) {
        const int y = 174 + i * 18;
        lv_obj_t *l = ui_label(s_main, &ui_font_14, C_TEXT2, names[i]);
        lv_obj_set_pos(l, UI_PAD + 8, y - 4);
        s_bars[i] = ui_bar(s_main, UI_PAD + 50, y + 2, UI_W - 2 * UI_PAD - 66, 8, colors[i]);
    }
    for (int i = 0; i < ACT_COUNT; i++) {
        s_acts[i] = ui_pill(s_main, k_act_names[i], C_TEXT2, C_SURFACE);
        lv_obj_set_style_pad_hor(s_acts[i], 14, 0);
        lv_obj_set_style_pad_ver(s_acts[i], 5, 0);
        lv_obj_align(s_acts[i], LV_ALIGN_TOP_MID, (i - 1) * 74, 228);
    }
    shell_hints("▲▼:选择|OK:照顾|长▼:图鉴");
    render();
    if (grew) {
        save();
        audio_beep(BEEP_WORK_DONE);
        s_jump_until = sys_now_ms() + 600;
        say(grew_text(s_pet.stage), 3000);
    }
}

static void destroy(void)
{
    if (s_loaded) save();
    s_root = s_main = s_dex = NULL;
}

static void act(void)
{
    static const char *const ok_text[ACT_COUNT] = {"好吃!谢谢~", "哈哈,再来!", "香喷喷~"};
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
    if (sync_time()) {
        text = grew_text(s_pet.stage);
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
    if (s_dex_open) {
        if (ev == SYS_EV_CLICK || ev == SYS_EV_LONG) dex_close();
        return true;
    }
    if (k == SYS_KEY_DOWN && ev == SYS_EV_LONG) {
        dex_open();
        return true;
    }
    if (k == SYS_KEY_OK && ev == SYS_EV_LONG) {
        int32_t min;
        int hour;
        if (now_minutes(&min, &hour) && pet_can_pass_on(&s_pet, min)) {
            shell_confirm("送它去旅行?", "它会留下一颗新蛋,图鉴保留", pass_on_answer);
        }
        return true;
    }
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
    if (s_dex_open) return;
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
        if (sync_time()) {
            audio_beep(BEEP_WORK_DONE);
            s_jump_until = now + 600;
            say(grew_text(s_pet.stage), 3000);
            save();   // 平时只在离开时保存,避免每分钟写一次闪存
        }
        render();
    }
}

const app_t APP_PET = {
    .name = "像素宠物",
    .desc = "照顾它 · 看它长大",
    .icon = &PET_ICON,
    .color = C_PET,
    .create = create,
    .destroy = destroy,
    .key = key,
    .tick = tick,
};

// main/pet_art.c —— 像素宠物的字符画:4 个物种 × 幼崽 / 少年 / 成年三张图,
// 蛋、闪亮形态、长老和图鉴里的剪影都用同一套图换调色板,不另画。
#include "pet_art.h"

#include <stddef.h>

// 通用:W 高光 K 眼睛 P 腮红 h 影子;E/e/S 蛋壳 / 蛋影 / 斑点
#define SHARED {'W', 0xFFFFFF}, {'K', 0x1F2430}, {'P', 0xFF7FA0}, {'h', 0x1E2531}

// ---------------------------------------------------------------------------
// 蛋(斑点颜色暗示里面是谁)
// ---------------------------------------------------------------------------
static const char *R_EGG[16] = {
    "................", "......EEEE......", ".....EEEEEE.....", "....EWEEEEEE....",
    "....WEEEESEE....", "...EEEEEESSEE...", "...EEEEEEEEEE...", "...ESSEEEEEEE...",
    "...ESSEEEEEEE...", "...EEEEEEESEE...", "...EEEEEEEEEE...", "....EEEEEEEE....",
    "....eEEEEEEe....", ".....eeeeee.....", "................", "...hhhhhhhhhh...",
};
#define EGG_PAL(spot) {{'E', 0xFFF1D6}, {'e', 0xE8C99A}, {'S', spot}, SHARED, {0, 0}}
static const pixel_color_t PAL_EGG[PET_SPECIES_COUNT][8] = {
    EGG_PAL(0xF59E0B), EGG_PAL(0x475A7A), EGG_PAL(0x22C55E), EGG_PAL(0xFDA4AF),
};

// ---------------------------------------------------------------------------
// 团子:圆滚滚的小橘团,少年头顶长叶子,成年戴小王冠
// O 身体 o 暗部 G/g 叶子 Y 王冠 R 宝石
// ---------------------------------------------------------------------------
static const char *R_DANGO[3][16] = {
    {
        "................", "................", "................", "................",
        "......OOOO......", "....OOOOOOOO....", "...OOWOOOOOOO...", "..OOWOOOOOOOOO..",
        "..OOKKOOOOKKOO..", "..OOKWOOOOKWOO..", "..OPOOOooOOOPO..", "..OOOOOOOOOOOO..",
        "...oOOOOOOOOo...", "....oooooooo....", "................", "...hhhhhhhhhh...",
    },
    {
        ".......GG.......", "......GgG.......", ".......g........", ".....OOOOOO.....",
        "...OOOOOOOOOO...", "..OOWOOOOOOOOO..", ".OOWOOOOOOOOOOO.", ".OOOKKOOOOOKKOO.",
        ".OOOKWOOOOOKWOO.", ".OPPOOOOOOOOPPO.", ".OOOOOoooOOOOOO.", ".OOOOOOOOOOOOOO.",
        "..oOOOOOOOOOOo..", "...oOo....oOo...", "................", "..hhhhhhhhhhhh..",
    },
    {
        "....Y..Y..Y.....", "....YYYYYYY.....", "....YRYYYRY.....", "...OOOOOOOOOO...",
        "..OOOOOOOOOOOO..", ".OOWOOOOOOOOOOO.", "OOWOOOOOOOOOOOOO", "OOOOKKOOOOOKKOOO",
        "OOOOKWOOOOOKWOOO", "OOPPOOOOOOOOOPPO", "OOOOOOoOOOoOOOOO", "OOOOOOOoooOOOOOO",
        ".OOOOOOOOOOOOOO.", "..oOOOOOOOOOOo..", "...oOo....oOo...", "..hhhhhhhhhhhh..",
    },
};
#define DANGO_PAL(body, shade) \
    {{'O', body}, {'o', shade}, {'G', 0x4ADE80}, {'g', 0x16A34A}, {'Y', 0xFACC15}, {'R', 0xF87171}, SHARED, {0, 0}}

// ---------------------------------------------------------------------------
// 企鹅:白肚皮、橙色小嘴和脚,成年围红围巾
// O 身体 B 肚皮 b 嘴和脚 R 围巾
// ---------------------------------------------------------------------------
static const char *R_PENGUIN[3][16] = {
    {
        "................", "................", "................", "................",
        "......OOOO......", ".....OOOOOO.....", "....OOBBBBOO....", "....OKBBBBKO....",
        "....OBBbbBBO....", "....OPBBBBPO....", "...OOBBBBBBOO...", "....OBBBBBBO....",
        ".....OBBBBO.....", ".....bb..bb.....", "................", "...hhhhhhhhhh...",
    },
    {
        "................", "................", ".....OOOOOO.....", "....OOOOOOOO....",
        "...OOBBBBBBOO...", "...OBKBBBBKBO...", "...OBBBbbBBBO...", "...OPBBBBBBPO...",
        "..OOBBBBBBBBOO..", ".O.OBBBBBBBBO.O.", "...OBBBBBBBBO...", "...OOBBBBBBOO...",
        "....OOOOOOOO....", ".....bb..bb.....", "................", "..hhhhhhhhhhhh..",
    },
    {
        "................", ".....OOOOOO.....", "....OOOOOOOO....", "...OOBBBBBBOO...",
        "...OBKBBBBKBO...", "...OBWBbbBWBO...", "...OPBBbbBBPO...", "..RRRRRRRRRRRR..",
        ".OOBRRBBBBBBBOO.", "O..OBRBBBBBBO..O", "...OBBBBBBBBO...", "...OBBBBBBBBO...",
        "...OOBBBBBBOO...", "....OOOOOOOO....", "....bbb..bbb....", "..hhhhhhhhhhhh..",
    },
};
#define PENGUIN_PAL(body, belly) \
    {{'O', body}, {'B', belly}, {'b', 0xF59E0B}, {'R', 0xEF4444}, SHARED, {0, 0}}

// ---------------------------------------------------------------------------
// 小恐龙:朝右的小绿龙,少年长出背刺,成年张开小手
// O 身体 o 嘴 B 肚皮 Y 背刺
// ---------------------------------------------------------------------------
static const char *R_DINO[3][16] = {
    {
        "................", "................", "................", ".......OOOO.....",
        "......OOOOOO....", "......OWKOOO....", "......OOOOOOO...", "......OOOOooo...",
        "..O..OOOOOO.....", "..OOOOBBBOO.....", "...OOOBBBOO.....", "....OOOOOO......",
        ".....O..O.......", "................", "................", "...hhhhhhhhhh...",
    },
    {
        "................", "........OOOO....", ".......OOOOOO...", ".......OWKOOOO..",
        ".......OOOOOOOO.", ".......OOOOoooo.", "...Y..OOOOOO....", "..YOYOOOOOO.....",
        ".OOOOOOBBBOO....", "OO..OOOBBBOO....", "O...OOOBBBOO....", "....OOOOOOO.....",
        ".....OO..OO.....", ".....OO..OO.....", "................", "..hhhhhhhhhhhh..",
    },
    {
        "........OOOOO...", ".......OOOOOOO..", ".......OWKOOOOO.", ".......OOOOOOOOO",
        ".......OOOOOoooo", "....Y..OOOOOO...", "...YOY.OOOOO....", "..YOOOYOOOOOOO..",
        ".OOOOOOOBBBOO.O.", "OOO.OOOOBBBBO...", "OO..OOOOBBBBO...", "O...OOOOBBBOO...",
        ".....OOOOOOO....", ".....OO...OO....", "....OOO..OOO....", "..hhhhhhhhhhhh..",
    },
};
#define DINO_PAL(body, mouth, spike) \
    {{'O', body}, {'o', mouth}, {'B', 0xFEF3C7}, {'Y', spike}, SHARED, {0, 0}}

// ---------------------------------------------------------------------------
// 兔兔:长耳朵,少年耳朵更长,成年有圆肚皮
// O 身体 o 脚 P 耳朵里和腮红 B 肚皮
// ---------------------------------------------------------------------------
static const char *R_BUNNY[3][16] = {
    {
        "................", "................", ".....O....O.....", ".....OP..PO.....",
        ".....OP..PO.....", ".....OOOOOO.....", "....OOOOOOOO....", "....OKOOOOKO....",
        "....OPOooOPO....", "....OOOOOOOO....", "...OOOOOOOOOO...", "...OOOOOOOOOO...",
        "....OOOOOOOO....", ".....oo..oo.....", "................", "...hhhhhhhhhh...",
    },
    {
        "................", "....OO....OO....", "....OP....PO....", "....OP....PO....",
        "....OPO..OPO....", ".....OOOOOO.....", "....OOOOOOOO....", "...OOKOOOOKOO...",
        "...OOOOooOOOO...", "...OPOOOOOOPO...", "..OOOOOOOOOOOO..", "..OOOOBBBBOOOO..",
        "...OOOBBBBOOO...", "....OOOOOOOO....", "....oo....oo....", "..hhhhhhhhhhhh..",
    },
    {
        "...OO......OO...", "...OP......PO...", "...OP......PO...", "...OPO....OPO...",
        "....OOOOOOOO....", "...OOOOOOOOOO...", "..OOKWOOOOKWOO..", "..OOKKOOOOKKOO..",
        "..OPOOOooOOOPO..", "..OOOOOOOOOOOO..", ".OOOOOBBBBOOOOO.", ".OOOOBBBBBBOOOO.",
        "..OOOBBBBBBOOO..", "...OOOOOOOOOO...", "...ooo....ooo...", "..hhhhhhhhhhhh..",
    },
};
#define BUNNY_PAL(body, foot, belly) \
    {{'O', body}, {'o', foot}, {'B', belly}, SHARED, {0, 0}}

// ---------------------------------------------------------------------------
// 调色板:[物种][普通 / 闪亮 / 长老]
// ---------------------------------------------------------------------------
enum { PAL_NORMAL = 0, PAL_SHINY, PAL_ELDER, PAL_KINDS };
static const pixel_color_t PAL[PET_SPECIES_COUNT][PAL_KINDS][12] = {
    {DANGO_PAL(0xFF9F5A, 0xD9653B), DANGO_PAL(0xFFD166, 0xE0A030), DANGO_PAL(0xE2C9B5, 0xB39A86)},
    {PENGUIN_PAL(0x475A7A, 0xF1F5F9), PENGUIN_PAL(0x60A5FA, 0xEFF6FF), PENGUIN_PAL(0x94A3B8, 0xE2E8F0)},
    {DINO_PAL(0x4ADE80, 0x16A34A, 0xFACC15), DINO_PAL(0xA78BFA, 0x7C3AED, 0xF0ABFC), DINO_PAL(0xA8C3A0, 0x6B8F63, 0xD6D3D1)},
    {BUNNY_PAL(0xF5F5F4, 0xD6D3D1, 0xFFE4E6), BUNNY_PAL(0xFCD34D, 0xF59E0B, 0xFEF3C7), BUNNY_PAL(0xD6D3D1, 0xA8A29E, 0xE7E5E4)},
};
// 图鉴里还没见过的:整只涂成暗色剪影
static const pixel_color_t PAL_SIL[] = {
    {'O', 0x2A3241}, {'o', 0x2A3241}, {'B', 0x2A3241}, {'b', 0x2A3241}, {'W', 0x2A3241}, {'K', 0x2A3241},
    {'P', 0x2A3241}, {'G', 0x2A3241}, {'g', 0x2A3241}, {'Y', 0x2A3241}, {'R', 0x2A3241}, {'h', 0x161B24},
    {0, 0},
};

static const char *const *rows_of(pet_species_t sp, int growth)   // growth:0 幼崽 1 少年 2 成年
{
    switch (sp) {
    case PET_SP_PENGUIN: return R_PENGUIN[growth];
    case PET_SP_DINO: return R_DINO[growth];
    case PET_SP_BUNNY: return R_BUNNY[growth];
    default: return R_DANGO[growth];
    }
}

// 精灵表:每个物种 [蛋, 幼崽, 少年, 成年普通, 成年闪亮, 长老普通, 长老闪亮, 剪影]
enum { SPR_EGG = 0, SPR_BABY, SPR_TEEN, SPR_ADULT, SPR_ADULT_SHINY, SPR_ELDER, SPR_ELDER_SHINY, SPR_SIL, SPR_COUNT };
static pixel_sprite_t s_spr[PET_SPECIES_COUNT][SPR_COUNT];
static bool s_built;

static void build(void)
{
    for (int sp = 0; sp < PET_SPECIES_COUNT; sp++) {
        pixel_sprite_t *t = s_spr[sp];
        for (int i = 0; i < SPR_COUNT; i++) t[i].width = t[i].height = 16;
        t[SPR_EGG].rows = R_EGG;
        t[SPR_EGG].colors = PAL_EGG[sp];
        t[SPR_BABY].rows = rows_of(sp, 0);
        t[SPR_BABY].colors = PAL[sp][PAL_NORMAL];
        t[SPR_TEEN].rows = rows_of(sp, 1);
        t[SPR_TEEN].colors = PAL[sp][PAL_NORMAL];
        for (int i = SPR_ADULT; i <= SPR_SIL; i++) t[i].rows = rows_of(sp, 2);
        t[SPR_ADULT].colors = PAL[sp][PAL_NORMAL];
        t[SPR_ADULT_SHINY].colors = PAL[sp][PAL_SHINY];
        t[SPR_ELDER].colors = PAL[sp][PAL_ELDER];
        t[SPR_ELDER_SHINY].colors = PAL[sp][PAL_ELDER];   // 长老都是银灰色
        t[SPR_SIL].colors = PAL_SIL;
    }
    s_built = true;
}

const pixel_sprite_t *pet_art(pet_species_t sp, pet_stage_t stage, pet_form_t form)
{
    if (!s_built) build();
    if (sp < 0 || sp >= PET_SPECIES_COUNT) sp = PET_SP_DANGO;
    const bool shiny = form == PET_FORM_SHINY;
    switch (stage) {
    case PET_EGG: return &s_spr[sp][SPR_EGG];
    case PET_BABY: return &s_spr[sp][SPR_BABY];
    case PET_TEEN: return &s_spr[sp][SPR_TEEN];
    case PET_ADULT: return &s_spr[sp][shiny ? SPR_ADULT_SHINY : SPR_ADULT];
    default: return &s_spr[sp][shiny ? SPR_ELDER_SHINY : SPR_ELDER];
    }
}

const pixel_sprite_t *pet_art_silhouette(pet_species_t sp)
{
    if (!s_built) build();
    return &s_spr[sp][SPR_SIL];
}

// 启动器图标:团子幼崽(静态常量,开机就能用)
const pixel_sprite_t PET_ICON = {16, 16, R_DANGO[0], PAL[PET_SP_DANGO][PAL_NORMAL]};

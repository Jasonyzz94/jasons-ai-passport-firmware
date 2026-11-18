// main/icons.c —— 工具箱像素图标。每行 16 字符,'.' 为透明。
// tests/test_icons.c 会校验每个精灵的行宽、行数与调色板完整性。
#include "icons.h"

#define ICON(name, rows_, pal_) \
    const pixel_sprite_t name = { .width = 16, .height = 16, .rows = rows_, .colors = pal_ }

// ---------------------------------------------------------------------------
// 番茄
// ---------------------------------------------------------------------------
static const pixel_color_t PAL_TOMATO[] = {
    {'R', 0xF2553F}, {'r', 0xC23A2B}, {'G', 0x46D97F}, {'g', 0x239A57},
    {'W', 0xFFB4A3}, {0, 0},
};
static const char *R_TOMATO[16] = {
    ".......g........",
    "......gg........",
    "....GGgGGG......",
    "...GgGGGGgG.....",
    "..RRRgGGgRRRR...",
    ".RRRRRRRRRRRRR..",
    "RRWWRRRRRRRRRRR.",
    "RWWRRRRRRRRRRRR.",
    "RWRRRRRRRRRRRRRr",
    "RRRRRRRRRRRRRRRr",
    "RRRRRRRRRRRRRRRr",
    ".RRRRRRRRRRRRRr.",
    ".RRRRRRRRRRRRrr.",
    "..rRRRRRRRRRrr..",
    "...rrRRRRRrrr...",
    ".....rrrrrr.....",
};
ICON(ICON_TOMATO, R_TOMATO, PAL_TOMATO);

// ---------------------------------------------------------------------------
// 天气(太阳 + 云)
// ---------------------------------------------------------------------------
static const pixel_color_t PAL_WX[] = {
    {'Y', 0xFFC83D}, {'y', 0xF59E0B}, {'W', 0xF4F7FB}, {'w', 0xB9C3D3},
    {'B', 0x60A5FA}, {'b', 0x3B82F6}, {'S', 0xDDEBFF}, {'L', 0xFDE047},
    {'G', 0x8A94A6}, {'M', 0xF3E7B3}, {0, 0},
};
static const char *R_WEATHER[16] = {
    "....Y...........",
    ".Y..Y..Y........",
    "..Y.YY.Y........",
    "...YYYYY........",
    "YYYYyYYYY.......",
    "...YYYYWWWW.....",
    "..YYYYWWWWWW....",
    ".Y.YYWWWWWWWWW..",
    "...WWWWWWWWWWWW.",
    "..WWWWWWWWWWWWWW",
    ".WWWWWWWWWWWWWWW",
    ".WWWWWWWWWWWWWWw",
    "..wwwwwwwwwwwww.",
    "................",
    "................",
    "................",
};
ICON(ICON_WEATHER, R_WEATHER, PAL_WX);

static const char *R_SUN[16] = {
    "................",
    ".......Y........",
    "..Y....Y....Y...",
    "...Y.......Y....",
    "......YYY.......",
    ".....YYYYY......",
    "....YYYYYYY.....",
    ".YY.YYYYYyY.YY..",
    "....YYYYYyY.....",
    ".....YYyyY......",
    "......YYY.......",
    "...Y.......Y....",
    "..Y....Y....Y...",
    ".......Y........",
    "................",
    "................",
};
ICON(WX_SUN, R_SUN, PAL_WX);

static const char *R_PARTLY[16] = {
    "................",
    "....Y...........",
    ".Y..Y..Y........",
    "...YYYYY........",
    "..YYYYYYY.......",
    "YYYYYYWWWW......",
    "..YYYWWWWWW.....",
    "...YWWWWWWWWW...",
    "...WWWWWWWWWWW..",
    "..WWWWWWWWWWWWW.",
    ".WWWWWWWWWWWWWW.",
    ".wwwwwwwwwwwwww.",
    "................",
    "................",
    "................",
    "................",
};
ICON(WX_PARTLY, R_PARTLY, PAL_WX);

static const char *R_CLOUD[16] = {
    "................",
    "................",
    "................",
    "......WWWW......",
    ".....WWWWWW.....",
    "...WWWWWWWWWW...",
    "..WWWWWWWWWWWW..",
    ".WWWWWWWWWWWWWW.",
    ".WWWWWWWWWWWWWW.",
    ".WWWWWWWWWWWWWw.",
    "..wwwwwwwwwwww..",
    "................",
    "................",
    "................",
    "................",
    "................",
};
ICON(WX_CLOUD, R_CLOUD, PAL_WX);

static const char *R_RAIN[16] = {
    "................",
    "......wwww......",
    ".....wwwwww.....",
    "...wwwwwwwwww...",
    "..wwwwwwwwwwww..",
    ".wwwwwwwwwwwwww.",
    ".wwwwwwwwwwwwww.",
    "..GGGGGGGGGGGG..",
    "................",
    "...B...B...B....",
    "..B...B...B.....",
    "................",
    "....B...B...B...",
    "...B...B...B....",
    "................",
    "................",
};
ICON(WX_RAIN, R_RAIN, PAL_WX);

static const char *R_SNOW[16] = {
    "................",
    "......wwww......",
    ".....wwwwww.....",
    "...wwwwwwwwww...",
    "..wwwwwwwwwwww..",
    ".wwwwwwwwwwwwww.",
    "..GGGGGGGGGGGG..",
    "................",
    "...S.....S......",
    "..SSS...SSS.....",
    "...S..S..S......",
    ".....SSS........",
    "......S.....S...",
    "...........SSS..",
    "............S...",
    "................",
};
ICON(WX_SNOW, R_SNOW, PAL_WX);

static const char *R_STORM[16] = {
    "................",
    "......GGGG......",
    ".....GGGGGG.....",
    "...GGGGGGGGGG...",
    "..GGGGGGGGGGGG..",
    ".GGGGGGGGGGGGGG.",
    ".GGGGGGGGGGGGGG.",
    "..wwwwLLLwwwww..",
    "......LL........",
    ".....LLLLL......",
    ".......LL.......",
    "......LL........",
    ".....L..........",
    "................",
    "................",
    "................",
};
ICON(WX_STORM, R_STORM, PAL_WX);

static const char *R_FOG[16] = {
    "................",
    "................",
    "................",
    "..wwwwwwwwwwww..",
    "................",
    "....wwwwwwwwwwww",
    "................",
    "wwwwwwwwwwww....",
    "................",
    "...wwwwwwwwwww..",
    "................",
    ".wwwwwwwwww.....",
    "................",
    "................",
    "................",
    "................",
};
ICON(WX_FOG, R_FOG, PAL_WX);

static const char *R_MOON[16] = {
    "................",
    "......MMMM......",
    "....MMMM........",
    "...MMMM.......Y.",
    "..MMMM..........",
    "..MMM...........",
    ".MMMM.......Y...",
    ".MMMM...........",
    ".MMMM...........",
    ".MMMMM..........",
    "..MMMMM.......M.",
    "..MMMMMMM...MMM.",
    "...MMMMMMMMMMM..",
    ".....MMMMMMM....",
    "................",
    "................",
};
ICON(WX_MOON, R_MOON, PAL_WX);

// ---------------------------------------------------------------------------
// 蓝牙遥控器
// ---------------------------------------------------------------------------
static const pixel_color_t PAL_REMOTE[] = {
    {'K', 0x3B3F58}, {'k', 0x272A3D}, {'P', 0xA594FF}, {'p', 0x7C6AF0},
    {'W', 0xE8E6FF}, {'R', 0xFF6B8A}, {'B', 0x60A5FA}, {0, 0},
};
static const char *R_REMOTE[16] = {
    "..........B.....",
    "....B....B......",
    ".....B..B..B....",
    "......B...B.....",
    ".....PPPPPP.....",
    "....PWWWWWWp....",
    "....PWRRRRWp....",
    "....PWWWWWWp....",
    "....PPPWPPPp....",
    "....PPWWWPPp....",
    "....PPPWPPPp....",
    "....PPPPPPPp....",
    "....PWPPWPPp....",
    "....PPPPPPPp....",
    "....PWPPWPPp....",
    ".....pppppp.....",
};
ICON(ICON_REMOTE, R_REMOTE, PAL_REMOTE);

// ---------------------------------------------------------------------------
// 声音工具(均衡器柱)
// ---------------------------------------------------------------------------
static const pixel_color_t PAL_SOUND[] = {
    {'A', 0x34D399}, {'B', 0x2DD4BF}, {'C', 0x38BDF8}, {'D', 0xA3E635},
    {'E', 0xFACC15}, {'a', 0x1F8F68}, {0, 0},
};
static const char *R_SOUND[16] = {
    "................",
    "..........E.....",
    "..........E.....",
    "....B.....E.....",
    "....B.....E.....",
    "....B.....E.....",
    "....B..C..E.....",
    ".A..B..C..E..D..",
    ".A..B..C..E..D..",
    ".A..B..C..E..D..",
    ".A..B..C..E..D..",
    ".A..B..C..E..D..",
    ".A..B..C..E..D..",
    ".A..B..C..E..D..",
    "................",
    "aaaaaaaaaaaaaaaa",
};
ICON(ICON_SOUND, R_SOUND, PAL_SOUND);

// ---------------------------------------------------------------------------
// AI 助手(小机器人)
// ---------------------------------------------------------------------------
static const pixel_color_t PAL_AI[] = {
    {'P', 0xB9A6FF}, {'p', 0x8B6CF6}, {'W', 0xFFFFFF}, {'K', 0x1E1B4B},
    {'Y', 0xFDE047}, {'C', 0x67E8F9}, {'R', 0xFF8FB1}, {0, 0},
};
static const char *R_AI[16] = {
    ".......Y........",
    ".......p........",
    "...PPPPPPPPPP...",
    "..PPPPPPPPPPPP..",
    "..PKKKKKKKKKKP..",
    ".pPKCCKKKKCCKPp.",
    ".pPKCCKKKKCCKPp.",
    ".pPKKKKKKKKKKPp.",
    "..PKKKCCCCKKKP..",
    "..PPPPPPPPPPPP..",
    "...pppppppppp...",
    ".....PPPPPP.....",
    "...PPPPPPPPPP...",
    "..PPRPPPPPPRPP..",
    "..PPPPPPPPPPPP..",
    "...pppppppppp...",
};
ICON(ICON_AI, R_AI, PAL_AI);

// ---------------------------------------------------------------------------
// 设备互联(两个对讲气泡)
// ---------------------------------------------------------------------------
static const pixel_color_t PAL_LINK[] = {
    {'P', 0xF472B6}, {'p', 0xDB2777}, {'C', 0x67E8F9}, {'c', 0x0EA5E9},
    {'W', 0xFFFFFF}, {0, 0},
};
static const char *R_LINK[16] = {
    "PPPPPPPPP.......",
    "PPPPPPPPP.......",
    "PPWPWPWPP.......",
    "PPPPPPPPP.......",
    "PPPPPPPPP.......",
    ".pP.............",
    ".p..............",
    ".......CCCCCCCCC",
    ".......CCCCCCCCC",
    ".......CCWWWWWCC",
    ".......CCCCCCCCC",
    ".......CCWWWCCCC",
    ".......CCCCCCCCC",
    ".............Cc.",
    "..............c.",
    "................",
};
ICON(ICON_LINK, R_LINK, PAL_LINK);

// ---------------------------------------------------------------------------
// 智能家居(房子)
// ---------------------------------------------------------------------------
static const pixel_color_t PAL_HOME[] = {
    {'R', 0xFB923C}, {'r', 0xEA580C}, {'W', 0xFEF3C7}, {'w', 0xFCD34D},
    {'B', 0x7C4A1E}, {'Y', 0xFDE047}, {0, 0},
};
static const char *R_HOME[16] = {
    "................",
    ".......RR.......",
    "......RRRR..rr..",
    ".....RRRRRR.rr..",
    "....RRRRRRRRrr..",
    "...RRRRRRRRRRR..",
    "..RRRRRRRRRRRRR.",
    ".rrrrrrrrrrrrrrr",
    "..WWWWWWWWWWWW..",
    "..WYYWWWWWWYYW..",
    "..WYYWWBBWWYYW..",
    "..WWWWWBBWWWWW..",
    "..WWWWWBBWWWWW..",
    "..WWWWWBwWWWWW..",
    "..wwwwwBBwwwww..",
    "................",
};
ICON(ICON_HOME, R_HOME, PAL_HOME);

// ---------------------------------------------------------------------------
// 信息看板
// ---------------------------------------------------------------------------
static const pixel_color_t PAL_BOARD[] = {
    {'B', 0x38BDF8}, {'b', 0x0284C7}, {'W', 0xE0F2FE}, {'G', 0x4ADE80},
    {'Y', 0xFACC15}, {'R', 0xFB7185}, {'K', 0x0C4A6E}, {0, 0},
};
static const char *R_BOARD[16] = {
    "................",
    ".BBBBBBBBBBBBBB.",
    ".BKKKKKKKKKKKKb.",
    ".BKWWWWKKKKKKKb.",
    ".BKKKKKKKKKYKKb.",
    ".BKKKKKKKKKYKKb.",
    ".BKKKKKGKKKYKKb.",
    ".BKKRKKGKKKYKKb.",
    ".BKKRKKGKKKYKKb.",
    ".BKKRKKGKKKYKKb.",
    ".BKKKKKKKKKKKKb.",
    ".bbbbbbbbbbbbbb.",
    "......bbbb......",
    "......bbbb......",
    "....bbbbbbbb....",
    "................",
};
ICON(ICON_BOARD, R_BOARD, PAL_BOARD);

// ---------------------------------------------------------------------------
// Wi-Fi 分析
// ---------------------------------------------------------------------------
static const pixel_color_t PAL_WIFI[] = {
    {'G', 0x4ADE80}, {'g', 0x22C55E}, {'T', 0x2DD4BF}, {'W', 0xECFDF5}, {0, 0},
};
static const char *R_WIFI[16] = {
    "................",
    "................",
    "....GGGGGGGG....",
    "..GGG......GGG..",
    ".GG..........GG.",
    "G....TTTTTT....G",
    "...TTT....TTT...",
    "..TT........TT..",
    "......gggg......",
    ".....gg..gg.....",
    "................",
    ".......WW.......",
    "......WWWW......",
    ".......WW.......",
    "................",
    "................",
};
ICON(ICON_WIFI, R_WIFI, PAL_WIFI);

// ---------------------------------------------------------------------------
// 计时器(秒表)
// ---------------------------------------------------------------------------
static const pixel_color_t PAL_TIMER[] = {
    {'Y', 0xFACC15}, {'y', 0xCA8A04}, {'W', 0xFFFBEB}, {'K', 0x422006},
    {'R', 0xF87171}, {0, 0},
};
static const char *R_TIMER[16] = {
    "......YYYY......",
    ".......YY.......",
    "....yYYYYYYy..y.",
    "...YYWWWWWWYYyy.",
    "..YWWWWKWWWWWY..",
    ".YWWWWWKWWWWWWY.",
    ".YWWWWWKWWWWWWY.",
    "YWWWWWWKWWWWWWWY",
    "YWWWWWWKRRRWWWWY",
    "YWWWWWWWWWWWWWWY",
    ".YWWWWWWWWWWWWY.",
    ".YWWWWWWWWWWWWY.",
    "..YWWWWWWWWWWY..",
    "...YYWWWWWWYY...",
    "....yYYYYYYy....",
    "................",
};
ICON(ICON_TIMER, R_TIMER, PAL_TIMER);

// ---------------------------------------------------------------------------
// 骰子
// ---------------------------------------------------------------------------
static const pixel_color_t PAL_DICE[] = {
    {'W', 0xFFFFFF}, {'w', 0xD6DCE5}, {'O', 0xFB923C}, {'K', 0x1F2937}, {0, 0},
};
static const char *R_DICE[16] = {
    "................",
    "..WWWWWWWWWWWW..",
    ".WWWWWWWWWWWWWW.",
    ".WOOWWWWWWWWOOw.",
    ".WOOWWWWWWWWOOw.",
    ".WWWWWWWWWWWWWw.",
    ".WWWWWWWWWWWWWw.",
    ".WWWWWWOOWWWWWw.",
    ".WWWWWWOOWWWWWw.",
    ".WWWWWWWWWWWWWw.",
    ".WWWWWWWWWWWWWw.",
    ".WOOWWWWWWWWOOw.",
    ".WOOWWWWWWWWOOw.",
    ".wwwwwwwwwwwwww.",
    "..wwwwwwwwwwww..",
    "................",
};
ICON(ICON_DICE, R_DICE, PAL_DICE);

// ---------------------------------------------------------------------------
// 设置(齿轮)
// ---------------------------------------------------------------------------
static const pixel_color_t PAL_GEAR[] = {
    {'S', 0xCBD5E1}, {'s', 0x94A3B8}, {'K', 0x0B0E14}, {0, 0},
};
static const char *R_GEAR[16] = {
    "................",
    ".......SS.......",
    "...SS..SS..SS...",
    "...SSSSSSSSSS...",
    "....SSSSSSSS....",
    "...SSSSSSSSSS...",
    ".SSSSSKKKKSSSSS.",
    ".SSSSKKKKKKSSSs.",
    ".SSSSKKKKKKSSSs.",
    ".sSSSSKKKKSSSSs.",
    "...SSSSSSSSSs...",
    "....SSSSSSSs....",
    "...SSSSSSSSss...",
    "...ss..ss..ss...",
    ".......ss.......",
    "................",
};
ICON(ICON_GEAR, R_GEAR, PAL_GEAR);

// ---------------------------------------------------------------------------
// 小图标
// ---------------------------------------------------------------------------
static const pixel_color_t PAL_SMALL[] = {
    {'Y', 0xFDE047}, {'y', 0xEAB308}, {'W', 0xF1F5F9}, {'w', 0x94A3B8},
    {'G', 0x4ADE80}, {'g', 0x16A34A}, {'B', 0x60A5FA}, {'b', 0x2563EB},
    {'R', 0xF87171}, {'r', 0xDC2626}, {'P', 0xF472B6}, {'V', 0xA78BFA},
    {'O', 0xFB923C}, {'K', 0x334155}, {'C', 0x22D3EE}, {0, 0},
};
static const char *R_BULB[16] = {
    "................",
    ".....YYYYYY.....",
    "....YYYYYYYY....",
    "...YYYWYYYYYY...",
    "...YYWYYYYYYY...",
    "...YYYYYYYYYY...",
    "...YYYYYYYYYy...",
    "....YYYYYYYy....",
    ".....YYYYYy.....",
    "......YYYy......",
    "......wwww......",
    "......WWWW......",
    "......wwww......",
    ".......ww.......",
    "................",
    "................",
};
ICON(IC_BULB, R_BULB, PAL_SMALL);

static const char *R_PLUG[16] = {
    "................",
    ".....W....W.....",
    ".....W....W.....",
    ".....W....W.....",
    "...WWWWWWWWWW...",
    "...WWWWWWWWWW...",
    "...WWWWWWWWWw...",
    "....WWWWWWWw....",
    ".....WWWWWw.....",
    "......wWWw......",
    ".......ww.......",
    ".......ww.......",
    ".......Gg.......",
    "........gG......",
    ".........gg.....",
    "................",
};
ICON(IC_PLUG, R_PLUG, PAL_SMALL);

static const char *R_SCENE[16] = {
    "................",
    ".......Y........",
    ".......Y........",
    "......YYY.......",
    "YYYYYYYYYYYYYYY.",
    ".YYYYYYYYYYYYY..",
    "...YYYYYYYYY....",
    "....YYYYYYY.....",
    "....YYYYYYY.....",
    "...YYYY.YYYY....",
    "...YYY...YYY....",
    "..YY.......YY...",
    "................",
    "................",
    "................",
    "................",
};
ICON(IC_SCENE, R_SCENE, PAL_SMALL);

static const char *R_SENSOR[16] = {
    "................",
    "......WWWW......",
    ".....WW..WW.....",
    ".....W....W.....",
    ".....W.RR.W.....",
    ".....W.RR.W.....",
    ".....W.RR.W.....",
    ".....W.RR.W.....",
    ".....W.RR.W.....",
    "....WW.RR.WW....",
    "...WW.RRRR.WW...",
    "...W.RRRRRR.W...",
    "...W.RRRRRR.W...",
    "...WW.RRRR.WW...",
    "....WWW..WWW....",
    "......WWWW......",
};
ICON(IC_SENSOR, R_SENSOR, PAL_SMALL);

static const char *R_MIC[16] = {
    "......WWWW......",
    ".....WWWWWW.....",
    ".....WWwWWW.....",
    ".....WWWWWW.....",
    ".....WWwWWW.....",
    ".....WWWWWW.....",
    "..w..WWWWWW..w..",
    "..w...WWWW...w..",
    "..ww........ww..",
    "...ww......ww...",
    "....wwwwwwww....",
    ".......ww.......",
    ".......ww.......",
    ".....wwwwww.....",
    "................",
    "................",
};
ICON(IC_MIC, R_MIC, PAL_SMALL);

static const char *R_WAVE[16] = {
    "................",
    "................",
    ".......G........",
    ".......G........",
    "...G...G....G...",
    "...G..GG....G...",
    "...G..GG..G.G...",
    ".G.G.GGG.GG.G.G.",
    ".G.GGGGGGGGGG.G.",
    "...G..GG..G.G...",
    "...G..GG....G...",
    "...G...G....G...",
    ".......G........",
    ".......G........",
    "................",
    "................",
};
ICON(IC_WAVE, R_WAVE, PAL_SMALL);

static const char *R_TUNER[16] = {
    "................",
    "....WWWWWWWW....",
    "..WW........WW..",
    ".W...w.ww.w...W.",
    "W..w........w..W",
    "W.......G......W",
    "W.......G......W",
    "W......GG......W",
    ".W.....G......W.",
    "..WW..GG....WW..",
    "....WWWWWWWW....",
    "................",
    "...BBBBBBBBBB...",
    "...BbbbbbbbbB...",
    "...BBBBBBBBBB...",
    "................",
};
ICON(IC_TUNER, R_TUNER, PAL_SMALL);

static const char *R_METRO[16] = {
    "................",
    ".......OO.......",
    "......OOOO..W...",
    "......OOOO.W....",
    ".....OOOOOW.....",
    ".....OOOOWO.....",
    "....OOOOWOOO....",
    "....OOOWOOOO....",
    "...OOOWOOOOOO...",
    "...OOWOOOOOOO...",
    "..OOOOOOOOOOOO..",
    "..OOOOOOOOOOOO..",
    ".OOOOOOOOOOOOOO.",
    ".KKKKKKKKKKKKKK.",
    "................",
    "................",
};
ICON(IC_METRO, R_METRO, PAL_SMALL);

static const char *R_SLIDES[16] = {
    "................",
    "BBBBBBBBBBBBBBBB",
    "BWWWWWWWWWWWWWWB",
    "BWBBBBBBWWWWWWWB",
    "BWWWWWWWWWWWWWWB",
    "BWwwwwwwwwWWWWWB",
    "BWwwwwwwWWWWYYWB",
    "BWwwwwwwwwWWYYWB",
    "BWWWWWWWWWWWWWWB",
    "BBBBBBBBBBBBBBBB",
    ".......bb.......",
    "......b..b......",
    ".....b....b.....",
    "....b......b....",
    "................",
    "................",
};
ICON(IC_SLIDES, R_SLIDES, PAL_SMALL);

static const char *R_MUSIC[16] = {
    "................",
    "........PPPPPP..",
    ".....PPPPPPPPP..",
    ".....PPPPP...P..",
    ".....P.......P..",
    ".....P.......P..",
    ".....P.......P..",
    ".....P.......P..",
    ".....P.......P..",
    ".....P....PPPP..",
    "..PPPP...PPPPP..",
    ".PPPPP...PPPPP..",
    ".PPPPP....PPP...",
    "..PPP...........",
    "................",
    "................",
};
ICON(IC_MUSIC, R_MUSIC, PAL_SMALL);

static const char *R_CAMERA[16] = {
    "................",
    "................",
    ".....wwwww......",
    "..WWWWWWWWWWWW..",
    ".WWWWWWWWWWWRRW.",
    ".WWWWKKKKWWWWWW.",
    ".WWWKKBBKKWWWWW.",
    ".WWKKBBBBKKWWWW.",
    ".WWKKBbbBKKWWWW.",
    ".WWKKBbbBKKWWWW.",
    ".WWWKKBBKKWWWWW.",
    ".WWWWKKKKWWWWWW.",
    ".wwwwwwwwwwwwww.",
    "................",
    "................",
    "................",
};
ICON(IC_CAMERA, R_CAMERA, PAL_SMALL);

static const char *R_CHAT[16] = {
    "................",
    "..PPPPPPPPPPPP..",
    ".PPPPPPPPPPPPPP.",
    ".PPWWPPWWPPWWPP.",
    ".PPWWPPWWPPWWPP.",
    ".PPPPPPPPPPPPPP.",
    "..PPPPPPPPPPPP..",
    "...PP...........",
    "..PP............",
    ".P..............",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
};
ICON(IC_CHAT, R_CHAT, PAL_SMALL);

static const char *R_HEART[16] = {
    "................",
    "................",
    "..RRR....RRR....",
    ".RRRRR..RRRRR...",
    "RRWRRRRRRRRRRR..",
    "RWRRRRRRRRRRRR..",
    "RRRRRRRRRRRRRR..",
    ".RRRRRRRRRRRR...",
    "..RRRRRRRRRR....",
    "...RRRRRRRR.....",
    "....RRRRRR......",
    ".....RRRR.......",
    "......RR........",
    "................",
    "................",
    "................",
};
ICON(IC_HEART, R_HEART, PAL_SMALL);

static const char *R_COIN[16] = {
    "................",
    ".....YYYYYY.....",
    "...YYyyyyyyYY...",
    "..YyYYYYYYYYyY..",
    "..YyYYYyyYYYyY..",
    ".YyYYYYyyYYYYyY.",
    ".YyYYyyyyyyYYyY.",
    ".YyYYYYyyYYYYyY.",
    ".YyYYyyyyyyYYyY.",
    ".YyYYYYyyYYYYyY.",
    "..YyYYYyyYYYyY..",
    "..YyYYYYYYYYyY..",
    "...YYyyyyyyYY...",
    ".....YYYYYY.....",
    "................",
    "................",
};
ICON(IC_COIN, R_COIN, PAL_SMALL);

static const char *R_CLOUD_OFF[16] = {
    "................",
    "...............R",
    "......wwww....R.",
    ".....wwwwww..R..",
    "...wwwwwwwwwR...",
    "..wwwwwwwwwRww..",
    ".wwwwwwwwwRwwww.",
    ".wwwwwwwwRwwwww.",
    ".wwwwwwwRwwwwww.",
    "..wwwwwRwwwwww..",
    "......R.........",
    ".....R..........",
    "....R...........",
    "................",
    "................",
    "................",
};
ICON(IC_CLOUD_OFF, R_CLOUD_OFF, PAL_SMALL);

static const char *R_CHECK[16] = {
    "................",
    "................",
    ".....GGGGGG.....",
    "...GGGGGGGGGG...",
    "..GGGGGGGGGGGG..",
    "..GGGGGGGGGWGG..",
    ".GGGGGGGGGWWGGG.",
    ".GGGWGGGGWWGGGG.",
    ".GGGWWGGWWGGGGG.",
    ".GGGGWWWWGGGGGG.",
    "..GGGGWWGGGGGG..",
    "..GGGGGGGGGGGG..",
    "...GGGGGGGGGG...",
    ".....GGGGGG.....",
    "................",
    "................",
};
ICON(IC_CHECK, R_CHECK, PAL_SMALL);

static const char *R_BELL[16] = {
    "................",
    ".......YY.......",
    ".....YYYYYY.....",
    "....YYYYYYYY....",
    "....YWYYYYYY....",
    "....YWYYYYYY....",
    "....YYYYYYYY....",
    "...YYYYYYYYYY...",
    "...YYYYYYYYYY...",
    "..YYYYYYYYYYYY..",
    ".yyyyyyyyyyyyyy.",
    "......yyyy......",
    ".......yy.......",
    "................",
    "................",
    "................",
};
ICON(IC_BELL, R_BELL, PAL_SMALL);

static const char *R_STAR[16] = {
    "................",
    ".......Y........",
    ".......Y........",
    "......YYY.......",
    "......YYY.......",
    "YYYYYYYYYYYYYYY.",
    ".YYYYYYWYYYYYY..",
    "...YYYYYYYYY....",
    "....YYYYYYY.....",
    "....YYYYYYY.....",
    "...YYYY.YYYY....",
    "...YYY...YYY....",
    "..YY.......YY...",
    "................",
    "................",
    "................",
};
ICON(IC_STAR, R_STAR, PAL_SMALL);

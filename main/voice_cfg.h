// main/voice_cfg.h —— 语音助手的纯逻辑:接口选择、报错文字、UTF-8 截断、24k→16k 重采样。
// 不依赖 ESP-IDF / LVGL,有主机测试(tests/test_voice_cfg.c)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 语音助手走哪条路(settings.ai_mode)。
typedef enum {
    VOICE_MODE_AUTO = 0,    // 连得上 Mac 中转就用中转,否则云端直连
    VOICE_MODE_RELAY = 1,   // 只用 Mac 中转
    VOICE_MODE_DIRECT = 2,  // 只用云端直连
} voice_mode_t;

// 朗读采样率(settings.tts_rate):服务商返回 PCM 的采样率;0 = 不朗读,只显示文字。
#define VOICE_TTS_OFF 0
#define VOICE_TTS_16K 16000
#define VOICE_TTS_24K 24000

// 这一轮是否走云端直连。relay_ok:中转已连接;has_key:直连的对话密钥已填。
bool voice_use_direct(int mode, bool relay_ok, bool has_key);
// 这一轮能不能开始说话(没有可用通道时返回 false)。
bool voice_ready(int mode, bool relay_ok, bool has_key, bool net_ok);

// 识别和朗读用的接口与密钥。语音接口留空 = 和对话同一家;
// 语音密钥留空时,只有语音接口和对话接口相同才沿用对话密钥(不把密钥发给别家)。
void voice_resolve(const char *api_base, const char *api_key, const char *voice_base,
                   const char *voice_key, const char **base, const char **key);

// HTTP 状态码 → 简短原因。status < 0 用下面的本地错误码。
#define VOICE_ERR_NET (-1)      // 连不上 / 超时
#define VOICE_ERR_MEM (-2)      // 内存不足,没发请求
#define VOICE_ERR_PARSE (-3)    // 返回内容看不懂(太长或格式不对)
#define VOICE_ERR_EMPTY (-4)    // 识别结果为空
#define VOICE_ERR_UPLOAD (-5)   // 上传中断
#define VOICE_ERR_INSECURE (-6) // 密钥不能用明文 http 发往公网
const char *voice_reason(int status);
// "识别:密钥无效" 这类屏幕提示。stage 例 "识别" / "对话" / "朗读"。
void voice_error_text(char *out, size_t cap, const char *stage, int status);

// 密钥能不能发往这个地址:https 都行;http 只允许本机和局域网
// (例如自建的 Ollama),免得密钥在公网上明文传输。没有密钥时任何地址都行。
bool voice_key_transport_ok(const char *url, const char *key);

// IMA ADPCM WAV(格式 0x11,单声道 16 kHz)的文件头,数据为 blocks 个 audio_dsp 的 ADPCM 块
// (每块 132 字节:4 字节头 + 256 个样本,低半字节在前)。云端识别用它上传录音。
#define VOICE_WAV_HEADER_BYTES 60
void voice_ima_wav_header(uint8_t h[VOICE_WAV_HEADER_BYTES], uint32_t blocks);

// 按 UTF-8 字符边界拷贝:放不下时在完整字符处截断,不会留下半个汉字。返回写入的字节数。
size_t utf8_copy(char *dst, size_t cap, const char *src);

// 24 kHz → 16 kHz 流式重采样(每 3 个输入出 2 个输出,带简单平滑)。
typedef struct {
    int16_t carry[2];
    int n;   // carry 里的样本数 0..2
} resample24_t;
// out 至少 (n + 2) * 2 / 3 + 1 个样本。返回输出样本数。
size_t resample24_to16(resample24_t *st, const int16_t *in, size_t n, int16_t *out);

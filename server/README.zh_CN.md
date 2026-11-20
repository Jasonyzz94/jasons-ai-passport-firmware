<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# 中转服务器(可选)

放在家里一台常开的电脑(例如 Mac mini)上,为工具箱提供:天气、信息看板、Home Assistant 转发,
以及语音助手(本地语音识别 → 大模型 → 朗读)。**没有它设备也能用**:找不到服务器时,设备会直连
天气、Home Assistant 和云端语音接口(见 [应用需求](../docs/development/app-requirements.zh_CN.md))。

## 安装(macOS)

```bash
cd server
export DEEPSEEK_API_KEY=sk-...   # 默认配置用 DeepSeek 对话;用其他服务见下文
./install-macos.sh              # 建虚拟环境、装依赖、用 launchd 开机自启
```

安装脚本会把已 export 的 `ANTHROPIC_API_KEY`、`DEEPSEEK_API_KEY`、`SILICONFLOW_API_KEY`、
`OPENAI_API_KEY`、`LLM_API_KEY` 写进 launchd 配置;换了密钥要重新运行脚本。

- 配置文件:`server/config.toml`(首次从 `config.example.toml` 复制)。改完后重新运行安装脚本。
- 日志:`~/.passport-server/server.log`;已配对设备与令牌:`~/.passport-server/devices.json`。
- 首次启动时 macOS 会询问是否允许 Python 接受网络连接,请选择允许。
- 其他系统:`pip install -r requirements.txt` 后运行 `python -m passport_server --config config.toml`。

## 语音助手怎么配

语音助手分三步:识别(语音→文字)、对话、朗读(文字→语音)。在 `config.toml` 里分别对应
`[asr]`、`[llm]`、`[tts]`,每一步可以选不同的服务。

| 你手上有的 | `[llm]` | `[asr]` / `[tts]` |
| --- | --- | --- |
| 只有 DeepSeek 或其他兼容 OpenAI 的对话 API | `provider = "openai_compatible"`,填 `base_url`、`model` 和密钥 | 保持默认:本机 `mlx_whisper` 识别、macOS `say` 朗读,免费 |
| 什么都不想买 | 本地 Ollama:`base_url = "http://localhost:11434/v1"`,不填密钥 | 同上 |
| 想要更好听的声音 | 任选 | `[tts] provider = "openai"`,指向硅基流动等有朗读接口的服务 |

DeepSeek 只提供对话,没有识别和朗读接口,所以 `[asr]` / `[tts]` 不要填 DeepSeek 的地址。
各步失败时,设备屏幕会显示哪一步、什么原因(例如"对话:余额不足"),详细错误在服务器日志里。

设备自己也能直连云端(不经过这台电脑):在设备的配置网页"语音助手"里选预设,例如
"DeepSeek 对话 + 硅基流动语音",分别填两个密钥。

## 设备如何找到服务器

1. 设备向 UDP 8788 广播询问,服务器回复自己的地址;
2. 服务器每 3 秒向 UDP 8789 主动广播自己的地址(VPN / 路由器只拦一个方向时仍能被发现);
3. 设备用 HTTP 8787 调 `/api/hello`,收到 401 后调 `/api/pair` 取令牌(`auto_approve = true` 时自动批准)。

找不到时依次检查:Mac 是否开着 VPN / TUN 模式代理、macOS 防火墙是否允许 Python、
路由器是否开启了"客户端隔离 / AP 隔离"。也可以在设备配置网页的"家里的中转服务器"里直接填
`http://<Mac 局域网地址>:8787` 跳过广播。设备的 设置 › 关于本机 会显示最近一次发现的结果。

## 接口

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| POST | `/api/pair` | `{"device","name"}` → `{"token"}`,唯一不需要令牌的接口 |
| GET | `/api/hello` | 服务器名称与时间 |
| GET | `/api/weather` | 与设备直连天气相同格式 |
| GET | `/api/board` | 看板卡片(在 `[[board.cards]]` 里配置) |
| GET / POST | `/api/home`、`/api/home/toggle` | Home Assistant 转发 |
| POST | `/api/assistant/talk` | 请求体为 IMA ADPCM 块(分块上传)→ `{"question","answer","audio"}`;朗读失败时多一个 `tts_error`;识别或对话失败返回 502 `{"error"}` |
| GET | `/api/assistant/audio/{id}` | 朗读音频,IMA ADPCM 块 |

## 测试

```bash
pip install -r requirements.txt
python -m unittest discover -s tests
```

测试覆盖与固件逐字节一致的 ADPCM 编解码、接口流程、天气 / 看板 / 家居数据整形、发现广播与大模型请求参数。

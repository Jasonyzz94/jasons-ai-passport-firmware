#!/usr/bin/env bash
# 在 Mac mini 上安装中转服务,并用 launchd 开机自启、崩溃自动重启。
# 用法:cd server && ./install-macos.sh
# 卸载:launchctl bootout gui/$(id -u)/com.passport.server && rm ~/Library/LaunchAgents/com.passport.server.plist
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
venv="${HOME}/.passport-server/venv"
plist="${HOME}/Library/LaunchAgents/com.passport.server.plist"
label="com.passport.server"

[[ "$(uname -s)" == "Darwin" ]] || { echo "只支持 macOS(其他系统直接运行 python -m passport_server)"; exit 1; }
command -v python3 >/dev/null || { echo "请先安装 Python 3.11+(brew install python)"; exit 1; }

mkdir -p "${HOME}/.passport-server"
python3 -m venv "${venv}"
"${venv}/bin/pip" install -q --upgrade pip
"${venv}/bin/pip" install -q -r "${here}/requirements.txt"
if [[ "$(uname -m)" == "arm64" ]]; then
    "${venv}/bin/pip" install -q mlx-whisper   # Apple 芯片本地语音识别
else
    "${venv}/bin/pip" install -q faster-whisper
fi

[[ -f "${here}/config.toml" ]] || cp "${here}/config.example.toml" "${here}/config.toml"

# launchd 不继承终端环境变量:把已 export 的密钥写进 plist(仅本机可读)。
# config.toml 里 api_key_env 用到的变量名要在这个列表里。
key_vars=(ANTHROPIC_API_KEY DEEPSEEK_API_KEY SILICONFLOW_API_KEY OPENAI_API_KEY LLM_API_KEY)
env_xml=""
found=""
for v in "${key_vars[@]}"; do
    val="${!v:-}"
    [[ -n "${val}" ]] || continue
    [[ "${val}" =~ ^[A-Za-z0-9._:/+=-]+$ ]] || { echo "${v} 含有不支持的字符,请检查后重试"; exit 1; }
    env_xml+="    <key>${v}</key><string>${val}</string>"$'\n'
    found+=" ${v}"
done
mkdir -p "$(dirname "${plist}")"
cat > "${plist}" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>${label}</string>
  <key>ProgramArguments</key><array>
    <string>${venv}/bin/python</string><string>-m</string><string>passport_server</string>
    <string>--config</string><string>${here}/config.toml</string>
  </array>
  <key>WorkingDirectory</key><string>${here}</string>
  <key>EnvironmentVariables</key><dict>
${env_xml}    <key>PATH</key><string>/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin</string>
  </dict>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>${HOME}/.passport-server/server.log</string>
  <key>StandardErrorPath</key><string>${HOME}/.passport-server/server.log</string>
</dict></plist>
PLIST
chmod 600 "${plist}"

launchctl bootout "gui/$(id -u)/${label}" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "${plist}"
echo "已启动。日志:~/.passport-server/server.log"
if [[ -n "${found}" ]]; then
    echo "已写入密钥变量:${found}"
else
    echo "提示:没检测到密钥变量(${key_vars[*]})。先 export,例如 export DEEPSEEK_API_KEY=sk-...,再重新运行本脚本;"
    echo "      或者在 config.toml 里直接写 api_key,或用本地 Ollama。"
fi
echo "首次启动 macOS 可能询问是否允许 Python 接受网络连接,请选择允许。"

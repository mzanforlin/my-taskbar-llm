# my-taskbar-llm

Monitor leve para a barra de tarefas do Windows 10/11, em C++ puro (Win32). Mostra:

- **CPU** e **RAM** (porcentagem + mini barra)
- **Rede** (download / upload)
- **Claude Code**: % usada do limite de 5h e horário do reset
- **Antigravity**: % usada do limite de 5h (Gemini e Claude/GPT) e horário do reset

Sem dependências, sem chaves de API. Os dados dos limites vêm das próprias ferramentas.

## Compilar

Precisa de um `g++` para Windows (ex.: [w64devkit](https://github.com/skeeto/w64devkit)).

```cmd
g++ -O3 -s -mwindows -municode main.cpp -o TaskbarPerf.exe -liphlpapi -lgdi32 -luser32 -ladvapi32
```

O `build.bat` faz o mesmo usando um `w64devkit` extraído na subpasta `w64devkit\`.

## Conectar o Claude Code

O Claude Code envia os dados de limite (`rate_limits.five_hour`) para o script de statusline. O `usage-bridge.js` grava esses dados em `~/.claude/claude-usage.txt`, que o widget lê.

1. Requer [Node.js](https://nodejs.org) no PATH.
2. Copie `usage-bridge.js` para `%USERPROFILE%\.claude\`.
3. Em `%USERPROFILE%\.claude\settings.json` adicione (ajuste o caminho):

```json
{
  "statusLine": {
    "type": "command",
    "command": "node \"C:/Users/SEU_USUARIO/.claude/usage-bridge.js\""
  }
}
```

4. Reinicie o Claude Code. O valor só atualiza enquanto houver uma sessão ativa. Os dados de limite aparecem para contas com assinatura Claude.ai.

## Conectar o Antigravity

Nada a configurar além de ter o CLI `agy` instalado em `%LOCALAPPDATA%\agy\bin\agy.exe`. A cada 2 minutos o widget roda, escondido:

```cmd
agy -p "/usage" --output-format json
```

e lê o resultado em `%TEMP%\taskbarperf-agy.json`. Se o `agy` não estiver nesse caminho, o bloco mostra `--`.

## Uso

- **Arrastar:** botão esquerdo.
- **Botão direito:** travar na barra de tarefas, iniciar com o Windows, sair.

## Privacidade

O app não lê credenciais nem envia nada pela rede. Lê apenas `~/.claude/claude-usage.txt` e a saída do `agy /usage`.

// Statusline do Claude Code: grava o limite de 5h em claude-usage.txt para o TaskbarPerf ler.
// Formato do arquivo: "<used_percentage> <resets_at_epoch_s>"
const fs = require('fs'), path = require('path'), os = require('os');
let raw = '';
process.stdin.on('data', d => raw += d).on('end', () => {
  try {
    const j = JSON.parse(raw);
    const fh = j.rate_limits && j.rate_limits.five_hour;
    if (fh && typeof fh.used_percentage === 'number' && fh.resets_at) {
      const f = path.join(os.homedir(), '.claude', 'claude-usage.txt');
      const tmp = f + '.tmp';
      fs.writeFileSync(tmp, `${fh.used_percentage} ${fh.resets_at}`);
      fs.renameSync(tmp, f);
    }
  } catch (e) {}
  process.stdout.write('');
});

// Windows + WSL interoperability check. Node.js standard library only.
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdirSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const workspace = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const options = new Map();
for (let i = 2; i < process.argv.length; i += 2) {
  const flag = process.argv[i];
  if (!['--windows-exe', '--linux-exe', '--distribution'].includes(flag) || !process.argv[i + 1]) {
    throw new Error('Usage: node tools/verify_interop.mjs [--windows-exe PATH] [--linux-exe WSL_PATH] [--distribution NAME]');
  }
  options.set(flag, process.argv[i + 1]);
}
const distribution = options.get('--distribution') ?? 'Ubuntu-22.04';
function run(program, args) {
  const result = spawnSync(program, args, { encoding: 'utf8' });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`${program} failed (${result.status}): ${result.stderr}`);
  return result.stdout.trim();
}
function linuxPath(value) {
  return run('wsl.exe', ['-d', distribution, '--exec', 'wslpath', '-a', value.replaceAll('\\', '/')]);
}
function linux(...args) {
  return run('wsl.exe', ['-d', distribution, '--exec', linuxExecutable, ...args]);
}
function hash(value) {
  return createHash('sha256').update(readFileSync(value)).digest('hex');
}
const windowsExecutable = path.resolve(options.get('--windows-exe') ?? path.join(workspace, 'build-windows', 'huffblock.exe'));
const linuxExecutable = options.get('--linux-exe') ?? linuxPath(path.join(workspace, 'build-release', 'huffblock'));
const parent = path.join(workspace, 'build-interop');
mkdirSync(parent, { recursive: true });
const work = mkdtempSync(path.join(parent, 'run-'));
const source = path.join(work, 'source.bin');
const bytes = Buffer.alloc(262181);
let state = 20261022;
for (let i = 0; i < bytes.length; ++i) {
  state ^= state << 13;
  state ^= state >>> 17;
  state ^= state << 5;
  bytes[i] = i < 65536 ? 65 : state & 255;
}
writeFileSync(source, bytes);
const sourceHash = hash(source);
for (const blockSize of [512, 65536]) {
  for (const workers of [1, 4]) {
    const windowsArchive = path.join(work, `windows-${blockSize}-${workers}.hfb`);
    const linuxArchive = path.join(work, `linux-${blockSize}-${workers}.hfb`);
    const windowsRestored = path.join(work, `windows-restored-${blockSize}-${workers}.bin`);
    const linuxRestored = path.join(work, `linux-restored-${blockSize}-${workers}.bin`);
    run(windowsExecutable, ['compress', '--threads', String(workers), '--block-size', String(blockSize), source, windowsArchive]);
    linux('decompress', '--threads', String(workers), linuxPath(windowsArchive), linuxPath(linuxRestored));
    linux('compress', '--threads', String(workers), '--block-size', String(blockSize), linuxPath(source), linuxPath(linuxArchive));
    run(windowsExecutable, ['decompress', '--threads', String(workers), linuxArchive, windowsRestored]);
    if (hash(windowsArchive) !== hash(linuxArchive)) throw new Error('Windows and Linux produced different archives');
    if (hash(windowsRestored) !== sourceHash || hash(linuxRestored) !== sourceHash) throw new Error('Interoperability round trip failed');
    console.log(`PASS Windows/Linux archive compatibility: block=${blockSize} workers=${workers}`);
  }
}
console.log(`Interoperability artifacts: ${work}`);

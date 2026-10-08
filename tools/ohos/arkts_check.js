// 不开 DevEco，用 OpenHarmony SDK 自带的 ohos-typescript 对 .ets 做类型检查 + ArkTS 1.1 linter。
// 用法：ETS_SDK=<sdk>/ets PROJ=<仓库>/harmony node tools/ohos/arkts_check.js
// 说明：只覆盖类型与 ArkTS 语法规则，不含 DevEco 编译期的 UI 装饰器校验，最终以 DevEco 构建为准。
const fs = require('fs');
const path = require('path');
const ETS = process.env.ETS_SDK;
const PROJ = process.env.PROJ;
const loader = path.join(ETS, 'build-tools/ets-loader');
const ts = require(path.join(loader, 'node_modules/typescript'));

const cfg = JSON.parse(fs.readFileSync(path.join(loader, 'tsconfig.json'), 'utf8'));
const parsed = ts.convertCompilerOptionsFromJson(cfg.compilerOptions, loader);
const options = Object.assign(parsed.options, {
  noEmit: true, skipLibCheck: true, allowJs: false, incremental: false,
  module: ts.ModuleKind.CommonJS, moduleResolution: ts.ModuleResolutionKind.NodeJs,
  target: ts.ScriptTarget.ES2021, lib: ['lib.es2021.d.ts'],
  needDoArkTsLinter: true, isCompatibleVersion: false,
  compatibleSdkVersion: 12, compatibleSdkVersionStage: 'beta1',
  etsLoaderPath: loader, packageManagerType: 'ohpm',
});
if (parsed.errors.length) console.log('option errors', parsed.errors.map(e => e.messageText));

const soDts = path.join(PROJ, 'entry/src/main/cpp/types/libentry/Index.d.ts');
function tryFiles(base) {
  for (const ext of ['.d.ts', '.d.ets', '.ets', '.ts']) if (fs.existsSync(base + ext)) return base + ext;
  return undefined;
}
const host = ts.createCompilerHost(options);
const origGet = host.getSourceFile.bind(host);
host.getSourceFile = (...a) => { const sf = origGet(...a); if (sf) sf.version = require('crypto').createHash('md5').update(sf.text).digest('hex'); return sf; };
host.resolveModuleNames = (names, containing) => names.map(name => {
  let f;
  if (name === 'libentry.so') f = soDts;
  else if (name.startsWith('@kit.')) f = tryFiles(path.join(ETS, 'kits', name));
  else if (name.startsWith('@ohos.') || name.startsWith('@system.')) f = tryFiles(path.join(ETS, 'api', name)) || tryFiles(path.join(ETS, 'arkts', name));
  else if (name.startsWith('@arkts.')) f = tryFiles(path.join(ETS, 'arkts', name));
  else if (name.startsWith('.')) f = tryFiles(path.resolve(path.dirname(containing), name)) || tryFiles(path.resolve(path.dirname(containing), name, 'index'));
  else {
    const r = ts.resolveModuleName(name, containing, options, host).resolvedModule;
    return r;
  }
  if (!f) return undefined;
  return { resolvedFileName: f, extension: f.endsWith('.d.ets') ? ts.Extension.Dets || '.d.ets' : f.endsWith('.d.ts') ? ts.Extension.Dts : f.endsWith('.ets') ? (ts.Extension.Ets || '.ets') : ts.Extension.Ts, isExternalLibraryImport: false };
});

const decl = path.join(loader, 'declarations');
const roots = [
  path.join(PROJ, 'entry/src/main/ets/pages/Index.ets'),
  path.join(PROJ, 'entry/src/main/ets/entryability/EntryAbility.ets'),
  path.join(PROJ, 'entry/src/main/ets/common/BackgroundKeeper.ets'),
  ...fs.readdirSync(decl).filter(f => f.endsWith('.d.ts')).map(f => path.join(decl, f)),
];
const builder = ts.createEmitAndSemanticDiagnosticsBuilderProgram(roots, options, host);
const program = builder.getProgram();
const mine = program.getSourceFiles().filter(sf => sf.fileName.startsWith(PROJ));
let diags = [...program.getOptionsDiagnostics(), ...program.getGlobalDiagnostics()];
for (const sf of mine) diags.push(...program.getSyntacticDiagnostics(sf), ...program.getSemanticDiagnostics(sf));

function show(label, list) {
  console.log(`== ${label}: ${list.length}`);
  for (const d of list) {
    const msg = ts.flattenDiagnosticMessageText(d.messageText, '\n');
    if (d.file) {
      const { line, character } = d.file.getLineAndCharacterOfPosition(d.start);
      console.log(`  ${path.relative(PROJ, d.file.fileName)}:${line + 1}:${character + 1} [${d.code}] ${msg}`);
    } else console.log(`  [${d.code}] ${msg}`);
  }
}
show('TypeScript 诊断', diags);
let failed = diags.length > 0;
try {
  let lint = ts.ArkTSLinter_1_1.runArkTSLinter(builder, undefined, undefined, 'ArkTS_1_1')
    .filter(d => d.file && d.file.fileName.startsWith(PROJ));
  show('ArkTS 1.1 linter', lint);
  failed = failed || lint.length > 0;
} catch (e) { console.log('linter 运行失败：', e.stack.split('\n').slice(0, 6).join('\n')); failed = true; }
process.exit(failed ? 1 : 0);

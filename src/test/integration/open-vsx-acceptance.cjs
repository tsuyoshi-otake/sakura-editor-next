'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
const { ExtensionLoader } = require('../../exthost/src/extension-loader.cjs');

const extensionPath = path.resolve(process.argv[2]);
const samplePath = path.resolve(process.argv[3]);
const profilePath = path.resolve(process.argv[4]);
const notifications = [];
const requests = [];
let activeExtensionId = 'unregistered-extension';

const transport = {
  notify(method, params) { notifications.push({ method, params }); },
  async request(method, params) {
    requests.push({ method, params });
    if (method === 'workbench/commands/list') return { commands: [] };
    if (method === 'workbench/extensions/ensureTrusted') return { trusted: true };
    if (method === 'workspace/findFiles') return { uris: [] };
    if (method === 'workspace/configuration/update') return {};
    if (method === 'env/clipboard/readText') return { value: '' };
    if (method === 'env/clipboard/writeText') return {};
    if (method === 'workbench/notification/show') return {};
    if (method === 'workbench/context/set') return {};
    if (method.startsWith('secrets/')) return {};
    throw new Error(`UnsupportedCapability: ${activeExtensionId} requires ${method}`);
  },
};

async function main() {
  const documentId = `${process.pid}:1`;
  const sampleUri = pathToFileURL(samplePath).href;
  const sampleText = fs.readFileSync(samplePath, 'utf8').replace(/^\uFEFF/, '');
  const loader = new ExtensionLoader(1, transport, {
    language: 'ja',
    workspaceFolders: [{ uri: pathToFileURL(path.dirname(samplePath)).href, name: 'acceptance', index: 0 }],
    documents: [{
      documentId,
      uri: sampleUri,
      fileName: samplePath,
      languageId: samplePath.endsWith('.md') ? 'markdown' : 'plaintext',
      version: 1,
      isDirty: false,
      isUntitled: false,
      text: sampleText,
      eol: 1,
      encoding: 'utf8',
    }],
    activeDocumentId: documentId,
  });
  try {
    const registered = loader.register([{
      path: extensionPath,
      storagePath: path.join(profilePath, 'workspace-storage'),
      globalStoragePath: path.join(profilePath, 'global-storage'),
      logPath: path.join(profilePath, 'logs'),
    }]);
    if (registered.failed.length) throw new Error(JSON.stringify(registered.failed));
    activeExtensionId = registered.registered[0].extensionId;
    await Promise.race([
      loader.activate(activeExtensionId, 'onStartupFinished'),
      new Promise((_, reject) => setTimeout(
        () => reject(new Error(`activation timed out: ${activeExtensionId}`)), 5000)),
    ]);
    await new Promise((resolve) => setTimeout(resolve, activeExtensionId.includes('spell') ? 1500 : 500));
    const record = loader.extensions.get(activeExtensionId);
    let formatResult;
    if (activeExtensionId === 'esbenp.prettier-vscode') {
      formatResult = await Promise.race([
        record.session.invokeLanguageProvider('formatDocument', {
          documentId,
          options: { insertSpaces: true, tabSize: 2 },
        }),
        new Promise((_, reject) => setTimeout(
          () => reject(new Error(`formatting timed out: ${activeExtensionId}`)), 5000)),
      ]);
    }
    const diagnosticUpdates = notifications.filter((entry) => entry.method === 'languages/diagnostics/set');
    process.stdout.write(`${JSON.stringify({
      extensionId: activeExtensionId,
      activated: true,
      methods: [...new Set(notifications.map((entry) => entry.method))].sort(),
      requestMethods: [...new Set(requests.map((entry) => entry.method))].sort(),
      formatResult,
      diagnosticCount: diagnosticUpdates.reduce(
        (count, entry) => count + (entry.params.diagnostics?.length || 0), 0),
      editorOptions: record.session.activeEditor?.options,
      outputTail: notifications.filter((entry) => entry.method === 'workbench/output/append')
        .slice(-10).map((entry) => entry.params.value),
    })}\n`);
  } finally {
    await Promise.race([loader.dispose(), new Promise((resolve) => setTimeout(resolve, 1000))]);
  }
}

main().then(() => {
  setTimeout(() => process.exit(0), 50);
}).catch((error) => {
  process.stderr.write(`${error?.stack || error}\n`);
  const output = notifications.filter((entry) => entry.method === 'workbench/output/append').slice(-20);
  process.stderr.write(`${JSON.stringify({ extensionId: activeExtensionId, output })}\n`);
  setTimeout(() => process.exit(1), 50);
});

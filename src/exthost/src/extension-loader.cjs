'use strict';

const fs = require('node:fs');
const path = require('node:path');
const Module = require('node:module');
const { fileURLToPath, pathToFileURL } = require('node:url');
const { ExtensionApiSession } = require('./vscode-api.cjs');

const MAX_MANIFEST_BYTES = 4 * 1024 * 1024;
const moduleApis = new Map();
let originalModuleLoad = null;
let esmModuleHooks = null;
const esmRegistrySymbol = Symbol.for('sakura.editor.extensionApis');

function normalizeRoot(value) {
  const resolved = path.resolve(value);
  return process.platform === 'win32' ? resolved.toLowerCase() : resolved;
}

function isWithin(root, candidate) {
  const normalizedRoot = normalizeRoot(root);
  const normalizedCandidate = normalizeRoot(candidate);
  return normalizedCandidate === normalizedRoot || normalizedCandidate.startsWith(`${normalizedRoot}${path.sep}`);
}

function installVscodeBridge() {
  if (originalModuleLoad) return;
  originalModuleLoad = Module._load;
  Module._load = function sakuraLoad(request, parent, isMain) {
    if (request === 'vscode' && parent?.filename) {
      const filename = normalizeRoot(parent.filename);
      let best = null;
      for (const [root, api] of moduleApis) {
        if ((filename === root || filename.startsWith(`${root}${path.sep}`)) && (!best || root.length > best.root.length)) {
          best = { root, api };
        }
      }
      if (best) return best.api;
    }
    return originalModuleLoad.call(this, request, parent, isMain);
  };
  if (typeof Module.registerHooks === 'function' && !esmModuleHooks) {
    globalThis[esmRegistrySymbol] = moduleApis;
    esmModuleHooks = Module.registerHooks({
      resolve(specifier, context, nextResolve) {
        if (specifier !== 'vscode' || !context?.parentURL?.startsWith('file:')) return nextResolve(specifier, context);
        let filename;
        try { filename = normalizeRoot(fileURLToPath(context.parentURL)); } catch { return nextResolve(specifier, context); }
        let bestRoot = null;
        for (const root of moduleApis.keys()) {
          if ((filename === root || filename.startsWith(`${root}${path.sep}`)) && (!bestRoot || root.length > bestRoot.length)) {
            bestRoot = root;
          }
        }
        if (!bestRoot) return nextResolve(specifier, context);
        return { url: `sakura-vscode:${Buffer.from(bestRoot, 'utf8').toString('base64url')}`, shortCircuit: true };
      },
      load(url, context, nextLoad) {
        if (!url.startsWith('sakura-vscode:')) return nextLoad(url, context);
        const root = Buffer.from(url.slice('sakura-vscode:'.length), 'base64url').toString('utf8');
        const api = moduleApis.get(root);
        if (!api) throw new Error(`VS Code ESM bridge has no API session for ${root}`);
        const exports = Object.keys(api).filter((key) => /^[A-Za-z_$][\w$]*$/.test(key) && key !== 'default');
        const source = [
          `const api = globalThis[Symbol.for('sakura.editor.extensionApis')].get(${JSON.stringify(root)});`,
          `if (!api) throw new Error('VS Code API session is no longer active');`,
          'export default api;',
          ...exports.map((key) => `export const ${key} = api[${JSON.stringify(key)}];`),
        ].join('\n');
        return { format: 'module', source, shortCircuit: true };
      },
    });
  }
}

function maybeUninstallVscodeBridge() {
  if (!originalModuleLoad || moduleApis.size !== 0) return;
  Module._load = originalModuleLoad;
  originalModuleLoad = null;
  esmModuleHooks?.deregister?.();
  esmModuleHooks = null;
  delete globalThis[esmRegistrySymbol];
}

function readManifest(extensionPath) {
  const root = fs.realpathSync.native(path.resolve(extensionPath));
  const stat = fs.lstatSync(root);
  if (!stat.isDirectory() || stat.isSymbolicLink()) throw new Error('extension root must be a non-symlink directory');
  const manifestPath = path.join(root, 'package.json');
  const manifestStat = fs.lstatSync(manifestPath);
  if (!manifestStat.isFile() || manifestStat.isSymbolicLink() || manifestStat.size > MAX_MANIFEST_BYTES) {
    throw new Error('extension package.json is missing, unsafe, or too large');
  }
  const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
  if (!manifest || typeof manifest !== 'object' || Array.isArray(manifest)) throw new Error('extension package.json must be an object');
  const publisher = typeof manifest.publisher === 'string' && manifest.publisher ? manifest.publisher : undefined;
  const name = typeof manifest.name === 'string' && manifest.name ? manifest.name : undefined;
  if (!publisher || !name || !/^[\w.-]+$/.test(publisher) || !/^[\w.-]+$/.test(name)) {
    throw new Error('extension publisher and name must be safe non-empty identifiers');
  }
  return { root, manifest, extensionId: `${publisher}.${name}`.toLowerCase() };
}

function contributionCommands(manifest) {
  const commands = Array.isArray(manifest?.contributes?.commands) ? manifest.contributes.commands : [];
  return commands.flatMap((entry) => {
    if (!entry || typeof entry.command !== 'string' || !entry.command) return [];
    return [{
      id: entry.command,
      title: typeof entry.title === 'string' ? entry.title : entry.command,
      category: typeof entry.category === 'string' ? entry.category : '',
      enablement: typeof entry.enablement === 'string' ? entry.enablement : '',
    }];
  });
}

function contributionViews(manifest) {
  const result = [];
  const containers = manifest?.contributes?.views;
  if (!containers || typeof containers !== 'object' || Array.isArray(containers)) return result;
  for (const [containerId, views] of Object.entries(containers)) {
    if (!Array.isArray(views)) continue;
    for (const view of views) {
      if (!view || typeof view.id !== 'string' || !view.id) continue;
      result.push({ id: view.id, name: typeof view.name === 'string' ? view.name : view.id, containerId });
    }
  }
  return result;
}

function contributionConfigurationDefaults(manifest) {
  const defaults = Object.create(null);
  const configurations = Array.isArray(manifest?.contributes?.configuration)
    ? manifest.contributes.configuration : [manifest?.contributes?.configuration];
  for (const configuration of configurations) {
    if (!configuration?.properties || typeof configuration.properties !== 'object') continue;
    for (const [key, schema] of Object.entries(configuration.properties)) {
      if (schema && Object.prototype.hasOwnProperty.call(schema, 'default')) defaults[key] = schema.default;
      else if (schema?.type === 'array') defaults[key] = [];
      else if (schema?.type === 'object') defaults[key] = {};
    }
  }
  if (manifest?.contributes?.configurationDefaults && typeof manifest.contributes.configurationDefaults === 'object') {
    Object.assign(defaults, manifest.contributes.configurationDefaults);
  }
  return defaults;
}

function activationEvents(record) {
  const explicit = Array.isArray(record.manifest.activationEvents)
    ? record.manifest.activationEvents.filter((value) => typeof value === 'string') : [];
  const inferred = [
    ...record.commands.map((command) => `onCommand:${command.id}`),
    ...record.views.map((view) => `onView:${view.id}`),
  ];
  return new Set([...explicit, ...inferred]);
}

function resolveMainEntry(root, main) {
  const candidate = path.resolve(root, main);
  if (!isWithin(root, candidate)) throw new Error(`extension main escapes extension root: ${main}`);
  let resolved;
  try {
    resolved = require.resolve(candidate);
  } catch {
    throw new Error(`extension main is missing or unsafe: ${main}`);
  }
  if (!isWithin(root, resolved)) throw new Error(`extension main escapes extension root: ${main}`);
  const stat = fs.lstatSync(resolved);
  const realEntry = fs.realpathSync.native(resolved);
  if (!stat.isFile() || stat.isSymbolicLink() || !isWithin(root, realEntry)) {
    throw new Error(`extension main is missing or unsafe: ${main}`);
  }
  return realEntry;
}

class ExtensionLoader {
  constructor(generation, transport, options = {}) {
    if (!Number.isSafeInteger(generation) || generation <= 0) throw new TypeError('generation must be positive');
    this.generation = generation;
    this.transport = transport;
    this.options = options;
    this.extensions = new Map();
    this.commandOwners = new Map();
    this.viewOwners = new Map();
    this.disposed = false;
  }

  register(extensionPaths) {
    if (this.disposed) throw new Error('extension loader is disposed');
    if (!Array.isArray(extensionPaths)) throw new TypeError('extensions must be an array');
    const registered = [];
    const failed = [];
    for (const descriptor of extensionPaths) {
      try {
        const extensionPath = typeof descriptor === 'string' ? descriptor : descriptor?.path;
        if (typeof extensionPath !== 'string' || !extensionPath) throw new TypeError('extension path is required');
        const parsed = readManifest(extensionPath);
        const existing = this.extensions.get(parsed.extensionId);
        if (existing) {
          if (normalizeRoot(existing.root) !== normalizeRoot(parsed.root)) {
            throw new Error(`extension ID is already registered from a different path: ${parsed.extensionId}`);
          }
          registered.push(existing.metadata);
          this.transport.notify('workbench/extensions/register', existing.metadata);
          continue;
        }
        const record = {
          ...parsed,
          commands: contributionCommands(parsed.manifest),
          views: contributionViews(parsed.manifest),
          configurationDefaults: contributionConfigurationDefaults(parsed.manifest),
          events: null,
          state: 'registered',
          session: null,
          context: null,
          exports: undefined,
          activation: null,
          paths: typeof descriptor === 'object' && descriptor ? descriptor : {},
        };
        record.events = activationEvents(record);
        this.extensions.set(record.extensionId, record);
        for (const command of record.commands) {
          if (!this.commandOwners.has(command.id)) this.commandOwners.set(command.id, record.extensionId);
        }
        for (const view of record.views) {
          if (!this.viewOwners.has(view.id)) this.viewOwners.set(view.id, record.extensionId);
        }
        const metadata = {
          extensionId: record.extensionId,
          generation: this.generation,
          version: typeof record.manifest.version === 'string' ? record.manifest.version : '',
          displayName: typeof record.manifest.displayName === 'string' ? record.manifest.displayName : record.extensionId,
          commands: record.commands,
          views: record.views,
          activationEvents: [...record.events],
        };
        record.metadata = metadata;
        registered.push(metadata);
        this.transport.notify('workbench/extensions/register', metadata);
      } catch (error) {
        failed.push({ path: typeof descriptor === 'string' ? descriptor : descriptor?.path, message: error instanceof Error ? error.message : String(error) });
      }
    }
    return { registered, failed };
  }

  async activate(extensionId, reason = '*') {
    if (this.disposed) throw new Error('extension loader is disposed');
    const record = this.extensions.get(String(extensionId).toLowerCase());
    if (!record) throw new Error(`extension is not registered: ${extensionId}`);
    if (record.state === 'active') return record.exports;
    if (record.activation) return record.activation;
    record.activation = this.activateRecord(record, reason);
    try {
      return await record.activation;
    } finally {
      record.activation = null;
    }
  }

  async activateRecord(record, reason) {
    record.state = 'activating';
    const trust = await this.transport.request('workbench/extensions/ensureTrusted', {
      extensionId: record.extensionId,
      version: typeof record.manifest.version === 'string' ? record.manifest.version : '',
      displayName: typeof record.manifest.displayName === 'string' ? record.manifest.displayName : record.extensionId,
      extensionPath: record.root,
    });
    if (trust?.trusted !== true) {
      record.state = 'blocked';
      this.transport.notify('workbench/extensions/didBlockActivation', {
        extensionId: record.extensionId, generation: this.generation, reason: 'TrustDenied',
      });
      throw new Error(`ExtensionTrustDenied: ${record.extensionId}`);
    }
    const session = new ExtensionApiSession(record.extensionId, this.generation, this.transport, {
      ...this.options,
      configurationDefaults: { ...(this.options.configurationDefaults || {}), ...record.configurationDefaults },
    });
    record.session = session;
    const normalizedRoot = normalizeRoot(record.root);
    try {
      record.context = session.createExtensionContext({
        extensionPath: record.root,
        storagePath: record.paths.storagePath,
        globalStoragePath: record.paths.globalStoragePath,
         logPath: record.paths.logPath,
        packageJSON: record.manifest,
      });
      const main = typeof record.manifest.main === 'string' ? record.manifest.main : undefined;
      if (!main) {
        record.exports = {};
        record.state = 'active';
        this.transport.notify('workbench/extensions/didActivate', {
          extensionId: record.extensionId, generation: this.generation, reason,
        });
        return record.exports;
      }
      const entry = resolveMainEntry(record.root, main);
      installVscodeBridge();
      moduleApis.set(normalizedRoot, session.api);
      let moduleExports;
      try {
        moduleExports = require(entry);
      } catch (error) {
        if (error?.code !== 'ERR_REQUIRE_ESM') throw error;
        moduleExports = await import(pathToFileURL(entry).href);
      }
      record.exports = typeof moduleExports?.activate === 'function'
        ? await moduleExports.activate(record.context) : moduleExports;
      record.module = moduleExports;
      record.state = 'active';
      this.transport.notify('workbench/extensions/didActivate', {
        extensionId: record.extensionId, generation: this.generation, reason,
      });
      return record.exports;
    } catch (error) {
      record.state = 'failed';
      session.dispose();
      record.session = null;
      record.context = null;
      moduleApis.delete(normalizedRoot);
      maybeUninstallVscodeBridge();
      this.transport.notify('workbench/extensions/didFailActivation', {
        extensionId: record.extensionId, generation: this.generation,
        message: error instanceof Error ? error.message : String(error),
      });
      throw error;
    }
  }

  async activateByEvent(event) {
    const activated = [];
    for (const record of this.extensions.values()) {
      if (record.state === 'registered' && (record.events.has(event) || record.events.has('*'))) {
        await this.activate(record.extensionId, event);
        activated.push(record.extensionId);
      }
    }
    return { activated };
  }

  sessionForHandle(kind, handle) {
    for (const record of this.extensions.values()) {
      if (!record.session) continue;
      if (kind === 'view' && record.session.views.has(handle)) return record.session;
      if (kind === 'progress' && record.session.progress.has(handle)) return record.session;
      if (kind === 'provider') {
        for (const providers of record.session.languageProviders.values()) if (providers.has(handle)) return record.session;
      }
    }
    return null;
  }

  async handleRequest(method, params) {
    if (method === 'extension/commands/execute') {
      const command = params?.command;
      const owner = this.commandOwners.get(command);
      if (owner) await this.activate(owner, `onCommand:${command}`);
      const session = owner ? this.extensions.get(owner)?.session : null;
      if (!session) throw new Error(`command handler is not registered: ${command}`);
      return session.handleRequest(method, params);
    }
    if (method.startsWith('extension/views/')) {
      let session = this.sessionForHandle('view', params?.handle);
      if (!session && typeof params?.viewId === 'string') {
        const owner = this.viewOwners.get(params.viewId);
        if (owner) await this.activate(owner, `onView:${params.viewId}`);
        session = owner ? this.extensions.get(owner)?.session : null;
      }
      if (!session) throw new Error(`tree view is not registered: ${params?.handle || params?.viewId}`);
      return session.handleRequest(method, params);
    }
    if (method === 'extension/progress/cancel') {
      return this.sessionForHandle('progress', params?.handle)?.handleRequest(method, params) ?? { accepted: false };
    }
    if (method === 'extension/secrets/didChange') {
      const record = this.extensions.get(String(params?.extensionId || '').toLowerCase());
      return record?.session?.handleRequest(method, params) ?? { accepted: false };
    }
    if (method.startsWith('extension/workspace/')) {
      if (method === 'extension/workspace/didOpen' && typeof params?.snapshot?.languageId === 'string') {
        await this.activateByEvent(`onLanguage:${params.snapshot.languageId}`);
      }
      const sessions = [...this.extensions.values()].map((record) => record.session).filter(Boolean);
      if (method === 'extension/workspace/willSave') {
        const results = await Promise.all(sessions.map((session) => session.handleRequest(method, params)));
        return {
          edits: results.flatMap((result) => result?.edits || []).slice(0, 10000),
          expectedVersion: results.find((result) => result?.expectedVersion !== undefined)?.expectedVersion,
        };
      }
      const results = await Promise.all(sessions.map((session) => session.handleRequest(method, params)));
      return { accepted: results.filter((result) => result?.accepted).length };
    }
    if (method.startsWith('extension/window/')) {
      const sessions = [...this.extensions.values()].map((record) => record.session).filter(Boolean);
      const results = await Promise.all(sessions.map((session) => session.handleRequest(method, params)));
      return { accepted: results.filter((result) => result?.accepted).length };
    }
    if (method === 'extension/languages/provide') {
      const direct = typeof params?.handle === 'string' ? this.sessionForHandle('provider', params.handle) : null;
      if (direct) return direct.handleRequest(method, params);
      const values = [];
      for (const record of this.extensions.values()) {
        if (!record.session) continue;
        const result = await record.session.handleRequest(method, params);
        if (result?.value !== undefined) values.push(result);
      }
      if (values.length === 0) return { value: undefined };
      if (values.length === 1) return values[0];
      return {
        value: values.every((result) => Array.isArray(result.value)) ? values.flatMap((result) => result.value) : values.map((result) => result.value),
        expectedVersion: values[0].expectedVersion,
      };
    }
    throw new Error(`unsupported extension client method: ${method}`);
  }

  async dispose() {
    if (this.disposed) return;
    this.disposed = true;
    for (const record of [...this.extensions.values()].reverse()) {
      if (record.state === 'active' && typeof record.module?.deactivate === 'function') {
        try { await record.module.deactivate(); } catch {}
      }
      if (record.context?.subscriptions) {
        for (const subscription of [...record.context.subscriptions].reverse()) {
          try { subscription?.dispose?.(); } catch {}
        }
      }
      record.session?.dispose();
      moduleApis.delete(normalizeRoot(record.root));
      record.state = 'disposed';
    }
    this.extensions.clear();
    this.commandOwners.clear();
    this.viewOwners.clear();
    maybeUninstallVscodeBridge();
  }
}

module.exports = {
  ExtensionLoader,
  MAX_MANIFEST_BYTES,
  contributionCommands,
  contributionConfigurationDefaults,
  contributionViews,
  isWithin,
  readManifest,
  resolveMainEntry,
};

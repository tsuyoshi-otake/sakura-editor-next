'use strict';

const net = require('node:net');
const process = require('node:process');
const { AsyncLocalStorage } = require('node:async_hooks');
const { TextDecoder } = require('node:util');
const { ExtensionApiSession } = require('./vscode-api.cjs');
const { ExtensionLoader } = require('./extension-loader.cjs');

const MAX_PAYLOAD_BYTES = 16 * 1024 * 1024;
const MAX_IN_FLIGHT_REQUESTS = 1024;
const MAX_QUEUED_WRITE_BYTES = 32 * 1024 * 1024;
const PROTOCOL_VERSION = 1;

class ProtocolError extends Error {
  constructor(message) {
    super(message);
    this.name = 'ProtocolError';
  }
}

class RpcError extends Error {
  constructor(code, message, data) {
    super(message);
    this.name = 'RpcError';
    this.code = code;
    this.data = data;
  }
}

class FrameDecoder {
  constructor(maxPayloadBytes = MAX_PAYLOAD_BYTES) {
    if (!Number.isSafeInteger(maxPayloadBytes) || maxPayloadBytes < 0) {
      throw new RangeError('maxPayloadBytes must be a non-negative safe integer');
    }
    this.maxPayloadBytes = maxPayloadBytes;
    this.header = Buffer.alloc(4);
    this.headerBytes = 0;
    this.payload = null;
    this.payloadBytes = 0;
    this.failed = false;
  }

  feed(chunk) {
    if (this.failed) {
      throw new ProtocolError('frame decoder is already in a terminal state');
    }
    if (!Buffer.isBuffer(chunk)) {
      chunk = Buffer.from(chunk);
    }

    const frames = [];
    let offset = 0;
    try {
      while (offset < chunk.length) {
        if (this.payload === null) {
          const count = Math.min(4 - this.headerBytes, chunk.length - offset);
          chunk.copy(this.header, this.headerBytes, offset, offset + count);
          this.headerBytes += count;
          offset += count;
          if (this.headerBytes !== 4) {
            continue;
          }

          const length = this.header.readUInt32BE(0);
          if (length > this.maxPayloadBytes) {
            throw new ProtocolError(`frame payload exceeds ${this.maxPayloadBytes} bytes`);
          }
          this.payload = Buffer.allocUnsafe(length);
          this.payloadBytes = 0;
          if (length === 0) {
            frames.push(Buffer.alloc(0));
            this.resetFrame();
          }
          continue;
        }

        const count = Math.min(this.payload.length - this.payloadBytes, chunk.length - offset);
        chunk.copy(this.payload, this.payloadBytes, offset, offset + count);
        this.payloadBytes += count;
        offset += count;
        if (this.payloadBytes === this.payload.length) {
          frames.push(this.payload);
          this.resetFrame();
        }
      }
      return frames;
    } catch (error) {
      this.failed = true;
      this.payload = null;
      throw error;
    }
  }

  resetFrame() {
    this.headerBytes = 0;
    this.payload = null;
    this.payloadBytes = 0;
  }
}

function encodeFrame(message, maxPayloadBytes = MAX_PAYLOAD_BYTES) {
  const payload = Buffer.from(JSON.stringify(message), 'utf8');
  if (payload.length > maxPayloadBytes || payload.length > 0xffffffff) {
    throw new ProtocolError(`frame payload exceeds ${maxPayloadBytes} bytes`);
  }
  const frame = Buffer.allocUnsafe(4 + payload.length);
  frame.writeUInt32BE(payload.length, 0);
  payload.copy(frame, 4);
  return frame;
}

function isRpcId(value) {
  return typeof value === 'string' || (typeof value === 'number' && Number.isFinite(value));
}

function pendingKey(id) {
  return `${typeof id}:${String(id)}`;
}

function parseRpcPayload(payload) {
  let text;
  try {
    text = new TextDecoder('utf-8', { fatal: true }).decode(payload);
  } catch {
    throw new ProtocolError('JSON-RPC payload is not valid UTF-8');
  }

  let message;
  try {
    message = JSON.parse(text);
  } catch {
    throw new ProtocolError('JSON-RPC payload is not valid JSON');
  }
  if (!message || typeof message !== 'object' || Array.isArray(message) || message.jsonrpc !== '2.0') {
    throw new ProtocolError('JSON-RPC payload must be a 2.0 object');
  }
  return message;
}

class RpcPeer {
  constructor(socket, requestHandler, options = {}) {
    this.socket = socket;
    this.requestHandler = requestHandler;
    this.decoder = new FrameDecoder(options.maxPayloadBytes);
    this.pending = new Map();
    this.incoming = new Map();
    this.nextRequestId = 1;
    this.closed = false;
    this.closeReason = null;

    socket.on('data', (chunk) => this.onData(chunk));
    socket.on('error', (error) => this.close(error));
    socket.on('close', () => this.close(this.closeReason || new Error('extension host connection closed')));
  }

  notify(method, params) {
    this.ensureOpen();
    this.send({ jsonrpc: '2.0', method, ...(params === undefined ? {} : { params }) });
  }

  request(method, params, options = {}) {
    this.ensureOpen();
    if (this.pending.size >= MAX_IN_FLIGHT_REQUESTS) {
      return Promise.reject(new RpcError(-32001, 'too many in-flight requests'));
    }

    const id = `host-${this.nextRequestId++}`;
    const key = pendingKey(id);
    return new Promise((resolve, reject) => {
      const pending = { resolve, reject, timer: null, abortListener: null };
      if (Number.isFinite(options.timeoutMs) && options.timeoutMs > 0) {
        pending.timer = setTimeout(() => {
          this.pending.delete(key);
          this.notify('$/cancelRequest', { id });
          reject(new RpcError(-32800, `request timed out after ${options.timeoutMs} ms`));
        }, options.timeoutMs);
      }
      if (options.signal) {
        pending.abortListener = () => {
          if (!this.pending.delete(key)) return;
          if (pending.timer) clearTimeout(pending.timer);
          this.notify('$/cancelRequest', { id });
          reject(new RpcError(-32800, 'request cancelled'));
        };
        options.signal.addEventListener('abort', pending.abortListener, { once: true });
      }
      this.pending.set(key, pending);
      try {
        this.send({ jsonrpc: '2.0', id, method, ...(params === undefined ? {} : { params }) });
      } catch (error) {
        this.finishPending(key, error);
      }
    });
  }

  send(message) {
    this.ensureOpen();
    const frame = encodeFrame(message, this.decoder.maxPayloadBytes);
    if (this.socket.writableLength + frame.length > MAX_QUEUED_WRITE_BYTES) {
      throw new RpcError(-32002, 'extension host write queue limit exceeded');
    }
    this.socket.write(frame);
  }

  onData(chunk) {
    if (this.closed) return;
    try {
      for (const payload of this.decoder.feed(chunk)) {
        this.dispatch(parseRpcPayload(payload));
      }
    } catch (error) {
      this.close(error);
      this.socket.destroy(error);
    }
  }

  dispatch(message) {
    const hasMethod = Object.prototype.hasOwnProperty.call(message, 'method');
    const hasId = Object.prototype.hasOwnProperty.call(message, 'id');
    const hasResult = Object.prototype.hasOwnProperty.call(message, 'result');
    const hasError = Object.prototype.hasOwnProperty.call(message, 'error');

    if (hasMethod) {
      if (typeof message.method !== 'string' || message.method.length === 0 || hasResult || hasError) {
        throw new ProtocolError('invalid JSON-RPC request or notification');
      }
      if (hasId && !isRpcId(message.id)) {
        throw new ProtocolError('JSON-RPC request id must be a string or finite number');
      }
      if (Object.prototype.hasOwnProperty.call(message, 'params') &&
          (!message.params || typeof message.params !== 'object')) {
        throw new ProtocolError('JSON-RPC params must be an object or array');
      }
      if (message.method === '$/cancelRequest') {
        this.cancelIncoming(message.params);
      } else if (hasId) {
        this.handleRequest(message);
      } else {
        this.handleNotification(message);
      }
      return;
    }

    if (!hasId || !isRpcId(message.id) || hasResult === hasError) {
      throw new ProtocolError('invalid JSON-RPC response');
    }
    this.handleResponse(message);
  }

  handleRequest(message) {
    if (this.incoming.size >= MAX_IN_FLIGHT_REQUESTS) {
      this.sendError(message.id, new RpcError(-32001, 'too many in-flight requests'));
      return;
    }
    const key = pendingKey(message.id);
    if (this.incoming.has(key)) {
      throw new ProtocolError('duplicate in-flight JSON-RPC request id');
    }
    const controller = new AbortController();
    this.incoming.set(key, controller);
    Promise.resolve()
      .then(() => this.requestHandler(message.method, message.params, controller.signal, this))
      .then((result) => {
        if (!this.closed) this.send({ jsonrpc: '2.0', id: message.id, result: result ?? null });
      })
      .catch((error) => {
        if (!this.closed) this.sendError(message.id, error);
      })
      .finally(() => this.incoming.delete(key));
  }

  handleNotification(message) {
    Promise.resolve()
      .then(() => this.requestHandler(message.method, message.params, undefined, this))
      .catch(() => {});
  }

  cancelIncoming(params) {
    if (!params || !isRpcId(params.id)) return;
    this.incoming.get(pendingKey(params.id))?.abort();
  }

  handleResponse(message) {
    const key = pendingKey(message.id);
    const pending = this.pending.get(key);
    if (!pending) return;
    this.pending.delete(key);
    this.cleanupPending(pending);
    if (Object.prototype.hasOwnProperty.call(message, 'error')) {
      const error = message.error;
      if (!error || typeof error !== 'object' || !Number.isInteger(error.code) || typeof error.message !== 'string') {
        throw new ProtocolError('invalid JSON-RPC error response');
      }
      pending.reject(new RpcError(error.code, error.message, error.data));
    } else {
      pending.resolve(message.result);
    }
  }

  sendError(id, error) {
    const rpcError = error instanceof RpcError
      ? error
      : new RpcError(-32603, error instanceof Error ? error.message : 'internal extension host error');
    this.send({
      jsonrpc: '2.0',
      id,
      error: {
        code: rpcError.code,
        message: rpcError.message,
        ...(rpcError.data === undefined ? {} : { data: rpcError.data }),
      },
    });
  }

  finishPending(key, error) {
    const pending = this.pending.get(key);
    if (!pending) return;
    this.pending.delete(key);
    this.cleanupPending(pending);
    pending.reject(error);
  }

  cleanupPending(pending) {
    if (pending.timer) clearTimeout(pending.timer);
    if (pending.abortListener) {
      // AbortSignal removes a once-listener after it fires. removeEventListener is safe either way.
      // The signal itself is not retained separately, so the listener only closes over this pending request.
    }
  }

  ensureOpen() {
    if (this.closed || this.socket.destroyed) {
      throw this.closeReason || new Error('extension host connection is closed');
    }
  }

  close(reason = new Error('extension host connection closed')) {
    if (this.closed) return;
    this.closed = true;
    this.closeReason = reason;
    for (const controller of this.incoming.values()) controller.abort();
    this.incoming.clear();
    for (const [key] of this.pending) this.finishPending(key, reason);
  }
}

function readHostConfig(environment = process.env) {
  const required = (name) => {
    const value = environment[name];
    if (typeof value !== 'string' || value.length === 0) {
      throw new Error(`missing required environment variable ${name}`);
    }
    return value;
  };
  if (required('SAKURA_EXTENSION_HOST') !== '1') {
    throw new Error('SAKURA_EXTENSION_HOST must be 1');
  }
  const profileHash = required('SAKURA_PROFILE_HASH');
  const bootId = required('SAKURA_BOOT_ID');
  const pipeName = required('SAKURA_PIPE_NAME');
  if (!/^[0-9a-f]{32}$/.test(profileHash) || !/^[0-9a-f]{32}$/.test(bootId)) {
    throw new Error('profile hash and boot ID must be 128-bit lowercase hexadecimal values');
  }
  const expectedPipeName = `\\\\.\\pipe\\sakura-exthost-${profileHash}-${bootId}`;
  if (pipeName !== expectedPipeName) {
    throw new Error('SAKURA_PIPE_NAME does not match the trusted profile identity');
  }
  const generation = Number(required('SAKURA_GENERATION'));
  const brokerProcessId = Number(required('SAKURA_BROKER_PID'));
  if (!Number.isSafeInteger(generation) || generation <= 0) {
    throw new Error('SAKURA_GENERATION must be a positive safe integer');
  }
  if (!Number.isInteger(brokerProcessId) || brokerProcessId <= 0 || brokerProcessId > 0xffffffff) {
    throw new Error('SAKURA_BROKER_PID must be a positive 32-bit process ID');
  }
  return { profileHash, bootId, pipeName, generation, brokerProcessId };
}

class MultiplexTransport {
  constructor() {
    this.peers = new Set();
    this.context = new AsyncLocalStorage();
    this.lastPeer = null;
    this.replayEntries = new Map();
  }

  add(peer) {
    this.peers.add(peer);
    this.lastPeer = peer;
  }

  remove(peer) {
    this.peers.delete(peer);
    if (this.lastPeer === peer) this.lastPeer = [...this.peers].at(-1) || null;
  }

  withPeer(peer, callback) {
    if (this.peers.has(peer)) this.lastPeer = peer;
    return this.context.run(peer, callback);
  }

  request(method, params, options) {
    const contextualPeer = this.context.getStore();
    const peer = this.peers.has(contextualPeer) ? contextualPeer : this.lastPeer;
    if (!peer || !this.peers.has(peer)) {
      return Promise.reject(new RpcError(-32003, 'no editor connection is available'));
    }
    return peer.request(method, params, options);
  }

  notify(method, params) {
    this.updateReplayState(method, params);
    for (const peer of this.peers) {
      try { peer.notify(method, params); } catch {}
    }
  }

  replay(peer) {
    if (!this.peers.has(peer)) return;
    for (const { method, params } of this.replayEntries.values()) {
      try { peer.notify(method, params); } catch { return; }
    }
  }

  updateReplayState(method, params) {
    const handle = typeof params?.handle === 'string' ? params.handle : '';
    const command = typeof params?.command === 'string' ? params.command : '';
    const extensionId = typeof params?.extensionId === 'string' ? params.extensionId : '';
    const contextKey = typeof params?.key === 'string' ? params.key : '';
    const generation = params?.generation;
    const put = (key) => this.replayEntries.set(key, { method, params });
    const remove = (...keys) => keys.forEach((key) => this.replayEntries.delete(key));

    switch (method) {
      case 'workbench/extensions/register':
        if (extensionId) put(`extension:${extensionId}`);
        break;
      case 'workbench/commands/registerHandler':
        if (command) put(`command:${command}`);
        break;
      case 'workbench/commands/unregisterHandler':
        if (command) remove(`command:${command}`);
        break;
      case 'workbench/context/set':
        if (contextKey) put(`context:${contextKey}`);
        break;
      case 'workbench/statusBar/update':
        if (handle) put(`status:${handle}`);
        break;
      case 'workbench/statusBar/remove':
        if (handle) remove(`status:${handle}`);
        break;
      case 'workbench/views/register':
        if (handle) put(`view-register:${handle}`);
        break;
      case 'workbench/views/update':
        if (handle) put(`view-update:${handle}`);
        break;
      case 'workbench/views/unregister':
        if (handle) remove(`view-register:${handle}`, `view-update:${handle}`);
        break;
      case 'workbench/output/create':
        if (handle) put(`output-create:${handle}`);
        break;
      case 'workbench/output/replace':
        if (handle) put(`output-replace:${handle}`);
        break;
      case 'workbench/output/dispose':
        if (handle) remove(`output-create:${handle}`, `output-replace:${handle}`);
        break;
      case 'workbench/extensions/removeGeneration':
        for (const [key, entry] of this.replayEntries) {
          if (entry.params?.generation === generation && entry.params?.extensionId === extensionId) {
            this.replayEntries.delete(key);
          }
        }
        break;
    }
  }
}

class ExtensionHost {
  constructor(config, options = {}) {
    this.config = config;
    this.processApi = options.processApi || process;
    this.server = net.createServer((socket) => this.accept(socket));
    this.peers = new Set();
    this.transport = new MultiplexTransport();
    this.extensionLoader = new ExtensionLoader(this.config.generation, this.transport);
    this.startedAt = Date.now();
    this.monitor = null;
    this.stopping = false;
    this.ready = false;
    this.server.on('error', (error) => {
      if (!this.stopping) this.fatal(error);
    });
  }

  start() {
    return new Promise((resolve, reject) => {
      const onError = (error) => reject(error);
      this.server.once('error', onError);
      this.server.listen(this.config.pipeName, () => {
        this.server.off('error', onError);
        this.ready = true;
        this.monitor = setInterval(() => this.checkBroker(), 2000);
        this.monitor.unref();
        resolve();
      });
    });
  }

  accept(socket) {
    socket.setNoDelay(true);
    const peer = new RpcPeer(socket, (method, params, signal, connection) =>
      this.handleRequest(method, params, signal, connection));
    this.peers.add(peer);
    this.transport.add(peer);
    socket.once('close', () => {
      this.peers.delete(peer);
      this.transport.remove(peer);
    });
    peer.notify('host/hello', this.info());
    this.transport.replay(peer);
  }

  info() {
    return {
      protocolVersion: PROTOCOL_VERSION,
      processId: this.processApi.pid,
      profileHash: this.config.profileHash,
      bootId: this.config.bootId,
      generation: this.config.generation,
    };
  }

  async handleRequest(method, params, signal, peer) {
    return this.transport.withPeer(peer, () => this.handleRequestForPeer(method, params, signal, peer));
  }

  async handleRequestForPeer(method, params, signal, peer) {
    switch (method) {
      case 'host/ping':
        return { ...this.info(), nonce: params?.nonce ?? null, uptimeMs: Date.now() - this.startedAt };
      case 'host/getDiagnostics':
        return {
          ...this.info(),
          connectionCount: this.peers.size,
          pendingOutboundRequests: [...this.peers].reduce((sum, item) => sum + item.pending.size, 0),
          activeInboundRequests: [...this.peers].reduce((sum, item) => sum + item.incoming.size, 0),
          uptimeMs: Date.now() - this.startedAt,
        };
      case 'host/requestClientEcho':
        return peer.request('client/echo', { value: params?.value ?? null }, { timeoutMs: 5000, signal });
      case 'host/quiesce':
        if (params?.generation !== this.config.generation) {
          throw new RpcError(-32010, 'stale extension host generation');
        }
        setImmediate(() => this.stop());
        return { accepted: true };
      case 'host/registerExtensions':
        return this.extensionLoader.register(params?.extensions);
      case 'host/activateExtension':
        await this.extensionLoader.activate(params?.extensionId, params?.reason || 'api');
        return { activated: true };
      case 'host/activateByEvent':
        return this.extensionLoader.activateByEvent(params?.event);
      case '$/cancelRequest':
        return null;
      default:
        if (method.startsWith('extension/')) {
          return this.extensionLoader.handleRequest(method, params, signal);
        }
        throw new RpcError(-32601, `method not found: ${method}`);
    }
  }

  checkBroker() {
    try {
      this.processApi.kill(this.config.brokerProcessId, 0);
    } catch (error) {
      if (error?.code !== 'EPERM') {
        this.fatal(new Error('extension host broker process is no longer available'));
      }
    }
  }

  async stop() {
    if (this.stopping) return;
    this.stopping = true;
    if (this.monitor) clearInterval(this.monitor);
    await this.extensionLoader.dispose();
    for (const peer of this.peers) {
      peer.close(new Error('extension host is stopping'));
      peer.socket.destroy();
    }
    this.peers.clear();
    if (!this.ready) return;
    await new Promise((resolve) => this.server.close(resolve));
    this.ready = false;
  }

  fatal(error) {
    if (this.stopping) return;
    this.stopping = true;
    if (this.monitor) clearInterval(this.monitor);
    this.extensionLoader.dispose().catch(() => {});
    for (const peer of this.peers) {
      peer.socket.destroy(error);
    }
    this.peers.clear();
    this.server.close(() => {});
    this.processApi.exitCode = 1;
  }
}

async function runMain() {
  const host = new ExtensionHost(readHostConfig());
  const shutdown = () => {
    host.stop().finally(() => process.exit(process.exitCode || 0));
  };
  process.once('SIGINT', shutdown);
  process.once('SIGTERM', shutdown);
  process.once('disconnect', shutdown);
  process.on('uncaughtException', (error) => {
    console.error(error);
    host.fatal(error);
  });
  process.on('unhandledRejection', (error) => {
    console.error(error);
    host.fatal(error instanceof Error ? error : new Error(String(error)));
  });
  await host.start();
}

if (require.main === module) {
  runMain().catch((error) => {
    console.error(error);
    process.exitCode = 1;
  });
}

module.exports = {
  ExtensionHost,
  ExtensionApiSession,
  ExtensionLoader,
  MultiplexTransport,
  FrameDecoder,
  MAX_PAYLOAD_BYTES,
  PROTOCOL_VERSION,
  ProtocolError,
  RpcError,
  RpcPeer,
  encodeFrame,
  parseRpcPayload,
  readHostConfig,
  runMain,
};

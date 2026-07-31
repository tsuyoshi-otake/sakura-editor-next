'use strict';

const process = require('node:process');
const fs = require('node:fs');
const path = require('node:path');
const { fileURLToPath, pathToFileURL } = require('node:url');

const OUTPUT_FLUSH_MS = 8;
const MAX_OUTPUT_BUFFER_CHARS = 1024 * 1024;
const MAX_OUTPUT_OPERATION_ID_LENGTH = 64;
let nextOutputOperationSession = 1;

function allocateOutputOperationPrefix(generation) {
  const session = nextOutputOperationSession;
  if (!Number.isSafeInteger(session)) {
    throw new RangeError('Output operation session ID space exhausted');
  }
  nextOutputOperationSession = session + 1;
  const prefix = `output-op-s${session}-g${generation}-`;
  if (prefix.length + String(Number.MAX_SAFE_INTEGER).length > MAX_OUTPUT_OPERATION_ID_LENGTH) {
    throw new RangeError('Output operation ID format exceeds its maximum length');
  }
  return prefix;
}

class UnsupportedCapabilityError extends Error {
  constructor(extensionId, capability) {
    super(`UnsupportedCapability: ${extensionId}: ${capability}`);
    this.name = 'UnsupportedCapabilityError';
    this.code = 'UnsupportedCapability';
    this.extensionId = extensionId;
    this.capability = capability;
  }
}

function requireString(value, name, allowEmpty = false) {
  if (typeof value !== 'string' || (!allowEmpty && value.length === 0) || value.includes('\0')) {
    throw new TypeError(`${name} must be a ${allowEmpty ? '' : 'non-empty '}string`);
  }
  return value;
}

class Disposable {
  constructor(callOnDispose = () => {}) {
    if (typeof callOnDispose !== 'function') throw new TypeError('callOnDispose must be a function');
    this.callOnDispose = callOnDispose;
    this.disposed = false;
  }

  dispose() {
    if (this.disposed) return;
    this.disposed = true;
    const callback = this.callOnDispose;
    this.callOnDispose = () => {};
    callback();
  }

  static from(...disposables) {
    return new Disposable(() => {
      for (const item of disposables) item?.dispose?.();
    });
  }
}

class EventEmitter {
  constructor() {
    this.listeners = new Set();
    this.disposed = false;
    this.event = (listener, thisArgs, disposables) => {
      if (this.disposed) throw new Error('EventEmitter is disposed');
      if (typeof listener !== 'function') throw new TypeError('listener must be a function');
      const entry = { listener, thisArgs };
      this.listeners.add(entry);
      const disposable = new Disposable(() => this.listeners.delete(entry));
      if (Array.isArray(disposables)) disposables.push(disposable);
      return disposable;
    };
  }

  fire(data) {
    if (this.disposed) return;
    for (const entry of [...this.listeners]) {
      try {
        entry.listener.call(entry.thisArgs, data);
      } catch (error) {
        // An extension listener is not allowed to turn a workbench event into a
        // host-wide uncaught exception. The native host reports extension
        // failures separately; keep delivery to the remaining listeners alive.
        process.emitWarning('Extension event listener failed', {
          code: 'SAKURA_EXTENSION_EVENT_LISTENER',
          detail: error instanceof Error ? error.name : 'unknown error',
        });
      }
    }
  }

  dispose() {
    if (this.disposed) return;
    this.disposed = true;
    this.listeners.clear();
  }
}

class CancellationTokenSource {
  constructor() {
    this.emitter = new EventEmitter();
    this.cancelled = false;
    const source = this;
    this.token = Object.freeze({
      get isCancellationRequested() { return source.cancelled; },
      onCancellationRequested: this.emitter.event,
    });
  }

  cancel() {
    if (this.cancelled) return;
    this.cancelled = true;
    this.emitter.fire(undefined);
  }

  dispose() {
    this.emitter.dispose();
  }
}

const CancellationToken = Object.freeze({
  None: Object.freeze({ isCancellationRequested: false, onCancellationRequested: () => new Disposable() }),
  Cancelled: Object.freeze({ isCancellationRequested: true, onCancellationRequested: (listener, thisArgs) => {
    queueMicrotask(() => listener.call(thisArgs));
    return new Disposable();
  } }),
});

class Position {
  constructor(line, character) {
    if (!Number.isInteger(line) || line < 0 || !Number.isInteger(character) || character < 0) {
      throw new TypeError('Position requires non-negative integer line and character');
    }
    this.line = line;
    this.character = character;
  }

  isBefore(other) { return this.compareTo(other) < 0; }
  isBeforeOrEqual(other) { return this.compareTo(other) <= 0; }
  isAfter(other) { return this.compareTo(other) > 0; }
  isAfterOrEqual(other) { return this.compareTo(other) >= 0; }
  isEqual(other) { return this.line === other?.line && this.character === other?.character; }
  compareTo(other) {
    if (!(other instanceof Position)) throw new TypeError('Position expected');
    return this.line === other.line ? this.character - other.character : this.line - other.line;
  }
  translate(lineDelta = 0, characterDelta = 0) {
    if (typeof lineDelta === 'object') {
      characterDelta = lineDelta.characterDelta ?? 0;
      lineDelta = lineDelta.lineDelta ?? 0;
    }
    return new Position(this.line + lineDelta, this.character + characterDelta);
  }
  with(line = this.line, character = this.character) {
    if (typeof line === 'object') {
      character = line.character ?? this.character;
      line = line.line ?? this.line;
    }
    return line === this.line && character === this.character ? this : new Position(line, character);
  }
}

class Range {
  constructor(startLineOrPosition, startCharacterOrEnd, endLine, endCharacter) {
    let first;
    let second;
    if (startLineOrPosition instanceof Position && startCharacterOrEnd instanceof Position) {
      first = startLineOrPosition;
      second = startCharacterOrEnd;
    } else {
      first = new Position(startLineOrPosition, startCharacterOrEnd);
      second = new Position(endLine, endCharacter);
    }
    this.start = first.isBeforeOrEqual(second) ? first : second;
    this.end = first.isBeforeOrEqual(second) ? second : first;
  }

  get isEmpty() { return this.start.isEqual(this.end); }
  get isSingleLine() { return this.start.line === this.end.line; }
  contains(value) {
    if (value instanceof Position) return value.isAfterOrEqual(this.start) && value.isBeforeOrEqual(this.end);
    if (value instanceof Range) return this.contains(value.start) && this.contains(value.end);
    return false;
  }
  isEqual(other) { return this.start.isEqual(other?.start) && this.end.isEqual(other?.end); }
  intersection(other) {
    const start = this.start.isAfter(other.start) ? this.start : other.start;
    const end = this.end.isBefore(other.end) ? this.end : other.end;
    return start.isAfter(end) ? undefined : new Range(start, end);
  }
  union(other) {
    const start = this.start.isBefore(other.start) ? this.start : other.start;
    const end = this.end.isAfter(other.end) ? this.end : other.end;
    return new Range(start, end);
  }
  with(start = this.start, end = this.end) {
    if (start && !(start instanceof Position)) {
      end = start.end ?? this.end;
      start = start.start ?? this.start;
    }
    return start === this.start && end === this.end ? this : new Range(start, end);
  }
}

class Selection extends Range {
  constructor(anchorLineOrPosition, anchorCharacterOrActive, activeLine, activeCharacter) {
    let anchor;
    let active;
    if (anchorLineOrPosition instanceof Position && anchorCharacterOrActive instanceof Position) {
      anchor = anchorLineOrPosition;
      active = anchorCharacterOrActive;
    } else {
      anchor = new Position(anchorLineOrPosition, anchorCharacterOrActive);
      active = new Position(activeLine, activeCharacter);
    }
    super(anchor, active);
    this.anchor = anchor;
    this.active = active;
  }
  get isReversed() { return this.anchor.isAfter(this.active); }
}

class Uri {
  constructor(scheme, authority = '', uriPath = '', query = '', fragment = '') {
    this.scheme = String(scheme || '');
    this.authority = String(authority || '');
    this.path = String(uriPath || '');
    this.query = String(query || '');
    this.fragment = String(fragment || '');
  }

  static parse(value, strict = false) {
    requireString(value, 'Uri value', true);
    try {
      const parsed = new URL(value);
      return new Uri(parsed.protocol.slice(0, -1), parsed.host,
        decodeURIComponent(parsed.pathname), parsed.search.slice(1), parsed.hash.slice(1));
    } catch (error) {
      if (strict) throw error;
      const match = /^([A-Za-z][\w+.-]*):([^?#]*)(?:\?([^#]*))?(?:#(.*))?$/.exec(value);
      if (!match) return new Uri('', '', value);
      return new Uri(match[1], '', match[2], match[3] || '', match[4] || '');
    }
  }

  static file(filePath) { return Uri.parse(pathToFileURL(path.resolve(String(filePath))).href, true); }
  static joinPath(base, ...segments) {
    if (!(base instanceof Uri)) throw new TypeError('Uri expected');
    const joined = path.posix.join(base.path, ...segments.map(String));
    return base.with({ path: joined });
  }
  static from(components) {
    if (!components || typeof components !== 'object') throw new TypeError('URI components expected');
    return new Uri(components.scheme, components.authority, components.path, components.query, components.fragment);
  }

  get fsPath() {
    if (this.scheme !== 'file') return this.path;
    try { return fileURLToPath(this.toString()); } catch { return this.path; }
  }
  with(change = {}) {
    return new Uri(change.scheme ?? this.scheme, change.authority ?? this.authority,
      change.path ?? this.path, change.query ?? this.query, change.fragment ?? this.fragment);
  }
  toString(skipEncoding = false) {
    const authority = this.authority ? `//${this.authority}` : (this.scheme === 'file' ? '//' : '');
    const encodedPath = skipEncoding ? this.path : this.path.split('/').map((part) => encodeURIComponent(part)).join('/');
    return `${this.scheme ? `${this.scheme}:` : ''}${authority}${encodedPath}` +
      `${this.query ? `?${this.query}` : ''}${this.fragment ? `#${this.fragment}` : ''}`;
  }
  toJSON() { return { scheme: this.scheme, authority: this.authority, path: this.path, query: this.query, fragment: this.fragment }; }
}

const EndOfLine = Object.freeze({ LF: 1, CRLF: 2 });
const TextDocumentSaveReason = Object.freeze({ Manual: 1, AfterDelay: 2, FocusOut: 3 });
const FileType = Object.freeze({ Unknown: 0, File: 1, Directory: 2, SymbolicLink: 64 });
class FileSystemError extends Error {
  constructor(messageOrUri = 'File system error') {
    super(messageOrUri instanceof Uri ? messageOrUri.toString() : String(messageOrUri));
    this.name = 'FileSystemError';
    this.code = 'Unknown';
  }
  static _create(code, value) { const error = new FileSystemError(value); error.code = code; return error; }
  static FileExists(value) { return FileSystemError._create('FileExists', value); }
  static FileNotFound(value) { return FileSystemError._create('FileNotFound', value); }
  static FileNotADirectory(value) { return FileSystemError._create('FileNotADirectory', value); }
  static FileIsADirectory(value) { return FileSystemError._create('FileIsADirectory', value); }
  static NoPermissions(value) { return FileSystemError._create('NoPermissions', value); }
  static Unavailable(value) { return FileSystemError._create('Unavailable', value); }
}
const ViewColumn = Object.freeze({ Active: -1, Beside: -2, One: 1, Two: 2, Three: 3, Four: 4, Five: 5, Six: 6, Seven: 7, Eight: 8, Nine: 9 });
const OverviewRulerLane = Object.freeze({ Left: 1, Center: 2, Right: 4, Full: 7 });
const ColorThemeKind = Object.freeze({ Light: 1, Dark: 2, HighContrast: 3, HighContrastLight: 4 });

class TextEdit {
  constructor(range, newText) {
    if (!(range instanceof Range)) throw new TypeError('TextEdit.range must be a Range');
    this.range = range;
    this.newText = String(newText);
    this.newEol = undefined;
  }
  static replace(range, newText) { return new TextEdit(range, newText); }
  static insert(position, newText) { return new TextEdit(new Range(position, position), newText); }
  static delete(range) { return new TextEdit(range, ''); }
  static setEndOfLine(newEol) {
    if (newEol !== EndOfLine.LF && newEol !== EndOfLine.CRLF) throw new TypeError('invalid EndOfLine');
    const edit = new TextEdit(new Range(0, 0, 0, 0), '');
    edit.newEol = newEol;
    return edit;
  }
}

function serializePosition(value) { return { line: value.line, character: value.character }; }
function serializeRange(value) { return { start: serializePosition(value.start), end: serializePosition(value.end) }; }
function serializeTextEdit(value) {
  return { range: serializeRange(value.range), newText: value.newText, newEol: value.newEol };
}

class WorkspaceEdit {
  constructor() { this._entries = new Map(); }
  _key(uri) { return uri instanceof Uri ? uri.toString() : String(uri); }
  replace(uri, range, newText, metadata) { this._push(uri, { ...TextEdit.replace(range, newText), metadata }); }
  insert(uri, position, newText, metadata) { this._push(uri, { ...TextEdit.insert(position, newText), metadata }); }
  delete(uri, range, metadata) { this._push(uri, { ...TextEdit.delete(range), metadata }); }
  has(uri) { return this._entries.has(this._key(uri)); }
  set(uri, edits) { this._entries.set(this._key(uri), [...edits]); }
  get(uri) { return [...(this._entries.get(this._key(uri)) || [])]; }
  entries() { return [...this._entries].map(([value, edits]) => [Uri.parse(value), [...edits]]); }
  size() { return this._entries.size; }
  _push(uri, edit) {
    const key = this._key(uri);
    const entries = this._entries.get(key) || [];
    entries.push(edit);
    this._entries.set(key, entries);
  }
}

function computeLineOffsets(text) {
  const offsets = [0];
  for (let index = 0; index < text.length; index++) if (text.charCodeAt(index) === 10) offsets.push(index + 1);
  return offsets;
}

class TextDocument {
  constructor(session, snapshot) {
    this.session = session;
    this._acceptSnapshot(snapshot);
  }
  _acceptSnapshot(snapshot) {
    this.documentId = requireString(snapshot.documentId, 'documentId');
    this.uri = snapshot.uri instanceof Uri ? snapshot.uri : Uri.parse(snapshot.uri || '');
    this.fileName = typeof snapshot.fileName === 'string' ? snapshot.fileName : this.uri.fsPath;
    this.isUntitled = snapshot.isUntitled === true;
    this.languageId = typeof snapshot.languageId === 'string' ? snapshot.languageId : 'plaintext';
    this.version = Number.isSafeInteger(snapshot.version) ? snapshot.version : 1;
    this.isDirty = snapshot.isDirty === true;
    this.isClosed = snapshot.isClosed === true;
    this.eol = snapshot.eol === EndOfLine.CRLF || snapshot.eol === 'crlf' ? EndOfLine.CRLF : EndOfLine.LF;
    this.encoding = typeof snapshot.encoding === 'string' ? snapshot.encoding : 'utf8';
    this._text = typeof snapshot.text === 'string' ? snapshot.text : '';
    this._lineOffsets = computeLineOffsets(this._text);
  }
  get lineCount() { return this._lineOffsets.length; }
  getText(range) {
    if (range === undefined) return this._text;
    return this._text.slice(this.offsetAt(range.start), this.offsetAt(range.end));
  }
  lineAt(lineOrPosition) {
    const line = lineOrPosition instanceof Position ? lineOrPosition.line : lineOrPosition;
    if (!Number.isInteger(line) || line < 0 || line >= this.lineCount) throw new RangeError('line is out of range');
    const start = this._lineOffsets[line];
    let end = line + 1 < this.lineCount ? this._lineOffsets[line + 1] : this._text.length;
    let endIncludingLineBreak = end;
    if (end > start && this._text.charCodeAt(end - 1) === 10) end--;
    if (end > start && this._text.charCodeAt(end - 1) === 13) end--;
    const text = this._text.slice(start, end);
    return Object.freeze({
      lineNumber: line, text,
      range: new Range(line, 0, line, text.length),
      rangeIncludingLineBreak: new Range(this.positionAt(start), this.positionAt(endIncludingLineBreak)),
      firstNonWhitespaceCharacterIndex: text.search(/\S|$/),
      isEmptyOrWhitespace: /^\s*$/.test(text),
    });
  }
  offsetAt(position) {
    if (!(position instanceof Position)) throw new TypeError('Position expected');
    if (position.line >= this.lineCount) return this._text.length;
    const start = this._lineOffsets[position.line];
    const end = position.line + 1 < this.lineCount ? this._lineOffsets[position.line + 1] : this._text.length;
    return Math.min(start + position.character, end);
  }
  positionAt(offset) {
    offset = Math.max(0, Math.min(Number(offset) || 0, this._text.length));
    let low = 0;
    let high = this._lineOffsets.length;
    while (low < high) {
      const middle = Math.floor((low + high) / 2);
      if (this._lineOffsets[middle] > offset) high = middle;
      else low = middle + 1;
    }
    const line = Math.max(0, low - 1);
    return new Position(line, offset - this._lineOffsets[line]);
  }
  getWordRangeAtPosition(position, regex = /(-?\d*\.\d\w*)|([^`~!@#$%^&*()\-=+[\]{}\\|;:'",.<>/?\s]+)/g) {
    const line = this.lineAt(position);
    regex.lastIndex = 0;
    for (let match; (match = regex.exec(line.text));) {
      if (match.index <= position.character && regex.lastIndex >= position.character) {
        return new Range(position.line, match.index, position.line, regex.lastIndex);
      }
      if (match[0].length === 0) regex.lastIndex++;
    }
    return undefined;
  }
  async save() {
    const result = await this.session.request('workspace/document/save', {
      documentId: this.documentId, expectedVersion: this.version,
    });
    return result?.saved === true;
  }
  validateRange(range) {
    return new Range(this.validatePosition(range.start), this.validatePosition(range.end));
  }
  validatePosition(position) {
    const line = Math.min(position.line, this.lineCount - 1);
    return new Position(line, Math.min(position.character, this.lineAt(line).text.length));
  }
}

class TextEditorEdit {
  constructor(document) { this.document = document; this.edits = []; this.newEol = undefined; this.finalized = false; }
  replace(location, value) {
    const range = location instanceof Position ? new Range(location, location) : location;
    this.edits.push(TextEdit.replace(range, value));
  }
  insert(location, value) { this.edits.push(TextEdit.insert(location, value)); }
  delete(location) { this.edits.push(TextEdit.delete(location)); }
  setEndOfLine(endOfLine) { this.newEol = endOfLine; }
}

class TextEditor {
  constructor(session, document, state = {}) {
    this.session = session;
    this.document = document;
    this.viewColumn = state.viewColumn ?? ViewColumn.One;
    this.selections = (state.selections || [new Selection(0, 0, 0, 0)]).map((value) => value instanceof Selection
      ? value : new Selection(new Position(value.anchor.line, value.anchor.character), new Position(value.active.line, value.active.character)));
    this._options = { ...(state.options || {}) };
    this.visibleRanges = (state.visibleRanges || []).map((value) => new Range(
      new Position(value.start.line, value.start.character), new Position(value.end.line, value.end.character)));
  }
  get selection() { return this.selections[0]; }
  set selection(value) { this.selections = [value]; this._setSelections(); }
  set selections(value) { this._selections = value; }
  get selections() { return this._selections; }
  get options() { return this._options; }
  set options(value) {
    this._options = { ...(value || {}) };
    if (this.session) this.session.notify('window/editor/setOptions', {
      documentId: this.document.documentId, options: this._options,
    });
  }
  _setSelections() {
    this.session.notify('window/editor/setSelections', {
      documentId: this.document.documentId,
      selections: this.selections.map((selection) => ({ anchor: serializePosition(selection.anchor), active: serializePosition(selection.active) })),
    });
  }
  async edit(callback, options = {}) {
    if (typeof callback !== 'function') throw new TypeError('edit callback expected');
    const builder = new TextEditorEdit(this.document);
    callback(builder);
    builder.finalized = true;
    const result = await this.session.request('workspace/applyEdit', {
      label: 'Extension edit', expectedVersions: { [this.document.documentId]: this.document.version },
      edits: [{ documentId: this.document.documentId, uri: this.document.uri.toString(),
        edits: builder.edits.map(serializeTextEdit), newEol: builder.newEol }],
      undoStopBefore: options.undoStopBefore !== false, undoStopAfter: options.undoStopAfter !== false,
    });
    if (result?.snapshot) this.document._acceptSnapshot(result.snapshot);
    return result?.applied === true;
  }
  revealRange(range, revealType = 0) {
    this.session.notify('window/editor/revealRange', {
      documentId: this.document.documentId, range: serializeRange(range), revealType,
    });
  }
  setDecorations(decorationType, rangesOrOptions) {
    this.session.notify('window/editor/setDecorations', {
      documentId: this.document.documentId, decorationKey: decorationType?.key,
      ranges: (rangesOrOptions || []).map((value) => serializeRange(value.range || value)),
    });
  }
  show() {}
  hide() {}
}

const DiagnosticSeverity = Object.freeze({ Error: 0, Warning: 1, Information: 2, Hint: 3 });
const DiagnosticTag = Object.freeze({ Unnecessary: 1, Deprecated: 2 });
class Location {
  constructor(uri, rangeOrPosition) {
    this.uri = uri;
    this.range = rangeOrPosition instanceof Position ? new Range(rangeOrPosition, rangeOrPosition) : rangeOrPosition;
  }
}
class DiagnosticRelatedInformation {
  constructor(location, message) { this.location = location; this.message = String(message); }
}
class Diagnostic {
  constructor(range, message, severity = DiagnosticSeverity.Error) {
    this.range = range;
    this.message = String(message);
    this.severity = severity;
    this.source = undefined;
    this.code = undefined;
    this.relatedInformation = undefined;
    this.tags = undefined;
  }
}
function serializeDiagnostic(value) {
  return {
    range: serializeRange(value.range), message: value.message, severity: value.severity,
    source: value.source, code: typeof value.code === 'object' ? value.code?.value : value.code,
    codeDescription: typeof value.code === 'object' ? serializeUri(value.code?.target) : undefined,
    tags: value.tags,
    relatedInformation: value.relatedInformation?.map((item) => ({
      message: item.message,
      location: { uri: serializeUri(item.location.uri), range: serializeRange(item.location.range) },
    })),
  };
}
class DiagnosticCollection {
  constructor(session, name) {
    this.session = session;
    this.name = name || session.extensionId;
    this.entries = new Map();
    this.disposed = false;
  }
  set(uriOrEntries, diagnostics) {
    if (!this.assertActive()) return;
    if (Array.isArray(uriOrEntries)) {
      this.clear();
      for (const [uri, values] of uriOrEntries) this.set(uri, values);
      return;
    }
    const uri = uriOrEntries instanceof Uri ? uriOrEntries : Uri.parse(String(uriOrEntries));
    const key = uri.toString();
    if (diagnostics === undefined) {
      this.delete(uri);
      return;
    }
    const values = [...diagnostics];
    this.entries.set(key, values);
    this.session.notify('languages/diagnostics/set', {
      extensionId: this.session.extensionId, generation: this.session.generation,
      collection: this.name, uri: key, diagnostics: values.map(serializeDiagnostic),
    });
  }
  delete(uri) {
    if (!this.assertActive()) return false;
    const key = uri instanceof Uri ? uri.toString() : String(uri);
    const deleted = this.entries.delete(key);
    if (deleted) this.session.notify('languages/diagnostics/delete', {
      extensionId: this.session.extensionId, generation: this.session.generation, collection: this.name, uri: key,
    });
    return deleted;
  }
  clear() {
    if (!this.assertActive()) return;
    this.entries.clear();
    this.session.notify('languages/diagnostics/clear', {
      extensionId: this.session.extensionId, generation: this.session.generation, collection: this.name,
    });
  }
  forEach(callback, thisArg) {
    for (const [uri, diagnostics] of this.entries) callback.call(thisArg, Uri.parse(uri), diagnostics, this);
  }
  get(uri) { return this.entries.get(uri instanceof Uri ? uri.toString() : String(uri)); }
  has(uri) { return this.entries.has(uri instanceof Uri ? uri.toString() : String(uri)); }
  assertActive() { return !this.disposed; }
  dispose() {
    if (this.disposed) return;
    this.clear();
    this.disposed = true;
    this.session.diagnosticCollections.delete(this.name);
  }
}

const LanguageStatusSeverity = Object.freeze({ Information: 0, Warning: 1, Error: 2 });
class LanguageStatusItem {
  constructor(session, id, selector) {
    this.session = session;
    this.id = requireString(id, 'LanguageStatusItem.id');
    this.selector = selector;
    this.name = id;
    this.severity = LanguageStatusSeverity.Information;
    this.text = '';
    this.detail = '';
    this.busy = false;
    this.command = undefined;
    this.accessibilityInformation = undefined;
    this.disposed = false;
    this.updateScheduled = false;
  }
  update() {
    if (this.disposed || this.updateScheduled) return;
    this.updateScheduled = true;
    queueMicrotask(() => {
      this.updateScheduled = false;
      if (!this.disposed) this.session.notify('workbench/languageStatus/update', {
        extensionId: this.session.extensionId, generation: this.session.generation,
        id: this.id, selector: this.selector, name: this.name, severity: this.severity,
        text: this.text, detail: this.detail, busy: this.busy, command: this.command,
        accessibilityInformation: this.accessibilityInformation,
      });
    });
  }
  dispose() {
    if (this.disposed) return;
    this.disposed = true;
    this.session.notify('workbench/languageStatus/remove', {
      extensionId: this.session.extensionId, generation: this.session.generation, id: this.id,
    });
  }
}

const CompletionItemKind = Object.freeze({
  Text: 0, Method: 1, Function: 2, Constructor: 3, Field: 4, Variable: 5, Class: 6,
  Interface: 7, Module: 8, Property: 9, Unit: 10, Value: 11, Enum: 12, Keyword: 13,
  Snippet: 14, Color: 15, File: 16, Reference: 17, Folder: 18, EnumMember: 19,
  Constant: 20, Struct: 21, Event: 22, Operator: 23, TypeParameter: 24, User: 25, Issue: 26,
});
class CompletionItem {
  constructor(label, kind) { this.label = label; this.kind = kind; }
}
class CompletionList {
  constructor(items = [], isIncomplete = false) { this.items = items; this.isIncomplete = isIncomplete; }
}
class CodeActionKindValue {
  constructor(value) { this.value = String(value); }
  append(parts) { return new CodeActionKindValue([this.value, String(parts)].filter(Boolean).join('.')); }
  contains(other) { return other instanceof CodeActionKindValue && (this.value === '' || other.value === this.value || other.value.startsWith(`${this.value}.`)); }
  intersects(other) { return this.contains(other) || other?.contains?.(this) === true; }
  toString() { return this.value; }
}
const CodeActionKind = Object.freeze({
  Empty: new CodeActionKindValue(''), QuickFix: new CodeActionKindValue('quickfix'),
  Refactor: new CodeActionKindValue('refactor'), RefactorExtract: new CodeActionKindValue('refactor.extract'),
  RefactorInline: new CodeActionKindValue('refactor.inline'), RefactorRewrite: new CodeActionKindValue('refactor.rewrite'),
  Source: new CodeActionKindValue('source'), SourceOrganizeImports: new CodeActionKindValue('source.organizeImports'),
  SourceFixAll: new CodeActionKindValue('source.fixAll'), Notebook: new CodeActionKindValue('notebook'),
});
const ConfigurationTarget = Object.freeze({ Global: 1, Workspace: 2, WorkspaceFolder: 3 });
class CodeAction {
  constructor(title, kind) { this.title = String(title); this.kind = kind; }
}
class CodeLens { constructor(range, command) { this.range = range; this.command = command; this.isResolved = Boolean(command); } }
class DocumentLink { constructor(range, target) { this.range = range; this.target = target; this.tooltip = undefined; } }
class Hover { constructor(contents, range) { this.contents = Array.isArray(contents) ? contents : [contents]; this.range = range; } }
class SymbolInformation {
  constructor(name, kind, containerNameOrRange, locationOrUri) {
    this.name = String(name); this.kind = kind;
    if (locationOrUri instanceof Location) { this.containerName = containerNameOrRange; this.location = locationOrUri; }
    else { this.containerName = ''; this.location = new Location(locationOrUri, containerNameOrRange); }
  }
}
class DocumentSymbol {
  constructor(name, detail, kind, range, selectionRange) {
    this.name = String(name); this.detail = String(detail); this.kind = kind; this.range = range;
    this.selectionRange = selectionRange; this.children = [];
  }
}
class CallHierarchyItem {
  constructor(kind, name, detail, uri, range, selectionRange) {
    this.kind = kind; this.name = name; this.detail = detail; this.uri = uri; this.range = range; this.selectionRange = selectionRange;
  }
}
class TypeHierarchyItem extends CallHierarchyItem {}
class InlayHint {
  constructor(position, label, kind) { this.position = position; this.label = label; this.kind = kind; }
}
class SnippetString {
  constructor(value = '') { this.value = String(value); }
  appendText(value) { this.value += String(value).replace(/[$}\\]/g, '\\$&'); return this; }
  appendPlaceholder(value, number) { this.value += `\${${number || ''}:${typeof value === 'function' ? '' : String(value)}}`; return this; }
  appendTabstop(number) { this.value += `$${number || ''}`; return this; }
  appendVariable(name, defaultValue) { this.value += `\${${name}:${typeof defaultValue === 'function' ? '' : String(defaultValue || '')}}`; return this; }
}
class RelativePattern {
  constructor(base, pattern) {
    this.baseUri = base instanceof Uri ? base : base?.uri instanceof Uri ? base.uri : Uri.file(String(base));
    this.base = this.baseUri.fsPath;
    this.pattern = requireString(pattern, 'RelativePattern.pattern');
  }
}
class ThemeIcon {
  constructor(id, color) { this.id = requireString(id, 'ThemeIcon.id'); this.color = color; }
  static File = Object.freeze(new ThemeIcon('file'));
  static Folder = Object.freeze(new ThemeIcon('folder'));
}
class CancellationError extends Error { constructor() { super('Canceled'); this.name = 'Canceled'; } }
class CustomExecution { constructor(callback) { if (typeof callback !== 'function') throw new TypeError('CustomExecution callback expected'); this.callback = callback; } }
class ShellExecution { constructor(commandLineOrCommand, argsOrOptions, options) { this.commandLine = commandLineOrCommand; this.args = Array.isArray(argsOrOptions) ? argsOrOptions : []; this.options = options || (Array.isArray(argsOrOptions) ? {} : argsOrOptions) || {}; } }
const TaskScope = Object.freeze({ Global: 1, Workspace: 2 });
class Task {
  constructor(definition, scope, name, source, execution, problemMatchers = []) {
    this.definition = definition; this.scope = scope; this.name = name; this.source = source;
    this.execution = execution; this.problemMatchers = Array.isArray(problemMatchers) ? problemMatchers : [problemMatchers];
    this.group = undefined; this.presentationOptions = {}; this.runOptions = {}; this.detail = undefined;
  }
}
class TaskGroup {
  constructor(id, label) { this.id = id; this.label = label; this.isDefault = undefined; }
  static Clean = Object.freeze(new TaskGroup('clean', 'Clean'));
  static Build = Object.freeze(new TaskGroup('build', 'Build'));
  static Rebuild = Object.freeze(new TaskGroup('rebuild', 'Rebuild'));
  static Test = Object.freeze(new TaskGroup('test', 'Test'));
}
const SymbolKind = Object.freeze({
  File: 0, Module: 1, Namespace: 2, Package: 3, Class: 4, Method: 5, Property: 6, Field: 7,
  Constructor: 8, Enum: 9, Interface: 10, Function: 11, Variable: 12, Constant: 13, String: 14,
  Number: 15, Boolean: 16, Array: 17, Object: 18, Key: 19, Null: 20, EnumMember: 21,
  Struct: 22, Event: 23, Operator: 24, TypeParameter: 25,
});
const InlayHintKind = Object.freeze({ Type: 1, Parameter: 2 });

function matchesSelector(selector, document) {
  if (Array.isArray(selector)) return selector.some((item) => matchesSelector(item, document));
  if (typeof selector === 'string') return selector === '*' || selector === document.languageId;
  if (!selector || typeof selector !== 'object') return false;
  if (selector.language && selector.language !== '*' && selector.language !== document.languageId) return false;
  if (selector.scheme && selector.scheme !== '*' && selector.scheme !== document.uri.scheme) return false;
  if (selector.pattern) {
    const pattern = typeof selector.pattern === 'string' ? selector.pattern : selector.pattern.pattern;
    if (pattern && !globMatches(pattern, document.uri.path)) return false;
  }
  return true;
}

function globMatches(pattern, value) {
  const escaped = String(pattern).replace(/[.+^${}()|[\]\\]/g, '\\$&')
    .replace(/\*\*/g, '\0').replace(/\*/g, '[^/]*').replace(/\?/g, '[^/]').replace(/\0/g, '.*');
  return new RegExp(`^${escaped}$`, process.platform === 'win32' ? 'i' : '').test(value.replace(/\\/g, '/'));
}

function serializeProviderResult(value) {
  if (value === undefined || value === null) return value;
  if (value instanceof TextEdit) return serializeTextEdit(value);
  if (value instanceof Range) return serializeRange(value);
  if (value instanceof Position) return serializePosition(value);
  if (value instanceof Uri) return value.toString();
  if (Array.isArray(value)) return value.map(serializeProviderResult);
  if (typeof value !== 'object') return value;
  const result = {};
  for (const [key, child] of Object.entries(value)) result[key] = serializeProviderResult(child);
  return result;
}

class Configuration {
  constructor(session, section, scope) { this.session = session; this.section = section || ''; this.scope = scope; }
  _path(key) { return [this.section, key].filter(Boolean).join('.'); }
  get(section, defaultValue) {
    const value = this.session.configurationValue(this._path(section));
    return value === undefined ? defaultValue : value;
  }
  has(section) { return this.session.configurationValue(this._path(section)) !== undefined; }
  inspect(section) {
    const key = this._path(section);
    const value = this.session.configurationValue(key);
    if (value === undefined) return undefined;
    return { key, defaultValue: this.session.configurationDefaults.get(key), globalValue: value,
      workspaceValue: undefined, workspaceFolderValue: undefined };
  }
  async update(section, value, configurationTarget, overrideInLanguage) {
    const key = this._path(section);
    if (value === undefined) this.session.configuration.delete(key);
    else this.session.configuration.set(key, value);
    await this.session.request('workspace/configuration/update', {
      extensionId: this.session.extensionId, key, value, configurationTarget, overrideInLanguage,
      scope: serializeUri(this.scope instanceof Uri ? this.scope : this.scope?.uri),
    });
    this.session.configurationEmitter.fire(Object.freeze({
      affectsConfiguration: (sectionToTest) => key === sectionToTest || key.startsWith(`${sectionToTest}.`),
    }));
  }
}

function createConfigurationProxy(session, section, scope) {
  const target = new Configuration(session, section, scope);
  const prefix = section ? `${section}.` : '';
  const keys = () => [...new Set([...session.configurationDefaults.keys(), ...session.configuration.keys()]
    .filter((key) => key.startsWith(prefix))
    .map((key) => key.slice(prefix.length).split('.')[0])
    .filter(Boolean))];
  return new Proxy(target, {
    get(object, property, receiver) {
      if (typeof property !== 'string' || Reflect.has(object, property)) return Reflect.get(object, property, receiver);
      return object.get(property);
    },
    ownKeys(object) { return [...new Set([...Reflect.ownKeys(object), ...keys()])]; },
    getOwnPropertyDescriptor(object, property) {
      return Reflect.getOwnPropertyDescriptor(object, property) ||
        (typeof property === 'string' && keys().includes(property)
          ? { configurable: true, enumerable: true, writable: false, value: object.get(property) } : undefined);
    },
  });
}

class JsonMemento {
  constructor(filePath) {
    this.filePath = filePath;
    this.values = Object.create(null);
    this.queue = Promise.resolve();
    if (!filePath) return;
    try {
      const parsed = JSON.parse(fs.readFileSync(filePath, 'utf8'));
      if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) this.values = parsed;
    } catch (error) {
      if (error?.code !== 'ENOENT') throw new Error(`state file is invalid: ${path.basename(filePath)}`);
    }
  }
  keys() { return Object.keys(this.values); }
  get(key, defaultValue) { return Object.prototype.hasOwnProperty.call(this.values, key) ? this.values[key] : defaultValue; }
  update(key, value) {
    requireString(key, 'state key');
    if (value === undefined) delete this.values[key];
    else {
      const encoded = JSON.stringify(value);
      if (encoded === undefined) throw new TypeError('state value must be JSON serializable');
      this.values[key] = JSON.parse(encoded);
    }
    if (!this.filePath) return Promise.resolve();
    const snapshot = JSON.stringify(this.values);
    this.queue = this.queue.then(async () => {
      await fs.promises.mkdir(path.dirname(this.filePath), { recursive: true });
      const temporary = `${this.filePath}.${process.pid}.${Date.now()}.tmp`;
      try {
        await fs.promises.writeFile(temporary, snapshot, { encoding: 'utf8', flag: 'wx' });
        await fs.promises.rename(temporary, this.filePath);
      } finally {
        await fs.promises.rm(temporary, { force: true }).catch(() => {});
      }
    });
    return this.queue;
  }
  setKeysForSync() {}
}

function statFileType(stat) {
  let type = FileType.Unknown;
  if (stat.isFile()) type |= FileType.File;
  if (stat.isDirectory()) type |= FileType.Directory;
  if (stat.isSymbolicLink()) type |= FileType.SymbolicLink;
  return type;
}

function createWorkspaceFileSystem() {
  const filePath = (uri) => {
    if (!(uri instanceof Uri) || uri.scheme !== 'file') throw new Error('workspace.fs supports file URIs only');
    return uri.fsPath;
  };
  return Object.freeze({
    async stat(uri) {
      const stat = await fs.promises.lstat(filePath(uri));
      return { type: statFileType(stat), ctime: stat.ctimeMs, mtime: stat.mtimeMs, size: stat.size, permissions: stat.mode };
    },
    async readDirectory(uri) {
      const entries = await fs.promises.readdir(filePath(uri), { withFileTypes: true });
      return entries.map((entry) => [entry.name, entry.isFile() ? FileType.File : entry.isDirectory() ? FileType.Directory : FileType.Unknown]);
    },
    async createDirectory(uri) { await fs.promises.mkdir(filePath(uri), { recursive: true }); },
    async readFile(uri) { return fs.promises.readFile(filePath(uri)); },
    async writeFile(uri, content) {
      await fs.promises.mkdir(path.dirname(filePath(uri)), { recursive: true });
      await fs.promises.writeFile(filePath(uri), content);
    },
    async delete(uri, options = {}) { await fs.promises.rm(filePath(uri), { recursive: options.recursive === true, force: options.useTrash !== true }); },
    async rename(source, target, options = {}) {
      if (!options.overwrite) {
        try { await fs.promises.access(filePath(target)); throw new Error('target already exists'); } catch (error) { if (error?.code !== 'ENOENT') throw error; }
      }
      await fs.promises.rename(filePath(source), filePath(target));
    },
    async copy(source, target, options = {}) {
      await fs.promises.copyFile(filePath(source), filePath(target), options.overwrite ? 0 : fs.constants.COPYFILE_EXCL);
    },
    isWritableFileSystem(scheme) { return scheme === 'file' ? true : undefined; },
  });
}

class FileSystemWatcher {
  constructor(pattern, ignoreCreateEvents = false, ignoreChangeEvents = false, ignoreDeleteEvents = false) {
    this.pattern = pattern;
    this.ignoreCreateEvents = ignoreCreateEvents;
    this.ignoreChangeEvents = ignoreChangeEvents;
    this.ignoreDeleteEvents = ignoreDeleteEvents;
    this.createEmitter = new EventEmitter();
    this.changeEmitter = new EventEmitter();
    this.deleteEmitter = new EventEmitter();
    this.onDidCreate = this.createEmitter.event;
    this.onDidChange = this.changeEmitter.event;
    this.onDidDelete = this.deleteEmitter.event;
    this.watcher = null;
    const basePath = pattern instanceof RelativePattern ? pattern.baseUri.fsPath : process.cwd();
    const filePattern = pattern instanceof RelativePattern ? pattern.pattern : String(pattern);
    try {
      this.watcher = fs.watch(basePath, { persistent: false }, async (_eventType, filename) => {
        if (!filename || !globMatches(filePattern, String(filename))) return;
        const uri = Uri.file(path.join(basePath, String(filename)));
        try {
          await fs.promises.lstat(uri.fsPath);
          if (!this.ignoreChangeEvents) this.changeEmitter.fire(uri);
          if (!this.ignoreCreateEvents) this.createEmitter.fire(uri);
        } catch (error) {
          if (error?.code === 'ENOENT' && !this.ignoreDeleteEvents) this.deleteEmitter.fire(uri);
        }
      });
      this.watcher.on('error', () => {});
    } catch {}
  }
  dispose() {
    this.watcher?.close();
    this.watcher = null;
    this.createEmitter.dispose(); this.changeEmitter.dispose(); this.deleteEmitter.dispose();
  }
}

class ThemeColor {
  constructor(id) {
    this.id = requireString(id, 'ThemeColor.id');
  }
}

class MarkdownString {
  constructor(value = '', supportThemeIcons = false) {
    this.value = String(value);
    this.isTrusted = undefined;
    this.supportHtml = false;
    this.supportThemeIcons = Boolean(supportThemeIcons);
    this.baseUri = undefined;
  }

  appendText(value) {
    this.value += String(value).replace(/[\\`*_{}\[\]()<>#+\-.!]/g, '\\$&');
    return this;
  }

  appendMarkdown(value) {
    this.value += String(value);
    return this;
  }

  appendCodeblock(value, language = '') {
    this.value += `\n\n\`\`\`${language}\n${String(value)}\n\`\`\`\n`;
    return this;
  }
}

function serializeThemeValue(value) {
  if (value instanceof ThemeColor) return { themeColor: value.id };
  if (value instanceof MarkdownString) return { markdown: value.value, isTrusted: value.isTrusted === true };
  return value;
}

class SecretStorage {
  constructor(session) {
    this.session = session;
    this.changeEmitter = new EventEmitter();
    this.onDidChange = this.changeEmitter.event;
  }

  async get(key) {
    requireString(key, 'secret key');
    const result = await this.session.request('secrets/get', { extensionId: this.session.extensionId, key });
    return result?.value;
  }

  async store(key, value) {
    requireString(key, 'secret key');
    requireString(value, 'secret value', true);
    await this.session.request('secrets/store', { extensionId: this.session.extensionId, key, value });
    this.changeEmitter.fire(Object.freeze({ key }));
  }

  async delete(key) {
    requireString(key, 'secret key');
    await this.session.request('secrets/delete', { extensionId: this.session.extensionId, key });
    this.changeEmitter.fire(Object.freeze({ key }));
  }

  async keys() {
    throw new UnsupportedCapabilityError(this.session.extensionId, 'SecretStorage.keys');
  }

  acceptChange(key) {
    if (typeof key === 'string' && key.length !== 0) this.changeEmitter.fire(Object.freeze({ key }));
  }

  dispose() {
    this.changeEmitter.dispose();
  }
}

class StatusBarItem {
  constructor(session, handle, id, alignment, priority, releaseId) {
    this.session = session;
    this.handle = handle;
    this.id = id;
    this.alignment = alignment;
    this.priority = priority;
    this.releaseId = releaseId;
    this._name = id;
    this._text = '';
    this._tooltip = undefined;
    this._command = undefined;
    this._color = undefined;
    this._backgroundColor = undefined;
    this._accessibilityInformation = undefined;
    this.visible = false;
    this.disposed = false;
    this.updateScheduled = false;
    this.scheduleUpdate();
  }

  get name() { return this._name; }
  set name(value) { this._name = requireString(value, 'StatusBarItem.name'); this.scheduleUpdate(); }
  get text() { return this._text; }
  set text(value) { this._text = requireString(value, 'StatusBarItem.text', true); this.scheduleUpdate(); }
  get tooltip() { return this._tooltip; }
  set tooltip(value) { this._tooltip = value; this.scheduleUpdate(); }
  get command() { return this._command; }
  set command(value) { this._command = value; this.scheduleUpdate(); }
  get color() { return this._color; }
  set color(value) { this._color = value; this.scheduleUpdate(); }
  get backgroundColor() { return this._backgroundColor; }
  set backgroundColor(value) { this._backgroundColor = value; this.scheduleUpdate(); }
  get accessibilityInformation() { return this._accessibilityInformation; }
  set accessibilityInformation(value) { this._accessibilityInformation = value; this.scheduleUpdate(); }

  show() {
    this.assertActive();
    this.visible = true;
    this.scheduleUpdate();
  }

  hide() {
    this.assertActive();
    this.visible = false;
    this.scheduleUpdate();
  }

  scheduleUpdate() {
    if (this.disposed || this.updateScheduled) return;
    this.updateScheduled = true;
    queueMicrotask(() => {
      this.updateScheduled = false;
      if (!this.disposed) this.session.notify('workbench/statusBar/update', this.snapshot());
    });
  }

  snapshot() {
    return {
      handle: this.handle,
      itemId: this.id,
      extensionId: this.session.extensionId,
      generation: this.session.generation,
      alignment: this.alignment === StatusBarAlignment.Right ? 'right' : 'left',
      priority: this.priority,
      name: this._name,
      text: this._text,
      tooltip: serializeThemeValue(this._tooltip),
      command: this._command,
      color: serializeThemeValue(this._color),
      backgroundColor: serializeThemeValue(this._backgroundColor),
      accessibilityInformation: this._accessibilityInformation,
      visible: this.visible,
    };
  }

  assertActive() {
    if (this.disposed) throw new Error('StatusBarItem is disposed');
  }

  dispose() {
    if (this.disposed) return;
    this.disposed = true;
    this.releaseId();
    this.session.notify('workbench/statusBar/remove', {
      handle: this.handle,
      extensionId: this.session.extensionId,
      generation: this.session.generation,
    });
  }
}

class OutputChannel {
  constructor(session, handle, name, languageId, log = false) {
    this.session = session;
    this.handle = handle;
    this.name = name;
    this.languageId = languageId;
    this.logLevel = log ? 2 : undefined;
    this.onDidChangeLogLevel = () => new Disposable();
    this.buffer = '';
    this.timer = null;
    this.disposed = false;
    this.notifyMutation('workbench/output/create', { name, languageId });
  }

  append(value) {
    if (!this.assertActive()) return;
    this.buffer += String(value);
    if (this.buffer.length >= MAX_OUTPUT_BUFFER_CHARS) this.flush();
    else if (!this.timer) {
      this.timer = setTimeout(() => this.flush(), OUTPUT_FLUSH_MS);
      this.timer.unref?.();
    }
  }

  appendLine(value) { this.append(`${String(value)}\n`); }
  trace(message, ...args) { this.appendLine(`[trace] ${[message, ...args].map(String).join(' ')}`); }
  debug(message, ...args) { this.appendLine(`[debug] ${[message, ...args].map(String).join(' ')}`); }
  info(message, ...args) { this.appendLine(`[info] ${[message, ...args].map(String).join(' ')}`); }
  warn(message, ...args) { this.appendLine(`[warn] ${[message, ...args].map(String).join(' ')}`); }
  error(error, ...args) { this.appendLine(`[error] ${[error?.stack || error, ...args].map(String).join(' ')}`); }

  replace(value) {
    if (!this.assertActive()) return;
    this.flush();
    this.notifyMutation('workbench/output/replace', { value: String(value) });
  }

  clear() {
    if (!this.assertActive()) return;
    this.buffer = '';
    if (this.timer) clearTimeout(this.timer);
    this.timer = null;
    this.notifyMutation('workbench/output/clear');
  }

  show(columnOrPreserveFocus, preserveFocus) {
    if (!this.assertActive()) return;
    this.notifyMutation('workbench/output/show', {
      preserveFocus: typeof columnOrPreserveFocus === 'boolean' ? columnOrPreserveFocus : Boolean(preserveFocus),
    });
  }

  hide() {
    if (!this.assertActive()) return;
    this.notifyMutation('workbench/output/hide');
  }

  flush() {
    if (this.timer) clearTimeout(this.timer);
    this.timer = null;
    if (this.disposed || this.buffer.length === 0) return;
    const value = this.buffer;
    this.buffer = '';
    this.notifyMutation('workbench/output/append', { value });
  }

  notifyMutation(method, extra = {}) {
    const params = this.parameters(extra, this.session.allocateOutputOperationId());
    this.session.notify(method, params);
  }

  parameters(extra = {}, operationId) {
    return {
      handle: this.handle,
      extensionId: this.session.extensionId,
      generation: this.session.generation,
      ...extra,
      operationId,
    };
  }

  assertActive() {
    return !this.disposed;
  }

  dispose() {
    if (this.disposed) return;
    this.flush();
    this.disposed = true;
    this.notifyMutation('workbench/output/dispose');
  }
}

const TreeItemCollapsibleState = Object.freeze({ None: 0, Collapsed: 1, Expanded: 2 });

class TreeItem {
  constructor(labelOrUri, collapsibleState = TreeItemCollapsibleState.None) {
    if (typeof labelOrUri === 'string') this.label = requireString(labelOrUri, 'TreeItem.label');
    else if (labelOrUri && typeof labelOrUri === 'object') this.resourceUri = labelOrUri;
    else throw new TypeError('TreeItem requires a label or resource URI');
    if (!Object.values(TreeItemCollapsibleState).includes(collapsibleState)) {
      throw new TypeError('invalid TreeItemCollapsibleState');
    }
    this.collapsibleState = collapsibleState;
  }
}

function serializeUri(value) {
  if (value === undefined || value === null) return undefined;
  if (typeof value === 'string') return value;
  if (typeof value.toString === 'function') return value.toString();
  return undefined;
}

function serializeTreeItem(item) {
  if (!item || typeof item !== 'object') throw new TypeError('TreeDataProvider.getTreeItem must return a TreeItem');
  let label;
  let highlights;
  if (typeof item.label === 'string') label = item.label;
  else if (item.label && typeof item.label.label === 'string') {
    label = item.label.label;
    highlights = item.label.highlights;
  }
  if (label === undefined && item.resourceUri !== undefined) label = serializeUri(item.resourceUri);
  requireString(label, 'TreeItem.label');
  const collapsibleState = item.collapsibleState ?? TreeItemCollapsibleState.None;
  if (!Object.values(TreeItemCollapsibleState).includes(collapsibleState)) {
    throw new TypeError('invalid TreeItemCollapsibleState');
  }
  return {
    id: typeof item.id === 'string' ? item.id : undefined,
    label,
    highlights,
    description: typeof item.description === 'string' || typeof item.description === 'boolean'
      ? item.description : undefined,
    resourceUri: serializeUri(item.resourceUri),
    tooltip: serializeThemeValue(item.tooltip),
    command: item.command,
    collapsibleState,
    contextValue: typeof item.contextValue === 'string' ? item.contextValue : undefined,
    checkboxState: item.checkboxState,
    accessibilityInformation: item.accessibilityInformation,
    iconPath: item.iconPath instanceof ThemeColor ? { themeColor: item.iconPath.id } : undefined,
  };
}

class TreeView {
  constructor(session, viewId, options) {
    if (!options || !options.treeDataProvider) throw new TypeError('createTreeView requires a treeDataProvider');
    this.session = session;
    this.viewId = requireString(viewId, 'viewId');
    this.provider = options.treeDataProvider;
    if (typeof this.provider.getTreeItem !== 'function' || typeof this.provider.getChildren !== 'function') {
      throw new TypeError('treeDataProvider must provide getTreeItem and getChildren');
    }
    this.handle = session.allocateHandle('view');
    this.elementToHandle = new Map();
    this.handleToElement = new Map();
    this.parentByHandle = new Map();
    this.nextElementHandle = 1;
    this.selection = [];
    this.visible = false;
    this.disposed = false;
    this._title = options.title;
    this._description = undefined;
    this._message = undefined;
    this._badge = undefined;
    this.selectionEmitter = new EventEmitter();
    this.visibilityEmitter = new EventEmitter();
    this.checkboxEmitter = new EventEmitter();
    this.onDidChangeSelection = this.selectionEmitter.event;
    this.onDidChangeVisibility = this.visibilityEmitter.event;
    this.onDidChangeCheckboxState = this.checkboxEmitter.event;
    this.refreshSubscription = typeof this.provider.onDidChangeTreeData === 'function'
      ? this.provider.onDidChangeTreeData((element) => this.refresh(element)) : undefined;
    session.notify('workbench/views/register', this.parameters({
      viewId: this.viewId,
      title: typeof this._title === 'string' ? this._title : this.viewId,
      canSelectMany: options.canSelectMany === true,
      showCollapseAll: options.showCollapseAll === true,
    }));
  }

  get title() { return this._title; }
  set title(value) { this._title = value === undefined ? undefined : requireString(value, 'TreeView.title'); this.updatePresentation(); }
  get description() { return this._description; }
  set description(value) { this._description = value === undefined ? undefined : requireString(value, 'TreeView.description', true); this.updatePresentation(); }
  get message() { return this._message; }
  set message(value) { this._message = value; this.updatePresentation(); }
  get badge() { return this._badge; }
  set badge(value) { this._badge = value; this.updatePresentation(); }

  parameters(extra = {}) {
    return { handle: this.handle, extensionId: this.session.extensionId, generation: this.session.generation, ...extra };
  }

  updatePresentation() {
    this.assertActive();
    this.session.notify('workbench/views/update', this.parameters({
      title: this._title,
      description: this._description,
      message: serializeThemeValue(this._message),
      badge: this._badge,
    }));
  }

  handleFor(element) {
    if (this.elementToHandle.has(element)) return this.elementToHandle.get(element);
    const handle = `${this.handle}:item:${this.nextElementHandle++}`;
    this.elementToHandle.set(element, handle);
    this.handleToElement.set(handle, element);
    return handle;
  }

  refresh(element) {
    if (this.disposed) return;
    const itemHandle = element === undefined || element === null ? undefined : this.elementToHandle.get(element);
    this.session.notify('workbench/views/refresh', this.parameters({ itemHandle }));
  }

  async getChildren(parentHandle) {
    this.assertActive();
    let parent;
    if (parentHandle !== undefined && parentHandle !== null) {
      if (!this.handleToElement.has(parentHandle)) throw new Error(`unknown tree item: ${parentHandle}`);
      parent = this.handleToElement.get(parentHandle);
    }
    const children = await Promise.resolve(this.provider.getChildren(parent));
    if (children === undefined || children === null) return { items: [] };
    if (!Array.isArray(children)) throw new TypeError('TreeDataProvider.getChildren must return an array');
    const items = [];
    for (const element of children) {
      const itemHandle = this.handleFor(element);
      this.parentByHandle.set(itemHandle, parentHandle);
      const treeItem = await Promise.resolve(this.provider.getTreeItem(element));
      items.push({ handle: itemHandle, parentHandle, ...serializeTreeItem(treeItem) });
    }
    return { items };
  }

  async reveal(element, options = {}) {
    this.assertActive();
    const itemHandle = this.handleFor(element);
    await this.session.request('workbench/views/reveal', this.parameters({ itemHandle, options }));
  }

  acceptSelection(handles) {
    this.assertActive();
    const selection = [];
    for (const handle of Array.isArray(handles) ? handles : []) {
      if (this.handleToElement.has(handle)) selection.push(this.handleToElement.get(handle));
    }
    this.selection = selection;
    this.selectionEmitter.fire(Object.freeze({ selection: [...selection] }));
    return { accepted: true };
  }

  acceptVisibility(visible) {
    this.assertActive();
    const next = Boolean(visible);
    if (next !== this.visible) {
      this.visible = next;
      this.visibilityEmitter.fire(Object.freeze({ visible: next }));
    }
    return { accepted: true };
  }

  acceptCheckboxState(items) {
    this.assertActive();
    const mapped = [];
    for (const pair of Array.isArray(items) ? items : []) {
      if (this.handleToElement.has(pair?.handle)) mapped.push([this.handleToElement.get(pair.handle), pair.state]);
    }
    if (mapped.length !== 0) this.checkboxEmitter.fire(Object.freeze({ items: mapped }));
    return { accepted: true };
  }

  assertActive() {
    if (this.disposed) throw new Error(`TreeView is disposed: ${this.viewId}`);
  }

  dispose() {
    if (this.disposed) return;
    this.disposed = true;
    this.refreshSubscription?.dispose?.();
    this.selectionEmitter.dispose();
    this.visibilityEmitter.dispose();
    this.checkboxEmitter.dispose();
    this.elementToHandle.clear();
    this.handleToElement.clear();
    this.parentByHandle.clear();
    this.session.views.delete(this.handle);
    this.session.viewIds.delete(this.viewId);
    this.session.notify('workbench/views/unregister', this.parameters({ viewId: this.viewId }));
  }
}

const StatusBarAlignment = Object.freeze({ Left: 1, Right: 2 });
const ProgressLocation = Object.freeze({ SourceControl: 1, Window: 10, Notification: 15 });

function parseMessageArguments(rest) {
  let options = {};
  if (rest.length !== 0 && rest[0] && typeof rest[0] === 'object' && !Array.isArray(rest[0]) &&
      !Object.prototype.hasOwnProperty.call(rest[0], 'title')) {
    options = rest.shift();
  }
  const original = rest;
  const actions = original.map((item) => typeof item === 'string'
    ? { title: item }
    : { title: requireString(item?.title, 'MessageItem.title'), isCloseAffordance: item.isCloseAffordance === true });
  return { options, original, actions };
}

class ExtensionApiSession {
  constructor(extensionId, generation, transport, options = {}) {
    this.extensionId = requireString(extensionId, 'extensionId').toLowerCase();
    if (!Number.isSafeInteger(generation) || generation <= 0) throw new TypeError('generation must be positive');
    if (!transport || typeof transport.request !== 'function' || typeof transport.notify !== 'function') {
      throw new TypeError('transport must provide request and notify');
    }
    this.generation = generation;
    this.transport = transport;
    this.options = options;
    this.outputOperationPrefix = allocateOutputOperationPrefix(generation);
    this.nextOutputOperation = 1;
    this.nextHandle = 1;
    this.commands = new Map();
    this.statusBarIds = new Set();
    this.views = new Map();
    this.viewIds = new Set();
    this.disposables = new Set();
    this.progress = new Map();
    this.documents = new Map();
    this.documentIdsByUri = new Map();
    this.editors = new Map();
    this.activeEditor = undefined;
    this.languageProviders = new Map();
    this.nextProviderHandle = 1;
    this.diagnosticCollections = new Map();
    this.configurationDefaults = new Map(Object.entries(options.configurationDefaults || {}));
    this.configuration = new Map(Object.entries(options.configuration || {}));
    this.documentOpenEmitter = new EventEmitter();
    this.documentChangeEmitter = new EventEmitter();
    this.documentSaveEmitter = new EventEmitter();
    this.documentCloseEmitter = new EventEmitter();
    this.documentWillSaveEmitter = new EventEmitter();
    this.configurationEmitter = new EventEmitter();
    this.activeEditorEmitter = new EventEmitter();
    this.visibleEditorsEmitter = new EventEmitter();
    this.selectionEmitter = new EventEmitter();
    this.editorOptionsEmitter = new EventEmitter();
    this.windowStateEmitter = new EventEmitter();
    this.secrets = new SecretStorage(this);
    this.disposed = false;
    this.api = this.createApi();
    for (const snapshot of Array.isArray(options.documents) ? options.documents : []) this.acceptDocument(snapshot, 'open', false);
    if (options.activeDocumentId) this.acceptActiveEditor({ documentId: options.activeDocumentId }, false);
  }

  request(method, params, options) {
    if (this.disposed) {
      process.emitWarning(`Ignored extension request after disposal: ${method}`, { code: 'SAKURA_EXTENSION_DISPOSED' });
      return Promise.resolve(undefined);
    }
    return this.transport.request(method, {
      ...(params && typeof params === 'object' ? params : {}),
      extensionId: this.extensionId,
      generation: this.generation,
    }, options);
  }

  notify(method, params) {
    if (!this.disposed) this.transport.notify(method, {
      ...(params && typeof params === 'object' ? params : {}),
      extensionId: this.extensionId,
      generation: this.generation,
    });
  }

  allocateHandle(kind) {
    return `${kind}:${this.extensionId}:${this.generation}:${this.nextHandle++}`;
  }

  allocateOutputOperationId() {
    const sequence = this.nextOutputOperation;
    if (!Number.isSafeInteger(sequence)) {
      throw new RangeError('Output operation ID space exhausted');
    }
    const operationId = `${this.outputOperationPrefix}${sequence}`;
    if (operationId.length > MAX_OUTPUT_OPERATION_ID_LENGTH) {
      throw new RangeError('Output operation ID exceeds its maximum length');
    }
    this.nextOutputOperation = sequence + 1;
    return operationId;
  }

  track(disposable) {
    this.disposables.add(disposable);
    return disposable;
  }

  configurationValue(key) {
    if (this.configuration.has(key)) return this.configuration.get(key);
    return this.configurationDefaults.get(key);
  }

  documentByReference(reference) {
    if (reference instanceof TextDocument) return reference;
    const documentId = typeof reference === 'string' && this.documents.has(reference)
      ? reference : this.documentIdsByUri.get(reference instanceof Uri ? reference.toString() : String(reference || ''));
    return documentId ? this.documents.get(documentId) : undefined;
  }

  acceptDocument(snapshot, eventKind = 'change', fire = true) {
    if (!snapshot || typeof snapshot !== 'object') throw new TypeError('document snapshot expected');
    const documentId = requireString(snapshot.documentId, 'documentId');
    let document = this.documents.get(documentId);
    const previousVersion = document?.version;
    if (document && Number.isSafeInteger(snapshot.version) && snapshot.version < document.version) return document;
    if (!document) {
      document = new TextDocument(this, snapshot);
      this.documents.set(documentId, document);
    } else {
      this.documentIdsByUri.delete(document.uri.toString());
      document._acceptSnapshot(snapshot);
    }
    this.documentIdsByUri.set(document.uri.toString(), documentId);
    if (!fire) return document;
    if (eventKind === 'open') this.documentOpenEmitter.fire(document);
    else if (eventKind === 'save') this.documentSaveEmitter.fire(document);
    else if (eventKind === 'change') {
      this.documentChangeEmitter.fire(Object.freeze({
        document,
        contentChanges: Array.isArray(snapshot.contentChanges) ? snapshot.contentChanges.map((change) => ({
          range: change.range ? new Range(new Position(change.range.start.line, change.range.start.character),
            new Position(change.range.end.line, change.range.end.character)) : undefined,
          rangeOffset: change.rangeOffset, rangeLength: change.rangeLength, text: String(change.text || ''),
        })) : [],
        reason: snapshot.reason,
        previousVersion,
      }));
    }
    return document;
  }

  acceptCloseDocument(reference) {
    const document = this.documentByReference(reference);
    if (!document) return false;
    document.isClosed = true;
    this.documents.delete(document.documentId);
    this.documentIdsByUri.delete(document.uri.toString());
    this.editors.delete(document.documentId);
    if (this.activeEditor?.document === document) {
      this.activeEditor = undefined;
      this.activeEditorEmitter.fire(undefined);
      this.visibleEditorsEmitter.fire([...this.editors.values()]);
    }
    this.documentCloseEmitter.fire(document);
    return true;
  }

  acceptActiveEditor(state, fire = true) {
    if (!state?.documentId) {
      this.activeEditor = undefined;
      if (fire) this.activeEditorEmitter.fire(undefined);
      return undefined;
    }
    const document = this.documents.get(state.documentId);
    if (!document) throw new Error(`unknown active document: ${state.documentId}`);
    let editor = this.editors.get(document.documentId);
    if (!editor) {
      editor = new TextEditor(this, document, state);
      this.editors.set(document.documentId, editor);
    } else {
      if (state.options) editor.options = { ...state.options };
      if (state.selections) editor.selections = state.selections.map((value) => new Selection(
        new Position(value.anchor.line, value.anchor.character), new Position(value.active.line, value.active.character)));
    }
    this.activeEditor = editor;
    if (fire) {
      this.activeEditorEmitter.fire(editor);
      this.visibleEditorsEmitter.fire([...this.editors.values()]);
    }
    return editor;
  }

  registerLanguageProvider(kind, selector, provider, methodName, metadata = {}) {
    if (!provider || typeof provider[methodName] !== 'function') throw new TypeError(`${methodName} provider expected`);
    const handle = `provider:${this.extensionId}:${this.generation}:${this.nextProviderHandle++}`;
    const providers = this.languageProviders.get(kind) || new Map();
    providers.set(handle, { selector, provider, methodName });
    this.languageProviders.set(kind, providers);
    this.notify('languages/provider/register', {
      handle, extensionId: this.extensionId, generation: this.generation, kind, selector, metadata,
    });
    return this.track(new Disposable(() => {
      providers.delete(handle);
      this.notify('languages/provider/unregister', {
        handle, extensionId: this.extensionId, generation: this.generation, kind,
      });
    }));
  }

  async invokeLanguageProvider(kind, params) {
    const document = this.documentByReference(params?.documentId || params?.uri);
    if (!document) throw new Error(`document is not open: ${params?.documentId || params?.uri}`);
    const providers = this.languageProviders.get(kind);
    if (!providers) return { value: undefined };
    const position = params?.position ? new Position(params.position.line, params.position.character) : undefined;
    const range = params?.range ? new Range(new Position(params.range.start.line, params.range.start.character),
      new Position(params.range.end.line, params.range.end.character)) : undefined;
    const token = CancellationToken.None;
    const values = [];
    for (const record of providers.values()) {
      if (!matchesSelector(record.selector, document)) continue;
      let value;
      if (kind === 'formatDocument') value = await record.provider[record.methodName](document, params?.options || {}, token);
      else if (kind === 'formatRange') value = await record.provider[record.methodName](document, range, params?.options || {}, token);
      else if (kind === 'completion') value = await record.provider[record.methodName](document, position, token, params?.context || {});
      else if (kind === 'codeActions') value = await record.provider[record.methodName](document, range, params?.context || {}, token);
      else if (kind === 'rename') value = await record.provider[record.methodName](document, position, params?.newName, token);
      else value = await record.provider[record.methodName](document, position ?? range, token);
      if (value !== undefined && value !== null) values.push(value);
    }
    const merged = values.length === 0 ? undefined : values.length === 1 ? values[0]
      : values.every(Array.isArray) ? values.flat() : values;
    return { value: serializeProviderResult(merged), expectedVersion: document.version };
  }

  createApi() {
    const session = this;
    const noOpEvent = () => new Disposable();
    const commands = {
      registerCommand(command, callback, thisArg) {
        requireString(command, 'command');
        if (typeof callback !== 'function') throw new TypeError('callback must be a function');
        if (session.commands.has(command)) throw new Error(`command already registered: ${command}`);
        session.commands.set(command, { callback, thisArg });
        session.notify('workbench/commands/registerHandler', {
          command, extensionId: session.extensionId, generation: session.generation,
        });
        const disposable = new Disposable(() => {
          if (!session.commands.delete(command)) return;
          session.notify('workbench/commands/unregisterHandler', {
            command, extensionId: session.extensionId, generation: session.generation,
          });
        });
        return session.track(disposable);
      },
      async executeCommand(command, ...args) {
        requireString(command, 'command');
        if (command === 'setContext') {
          const [key, value] = args;
          requireString(key, 'context key');
          await session.request('workbench/context/set', {
            key, value, extensionId: session.extensionId, generation: session.generation,
          });
          return undefined;
        }
        const local = session.commands.get(command);
        if (local) return local.callback.apply(local.thisArg, args);
        const result = await session.request('workbench/commands/execute', { command, args });
        return result?.value;
      },
      async getCommands(filterInternal = false) {
        const result = await session.request('workbench/commands/list', { filterInternal: Boolean(filterInternal) });
        const commands = new Set(Array.isArray(result?.commands) ? result.commands : []);
        for (const command of session.commands.keys()) commands.add(command);
        if (filterInternal) return [...commands].filter((command) => !command.startsWith('_')).sort();
        return [...commands].sort();
      },
    };

    const workspaceFileSystem = createWorkspaceFileSystem();
    const workspace = {
      isTrusted: true,
      get textDocuments() { return [...session.documents.values()]; },
      get workspaceFolders() {
        return Array.isArray(session.options.workspaceFolders) ? session.options.workspaceFolders.map((folder, index) => ({
          uri: folder.uri instanceof Uri ? folder.uri : Uri.parse(folder.uri),
          name: folder.name || path.basename((folder.uri instanceof Uri ? folder.uri : Uri.parse(folder.uri)).fsPath),
          index: folder.index ?? index,
        })) : undefined;
      },
      get name() { return session.options.workspaceName; },
      get workspaceFile() { return session.options.workspaceFile ? Uri.parse(session.options.workspaceFile) : undefined; },
      fs: workspaceFileSystem,
      onDidOpenTextDocument: session.documentOpenEmitter.event,
      onDidChangeTextDocument: session.documentChangeEmitter.event,
      onDidSaveTextDocument: session.documentSaveEmitter.event,
      onDidCloseTextDocument: session.documentCloseEmitter.event,
      onWillSaveTextDocument: session.documentWillSaveEmitter.event,
      onDidChangeConfiguration: session.configurationEmitter.event,
      onDidGrantWorkspaceTrust() { return new Disposable(); },
      onDidChangeWorkspaceFolders() { return new Disposable(); },
      onDidCreateFiles: noOpEvent,
      onDidDeleteFiles: noOpEvent,
      onDidRenameFiles: noOpEvent,
      async openTextDocument(uriOrOptions, openOptions) {
        let uri;
        let options = openOptions || {};
        if (uriOrOptions instanceof Uri) uri = uriOrOptions;
        else if (typeof uriOrOptions === 'string') uri = path.isAbsolute(uriOrOptions) ? Uri.file(uriOrOptions) : Uri.parse(uriOrOptions);
        else options = uriOrOptions || {};
        if (uri) {
          const existing = session.documentByReference(uri);
          if (existing && !options.encoding) return existing;
        }
        const result = await session.request('workspace/openTextDocument', {
          uri: uri?.toString(), language: options.language, content: options.content, encoding: options.encoding,
        });
        if (!result?.snapshot) throw new Error('workspace/openTextDocument did not return a document snapshot');
        return session.acceptDocument(result.snapshot, 'open', result.opened !== false);
      },
      async applyEdit(edit, metadata) {
        if (!(edit instanceof WorkspaceEdit)) throw new TypeError('WorkspaceEdit expected');
        const edits = edit.entries().map(([uri, values]) => {
          const document = session.documentByReference(uri);
          return {
            documentId: document?.documentId, uri: uri.toString(),
            edits: values.map((value) => serializeTextEdit(value)),
          };
        });
        const expectedVersions = {};
        for (const document of session.documents.values()) expectedVersions[document.documentId] = document.version;
        const result = await session.request('workspace/applyEdit', {
          label: metadata?.label || 'Extension workspace edit', edits, expectedVersions,
        });
        for (const snapshot of Array.isArray(result?.snapshots) ? result.snapshots : []) {
          const current = session.documentByReference(snapshot?.documentId || snapshot?.uri);
          if (!current || !Number.isSafeInteger(snapshot?.version) || snapshot.version > current.version) {
            session.acceptDocument(snapshot, 'change');
          }
        }
        return result?.applied === true;
      },
      getConfiguration(section, scope) { return createConfigurationProxy(session, section, scope); },
      asRelativePath(pathOrUri, includeWorkspaceFolder = false) {
        const value = pathOrUri instanceof Uri ? pathOrUri.fsPath : String(pathOrUri);
        const folders = workspace.workspaceFolders;
        const root = folders?.find((folder) => {
          const relative = path.relative(folder.uri.fsPath, value);
          return relative && !relative.startsWith('..') && !path.isAbsolute(relative);
        }) || folders?.[0];
        if (!root) return value;
        const relative = path.relative(root.uri.fsPath, value);
        return includeWorkspaceFolder ? path.join(root.name, relative) : relative;
      },
      async findFiles(include, exclude, maxResults, token) {
        if (token?.isCancellationRequested) return [];
        const result = await session.request('workspace/findFiles', { include, exclude, maxResults });
        return (result?.uris || []).map((value) => Uri.parse(value));
      },
      getWorkspaceFolder(uri) {
        const folders = workspace.workspaceFolders || [];
        return folders.find((folder) => {
          const relative = path.relative(folder.uri.fsPath, uri.fsPath);
          return relative === '' || (!relative.startsWith('..') && !path.isAbsolute(relative));
        });
      },
      createFileSystemWatcher(globPattern, ignoreCreateEvents, ignoreChangeEvents, ignoreDeleteEvents) {
        return session.track(new FileSystemWatcher(globPattern, ignoreCreateEvents, ignoreChangeEvents, ignoreDeleteEvents));
      },
    };

    const languages = {
      async getLanguages() {
        return [...new Set([...(session.options.languages || []), ...[...session.documents.values()].map((document) => document.languageId)])].sort();
      },
      createLanguageStatusItem(id, selector) {
        const item = new LanguageStatusItem(session, id, selector);
        const proxy = new Proxy(item, {
          set(target, property, value) {
            target[property] = value;
            if (!String(property).startsWith('_') && property !== 'updateScheduled' && property !== 'disposed') target.update();
            return true;
          },
        });
        item.update();
        return session.track(proxy);
      },
      createDiagnosticCollection(name = session.extensionId) {
        requireString(name, 'DiagnosticCollection.name');
        if (session.diagnosticCollections.has(name)) session.diagnosticCollections.get(name).dispose();
        const collection = new DiagnosticCollection(session, name);
        session.diagnosticCollections.set(name, collection);
        return session.track(collection);
      },
      getDiagnostics(uri) {
        if (uri) {
          const key = uri instanceof Uri ? uri.toString() : String(uri);
          return [...session.diagnosticCollections.values()].flatMap((collection) => collection.entries.get(key) || []);
        }
        const result = new Map();
        for (const collection of session.diagnosticCollections.values()) {
          for (const [key, values] of collection.entries) result.set(key, [...(result.get(key) || []), ...values]);
        }
        return [...result].map(([key, values]) => [Uri.parse(key), values]);
      },
      match(selector, document) { return matchesSelector(selector, document) ? 10 : 0; },
      registerDocumentFormattingEditProvider(selector, provider) {
        return session.registerLanguageProvider('formatDocument', selector, provider, 'provideDocumentFormattingEdits');
      },
      registerDocumentRangeFormattingEditProvider(selector, provider) {
        return session.registerLanguageProvider('formatRange', selector, provider, 'provideDocumentRangeFormattingEdits');
      },
      registerCompletionItemProvider(selector, provider, ...triggerCharacters) {
        return session.registerLanguageProvider('completion', selector, provider, 'provideCompletionItems', { triggerCharacters });
      },
      registerCodeActionsProvider(selector, provider, metadata) {
        return session.registerLanguageProvider('codeActions', selector, provider, 'provideCodeActions', metadata);
      },
      registerHoverProvider(selector, provider) {
        return session.registerLanguageProvider('hover', selector, provider, 'provideHover');
      },
      registerDefinitionProvider(selector, provider) {
        return session.registerLanguageProvider('definition', selector, provider, 'provideDefinition');
      },
      registerReferenceProvider(selector, provider) {
        return session.registerLanguageProvider('references', selector, provider, 'provideReferences');
      },
      registerRenameProvider(selector, provider) {
        return session.registerLanguageProvider('rename', selector, provider, 'provideRenameEdits');
      },
      registerDocumentSymbolProvider(selector, provider) {
        return session.registerLanguageProvider('documentSymbols', selector, provider, 'provideDocumentSymbols');
      },
      registerWorkspaceSymbolProvider(provider) {
        return session.registerLanguageProvider('workspaceSymbols', '*', provider, 'provideWorkspaceSymbols');
      },
      async setTextDocumentLanguage(document, languageId) {
        const result = await session.request('workspace/document/setLanguage', {
          documentId: document.documentId, languageId: requireString(languageId, 'languageId'),
        });
        if (result?.snapshot) session.acceptDocument(result.snapshot, 'change');
        return document;
      },
    };

    const showMessage = async (severity, message, values) => {
      requireString(message, 'message');
      const { options, original, actions } = parseMessageArguments([...values]);
      const result = await session.request('workbench/notification/show', {
        extensionId: session.extensionId,
        generation: session.generation,
        severity,
        message,
        detail: options.detail,
        modal: options.modal === true,
        actions,
      });
      return Number.isInteger(result?.selectedIndex) ? original[result.selectedIndex] : undefined;
    };

    const window = {
      get activeTextEditor() { return session.activeEditor; },
      get visibleTextEditors() { return [...session.editors.values()]; },
      get activeNotebookEditor() { return undefined; },
      get visibleNotebookEditors() { return []; },
      get activeColorTheme() { return Object.freeze({ kind: ColorThemeKind.Light }); },
      onDidChangeActiveTextEditor: session.activeEditorEmitter.event,
      onDidChangeVisibleTextEditors: session.visibleEditorsEmitter.event,
      onDidChangeTextEditorSelection: session.selectionEmitter.event,
      onDidChangeTextEditorOptions: session.editorOptionsEmitter.event,
      onDidChangeWindowState: session.windowStateEmitter.event,
      onDidChangeActiveColorTheme: noOpEvent,
      onDidChangeActiveNotebookEditor: noOpEvent,
      onDidChangeVisibleNotebookEditors: noOpEvent,
      onDidChangeNotebookEditorSelection: noOpEvent,
      onDidChangeNotebookEditorVisibleRanges: noOpEvent,
      tabGroups: Object.freeze({
        get all() { return []; }, get activeTabGroup() { return undefined; },
        onDidChangeTabs: noOpEvent, onDidChangeTabGroups: noOpEvent,
        async close() { return false; },
      }),
      createStatusBarItem(idOrAlignment, alignmentOrPriority, priority) {
        let id;
        let alignment;
        let resolvedPriority;
        let releaseId = () => {};
        if (typeof idOrAlignment === 'string') {
          id = requireString(idOrAlignment, 'StatusBarItem.id');
          if (session.statusBarIds.has(id)) throw new Error(`status bar item already exists: ${id}`);
          session.statusBarIds.add(id);
          releaseId = () => session.statusBarIds.delete(id);
          alignment = alignmentOrPriority ?? StatusBarAlignment.Left;
          resolvedPriority = priority ?? 0;
        } else {
          id = session.extensionId;
          alignment = idOrAlignment ?? StatusBarAlignment.Left;
          resolvedPriority = alignmentOrPriority ?? 0;
        }
        if (alignment !== StatusBarAlignment.Left && alignment !== StatusBarAlignment.Right) {
          releaseId();
          throw new TypeError('invalid StatusBarAlignment');
        }
        if (typeof resolvedPriority !== 'number' || !Number.isFinite(resolvedPriority)) {
          releaseId();
          throw new TypeError('priority must be a finite number');
        }
        return session.track(new StatusBarItem(session, session.allocateHandle('status'), id,
          alignment, resolvedPriority, releaseId));
      },
      createOutputChannel(name, languageId) {
        requireString(name, 'OutputChannel.name');
        const log = languageId && typeof languageId === 'object' && languageId.log === true;
        if (languageId && typeof languageId === 'object') languageId = languageId.languageId;
        if (languageId !== undefined) requireString(languageId, 'OutputChannel.languageId');
        return session.track(new OutputChannel(session, session.allocateHandle('output'), name, languageId, log));
      },
      createLanguageStatusItem(id, selector) {
        const item = new LanguageStatusItem(session, id, selector);
        const proxy = new Proxy(item, {
          set(target, property, value) {
            target[property] = value;
            if (!String(property).startsWith('_') && property !== 'updateScheduled' && property !== 'disposed') target.update();
            return true;
          },
        });
        item.update();
        return session.track(proxy);
      },
      createTextEditorDecorationType(options = {}) {
        const key = session.allocateHandle('decoration');
        session.notify('window/editor/registerDecorationType', {
          key, extensionId: session.extensionId, generation: session.generation, options,
        });
        return session.track(Object.freeze({
          key,
          dispose() {
            session.notify('window/editor/removeDecorationType', {
              key, extensionId: session.extensionId, generation: session.generation,
            });
          },
        }));
      },
      registerTerminalProfileProvider(id, provider) {
        requireString(id, 'terminal profile id');
        if (!provider || typeof provider.provideTerminalProfile !== 'function') throw new TypeError('TerminalProfileProvider expected');
        session.notify('workbench/terminal/registerProfileProvider', {
          id, extensionId: session.extensionId, generation: session.generation,
        });
        return session.track(new Disposable(() => session.notify('workbench/terminal/unregisterProfileProvider', {
          id, extensionId: session.extensionId, generation: session.generation,
        })));
      },
      registerWebviewViewProvider(viewId, provider) {
        requireString(viewId, 'webview view id');
        if (!provider || typeof provider.resolveWebviewView !== 'function') throw new TypeError('WebviewViewProvider expected');
        session.notify('workbench/webview/registerUnsupported', {
          viewId, extensionId: session.extensionId, generation: session.generation,
          error: { code: 'UnsupportedCapability', capability: 'window.registerWebviewViewProvider' },
        });
        return session.track(new Disposable(() => session.notify('workbench/webview/unregisterUnsupported', {
          viewId, extensionId: session.extensionId, generation: session.generation,
        })));
      },
      createWebviewPanel() { throw new UnsupportedCapabilityError(session.extensionId, 'window.createWebviewPanel'); },
      createTreeView(viewId, options) {
        requireString(viewId, 'viewId');
        if (session.viewIds.has(viewId)) throw new Error(`tree view already registered: ${viewId}`);
        const view = new TreeView(session, viewId, options);
        session.viewIds.add(viewId);
        session.views.set(view.handle, view);
        return session.track(view);
      },
      registerTreeDataProvider(viewId, treeDataProvider) {
        requireString(viewId, 'viewId');
        if (session.viewIds.has(viewId)) throw new Error(`tree view already registered: ${viewId}`);
        const view = new TreeView(session, viewId, { treeDataProvider });
        session.viewIds.add(viewId);
        session.views.set(view.handle, view);
        return session.track(view);
      },
      showInformationMessage(message, ...items) { return showMessage('information', message, items); },
      showWarningMessage(message, ...items) { return showMessage('warning', message, items); },
      showErrorMessage(message, ...items) { return showMessage('error', message, items); },
      async showQuickPick(items, options = {}, token) {
        const values = await Promise.resolve(items);
        if (!Array.isArray(values)) throw new TypeError('QuickPick items must be an array');
        if (token?.isCancellationRequested) return undefined;
        const result = await session.request('workbench/quickInput/showQuickPick', {
          extensionId: session.extensionId,
          generation: session.generation,
          options,
          items: values.map((item, index) => typeof item === 'string'
            ? { index, label: item }
            : { index, ...item }),
        });
        if (options.canPickMany) {
          return Array.isArray(result?.selectedIndices)
            ? result.selectedIndices.filter((index) => Number.isInteger(index) && index >= 0 && index < values.length)
              .map((index) => values[index])
            : undefined;
        }
        return Number.isInteger(result?.selectedIndex) ? values[result.selectedIndex] : undefined;
      },
      async showInputBox(options = {}, token) {
        if (token?.isCancellationRequested) return undefined;
        const result = await session.request('workbench/quickInput/showInputBox', {
          extensionId: session.extensionId, generation: session.generation, options,
        });
        return typeof result?.value === 'string' ? result.value : undefined;
      },
      async showTextDocument(documentOrUri, columnOrOptions, preserveFocus) {
        const document = documentOrUri instanceof TextDocument ? documentOrUri : await workspace.openTextDocument(documentOrUri);
        const options = typeof columnOrOptions === 'object' ? columnOrOptions
          : { viewColumn: columnOrOptions, preserveFocus: Boolean(preserveFocus) };
        const result = await session.request('window/showTextDocument', {
          documentId: document.documentId, uri: document.uri.toString(), options,
        });
        return session.acceptActiveEditor(result?.editor || { documentId: document.documentId, ...options });
      },
      async withProgress(options, task) {
        if (!options || typeof task !== 'function') throw new TypeError('withProgress requires options and task');
        const handle = session.allocateHandle('progress');
        const cancellation = new CancellationTokenSource();
        session.progress.set(handle, cancellation);
        session.notify('workbench/progress/start', {
          handle, extensionId: session.extensionId, generation: session.generation, options,
        });
        try {
          return await task({
            report(value) {
              session.notify('workbench/progress/report', {
                handle, extensionId: session.extensionId, generation: session.generation, value,
              });
            },
          }, cancellation.token);
        } finally {
          session.progress.delete(handle);
          cancellation.dispose();
          session.notify('workbench/progress/end', {
            handle, extensionId: session.extensionId, generation: session.generation,
          });
        }
      },
    };

    const env = Object.freeze({
      appName: 'Sakura Editor NEXT', appRoot: session.options.appRoot || '', appHost: 'desktop',
      language: optionsLanguage(session.options), uiKind: 1, uriScheme: 'sakura', remoteName: undefined,
      shell: process.env.ComSpec || '', machineId: session.options.machineId || 'sakura-anonymous',
      sessionId: session.options.sessionId || `sakura-${process.pid}`,
      clipboard: Object.freeze({
        async readText() { return (await session.request('env/clipboard/readText', {}))?.value || ''; },
        async writeText(value) { await session.request('env/clipboard/writeText', { value: String(value) }); },
      }),
      async openExternal(uri) { return (await session.request('env/openExternal', { uri: serializeUri(uri) }))?.opened === true; },
      async asExternalUri(uri) { return uri; },
      createTelemetryLogger() {
        return Object.freeze({
          isUsageEnabled: false, isErrorsEnabled: false, onDidChangeEnableStates: noOpEvent,
          logUsage() {}, logError() {}, dispose() {},
        });
      },
    });

    const tasks = Object.freeze({
      registerTaskProvider(type, provider) {
        requireString(type, 'task provider type');
        if (!provider || typeof provider.provideTasks !== 'function') throw new TypeError('TaskProvider expected');
        const handle = session.allocateHandle('taskProvider');
        session.notify('workbench/tasks/registerProvider', {
          handle, type, extensionId: session.extensionId, generation: session.generation,
        });
        return session.track(new Disposable(() => session.notify('workbench/tasks/unregisterProvider', {
          handle, type, extensionId: session.extensionId, generation: session.generation,
        })));
      },
      onDidStartTask: noOpEvent,
      onDidEndTask: noOpEvent,
      onDidStartTaskProcess: noOpEvent,
      onDidEndTaskProcess: noOpEvent,
      get taskExecutions() { return []; },
      async fetchTasks() { throw new UnsupportedCapabilityError(session.extensionId, 'tasks.fetchTasks'); },
      async executeTask() { throw new UnsupportedCapabilityError(session.extensionId, 'tasks.executeTask'); },
    });

    const unsupportedNamespace = (name) => new Proxy(Object.create(null), {
      get(_target, property) {
        if (property === Symbol.toStringTag) return 'UnsupportedCapability';
        throw new UnsupportedCapabilityError(session.extensionId, `${name}.${String(property)}`);
      },
    });

    return Object.freeze({
      version: '1.104.0',
      commands: Object.freeze(commands),
      window: Object.freeze(window),
      workspace: Object.freeze(workspace),
      languages: Object.freeze(languages),
      env,
      debug: unsupportedNamespace('debug'),
      tasks,
      scm: unsupportedNamespace('scm'),
      Disposable,
      EventEmitter,
      CancellationToken,
      CancellationTokenSource,
      Position,
      Range,
      Selection,
      Uri,
      Location,
      TextEdit,
      WorkspaceEdit,
      EndOfLine,
      TextDocumentSaveReason,
      FileType,
      FileSystemError,
      ViewColumn,
      OverviewRulerLane,
      ColorThemeKind,
      Diagnostic,
      DiagnosticRelatedInformation,
      DiagnosticSeverity,
      DiagnosticTag,
      LanguageStatusSeverity,
      CompletionItem,
      CompletionItemKind,
      CompletionList,
      CodeAction,
      CodeActionKind,
      CodeLens,
      DocumentLink,
      Hover,
      SymbolInformation,
      DocumentSymbol,
      CallHierarchyItem,
      TypeHierarchyItem,
      InlayHint,
      InlayHintKind,
      SnippetString,
      RelativePattern,
      ThemeIcon,
      SymbolKind,
      CancellationError,
      CustomExecution,
      ShellExecution,
      Task,
      TaskGroup,
      TaskScope,
      ConfigurationTarget,
      ThemeColor,
      MarkdownString,
      StatusBarAlignment,
      ProgressLocation,
      TreeItem,
      TreeItemCollapsibleState,
    });
  }

  async handleRequest(method, params) {
    switch (method) {
      case 'extension/commands/execute': {
        const command = requireString(params?.command, 'command');
        const registered = this.commands.get(command);
        if (!registered) throw new Error(`command handler is not registered: ${command}`);
        const args = Array.isArray(params?.args) ? params.args : [];
        return { value: await registered.callback.apply(registered.thisArg, args) };
      }
      case 'extension/progress/cancel':
        this.progress.get(params?.handle)?.cancel();
        return { accepted: this.progress.has(params?.handle) };
      case 'extension/secrets/didChange':
        this.secrets.acceptChange(params?.key);
        return { accepted: true };
      case 'extension/workspace/didOpen':
        return { accepted: Boolean(this.acceptDocument(params?.snapshot, 'open')) };
      case 'extension/workspace/didChange': {
        const current = this.documentByReference(params?.snapshot?.documentId || params?.documentId);
        if (current && params?.snapshotOnly !== true && Number.isSafeInteger(params?.snapshot?.version) && params.snapshot.version > current.version + 1) {
          this.notify('workspace/document/versionGap', {
            documentId: current.documentId, expectedVersion: current.version + 1, actualVersion: params.snapshot.version,
          });
        }
        return { accepted: Boolean(this.acceptDocument(params?.snapshot, 'change')) };
      }
      case 'extension/workspace/didSave':
        return { accepted: Boolean(this.acceptDocument(params?.snapshot, 'save')) };
      case 'extension/workspace/didClose':
        return { accepted: this.acceptCloseDocument(params?.documentId || params?.uri) };
      case 'extension/workspace/willSave': {
        const document = this.documentByReference(params?.documentId || params?.uri);
        if (!document) return { edits: [], expectedVersion: undefined };
        const pending = [];
        let accepting = true;
        const event = Object.freeze({
          document,
          reason: params?.reason ?? TextDocumentSaveReason.Manual,
          waitUntil(thenable) {
            if (!accepting) throw new Error('waitUntil must be called synchronously during onWillSaveTextDocument');
            pending.push(Promise.resolve(thenable));
          },
        });
        this.documentWillSaveEmitter.fire(event);
        accepting = false;
        const groups = await Promise.all(pending);
        const edits = groups.flatMap((group) => Array.isArray(group) ? group : []).slice(0, 10000);
        return { edits: edits.map(serializeTextEdit), expectedVersion: document.version };
      }
      case 'extension/window/didChangeActiveTextEditor':
        return { accepted: true, active: Boolean(this.acceptActiveEditor(params?.editor)) };
      case 'extension/window/didChangeSelection': {
        const editor = this.editors.get(params?.documentId);
        if (!editor) return { accepted: false };
        editor.selections = (params?.selections || []).map((value) => new Selection(
          new Position(value.anchor.line, value.anchor.character), new Position(value.active.line, value.active.character)));
        this.selectionEmitter.fire(Object.freeze({ textEditor: editor, selections: [...editor.selections], kind: params?.kind }));
        return { accepted: true };
      }
      case 'extension/window/didChangeState':
        this.windowStateEmitter.fire(Object.freeze({ focused: params?.focused === true, active: params?.active !== false }));
        return { accepted: true };
      case 'extension/workspace/didChangeConfiguration':
        for (const [key, value] of Object.entries(params?.values || {})) {
          if (value === undefined) this.configuration.delete(key);
          else this.configuration.set(key, value);
        }
        this.configurationEmitter.fire(Object.freeze({
          affectsConfiguration: (section) => Object.keys(params?.values || {}).some((key) => key === section || key.startsWith(`${section}.`)),
        }));
        return { accepted: true };
      case 'extension/languages/provide':
        return this.invokeLanguageProvider(requireString(params?.kind, 'provider kind'), params);
      case 'extension/views/getChildren': {
        const view = this.views.get(params?.handle);
        if (!view) throw new Error(`tree view is not registered: ${params?.handle}`);
        return view.getChildren(params?.parentHandle);
      }
      case 'extension/views/didSelect': {
        const view = this.views.get(params?.handle);
        if (!view) throw new Error(`tree view is not registered: ${params?.handle}`);
        return view.acceptSelection(params?.itemHandles);
      }
      case 'extension/views/didChangeVisibility': {
        const view = this.views.get(params?.handle);
        if (!view) throw new Error(`tree view is not registered: ${params?.handle}`);
        return view.acceptVisibility(params?.visible);
      }
      case 'extension/views/didChangeCheckboxState': {
        const view = this.views.get(params?.handle);
        if (!view) throw new Error(`tree view is not registered: ${params?.handle}`);
        return view.acceptCheckboxState(params?.items);
      }
      default:
        return undefined;
    }
  }

  createExtensionContext(paths = {}) {
    const extensionPath = paths.extensionPath || '';
    const storagePath = paths.storagePath;
    const globalStoragePath = paths.globalStoragePath || '';
    const logPath = paths.logPath || '';
    const workspaceState = new JsonMemento(storagePath ? path.join(storagePath, 'workspace-state.json') : undefined);
    const globalState = new JsonMemento(globalStoragePath ? path.join(globalStoragePath, 'global-state.json') : undefined);
    return {
      subscriptions: [],
      extensionPath,
      extensionUri: extensionPath ? Uri.file(extensionPath) : Uri.file(process.cwd()),
      storagePath,
      storageUri: storagePath ? Uri.file(storagePath) : undefined,
      globalStoragePath,
      globalStorageUri: globalStoragePath ? Uri.file(globalStoragePath) : Uri.file(process.cwd()),
      logPath,
      logUri: logPath ? Uri.file(logPath) : Uri.file(process.cwd()),
      workspaceState,
      globalState,
      secrets: this.secrets,
      extensionMode: 1,
      extension: Object.freeze({ id: this.extensionId, extensionPath, extensionUri: extensionPath ? Uri.file(extensionPath) : undefined,
        isActive: true, packageJSON: paths.packageJSON || {} }),
      asAbsolutePath(relativePath) { return path.join(extensionPath, String(relativePath)); },
    };
  }

  assertActive() {
    if (this.disposed) throw new Error(`extension API session is disposed: ${this.extensionId}`);
  }

  dispose() {
    if (this.disposed) return;
    for (const disposable of [...this.disposables]) {
      try { disposable.dispose(); } catch {}
    }
    this.disposables.clear();
    for (const source of this.progress.values()) source.cancel();
    this.progress.clear();
    for (const collection of [...this.diagnosticCollections.values()]) collection.dispose();
    this.diagnosticCollections.clear();
    for (const emitter of [this.documentOpenEmitter, this.documentChangeEmitter, this.documentSaveEmitter,
      this.documentCloseEmitter, this.documentWillSaveEmitter, this.configurationEmitter, this.activeEditorEmitter,
      this.visibleEditorsEmitter, this.selectionEmitter, this.editorOptionsEmitter, this.windowStateEmitter]) emitter.dispose();
    this.documents.clear();
    this.documentIdsByUri.clear();
    this.editors.clear();
    this.languageProviders.clear();
    this.secrets.dispose();
    this.disposed = true;
    this.transport.notify('workbench/extensions/removeGeneration', {
      extensionId: this.extensionId, generation: this.generation,
    });
  }
}

function optionsLanguage(options) {
  return typeof options?.language === 'string' && options.language.length !== 0 ? options.language : 'ja';
}

module.exports = {
  CodeAction,
  CodeActionKind,
  CodeLens,
  DocumentLink,
  Hover,
  SymbolInformation,
  DocumentSymbol,
  CallHierarchyItem,
  TypeHierarchyItem,
  InlayHint,
  InlayHintKind,
  SnippetString,
  RelativePattern,
  ThemeIcon,
  SymbolKind,
  CancellationError,
  CustomExecution,
  ShellExecution,
  Task,
  TaskGroup,
  TaskScope,
  CancellationTokenSource,
  CancellationToken,
  CompletionItem,
  CompletionItemKind,
  CompletionList,
  ConfigurationTarget,
  Diagnostic,
  DiagnosticRelatedInformation,
  DiagnosticSeverity,
  DiagnosticTag,
  Disposable,
  EndOfLine,
  EventEmitter,
  ExtensionApiSession,
  FileType,
  FileSystemError,
  Location,
  LanguageStatusSeverity,
  MarkdownString,
  OUTPUT_FLUSH_MS,
  Position,
  ProgressLocation,
  Range,
  Selection,
  StatusBarAlignment,
  TextDocument,
  TextDocumentSaveReason,
  TextEdit,
  ThemeColor,
  TreeItem,
  TreeItemCollapsibleState,
  UnsupportedCapabilityError,
  Uri,
  ViewColumn,
  OverviewRulerLane,
  ColorThemeKind,
  WorkspaceEdit,
};

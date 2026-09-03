import Parser from 'web-tree-sitter';
import { fileURLToPath } from 'node:url';
import { dirname, join, extname } from 'node:path';

const HERE = dirname(fileURLToPath(import.meta.url));
const GRAMMARS = join(HERE, '..', 'grammars');

const LANG_BY_EXT = {
  '.js': 'javascript', '.jsx': 'javascript', '.mjs': 'javascript', '.cjs': 'javascript',
  '.ts': 'typescript', '.tsx': 'typescript',
  '.py': 'python',
};

// Node types that name a top-level code unit → a "kind".
const SYMBOL_TYPES = {
  function_declaration: 'function',
  function_definition: 'function',   // python
  generator_function_declaration: 'function',
  class_declaration: 'class',
  class_definition: 'class',          // python
  lexical_declaration: 'function',    // const x = () => ...  (resolved in nameOf)
};

const METHOD_TYPES = {
  method_definition: 'method',        // js/ts
  function_definition: 'method',      // python: def inside a class body
};

// Wrappers to descend through when scanning top level / class bodies.
const WRAPPERS = new Set(['export_statement', 'decorated_definition']);

let _init;
const _langCache = new Map();

async function getLanguage(lang) {
  if (!_init) _init = Parser.init({ locateFile: () => join(GRAMMARS, 'tree-sitter.wasm') });
  await _init;
  if (_langCache.has(lang)) return _langCache.get(lang);
  const L = await Parser.Language.load(join(GRAMMARS, `tree-sitter-${lang}.wasm`));
  _langCache.set(lang, L);
  return L;
}

function unwrap(node) {
  // export_statement / decorated_definition wrap the real declaration.
  let n = node;
  while (n && WRAPPERS.has(n.type)) {
    const inner = n.namedChildren.find(c => SYMBOL_TYPES[c.type] || METHOD_TYPES[c.type] || WRAPPERS.has(c.type));
    if (!inner) break;
    n = inner;
  }
  return n;
}

function nameOf(node) {
  const id = node.childForFieldName('name');
  if (id) return id.text;
  // arrow/function assigned to a const: variable_declarator(name, value)
  const decl = node.namedChildren.find(c => c.type === 'variable_declarator');
  if (decl) {
    const nm = decl.childForFieldName('name');
    const val = decl.childForFieldName('value');
    if (nm && val && (val.type === 'arrow_function' || val.type === 'function' || val.type === 'function_expression')) return nm.text;
  }
  return null;
}

function record(chunks, path, source, node, kind, name) {
  chunks.push({
    path,
    span: [node.startPosition.row + 1, node.endPosition.row + 1],
    symbol: name,
    kind,
    text: source.slice(node.startIndex, node.endIndex),
  });
}

// Find the body node of a class (js: class_body; python: block).
function classBody(node) {
  return node.namedChildren.find(c => c.type === 'class_body' || c.type === 'block');
}

export async function chunkTreeSitter(path, source) {
  const lang = LANG_BY_EXT[extname(path).toLowerCase()];
  if (!lang) return null;
  const L = await getLanguage(lang);
  const parser = new Parser();
  parser.setLanguage(L);
  const tree = parser.parse(source);
  const chunks = [];

  for (const top of tree.rootNode.namedChildren) {
    const node = unwrap(top);
    if (!node) continue;
    const kind = SYMBOL_TYPES[node.type];
    if (!kind) continue;
    const name = nameOf(node);
    if (!name) continue;
    record(chunks, path, source, node, kind, name);
    if (kind === 'class') {
      const body = classBody(node);
      if (body) {
        for (const m of body.namedChildren) {
          const mn = unwrap(m);
          const mk = METHOD_TYPES[mn?.type];
          if (!mk) continue;
          const mname = nameOf(mn);
          if (mname) record(chunks, path, source, mn, mk, mname);
        }
      }
    }
  }
  return chunks;
}

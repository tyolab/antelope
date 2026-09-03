import { querySearch } from './query.js';
import { reindex } from './reindex.js';

// Plain callable tools (no transport) so they are unit-testable.
export function buildTools(root, cfg, { pq = false } = {}) {
  return {
    async search({ query, k = 8, paths, mode = 'hybrid', max_tokens = 1500 }) {
      return querySearch(root, cfg, { query, k, maxTokens: max_tokens, mode, pq });
    },
    async index({ paths, incremental = true } = {}) {
      return reindex(root, cfg, { pq });
    },
  };
}

export async function startMcp(root, cfg, opts = {}) {
  // Lazy-import the SDK so the unit test (which targets buildTools) needs no transport.
  const { McpServer } = await import('@modelcontextprotocol/sdk/server/mcp.js');
  const { StdioServerTransport } = await import('@modelcontextprotocol/sdk/server/stdio.js');
  const { z } = await import('zod');

  const tools = buildTools(root, cfg, opts);
  const server = new McpServer({ name: 'codesearch', version: '0.0.1' });

  server.tool(
    'search',
    'Local code search: hybrid lexical+vector, token-budgeted, source-linked (path:start-end).',
    { query: z.string(), k: z.number().default(8), mode: z.enum(['hybrid', 'lexical', 'vector']).default('hybrid'), max_tokens: z.number().default(1500) },
    async (args) => ({ content: [{ type: 'text', text: JSON.stringify(await tools.search(args)) }] }),
  );
  server.tool(
    'index',
    'Incrementally (re)index the workspace.',
    { incremental: z.boolean().default(true) },
    async (args) => ({ content: [{ type: 'text', text: JSON.stringify(await tools.index(args)) }] }),
  );

  await server.connect(new StdioServerTransport());
}

export function loadConfig(env = process.env) {
  const indexDir = env.SPOOR_INDEX_DIR;
  if (!indexDir) throw new Error('SPOOR_INDEX_DIR is required');
  const embedUrl = env.EMBED_URL;
  if (!embedUrl) throw new Error('EMBED_URL is required');
  const embedMode = env.EMBED_MODE === 'multi' ? 'multi' : 'single';
  return {
    indexDir,
    embedUrl: embedUrl.replace(/\/+$/, ''),
    embedModel: env.EMBED_MODEL || (embedMode === 'multi' ? 'gte-moderncolbert' : 'nomic-embed-text'),
    embedMode,
    addonPath: env.ANTELOPE_ADDON || null,
  };
}

export const LIMITS = Object.freeze({ post: 10, comment: 60, reaction: 120, upload: 30, preview: 120, edit: 60 });
const HOUR_MS = 3600000;

export function initializeQuota(storage) {
  storage.sql.exec('CREATE TABLE IF NOT EXISTS quota (kind TEXT PRIMARY KEY, window INTEGER NOT NULL, used INTEGER NOT NULL)');
}

// The clock is supplied by the Durable Object, never by an HTTP client.
export function consumeQuota(storage, kind, now) {
  if (!Object.hasOwn(LIMITS, kind)) throw new Error('unknown quota kind');
  const window = Math.floor(now / HOUR_MS);
  return storage.transactionSync(() => {
    const rows = storage.sql.exec('SELECT window, used FROM quota WHERE kind = ?', kind).toArray();
    const used = rows.length && rows[0].window === window ? rows[0].used : 0;
    if (used >= LIMITS[kind]) return { allowed: false };
    storage.sql.exec('INSERT INTO quota (kind, window, used) VALUES (?, ?, ?) ON CONFLICT(kind) DO UPDATE SET window = excluded.window, used = excluded.used', kind, window, used + 1);
    return { allowed: true };
  });
}

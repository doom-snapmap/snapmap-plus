export class RequestError extends Error {
  constructor(message, status) {
    super(message);
    this.status = status;
  }
}

// Count bytes before retaining them. Buffer growth and chunk metadata stay bounded.
export async function readBody(request, maxBytes) {
  const declared = request.headers.get('Content-Length');
  if (declared !== null && (!/^\d+$/.test(declared) || Number(declared) > maxBytes)) {
    try { await request.body?.cancel(); } catch { /* A disconnected client may already have closed it. */ }
    throw new RequestError(/^\d+$/.test(declared) ? 'too large' : 'bad content length', /^\d+$/.test(declared) ? 413 : 400);
  }
  if (!request.body) return new Uint8Array(0);
  const reader = request.body.getReader();
  let buffer = new Uint8Array(Math.min(16384, maxBytes));
  let length = 0;
  try {
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      if (!(value instanceof Uint8Array)) throw new RequestError('bad request body', 400);
      if (value.byteLength > maxBytes - length) throw new RequestError('too large', 413);
      if (length + value.byteLength > buffer.length) {
        const larger = new Uint8Array(Math.min(maxBytes, Math.max(buffer.length * 2, length + value.byteLength)));
        larger.set(buffer.subarray(0, length));
        buffer = larger;
      }
      buffer.set(value, length);
      length += value.byteLength;
    }
    return buffer.subarray(0, length);
  } catch (error) {
    try { await reader.cancel(); } catch { /* Preserve the original read error. */ }
    if (error instanceof RequestError) throw error;
    throw new RequestError('bad request body', 400);
  } finally {
    reader.releaseLock();
  }
}

export async function readJsonObject(request, maxBytes, maxChars = Infinity) {
  const bytes = await readBody(request, maxBytes);
  let text, body;
  try { text = new TextDecoder('utf-8', { fatal: true }).decode(bytes); }
  catch { throw new RequestError('bad utf-8', 400); }
  if (text.length > maxChars) throw new RequestError('too large', 413);
  try { body = JSON.parse(text); }
  catch { throw new RequestError('bad json', 400); }
  if (body === null || typeof body !== 'object' || Array.isArray(body)) throw new RequestError('bad json', 400);
  return body;
}

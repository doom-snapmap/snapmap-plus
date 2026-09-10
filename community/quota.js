import { DurableObject } from 'cloudflare:workers';
import { initializeQuota, consumeQuota } from './quota_store.js';

// One object coordinates every session belonging to the same GitHub account.
export class CommunityQuota extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    initializeQuota(this.ctx.storage);
  }

  consume(kind) {
    return consumeQuota(this.ctx.storage, kind, Date.now());
  }
}

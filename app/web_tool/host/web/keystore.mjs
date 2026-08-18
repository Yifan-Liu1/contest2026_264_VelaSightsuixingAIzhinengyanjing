/* app/web_tool/host/web/keystore.mjs
 *
 * Keep the LLM API key in localStorage encrypted under a numeric PIN, and wipe
 * it after five consecutive wrong PINs.
 *
 * What this does and does not buy, stated plainly because a security feature
 * whose limits are unclear is worse than none:
 *
 *   It does protect against someone who gets at the browser afterwards and
 *   starts guessing: five wrong PINs and the ciphertext is gone.
 *
 *   It does NOT protect against someone who copies the localStorage record and
 *   attacks it elsewhere.  The counter lives next to the ciphertext, so an
 *   offline attacker simply does not increment it.  What stands between them
 *   and the key is only the PIN's entropy stretched by PBKDF2 -- and a 4-digit
 *   PIN is 10^4 guesses, which is nothing.  Hence 6 digits minimum here, a
 *   deliberately high iteration count, and a UI that says so.  If the key is
 *   worth more than that, do not tick "remember".
 *
 * Primitives are WebCrypto only: PBKDF2-HMAC-SHA-256 to stretch the PIN,
 * AES-256-GCM to encrypt.  GCM because the key must be detected as wrong
 * rather than decrypting to garbage -- an authentication failure is exactly the
 * signal the attempt counter needs, and rolling our own check over an
 * unauthenticated mode is how people end up with padding oracles.
 *
 * Runs unchanged in the browser and in Node (WebCrypto is global in both), so
 * host/tests/test_keystore.mjs can drive every branch including the wipe.
 */

const STORE_KEY = 'vela.web_tool.secret.v1';

export const MAX_ATTEMPTS = 5;
export const MIN_PIN_DIGITS = 6;

/* 600k iterations: OWASP's 2023 floor for PBKDF2-HMAC-SHA-256, and about a
 * quarter second here.  A quarter second is unnoticeable when unlocking once,
 * and it multiplies the cost of every offline guess by the same factor. */
const PBKDF2_ITERATIONS = 600000;

const enc = new TextEncoder();
const dec = new TextDecoder();

function b64(bytes) {
  let s = '';
  const a = new Uint8Array(bytes);
  for (let i = 0; i < a.length; i++) { s += String.fromCharCode(a[i]); }
  return btoa(s);
}

function unb64(text) {
  const s = atob(text);
  const a = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) { a[i] = s.charCodeAt(i); }
  return a;
}

export function isValidPin(pin) {
  return typeof pin === 'string' && /^[0-9]+$/.test(pin)
    && pin.length >= MIN_PIN_DIGITS;
}

async function deriveKey(pin, salt) {
  const base = await crypto.subtle.importKey(
    'raw', enc.encode(pin), 'PBKDF2', false, ['deriveKey']);
  return crypto.subtle.deriveKey(
    { name: 'PBKDF2', salt, iterations: PBKDF2_ITERATIONS, hash: 'SHA-256' },
    base, { name: 'AES-GCM', length: 256 }, false, ['encrypt', 'decrypt']);
}

/* The storage backend is injectable so the tests do not need a browser and so
 * a future move to IndexedDB does not touch the crypto. */
export function makeKeyStore(storage) {
  const store = storage || globalThis.localStorage;

  function readRecord() {
    const raw = store.getItem(STORE_KEY);
    if (!raw) { return null; }
    try {
      const rec = JSON.parse(raw);
      return rec && rec.v === 1 ? rec : null;
    } catch (e) {
      return null;
    }
  }

  function writeRecord(rec) {
    store.setItem(STORE_KEY, JSON.stringify(rec));
  }

  return {
    /* Is there something stored, and how many tries are left. */
    status() {
      const rec = readRecord();
      if (!rec) { return { stored: false, attemptsLeft: MAX_ATTEMPTS }; }
      return {
        stored: true,
        attemptsLeft: Math.max(0, MAX_ATTEMPTS - (rec.attempts || 0)),
        savedAt: rec.savedAt || null,
        hint: rec.hint || '',
      };
    },

    async save(pin, secret) {
      if (!isValidPin(pin)) {
        throw new Error('PIN 必须是至少 ' + MIN_PIN_DIGITS + ' 位数字');
      }
      if (!secret) { throw new Error('没有要保存的内容'); }

      const salt = crypto.getRandomValues(new Uint8Array(16));
      const iv = crypto.getRandomValues(new Uint8Array(12));
      const key = await deriveKey(pin, salt);
      const ct = await crypto.subtle.encrypt(
        { name: 'AES-GCM', iv }, key, enc.encode(secret));

      writeRecord({
        v: 1,
        salt: b64(salt),
        iv: b64(iv),
        ct: b64(ct),
        attempts: 0,
        savedAt: new Date().toISOString(),
        /* Enough to recognise which key is stored, never enough to use it --
         * the same rule the board applies to kvdb secrets. */
        hint: secret.length > 8
          ? secret.slice(0, 4) + '…' + secret.slice(-4) +
            ' (' + secret.length + ' 字符)'
          : '**** (' + secret.length + ' 字符)',
      });
      return this.status();
    },

    async unlock(pin) {
      const rec = readRecord();
      if (!rec) {
        return { ok: false, reason: 'empty', attemptsLeft: MAX_ATTEMPTS };
      }

      /* Count the attempt before trying it, and persist immediately.  Counting
       * afterwards would let an attacker reload the page between guesses and
       * never reach the limit. */
      const used = (rec.attempts || 0) + 1;
      rec.attempts = used;
      writeRecord(rec);

      let plain;
      try {
        const key = await deriveKey(pin, unb64(rec.salt));
        plain = await crypto.subtle.decrypt(
          { name: 'AES-GCM', iv: unb64(rec.iv) }, key, unb64(rec.ct));
      } catch (e) {
        /* GCM said the key was wrong.  No other outcome is possible here, so
         * there is no ambiguity to resolve. */
        if (used >= MAX_ATTEMPTS) {
          store.removeItem(STORE_KEY);
          return { ok: false, reason: 'wiped', attemptsLeft: 0 };
        }
        return {
          ok: false, reason: 'wrong',
          attemptsLeft: MAX_ATTEMPTS - used,
        };
      }

      rec.attempts = 0;
      writeRecord(rec);
      return { ok: true, secret: dec.decode(plain),
               attemptsLeft: MAX_ATTEMPTS };
    },

    forget() {
      store.removeItem(STORE_KEY);
      return this.status();
    },
  };
}

/* app/web_tool/host/tests/test_keystore.mjs
 *
 * The key store is the one piece of this tool where getting it subtly wrong
 * loses an API key instead of a log line, so it is tested rather than reasoned
 * about.  The cases that matter are the ones a reviewer would ask about:
 *
 *   - does a wrong PIN really fail, and is it counted
 *   - does the counter survive a page reload (i.e. is it persisted before the
 *     attempt, not after)
 *   - does the fifth wrong PIN actually remove the ciphertext
 *   - does a correct PIN reset the counter, so five wrong tries spread over a
 *     week do not wipe a key that is being used
 *   - is the stored record free of the plaintext, and does the hint leak only
 *     what kvdb's masking would
 *
 * Run: node test_keystore.mjs   (Node 22 has WebCrypto and atob/btoa global)
 */

import { makeKeyStore, isValidPin, MAX_ATTEMPTS, MIN_PIN_DIGITS }
  from '../web/keystore.mjs';

let checks = 0;
const failures = [];

function check(cond, what) {
  checks++;
  if (cond) { return true; }
  failures.push(what);
  console.log('  FAIL ' + what);
  return false;
}

/* localStorage stand-in.  Deliberately just a Map: the store must not depend on
 * anything browser-specific, and a reload is then simply a new store object
 * over the same backing map. */
function fakeStorage(backing) {
  const map = backing || new Map();
  return {
    map,
    getItem: (k) => (map.has(k) ? map.get(k) : null),
    setItem: (k, v) => map.set(k, String(v)),
    removeItem: (k) => map.delete(k),
  };
}

const SECRET = 'sk-proj-0123456789abcdefghijklmnopqrstuvwxyz';
const PIN = '135790';
const WRONG = '000000';

async function testPinRules() {
  console.log('PIN rules');
  check(!isValidPin('12345'), 'five digits is too short');
  check(isValidPin('123456'), 'six digits is accepted');
  check(!isValidPin('12345a'), 'letters are rejected');
  check(!isValidPin(''), 'empty is rejected');
  check(!isValidPin(123456), 'a number is not a string');
  check(MIN_PIN_DIGITS === 6, 'minimum is 6 digits');

  const ks = makeKeyStore(fakeStorage());
  let threw = false;
  try { await ks.save('1234', SECRET); } catch (e) { threw = true; }
  check(threw, 'save refuses a short PIN');
}

async function testRoundTrip() {
  console.log('save and unlock');
  const st = fakeStorage();
  const ks = makeKeyStore(st);

  check(ks.status().stored === false, 'nothing stored to begin with');

  const after = await ks.save(PIN, SECRET);
  check(after.stored === true, 'stored after save');
  check(after.attemptsLeft === MAX_ATTEMPTS, 'attempts start full');

  const raw = st.getItem('vela.web_tool.secret.v1');
  check(!raw.includes(SECRET), 'the plaintext is not in the record');
  check(!raw.includes('abcdefghijklmnop'),
        'no fragment of the secret is in the record');
  const rec = JSON.parse(raw);
  check(rec.salt && rec.iv && rec.ct, 'salt, iv and ciphertext are present');
  check(rec.hint === 'sk-p…wxyz (44 字符)',
        'the hint identifies the key without being usable: ' + rec.hint);

  const ok = await ks.unlock(PIN);
  check(ok.ok === true, 'the right PIN unlocks');
  check(ok.secret === SECRET, 'and returns the secret unchanged');
}

async function testWrongPinCounts() {
  console.log('wrong PIN is counted');
  const st = fakeStorage();
  const ks = makeKeyStore(st);
  await ks.save(PIN, SECRET);

  for (let i = 1; i < MAX_ATTEMPTS; i++) {
    const r = await ks.unlock(WRONG);
    check(r.ok === false && r.reason === 'wrong',
          'attempt ' + i + ' fails as "wrong"');
    check(r.attemptsLeft === MAX_ATTEMPTS - i,
          'attempt ' + i + ' leaves ' + (MAX_ATTEMPTS - i) + ' -> got ' +
          r.attemptsLeft);
    check(ks.status().stored === true,
          'still stored after ' + i + ' wrong attempt(s)');
  }

  const last = await ks.unlock(WRONG);
  check(last.ok === false && last.reason === 'wiped',
        'the ' + MAX_ATTEMPTS + 'th wrong PIN reports a wipe');
  check(last.attemptsLeft === 0, 'no attempts left');
  check(ks.status().stored === false, 'and the record is gone');
  check(st.getItem('vela.web_tool.secret.v1') === null,
        'the ciphertext is really removed from storage, not just flagged');

  const after = await ks.unlock(PIN);
  check(after.ok === false && after.reason === 'empty',
        'even the correct PIN cannot bring it back');
}

async function testCounterSurvivesReload() {
  console.log('the counter survives a reload');
  const backing = new Map();
  const ks1 = makeKeyStore(fakeStorage(backing));
  await ks1.save(PIN, SECRET);

  /* Three wrong tries, each one through a *fresh* store object over the same
   * backing map -- which is what reloading the page looks like.  If the count
   * were kept in memory, or written only after a failure, this loop would never
   * reach the limit and the wipe would be defeated by pressing F5. */
  for (let i = 1; i <= 4; i++) {
    const ks = makeKeyStore(fakeStorage(backing));
    const r = await ks.unlock(WRONG);
    check(r.attemptsLeft === MAX_ATTEMPTS - i,
          'reload ' + i + ' still counts (left ' + r.attemptsLeft + ')');
  }

  const ksLast = makeKeyStore(fakeStorage(backing));
  const r = await ksLast.unlock(WRONG);
  check(r.reason === 'wiped', 'the fifth try across reloads wipes');
}

async function testSuccessResetsCounter() {
  console.log('a correct PIN resets the counter');
  const st = fakeStorage();
  const ks = makeKeyStore(st);
  await ks.save(PIN, SECRET);

  await ks.unlock(WRONG);
  await ks.unlock(WRONG);
  check(ks.status().attemptsLeft === MAX_ATTEMPTS - 2, 'two used');

  const ok = await ks.unlock(PIN);
  check(ok.ok === true, 'correct PIN works with attempts outstanding');
  check(ks.status().attemptsLeft === MAX_ATTEMPTS,
        'and the budget is restored, so occasional typos over months do not '
        + 'add up to a wipe');
}

async function testForget() {
  console.log('explicit forget');
  const ks = makeKeyStore(fakeStorage());
  await ks.save(PIN, SECRET);
  const st = ks.forget();
  check(st.stored === false, 'forget removes the record');
}

async function testCiphertextIsSalted() {
  console.log('each save is independently salted');
  const s1 = fakeStorage();
  const s2 = fakeStorage();
  const k1 = makeKeyStore(s1);
  const k2 = makeKeyStore(s2);
  await k1.save(PIN, SECRET);
  await k2.save(PIN, SECRET);
  const r1 = JSON.parse(s1.getItem('vela.web_tool.secret.v1'));
  const r2 = JSON.parse(s2.getItem('vela.web_tool.secret.v1'));
  check(r1.salt !== r2.salt, 'salts differ');
  check(r1.iv !== r2.iv, 'IVs differ');
  check(r1.ct !== r2.ct,
        'same secret and same PIN still give different ciphertext, so the '
        + 'record does not reveal that two browsers hold the same key');
}

await testPinRules();
await testRoundTrip();
await testWrongPinCounts();
await testCounterSurvivesReload();
await testSuccessResetsCounter();
await testForget();
await testCiphertextIsSalted();

console.log('\n' + checks + ' checks, ' + failures.length + ' failure(s)');
for (const f of failures) { console.log('  - ' + f); }
process.exit(failures.length ? 1 : 0);

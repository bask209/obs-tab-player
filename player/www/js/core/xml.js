// SPDX-License-Identifier: Apache-2.0
// Tiny, fast XML reader for well-formed documents (GPIF, MusicXML).
// Produces { name, attrs, children[], text } nodes. No namespaces, no DTD processing.

const ENTITIES = { lt: '<', gt: '>', amp: '&', quot: '"', apos: "'" };
const decode = (s) => (s.indexOf('&') < 0 ? s : s.replace(/&(#x[0-9a-f]+|#\d+|\w+);/gi, (m, e) => {
  if (e[0] === '#') return String.fromCodePoint(e[1] === 'x' || e[1] === 'X' ? parseInt(e.slice(2), 16) : parseInt(e.slice(1), 10));
  return ENTITIES[e] ?? m;
}));

export function parseXml(src) {
  const root = { name: '#root', attrs: {}, children: [], text: '' };
  const stack = [root];
  let i = 0;
  const n = src.length;
  while (i < n) {
    const lt = src.indexOf('<', i);
    if (lt < 0) break;
    if (lt > i) {
      const t = src.slice(i, lt);
      if (t.trim()) stack[stack.length - 1].text += decode(t);
    }
    if (src.startsWith('<![CDATA[', lt)) {
      const end = src.indexOf(']]>', lt + 9);
      stack[stack.length - 1].text += src.slice(lt + 9, end < 0 ? n : end);
      i = end < 0 ? n : end + 3;
    } else if (src.startsWith('<!--', lt)) {
      const end = src.indexOf('-->', lt + 4);
      i = end < 0 ? n : end + 3;
    } else if (src[lt + 1] === '?' || src[lt + 1] === '!') {
      const end = src.indexOf('>', lt + 1);
      i = end < 0 ? n : end + 1;
    } else if (src[lt + 1] === '/') {
      const end = src.indexOf('>', lt + 2);
      if (stack.length > 1) stack.pop();
      i = end < 0 ? n : end + 1;
    } else {
      const end = findTagEnd(src, lt + 1);
      const selfClose = src[end - 1] === '/';
      const body = src.slice(lt + 1, selfClose ? end - 1 : end);
      const sp = body.search(/[\s]/);
      const name = sp < 0 ? body : body.slice(0, sp);
      const node = { name, attrs: sp < 0 ? {} : parseAttrs(body.slice(sp)), children: [], text: '' };
      stack[stack.length - 1].children.push(node);
      if (!selfClose) stack.push(node);
      i = end + 1;
    }
  }
  return root;
}

function findTagEnd(src, i) {
  let q = null;
  for (; i < src.length; i++) {
    const c = src[i];
    if (q) { if (c === q) q = null; } else if (c === '"' || c === "'") q = c; else if (c === '>') return i;
  }
  return src.length;
}

function parseAttrs(s) {
  const attrs = {};
  const re = /([^\s=]+)\s*=\s*("([^"]*)"|'([^']*)')/g;
  let m;
  while ((m = re.exec(s))) attrs[m[1]] = decode(m[3] ?? m[4]);
  return attrs;
}

// Helpers
export const child = (node, name) => node?.children.find((c) => c.name === name) || null;
export const children = (node, name) => (node ? node.children.filter((c) => c.name === name) : []);
export const text = (node, name) => {
  const c = name ? child(node, name) : node;
  return c ? c.text.trim() : '';
};
export const path = (node, ...names) => {
  let cur = node;
  for (const n of names) { cur = child(cur, n); if (!cur) return null; }
  return cur;
};

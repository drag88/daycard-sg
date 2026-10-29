// Quotes from Wikiquote, set on the board in Newsreader. Pure C++ (no Arduino), so tools/test_logic.cpp checks it.
// The fetch itself is fetchQuote() in daycard.ino; this file turns a page's wikitext into quotes and a quote into ink.
#pragma once
#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include "assets.h"

// ---- wikitext -> quotes ----

// Sourced quotes come first on a Wikiquote page; stop at the first level-2 heading after them.
inline bool wikiquoteStop(const char *line) {
  if (strncmp(line, "==", 2) || line[2] == '=') return false;
  static const char *const after[] = {"disputed", "misattributed", "attributed", "about", "see also", "external links",
                                      "unsourced", "sources"};
  for (const char *w : after) {
    for (const char *p = line; *p; p++) {
      size_t i = 0;
      while (w[i] && tolower((unsigned char)p[i]) == w[i]) i++;
      if (!w[i]) return true;
    }
  }
  return false;
}

// Pages quoting a translated author often put the original first; a line with none of these words isn't English.
inline bool looksEnglish(const char *s) {
  static const char *const common[] = {"the", "and", "of", "to", "is", "that", "it", "you", "we", "not", "be",
                                       "are", "what", "for", "with", "his", "our", "your", "who", "have", "my"};
  while (*s) {
    while (*s && !isalpha((unsigned char)*s)) s++;
    const char *w = s;
    while (isalpha((unsigned char)*s)) s++;
    for (const char *c : common)
      if (strlen(c) == (size_t)(s - w) && !strncasecmp(w, c, s - w)) return true;
  }
  return false;
}

// One top-level bullet ("* ...") as plain text: links to their text, <br> to a space, refs, tags and emphasis
// dropped. boldOnly keeps just the first '''bold''' stretch, which is how Wikiquote marks the line to remember
// inside a long passage. False for anything else, or for markup this doesn't understand (templates, entities).
inline bool wikiquoteText(const char *line, char *out, size_t n, bool boldOnly = false) {
  if (line[0] != '*' || line[1] == '*' || line[1] == ':') return false;
  const char *s = line + 1;
  size_t len = 0;
  bool bold = false, boldSeen = false;
  auto put = [&](char c) {
    if ((boldOnly && !bold) || len + 1 >= n) return;
    if (c == ' ' && (!len || out[len - 1] == ' ')) return;  // collapse runs of spaces
    out[len++] = c;
  };
  while (*s) {
    if (!strncmp(s, "'''", 3)) {
      if (boldOnly && bold) break;  // the first bold stretch is all we want
      bold = !bold, boldSeen = true, s += 3;
    } else if (!strncmp(s, "''", 2)) {
      s += 2;
    } else if (!strncmp(s, "<ref", 4)) {
      const char *selfClose = strstr(s, "/>"), *end = strstr(s, "</ref>");
      const char *gt = strchr(s, '>');
      if (gt && selfClose && selfClose < gt + 1) s = selfClose + 2;
      else if (end) s = end + 6;
      else return false;
    } else if (*s == '<') {
      if (!strncasecmp(s, "<br", 3)) put(' ');
      const char *gt = strchr(s, '>');
      if (!gt) return false;
      s = gt + 1;
    } else if (!strncmp(s, "[[", 2)) {
      const char *end = strstr(s, "]]");
      if (!end) return false;
      const char *text = s + 2;
      for (const char *p = text; p < end; p++)
        if (*p == '|') text = p + 1;
      if (text == s + 2 && memchr(text, ':', end - text)) return false;  // File:, Category:, w:Article
      while (text < end) put(*text++);
      s = end + 2;
    } else if (*s == '{' || *s == '[' || *s == '&') {
      return false;
    } else {
      char c = *s++;
      put(c == '\t' ? ' ' : c);
    }
  }
  while (len && out[len - 1] == ' ') len--;
  out[len] = 0;
  if (boldOnly && !boldSeen) return false;
  // An editor's note at the end, "(Hays translation)", isn't part of the quote.
  if (len && out[len - 1] == ')')
    if (char *open = strrchr(out, '('); open && open > out && open[-1] == ' ') len = open - out - 1, out[len] = 0;
  // Already in quotation marks: drop them, the card adds its own.
  static const char *const marks[][2] = {{"\"", "\""}, {"“", "”"}};
  for (auto &m : marks) {
    size_t a = strlen(m[0]), b = strlen(m[1]);
    if (len > a + b && !strncmp(out, m[0], a) && !strcmp(out + len - b, m[1])) {
      len -= a + b;
      memmove(out, out + a, len);
      out[len] = 0;
    }
  }
  // A whole sentence, not a fragment lifted from one: a capital (or accented letter) first, a full stop last.
  if (len < 20 || !(isupper((unsigned char)out[0]) || (uint8_t)out[0] >= 0xC0) || !strchr(".!?", out[len - 1]) ||
      !looksEnglish(out))
    return false;
  for (const char *p = out; *p; p++)
    if (strchr("|[]{}<>=", *p)) return false;
  return true;
}

// ---- quote -> ink, with the glyphs tools/make_assets.py rendered into assets.h ----

// Next code point of a UTF-8 string, advancing s; 0xFFFD for a malformed byte.
inline uint32_t nextCodepoint(const char *&s) {
  uint8_t c = (uint8_t)*s++;
  if (c < 0x80) return c;
  int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
  if (more < 0) return 0xFFFD;
  uint32_t cp = c & (0x3F >> more);
  while (more--) {
    if (((uint8_t)*s & 0xC0) != 0x80) return 0xFFFD;
    cp = cp << 6 | ((uint8_t)*s++ & 0x3F);
  }
  return cp;
}

inline const Glyph *glyphFor(const Font &f, uint32_t cp) {
  int lo = 0, hi = f.count - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (f.glyphs[mid].code == cp) return &f.glyphs[mid];
    if (f.glyphs[mid].code < cp) lo = mid + 1;
    else hi = mid - 1;
  }
  return nullptr;
}

// Advance width of s[0..len) in 1/16 px, or -1 if the font lacks a character.
inline int textWidth16(const Font &f, const char *s, size_t len) {
  int w = 0;
  for (const char *end = s + len; s < end;) {
    const Glyph *g = glyphFor(f, nextCodepoint(s));
    if (!g) return -1;
    w += g->adv;
  }
  return w;
}

constexpr int QUOTE_W = 376, QUOTE_STRIDE = QUOTE_W / 8, QUOTE_MAX_H = 72, QUOTE_BUF = QUOTE_STRIDE * (QUOTE_MAX_H + 10);
constexpr int QUOTE_MAX_LINES = 3;

// Greedy word wrap, as make_assets.py does it: line starts into `starts`, returns the line count (more than
// QUOTE_MAX_LINES means it doesn't fit), or -1 if a character is missing from the font.
inline int wrapLines(const Font &f, const char *text, int width, const char *starts[], const char *ends[]) {
  int lines = 0, space = glyphFor(f, ' ')->adv;
  const char *s = text;
  while (*s) {
    while (*s == ' ') s++;
    if (!*s) break;
    const char *start = s, *end = s;
    int w = 0;
    for (;;) {
      const char *word = end;
      while (*word == ' ') word++;
      const char *wordEnd = word;
      while (*wordEnd && *wordEnd != ' ') wordEnd++;
      if (word == wordEnd) break;
      int ww = textWidth16(f, word, wordEnd - word);
      if (ww < 0) return -1;
      int trial = end == start ? ww : w + space + ww;
      if (end != start && trial > width * 16) break;
      w = trial, end = wordEnd;
    }
    if (lines < QUOTE_MAX_LINES) starts[lines] = start, ends[lines] = end;
    lines++;
    s = end;
  }
  return lines;
}

inline void blitGlyph(uint8_t *buf, int rows, const Font &f, const Glyph &g, int x, int y) {
  const uint8_t *bits = f.bits + g.off;
  int stride = (g.w + 7) / 8;
  for (int r = 0; r < g.h; r++)
    for (int c = 0; c < g.w; c++) {
      int px = x + g.x + c, py = y + g.y + r;
      if (px >= 0 && px < QUOTE_W && py >= 0 && py < rows && (bits[r * stride + c / 8] >> (c & 7) & 1))
        buf[py * QUOTE_STRIDE + px / 8] |= 1 << (px & 7);
    }
}

inline void setLine(uint8_t *buf, int rows, const Font &f, const char *s, const char *end, int baseline) {
  int pen = (QUOTE_W * 16 - textWidth16(f, s, end - s)) / 2;  // centred on its advance, like anchor "ms"
  while (s < end) {
    const Glyph *g = glyphFor(f, nextCodepoint(s));
    blitGlyph(buf, rows, f, *g, pen / 16, baseline);
    pen += g->adv;
  }
}

// Set "text" and "— author" the way make_assets.py sets the built-in quotes: the largest size that fits in
// QUOTE_MAX_LINES and QUOTE_MAX_H, the measure narrowed while the line count holds so lines balance. With buf
// (QUOTE_BUF bytes) it draws, and `out` is the ink cropped to its rows; without, it only answers "does it fit".
inline bool setQuote(const char *text, const char *author, uint8_t *buf, Bitmap *out) {
  char quoted[400];
  if (snprintf(quoted, sizeof quoted, "“%s”", text) >= (int)sizeof quoted) return false;
  const char *starts[QUOTE_MAX_LINES], *ends[QUOTE_MAX_LINES];
  for (const Font &f : QUOTE_FONTS) {
    int lines = wrapLines(f, quoted, QUOTE_W, starts, ends);
    if (lines < 0) return false;
    int width = QUOTE_W;
    while (lines > 1 && wrapLines(f, quoted, width - 4, starts, ends) == lines) width -= 4;
    wrapLines(f, quoted, width, starts, ends);
    int lead = (f.size * 12 + 5) / 10, height = lines * lead + 20;
    if (lines > QUOTE_MAX_LINES || height > QUOTE_MAX_H) continue;
    char by[96];
    snprintf(by, sizeof by, "— %s", author);
    if (textWidth16(BYLINE_FONT, by, strlen(by)) < 0) return false;
    if (!buf) return true;
    int rows = height + 10;
    memset(buf, 0, QUOTE_BUF);
    for (int i = 0; i < lines; i++) setLine(buf, rows, f, starts[i], ends[i], f.size + i * lead);
    setLine(buf, rows, BYLINE_FONT, by, by + strlen(by), f.size + lines * lead + 1);
    int top = 0, bottom = rows - 1;
    auto blank = [&](int r) {
      for (int i = 0; i < QUOTE_STRIDE; i++)
        if (buf[r * QUOTE_STRIDE + i]) return false;
      return true;
    };
    while (top < bottom && blank(top)) top++;
    while (bottom > top && blank(bottom)) bottom--;
    *out = {QUOTE_W, (uint16_t)(bottom - top + 1), buf + top * QUOTE_STRIDE};
    return true;
  }
  return false;
}

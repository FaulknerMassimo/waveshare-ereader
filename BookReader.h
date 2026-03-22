#pragma once

#include <Arduino.h>
#include <SD_MMC.h>
#include <FS.h>
#include <string>
#include <vector>
#include <algorithm>

#include "WaveshareRenderer.h"
#include "GUI_Paint.h"
#include "fonts.h"
#include "miniz.h"
#include "Cache.h"

static std::string strip_tags(const char *html, size_t len)
{
  std::string out;
  out.reserve(len);
  bool in_tag = false;
  std::string tag_name;

  auto is_block = [](const std::string &t) {
    return t == "p"  || t == "div" || t == "br" || t == "h1" || t == "h2"  || t == "h3" || t == "h4" || t == "h5"  || t == "h6" || t == "li" || t == "tr";
  };

  for (size_t i = 0; i < len; i++)
  {
    unsigned char c = (unsigned char)html[i];

    if (c == '<')
    {
      in_tag = true;
      tag_name.clear();
      continue;
    }
    if (c == '>')
    {
      in_tag = false;
      std::string tn = tag_name;
      if (!tn.empty() && tn[0] == '/') tn = tn.substr(1);
      size_t sp = tn.find_first_of(" \t\r\n");
      if (sp != std::string::npos) tn = tn.substr(0, sp);
      for (char &ch : tn) ch = tolower((unsigned char)ch);

      if (is_block(tn))
      {
        if (!out.empty() && out.back() != '\n')
          out += '\n';
      }
      else
      {
        if (!out.empty() && out.back() != ' ' && out.back() != '\n')
          out += ' ';
      }
      continue;
    }
    if (in_tag)
    {
      tag_name += (char)c;
      continue;
    }
    if (c == '\r') continue;
    if (c == '\n' || c == '\t') c = ' ';
    if (c == ' ' && !out.empty() && (out.back() == ' ' || out.back() == '\n'))
      continue;
    out += (char)c;
  }
  return out;
}

static unsigned char utf8_to_latin1(const std::string &s, size_t &i)
{
  unsigned char c0 = (unsigned char)s[i];
  size_t len = s.size();

  if ((c0 & 0xE0) == 0xC0 && i + 1 < len)
  {
    unsigned char c1 = (unsigned char)s[i+1];
    uint32_t cp = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
    i += 1;
    if (cp >= 0x00A0 && cp <= 0x00FF) return (unsigned char)cp;
    if (cp == 0x00A0) return ' ';
    if (cp >= 0x0100 && cp <= 0x017E)
    {
      const char bases[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIJijJjKkkLlLlLlLlLlNnNnNnnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZz";
      int idx = (int)(cp - 0x0100);
      if (idx < (int)sizeof(bases)-1) return (unsigned char)bases[idx];
    }

    if (cp == 0x0152) return 'OE'; // Œ
    if (cp == 0x0153) return 'oe'; // œ
    if (cp == 0x0160) return 'S'; // Š
    if (cp == 0x0161) return 's'; // š
    if (cp == 0x017D) return 'Z'; // Ž
    if (cp == 0x017E) return 'z'; // ž
    if (cp == 0x2019 || cp == 0x2018) return '\'';
    if (cp == 0x201C || cp == 0x201D) return '"';
    if (cp == 0x2014 || cp == 0x2013) return '-';
    return '?';
  }

  if ((c0 & 0xF0) == 0xE0 && i + 2 < len)
  {
    unsigned char c1 = (unsigned char)s[i+1];
    unsigned char c2 = (unsigned char)s[i+2];
    uint32_t cp = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
    i += 2;
    if (cp >= 0x2000 && cp <= 0x200A) return ' ';
    if (cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF) return 0;
    if (cp == 0x2010 || cp == 0x2011) return '-';
    if (cp == 0x2012 || cp == 0x2013) return '-';
    if (cp == 0x2014 || cp == 0x2015) return '-';
    if (cp == 0x2018 || cp == 0x2019) return '\'';
    if (cp == 0x201A || cp == 0x201B) return '\'';
    if (cp == 0x201C || cp == 0x201D) return '"';
    if (cp == 0x201E || cp == 0x201F) return '"';
    if (cp == 0x2022) return '*';
    if (cp == 0x2026) return '.';
    if (cp >= 0x2190 && cp <= 0x21FF) return '>';
    return '?';
  }
  if ((c0 & 0xF8) == 0xF0 && i + 3 < len) { i += 3; return 0; }
  return 0;
}

static char utf8_to_ascii(const std::string &s, size_t &i)
{
  return (char)utf8_to_latin1(s, i);
}

static std::string decode_entities(const std::string &s)
{
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++)
  {
    unsigned char c = (unsigned char)s[i];

    if (c == '&')
    {
      size_t semi = s.find(';', i + 1);
      if (semi != std::string::npos && semi - i <= 12)
      {
        std::string e = s.substr(i + 1, semi - i - 1);
        i = semi;

        if      (e == "amp")    { out += '&';  continue; }
        else if (e == "lt")     { out += '<';  continue; }
        else if (e == "gt")     { out += '>';  continue; }
        else if (e == "nbsp")   { out += ' ';  continue; }
        else if (e == "quot")   { out += '"';  continue; }
        else if (e == "apos")   { out += '\''; continue; }
        else if (e == "mdash")  { out += '-';  continue; }
        else if (e == "ndash")  { out += '-';  continue; }
        else if (e == "lsquo" || e == "rsquo")  { out += '\''; continue; }
        else if (e == "ldquo" || e == "rdquo")  { out += '"';  continue; }
        else if (e == "laquo"  || e == "raquo") { out += '"';  continue; }
        else if (e == "hellip") { out += "..."; continue; }
        else if (e == "bull")   { out += '*';  continue; }
        else if (e == "copy")   { out += "(c)"; continue; }
        else if (e == "reg")    { out += "(r)"; continue; }
        else if (e == "trade")  { out += "TM"; continue; }
        else if (e == "shy")    { continue; }
        else if (e == "agrave") { out += (char)0xE0; continue; }
        else if (e == "Agrave") { out += (char)0xC0; continue; }
        else if (e == "aacute") { out += (char)0xE1; continue; }
        else if (e == "Aacute") { out += (char)0xC1; continue; }
        else if (e == "acirc")  { out += (char)0xE2; continue; }
        else if (e == "Acirc")  { out += (char)0xC2; continue; }
        else if (e == "atilde") { out += (char)0xE3; continue; }
        else if (e == "Atilde") { out += (char)0xC3; continue; }
        else if (e == "auml")   { out += (char)0xE4; continue; }
        else if (e == "Auml")   { out += (char)0xC4; continue; }
        else if (e == "aring")  { out += (char)0xE5; continue; }
        else if (e == "Aring")  { out += (char)0xC5; continue; }
        else if (e == "aelig")  { out += (char)0xE6; continue; }
        else if (e == "AElig")  { out += (char)0xC6; continue; }
        else if (e == "ccedil") { out += (char)0xE7; continue; }
        else if (e == "Ccedil") { out += (char)0xC7; continue; }
        else if (e == "egrave") { out += (char)0xE8; continue; }
        else if (e == "Egrave") { out += (char)0xC8; continue; }
        else if (e == "eacute") { out += (char)0xE9; continue; }
        else if (e == "Eacute") { out += (char)0xC9; continue; }
        else if (e == "ecirc")  { out += (char)0xEA; continue; }
        else if (e == "Ecirc")  { out += (char)0xCA; continue; }
        else if (e == "euml")   { out += (char)0xEB; continue; }
        else if (e == "Euml")   { out += (char)0xCB; continue; }
        else if (e == "igrave") { out += (char)0xEC; continue; }
        else if (e == "Igrave") { out += (char)0xCC; continue; }
        else if (e == "iacute") { out += (char)0xED; continue; }
        else if (e == "Iacute") { out += (char)0xCD; continue; }
        else if (e == "icirc")  { out += (char)0xEE; continue; }
        else if (e == "Icirc")  { out += (char)0xCE; continue; }
        else if (e == "iuml")   { out += (char)0xEF; continue; }
        else if (e == "Iuml")   { out += (char)0xCF; continue; }
        else if (e == "ntilde") { out += (char)0xF1; continue; }
        else if (e == "Ntilde") { out += (char)0xD1; continue; }
        else if (e == "ograve") { out += (char)0xF2; continue; }
        else if (e == "Ograve") { out += (char)0xD2; continue; }
        else if (e == "oacute") { out += (char)0xF3; continue; }
        else if (e == "Oacute") { out += (char)0xD3; continue; }
        else if (e == "ocirc")  { out += (char)0xF4; continue; }
        else if (e == "Ocirc")  { out += (char)0xD4; continue; }
        else if (e == "otilde") { out += (char)0xF5; continue; }
        else if (e == "Otilde") { out += (char)0xD5; continue; }
        else if (e == "ouml")   { out += (char)0xF6; continue; }
        else if (e == "Ouml")   { out += (char)0xD6; continue; }
        else if (e == "oslash") { out += (char)0xF8; continue; }
        else if (e == "Oslash") { out += (char)0xD8; continue; }
        else if (e == "ugrave") { out += (char)0xF9; continue; }
        else if (e == "Ugrave") { out += (char)0xD9; continue; }
        else if (e == "uacute") { out += (char)0xFA; continue; }
        else if (e == "Uacute") { out += (char)0xDA; continue; }
        else if (e == "ucirc")  { out += (char)0xFB; continue; }
        else if (e == "Ucirc")  { out += (char)0xDB; continue; }
        else if (e == "uuml")   { out += (char)0xFC; continue; }
        else if (e == "Uuml")   { out += (char)0xDC; continue; }
        else if (e == "yacute") { out += (char)0xFD; continue; }
        else if (e == "Yacute") { out += (char)0xDD; continue; }
        else if (e == "yuml")   { out += (char)0xFF; continue; }
        else if (e == "szlig")  { out += (char)0xDF; continue; }
        else if (e == "laquo")  { out += (char)0xAB; continue; }
        else if (e == "raquo")  { out += (char)0xBB; continue; }
        else if (!e.empty() && e[0] == '#')
        {
          uint32_t cp = 0;
          if (e.size() > 1 && (e[1] == 'x' || e[1] == 'X'))
            cp = strtoul(e.c_str() + 2, nullptr, 16);
          else
            cp = strtoul(e.c_str() + 1, nullptr, 10);
          if (cp >= 32 && cp < 127)       { out += (char)cp; }
          else if (cp == 0x2019 || cp == 0x2018) out += '\'';
          else if (cp == 0x201C || cp == 0x201D) out += '"';
          else if (cp == 0x2014 || cp == 0x2013) out += '-';
          else if (cp == 0x00A0) out += ' ';
          else if (cp >= 0xC0 && cp <= 0xFF)
          {
            char tmp[3] = { (char)(0xC0 | (cp >> 6)), (char)(0x80 | (cp & 0x3F)), 0 };
            std::string ts(tmp, 2);
            size_t idx = 0;
            char r = utf8_to_ascii(ts, idx);
            if (r != '\0') out += r;
          }
          continue;
        }
        continue;
      }
      out += '&';
      continue;
    }

    if (c < 0x80)
    {
      out += (char)c;
      continue;
    }

    unsigned char replacement = utf8_to_latin1(s, i);
    if (replacement != 0)
      out += (char)replacement;
  }
  return out;
}

struct Page
{
  std::vector<std::string> lines;
};

class BookReader
{
public:
  BookReader(WaveshareRenderer *renderer, const char *path)
    : m_renderer(renderer), m_path(path),
      m_current_section(0), m_current_page(0) {}

  ~BookReader() {}

  bool load()
  {
    std::string p = m_path;
    std::transform(p.begin(), p.end(), p.begin(), ::tolower);
    if (p.rfind(".epub") != std::string::npos)
      return load_epub();
    else
      return load_txt();
  }

  void next()
  {
    if (m_current_page < (int)m_pages.size() - 1)
    {
      m_current_page++;
    }
    else if (m_current_section < m_num_sections - 1)
    {
      m_current_section++;
      m_current_page = 0;
      load_current_section();
      build_pages_for_current_section();
    }
  }

  void prev()
  {
    if (m_current_page > 0)
    {
      m_current_page--;
    }
    else if (m_current_section > 0)
    {
      m_current_section--;
      load_current_section();
      build_pages_for_current_section();
      m_current_page = (int)m_pages.size() - 1;
    }
  }

  void go_to(int section, int page)
  {
    m_current_section = std::max(0, std::min(section, m_num_sections - 1));
    load_current_section();
    build_pages_for_current_section();
    m_current_page = std::max(0, std::min(page, (int)m_pages.size() - 1));
  }

  void render()
  {
    if (m_pages.empty()) return;
    const Page &pg = m_pages[m_current_page];

    int y = m_renderer->margin_top();
    int x = m_renderer->margin_left();
    int line_h = m_renderer->get_line_height();

    for (const auto &line : pg.lines)
    {
      m_renderer->draw_text(x, y, line.c_str());
      y += line_h;
      if (y + line_h > m_renderer->get_page_height() + m_renderer->margin_top())
        break;
    }
  }

  int current_section() const { return m_current_section; }
  int current_page() const { return m_current_page; }
  int total_pages_in_section() const { return (int)m_pages.size(); }
  int current_page_in_book()
  {
    ensure_book_pagination();
    int page = 0;
    for (int i = 0; i < m_current_section; i++)
      page += get_or_compute_section_page_count(i);
    return page + m_current_page;
  }
  int total_pages_in_book()
  {
    ensure_book_pagination();
    return std::max(1, m_total_pages_in_book);
  }
  int num_sections() const { return m_num_sections; }
  const char *title() const { return m_title.c_str(); }
  const char *path() const { return m_path.c_str(); }

private:
  WaveshareRenderer *m_renderer;
  std::string m_path;
  std::string m_title;

  std::vector<SectionIndex> m_section_index;
  uint32_t m_header_size = 0;
  int m_current_section;
  int m_num_sections = 0;

  std::string m_current_section_text;

  std::vector<Page>  m_pages;
  int m_current_page;

  std::vector<std::string> m_txt_sections;
  bool m_is_txt = false;
  std::vector<int> m_section_page_counts;
  int m_total_pages_in_book = 0;
  bool m_book_pagination_ready = false;

  void load_current_section()
  {
    if (m_is_txt)
    {
      if (m_current_section < (int)m_txt_sections.size())
        m_current_section_text = m_txt_sections[m_current_section];
      else
        m_current_section_text = "";
    }
    else
    {
      if (m_current_section < (int)m_section_index.size())
        m_current_section_text = book_cache_read_section(
            m_path.c_str(), m_header_size, m_section_index[m_current_section]);
      else
        m_current_section_text = "";
    }
  }

  bool load_txt()
  {
    m_is_txt = true;
    File f = SD_MMC.open(m_path.c_str());
    if (!f) return false;

    size_t sz = f.size();
    char *buf = (char *)heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (char *)malloc(sz + 1);
    if (!buf) { f.close(); return false; }
    f.read((uint8_t *)buf, sz);
    buf[sz] = '\0';
    f.close();

    m_title = m_path.substr(m_path.rfind('/') + 1);
    size_t dot = m_title.rfind('.');
    if (dot != std::string::npos) m_title = m_title.substr(0, dot);

    const size_t CHUNK = 8192;
    for (size_t off = 0; off < sz; off += CHUNK)
    {
      size_t clen = std::min(CHUNK, sz - off);
      m_txt_sections.push_back(std::string(buf + off, clen));
    }
    free(buf);

    if (m_txt_sections.empty()) return false;
    m_num_sections = (int)m_txt_sections.size();
    m_section_page_counts.assign(m_num_sections, -1);
    m_total_pages_in_book = 0;
    m_book_pagination_ready = false;
    load_current_section();
    build_pages_for_current_section();
    return true;
  }

  static size_t sd_file_read(void *pOpaque, mz_uint64 file_ofs, void *pBuf, size_t n)
  {
    File *f = reinterpret_cast<File *>(pOpaque);
    if (!f || !(*f)) return 0;
    if (!f->seek((uint32_t)file_ofs)) return 0;
    return f->read((uint8_t *)pBuf, n);
  }

  bool load_epub()
  {
    m_is_txt = false;
    Serial.printf("[EPUB] Loading: %s  heap=%u\n", m_path.c_str(), ESP.getFreeHeap());

    if (book_cache_exists(m_path.c_str()))
    {
      Serial.println("[EPUB] Cache hit");
      if (book_cache_read_index(m_path.c_str(), m_section_index, m_title))
      {
        m_num_sections = (int)m_section_index.size();
        m_section_page_counts.assign(m_num_sections, -1);
        m_total_pages_in_book = 0;
        m_book_pagination_ready = false;
        m_header_size  = 16 + (uint32_t)m_title.size() + m_num_sections * 8;
        load_current_section();
        build_pages_for_current_section();
        Serial.printf("[EPUB] Loaded from cache: %d sections\n", m_num_sections);
        return m_num_sections > 0;
      }
      Serial.println("[EPUB] Cache corrupt, re-parsing");
    }

    File f = SD_MMC.open(m_path.c_str());
    if (!f) { Serial.println("[EPUB] Cannot open"); return false; }

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    zip.m_pRead      = sd_file_read;
    zip.m_pIO_opaque = &f;

    if (!mz_zip_reader_init(&zip, f.size(), 0))
    {
      Serial.println("[EPUB] miniz init failed");
      f.close(); return false;
    }

    std::string opf_path = find_opf(&zip);
    if (opf_path.empty())
    {
      mz_zip_reader_end(&zip); f.close(); return false;
    }

    if (!parse_opf_streaming(&zip, opf_path))
    {
      mz_zip_reader_end(&zip); f.close(); return false;
    }
    mz_zip_reader_end(&zip);
    f.close();

    if (!book_cache_read_index(m_path.c_str(), m_section_index, m_title))
    {
      Serial.println("[EPUB] Cache read-back failed");
      return false;
    }

    m_num_sections = (int)m_section_index.size();
    m_section_page_counts.assign(m_num_sections, -1);
    m_total_pages_in_book = 0;
    m_book_pagination_ready = false;
    m_header_size  = 16 + (uint32_t)m_title.size() + m_num_sections * 8;
    load_current_section();
    build_pages_for_current_section();
    Serial.printf("[EPUB] Parsed+cached: %d sections  heap=%u\n", m_num_sections, ESP.getFreeHeap());
    return true;
  }

  struct KV { std::string key, val; };
  using unordered_map_lite = std::vector<KV>;

  static void *epub_extract(mz_zip_archive *zip, const char *name, size_t *out_sz)
  {
    mz_uint32 idx;
    if (!mz_zip_reader_locate_file_v2(zip, name, nullptr, 0, &idx))
      return nullptr;

    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(zip, idx, &stat))
      return nullptr;

    size_t sz = (size_t)stat.m_uncomp_size;
    void *buf = heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(sz + 1);
    if (!buf) return nullptr;

    if (!mz_zip_reader_extract_to_mem(zip, idx, buf, sz, 0))
    {
      free(buf);
      return nullptr;
    }
    ((uint8_t *)buf)[sz] = 0;
    if (out_sz) *out_sz = sz;
    return buf;
  }

  std::string find_opf(mz_zip_archive *zip)
  {
    size_t sz;
    char *data = (char *)epub_extract(zip, "META-INF/container.xml", &sz);
    if (!data) return "";

    std::string xml(data, sz);
    free(data);

    const char *needle = "full-path=\"";
    size_t pos = xml.find(needle);
    if (pos == std::string::npos) return "";
    pos += strlen(needle);
    size_t end = xml.find('"', pos);
    if (end == std::string::npos) return "";
    return xml.substr(pos, end - pos);
  }

  bool parse_opf_streaming(mz_zip_archive *zip, const std::string &opf_path)
  {
    size_t sz;
    char *data = (char *)epub_extract(zip, opf_path.c_str(), &sz);
    if (!data) return false;
    std::string xml(data, sz);
    free(data);

    std::string base = opf_path.substr(0, opf_path.rfind('/') + 1);

    extract_tag_content(xml, "dc:title", m_title);
    if (m_title.empty()) extract_tag_content(xml, "title", m_title);
    m_title = decode_entities(m_title);
    Serial.printf("[EPUB] Title: '%s'\n", m_title.c_str());

    BookCacheWriter w2(m_path.c_str(), m_title);
    if (!w2.begin()) return false;

    std::vector<KV> id_to_href;
    {
      size_t p = 0;
      while ((p = xml.find("<item ", p)) != std::string::npos)
      {
        size_t et = xml.find('>', p);
        if (et == std::string::npos) break;
        std::string tag = xml.substr(p, et - p + 1);
        std::string id   = attr_value(tag, "id");
        std::string href = attr_value(tag, "href");
        if (!id.empty() && !href.empty())
          id_to_href.push_back({id, base + href});
        p = et + 1;
      }
    }
    Serial.printf("[EPUB] Manifest: %d items\n", (int)id_to_href.size());

    size_t spine_start = xml.find("<spine");
    size_t spine_end   = xml.find("</spine>", spine_start);
    if (spine_start == std::string::npos) { w2.abort_write(); return false; }
    std::string spine_xml = xml.substr(spine_start, spine_end - spine_start);
    xml.clear(); xml.shrink_to_fit();

    size_t p = 0;
    int count = 0;
    while ((p = spine_xml.find("<itemref ", p)) != std::string::npos)
    {
      size_t et = spine_xml.find('>', p);
      if (et == std::string::npos) break;
      std::string tag   = spine_xml.substr(p, et - p + 1);
      std::string idref = attr_value(tag, "idref");
      p = et + 1;
      count++;

      for (auto &kv : id_to_href)
      {
        if (kv.key != idref) continue;
        size_t html_sz;
        char *html = (char *)epub_extract(zip, kv.val.c_str(), &html_sz);
        if (!html) break;

        std::string text = decode_entities(strip_tags(html, html_sz));
        free(html);

        auto first = text.find_first_not_of(' ');
        auto last  = text.find_last_not_of(' ');
        if (first != std::string::npos)
          text = text.substr(first, last - first + 1);

        if (!text.empty())
          w2.write_section(text);

        if (count % 20 == 0)
          Serial.printf("[EPUB] sect %d  heap=%u\n", count, ESP.getFreeHeap());
        vTaskDelay(1);
        break;
      }
    }

    Serial.printf("[EPUB] %d spine items, %d sections written\n", count, w2.section_count());

    bool ok = (w2.section_count() > 0) && w2.finish();
    if (!ok) w2.abort_write();
    return ok;
  }

  void build_pages_for_current_section()
  {
    m_pages.clear();
    if (m_current_section_text.empty()) return;

    const std::string &text = m_current_section_text;
    int page_w = m_renderer->get_page_width();
    int page_h = m_renderer->get_page_height();
    int line_h = m_renderer->get_line_height();
    int max_lines = page_h / line_h;

    std::vector<std::string> all_lines = word_wrap(text, page_w);

    for (int i = 0; i < (int)all_lines.size(); i += max_lines)
    {
      Page pg;
      int end = std::min(i + max_lines, (int)all_lines.size());
      for (int j = i; j < end; j++)
        pg.lines.push_back(all_lines[j]);
      m_pages.push_back(std::move(pg));
    }

    if (m_pages.empty())
      m_pages.push_back(Page{});

    if (m_current_section >= 0 && m_current_section < (int)m_section_page_counts.size())
    {
      int pages_now = (int)m_pages.size();
      if (m_section_page_counts[m_current_section] != pages_now)
        m_book_pagination_ready = false;
      m_section_page_counts[m_current_section] = pages_now;
    }
  }

  int compute_page_count_from_text(const std::string &text)
  {
    if (text.empty()) return 1;

    int page_h = m_renderer->get_page_height();
    int line_h = m_renderer->get_line_height();
    int max_lines = std::max(1, page_h / line_h);
    int page_w = m_renderer->get_page_width();

    std::vector<std::string> all_lines = word_wrap(text, page_w);
    if (all_lines.empty()) return 1;

    return std::max(1, (int)((all_lines.size() + max_lines - 1) / max_lines));
  }

  int get_or_compute_section_page_count(int section)
  {
    if (section < 0 || section >= m_num_sections) return 0;
    if (section >= (int)m_section_page_counts.size()) return 0;

    int cached = m_section_page_counts[section];
    if (cached > 0) return cached;

    if (section == m_current_section && !m_pages.empty())
    {
      m_section_page_counts[section] = (int)m_pages.size();
      return m_section_page_counts[section];
    }

    std::string section_text;
    if (m_is_txt)
    {
      if (section < (int)m_txt_sections.size())
        section_text = m_txt_sections[section];
    }
    else
    {
      if (section < (int)m_section_index.size())
        section_text = book_cache_read_section(m_path.c_str(), m_header_size, m_section_index[section]);
    }

    int pages = compute_page_count_from_text(section_text);
    m_section_page_counts[section] = pages;
    return pages;
  }

  void ensure_book_pagination()
  {
    if (m_book_pagination_ready) return;

    m_total_pages_in_book = 0;
    for (int i = 0; i < m_num_sections; i++)
      m_total_pages_in_book += get_or_compute_section_page_count(i);

    if (m_total_pages_in_book <= 0)
      m_total_pages_in_book = std::max(1, (int)m_pages.size());

    m_book_pagination_ready = true;
  }

  std::vector<std::string> word_wrap(const std::string &text, int max_width)
  {
    std::vector<std::string> lines;
    std::string current_line;

    size_t pos = 0;
    while (pos < text.size())
    {
      if (current_line.empty() && text[pos] == ' ')
      {
        pos++;
        continue;
      }

      size_t word_end = text.find(' ', pos);
      if (word_end == std::string::npos) word_end = text.size();

      std::string word = text.substr(pos, word_end - pos);

      size_t nl = word.find('\n');
      if (nl != std::string::npos)
      {
        word = word.substr(0, nl);

        std::string test = current_line.empty() ? word : current_line + " " + word;
        if (m_renderer->get_text_width(test.c_str()) <= max_width)
          current_line = test;
        else
        {
          if (!current_line.empty()) lines.push_back(current_line);
          current_line = word;
        }
        lines.push_back(current_line);
        current_line = "";
        pos = pos + nl + 1;
        continue;
      }

      std::string test = current_line.empty() ? word : current_line + " " + word;

      if (m_renderer->get_text_width(test.c_str()) <= max_width)
      {
        current_line = test;
      }
      else
      {
        if (!current_line.empty())
          lines.push_back(current_line);
        while (m_renderer->get_text_width(word.c_str()) > max_width && word.size() > 1)
        {
          int lo = 1, hi = (int)word.size();
          while (lo < hi - 1)
          {
            int mid = (lo + hi) / 2;
            if (m_renderer->get_text_width(word.substr(0, mid).c_str()) <= max_width)
              lo = mid;
            else
              hi = mid;
          }
          lines.push_back(word.substr(0, lo));
          word = word.substr(lo);
        }
        current_line = word;
      }
      pos = word_end + 1;
    }
    if (!current_line.empty())
      lines.push_back(current_line);
    return lines;
  }

  static std::string attr_value(const std::string &tag, const char *attr)
  {
    std::string search = std::string(attr) + "=\"";
    size_t pos = tag.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    size_t end = tag.find('"', pos);
    if (end == std::string::npos) return "";
    return tag.substr(pos, end - pos);
  }

  static void extract_tag_content(const std::string &xml, const char *tag, std::string &out)
  {
    std::string open = std::string("<") + tag + ">";
    std::string open2 = std::string("<") + tag + " ";
    std::string close = std::string("</") + tag + ">";

    size_t p = xml.find(open);
    if (p == std::string::npos)
    {
      p = xml.find(open2);
      if (p == std::string::npos) return;
      p = xml.find('>', p);
      if (p == std::string::npos) return;
      p++;
    }
    else
    {
      p += open.size();
    }
    size_t e = xml.find(close, p);
    if (e == std::string::npos) return;
    out = xml.substr(p, e - p);
  }
};

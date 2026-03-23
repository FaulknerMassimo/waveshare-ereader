#pragma once

#include <Arduino.h>
#include <SD_MMC.h>
#include <string>
#include "CoverLoader.h"

#define CACHE_DIR "/.ereader"
#define CACHE_MAGIC 0x42484B43u

#define SLEEP_COVER_W 464
#define SLEEP_COVER_H 533
#define SLEEP_COVER_STRIDE ((SLEEP_COVER_W + 3) / 4)
#define SLEEP_COVER_BUF_SZ (SLEEP_COVER_STRIDE * SLEEP_COVER_H)

static uint32_t cache_hash(const char *s)
{
	uint32_t h = 5381;
	while (*s) h = ((h << 5) + h) ^ (uint8_t)*s++;
	return h;
}

static std::string cache_path(const char *epub_path, const char *ext)
{
	char name[32];
	snprintf(name, sizeof(name), "%08lx%s", (unsigned long)cache_hash(epub_path), ext);
	return std::string(CACHE_DIR "/") + name;
}

static void ensure_cache_dir()
{
	if (!SD_MMC.exists(CACHE_DIR))
		SD_MMC.mkdir(CACHE_DIR);
}

static uint8_t *cover_cache_load(const char *epub_path)
{
	std::string cp = cache_path(epub_path, ".cov");
	File f = SD_MMC.open(cp.c_str(), FILE_READ);
	if (!f) return nullptr;
	if ((size_t)f.size() != COVER_BUF_SZ) { f.close(); return nullptr; }

	uint8_t *buf = (uint8_t *)heap_caps_malloc(COVER_BUF_SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!buf) buf = (uint8_t *)malloc(COVER_BUF_SZ);
	if (!buf) { f.close(); return nullptr; }

	size_t rd = f.read(buf, COVER_BUF_SZ);
	f.close();
	if (rd != COVER_BUF_SZ) { free(buf); return nullptr; }
	Serial.printf("[CACHE] Cover loaded from cache: %s\n", cp.c_str());
	return buf;
}

static uint8_t *sleep_cover_cache_load(const char *epub_path)
{
	std::string cp = cache_path(epub_path, ".slp");
	File f = SD_MMC.open(cp.c_str(), FILE_READ);
	if (!f) return nullptr;
	if ((size_t)f.size() != SLEEP_COVER_BUF_SZ) { f.close(); return nullptr; }

	uint8_t *buf = (uint8_t *)heap_caps_malloc(SLEEP_COVER_BUF_SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!buf) buf = (uint8_t *)malloc(SLEEP_COVER_BUF_SZ);
	if (!buf) { f.close(); return nullptr; }

	size_t rd = f.read(buf, SLEEP_COVER_BUF_SZ);
	f.close();
	if (rd != SLEEP_COVER_BUF_SZ) { free(buf); return nullptr; }
	Serial.printf("[CACHE] Sleep cover loaded from cache: %s\n", cp.c_str());
	return buf;
}

static bool cover_cache_save(const char *epub_path, const uint8_t *buf)
{
	ensure_cache_dir();
	std::string cp = cache_path(epub_path, ".cov");
	File f = SD_MMC.open(cp.c_str(), FILE_WRITE);
	if (!f) return false;
	size_t written = f.write(buf, COVER_BUF_SZ);
	f.close();
	Serial.printf("[CACHE] Cover saved: %s (%u bytes)\n", cp.c_str(), written);
	return written == COVER_BUF_SZ;
}

static bool sleep_cover_cache_save(const char *epub_path, const uint8_t *buf)
{
	ensure_cache_dir();
	std::string cp = cache_path(epub_path, ".slp");
	File f = SD_MMC.open(cp.c_str(), FILE_WRITE);
	if (!f) return false;
	size_t written = f.write(buf, SLEEP_COVER_BUF_SZ);
	f.close();
	Serial.printf("[CACHE] Sleep cover saved: %s (%u bytes)\n", cp.c_str(), written);
	return written == SLEEP_COVER_BUF_SZ;
}

static uint8_t *cover_load_cached(const char *epub_path)
{
	uint8_t *buf = cover_cache_load(epub_path);
	if (buf) return buf;

	buf = CoverLoader::load(epub_path);
	if (!buf) return nullptr;

	cover_cache_save(epub_path, buf);
	return buf;
}

static uint8_t *cover_load_sleep_cached(const char *epub_path)
{
	uint8_t *buf = sleep_cover_cache_load(epub_path);
	if (buf) return buf;

	buf = CoverLoader::load_scaled(epub_path, SLEEP_COVER_W, SLEEP_COVER_H);
	if (!buf) return nullptr;

	sleep_cover_cache_save(epub_path, buf);
	return buf;
}

struct SectionIndex {
	uint32_t offset;
	uint32_t length;
};

static bool book_cache_exists(const char *epub_path)
{
	std::string cp = cache_path(epub_path, ".book");
	if (!SD_MMC.exists(cp.c_str())) return false;
	File f = SD_MMC.open(cp.c_str(), FILE_READ);
	if (!f) return false;
	uint32_t magic = 0, version = 0;
	f.read((uint8_t *)&magic, 4);
	f.read((uint8_t *)&version, 4);
	f.close();
	return magic == CACHE_MAGIC && version == 1;
}

static bool book_cache_read_index(const char *epub_path, std::vector<SectionIndex> &index, std::string &title)
{
	std::string cp = cache_path(epub_path, ".book");
	File f = SD_MMC.open(cp.c_str(), FILE_READ);
	if (!f) return false;

	uint32_t magic, version, num_sections, title_len;
	f.read((uint8_t *)&magic, 4);
	f.read((uint8_t *)&version, 4);
	f.read((uint8_t *)&num_sections, 4);
	f.read((uint8_t *)&title_len, 4);

	if (magic != CACHE_MAGIC || version != 1 || num_sections == 0 || title_len > 512)
	{ f.close(); return false; }

	char tbuf[513] = {0};
	f.read((uint8_t *)tbuf, title_len);
	title = std::string(tbuf, title_len);

	index.resize(num_sections);
	for (uint32_t i = 0; i < num_sections; i++)
	{
		f.read((uint8_t *)&index[i].offset, 4);
		f.read((uint8_t *)&index[i].length, 4);
	}
	f.close();
	return true;
}

static std::string book_cache_read_section(const char *epub_path, uint32_t header_size, const SectionIndex &si)
{
	std::string cp = cache_path(epub_path, ".book");
	File f = SD_MMC.open(cp.c_str(), FILE_READ);
	if (!f) return "";

	uint32_t abs_offset = header_size + si.offset;
	if (!f.seek(abs_offset)) { f.close(); return ""; }

	char *buf = (char *)heap_caps_malloc(si.length + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!buf) buf = (char *)malloc(si.length + 1);
	if (!buf) { f.close(); return ""; }

	f.read((uint8_t *)buf, si.length);
	buf[si.length] = '\0';
	f.close();

	std::string result(buf, si.length);
	free(buf);
	return result;
}

static bool book_cache_write(const char *epub_path, const std::string &title, const std::vector<std::string> &sections)
{
	ensure_cache_dir();
	std::string cp = cache_path(epub_path, ".book");
	File f = SD_MMC.open(cp.c_str(), FILE_WRITE);
	if (!f) return false;

	uint32_t magic = CACHE_MAGIC;
	uint32_t version = 1;
	uint32_t num_sects = (uint32_t)sections.size();
	uint32_t title_len = (uint32_t)title.size();

	f.write((uint8_t *)&magic, 4);
	f.write((uint8_t *)&version, 4);
	f.write((uint8_t *)&num_sects, 4);
	f.write((uint8_t *)&title_len, 4);
	f.write((uint8_t *)title.c_str(), title_len);

	uint32_t offset = 0;
	for (const auto &s : sections)
	{
		uint32_t len = (uint32_t)s.size();
		f.write((uint8_t *)&offset, 4);
		f.write((uint8_t *)&len, 4);
		offset += len;
	}

	for (const auto &s : sections)
		f.write((uint8_t *)s.c_str(), s.size());

	f.close();
	Serial.printf("[CACHE] Book saved: %s (%d sections)\n", cp.c_str(), (int)sections.size());
	return true;
}

class BookCacheWriter
{
public:
	BookCacheWriter(const char *epub_path, const std::string &title)
		: m_path(epub_path), m_title(title), m_ok(false), m_offset(0) {}

	bool begin()
	{
		ensure_cache_dir();
		m_data_path = cache_path(m_path, ".tmp");
		m_f = SD_MMC.open(m_data_path.c_str(), FILE_WRITE);
		m_ok = (bool)m_f;
		m_offset = 0;
		m_index.clear();
		return m_ok;
	}

	bool write_section(const std::string &text)
	{
		if (!m_ok || text.empty()) return false;
		SectionIndex si { m_offset, (uint32_t)text.size() };
		m_index.push_back(si);
		m_f.write((uint8_t *)text.c_str(), text.size());
		m_offset += si.length;
		return true;
	}

	bool finish()
	{
		if (!m_ok) return false;
		m_f.close();

		std::string final_path = cache_path(m_path, ".book");
		File out = SD_MMC.open(final_path.c_str(), FILE_WRITE);
		if (!out) { SD_MMC.remove(m_data_path.c_str()); return false; }

		uint32_t magic = CACHE_MAGIC;
		uint32_t version = 1;
		uint32_t num_sects = (uint32_t)m_index.size();
		uint32_t title_len = (uint32_t)m_title.size();

		out.write((uint8_t *)&magic, 4);
		out.write((uint8_t *)&version, 4);
		out.write((uint8_t *)&num_sects, 4);
		out.write((uint8_t *)&title_len, 4);
		out.write((uint8_t *)m_title.c_str(), title_len);
		for (auto &si : m_index)
		{
			out.write((uint8_t *)&si.offset, 4);
			out.write((uint8_t *)&si.length, 4);
		}

		File tmp = SD_MMC.open(m_data_path.c_str(), FILE_READ);
		if (tmp)
		{
			uint8_t buf[512];
			size_t n;
			while ((n = tmp.read(buf, sizeof(buf))) > 0)
				out.write(buf, n);
			tmp.close();
		}
		out.close();
		SD_MMC.remove(m_data_path.c_str());
		Serial.printf("[CACHE] Book saved: %s (%u sections)\n", final_path.c_str(), num_sects);
		return true;
	}

	void abort_write()
	{
		if (m_f) m_f.close();
		SD_MMC.remove(m_data_path.c_str());
	}

	int section_count() const { return (int)m_index.size(); }

private:
	const char *m_path;
	std::string m_title;
	std::string m_data_path;
	File m_f;
	bool m_ok;
	uint32_t m_offset;
	std::vector<SectionIndex> m_index;
};

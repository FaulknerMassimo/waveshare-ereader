#pragma once

#include <Arduino.h>
#include <SD_MMC.h>
#include <vector>
#include <string>
#include <algorithm>
#include "DEV_Config.h"
#include "EPD_3in97.h"
#include "GUI_Paint.h"
#include "fonts.h"
#include "CoverLoader.h"
#include "Cache.h"
#include "WaveshareRenderer.h"

#define FL_MAX_FILES 64
#define FL_GAP 8
#define FL_STATUS_H 20

#define FL_CELL_W COVER_W
#define FL_CELL_H COVER_H
#define FL_BORDER     8
#define FL_SEL_OUTSET 2

#define TONE_BLACK 0x00
#define TONE_DGRAY 0x01
#define TONE_LGRAY 0x02
#define TONE_WHITE 0x03

struct BookEntry {
	std::string path, display_name;
	uint8_t *cover_buf   = nullptr;
	bool load_attempted  = false;
};

class FileList {
public:
	FileList() : m_sel(0), m_page(0) {}
	~FileList() { for (auto &e : m_entries) if (e.cover_buf) { free(e.cover_buf); e.cover_buf = nullptr; } }

	void scan(const char *path) {
		for (auto &e : m_entries) if (e.cover_buf) free(e.cover_buf);
		m_entries.clear();
		m_sel = 0;
		m_page = 0;
		scan_dir(path);
		std::sort(m_entries.begin(), m_entries.end(),
			[](const BookEntry &a, const BookEntry &b) { return a.display_name < b.display_name; });
		Serial.printf("[FileList] %d books\n", (int)m_entries.size());
	}

	void next() {
		if (m_sel < (int)m_entries.size() - 1) {
			m_sel++;
			int np = m_sel / per_page();
			if (np != m_page) { m_page = np; free_offscreen(); }
		}
	}

	void prev() {
		if (m_sel > 0) {
			m_sel--;
			int np = m_sel / per_page();
			if (np != m_page) { m_page = np; free_offscreen(); }
		}
	}

	bool needs_cover_load() const {
		int pp = per_page();
		int ps = m_page * pp, pe = std::min(ps + pp, (int)m_entries.size());
		for (int i = ps; i < pe; i++) {
			std::string lp = m_entries[i].path;
			std::transform(lp.begin(), lp.end(), lp.begin(), ::tolower);
			if (lp.rfind(".epub") != std::string::npos && !m_entries[i].load_attempted)
				return true;
		}
		return false;
	}

	void load_covers_on_task() {
		load_covers_on_task_with_wdt();
	}

	void load_covers_on_task_with_wdt() {
		int pp = per_page();
		int ps = m_page * pp, pe = std::min(ps + pp, (int)m_entries.size());
		for (int i = ps; i < pe; i++) {
			BookEntry &e = m_entries[i];
			if (e.load_attempted) continue;
			e.load_attempted = true;
			std::string lp = e.path;
			std::transform(lp.begin(), lp.end(), lp.begin(), ::tolower);
			if (lp.rfind(".epub") != std::string::npos) {
				Serial.printf("[COVER] %s  heap=%u\n", e.path.c_str(), ESP.getFreeHeap());
				vTaskDelay(1);
				e.cover_buf = cover_load_cached(e.path.c_str());
				vTaskDelay(1);
				Serial.printf("[COVER] %s\n", e.cover_buf ? "OK" : "FAILED");
			}
		}
	}

	void render(UBYTE *fb, int battery_pct) {
		m_battery_pct = battery_pct;

		Paint_NewImage(fb, EPD_3IN97_WIDTH, EPD_3IN97_HEIGHT, WaveshareRenderer::display_rotation(), WHITE);
		Paint_SetScale(2);
		Paint_Clear(WHITE);

		if (m_entries.empty()) {
			const int w = display_width();
			Paint_DrawString_EN(std::max(8, (w - (int)strlen("No .epub or .txt files found.") * Font20.Width) / 2), 200, "No .epub or .txt files found.", &Font20, WHITE, BLACK);
			Paint_DrawString_EN(std::max(8, (w - (int)strlen("Copy files to the SD card root.") * Font16.Width) / 2), 230, "Copy files to the SD card root.", &Font16, WHITE, BLACK);
			return;
		}

		int pp = per_page();
		int ps = m_page * pp, pe = std::min(ps + pp, (int)m_entries.size());
		for (int i = ps; i < pe; i++) {
			int slot = i - ps;
			int cx = 0;
			int cy = 0;
			slot_to_cell(slot, cx, cy);
			draw_cell(fb, m_entries[i], cx, cy, i == m_sel);
		}

		draw_status_bar(fb, battery_pct);
	}

	void update_selection(UBYTE *fb, int old_sel, int new_sel) {
		if (old_sel == new_sel) return;
		if (old_sel < 0 || new_sel < 0) return;
		if (old_sel >= (int)m_entries.size() || new_sel >= (int)m_entries.size()) return;

		const int pp = per_page();
		const int old_page = old_sel / pp;
		const int new_page = new_sel / pp;
		if (old_page != new_page) return;

		const int ps = m_page * pp;
		const int slot_old = old_sel - ps;
		const int slot_new = new_sel - ps;
		if (slot_old < 0 || slot_old >= pp) return;
		if (slot_new < 0 || slot_new >= pp) return;

		int cx_old = 0, cy_old = 0;
		slot_to_cell(slot_old, cx_old, cy_old);
		draw_cell(fb, m_entries[old_sel], cx_old, cy_old, false);
		clear_ring_gap_outside_cell(fb, cx_old, cy_old);

		int cx_new = 0, cy_new = 0;
		slot_to_cell(slot_new, cx_new, cy_new);
		draw_selection_ring(fb, cx_new, cy_new);

		render_status_bar_only(fb);
	}

	const char *selected_path() const {
		if (m_entries.empty() || m_sel < 0 || m_sel >= (int)m_entries.size()) return nullptr;
		return m_entries[m_sel].path.c_str();
	}

	int selected_index() const { return m_sel; }
	int items_per_page() const { return per_page(); }
	bool empty() const { return m_entries.empty(); }

	bool find(const char *path) {
		for (int i = 0; i < (int)m_entries.size(); i++) {
			if (m_entries[i].path == path) {
				m_sel = i;
				m_page = m_sel / per_page();
				return true;
			}
		}
		return false;
	}

private:
	std::vector<BookEntry> m_entries;
	int m_sel, m_page;
	int m_battery_pct = 0;

	static bool gray_pixel_to_black(uint8_t tone, int x, int y) {
		if (tone == TONE_BLACK) return true;
		if (tone == TONE_WHITE) return false;

		static const uint8_t bayer4x4[4][4] = {
			{  0,  8,  2, 10 },
			{ 12,  4, 14,  6 },
			{  3, 11,  1,  9 },
			{ 15,  7, 13,  5 }
		};

		uint8_t black_count = (tone == TONE_DGRAY) ? 10 : 5;
		return bayer4x4[y & 3][x & 3] < black_count;
	}

	void draw_cell(UBYTE *fb, BookEntry &e, int cx, int cy, bool sel) {
		if (e.cover_buf) {
			blit(fb, e.cover_buf, cx, cy);
		} else {
			fill_rect(fb, cx, cy, FL_CELL_W, FL_CELL_H, TONE_LGRAY);
			int bx = cx + FL_CELL_W / 2 - 16, by = cy + FL_CELL_H / 2 - 24;
			fill_rect(fb, bx, by, 32, 48, TONE_WHITE);
			fill_rect(fb, bx, by, 5, 48, TONE_DGRAY);
			border(fb, bx, by, 32, 48, TONE_BLACK);

			std::string abbr = e.display_name.substr(0, std::min((int)e.display_name.size(), 10));
			WaveshareRenderer::draw_string(cx + 4, cy + FL_CELL_H - 14, abbr.c_str(), &Font8, WHITE, BLACK);
		}

		if (sel) {
			draw_selection_ring(fb, cx, cy);
		}
	}

	void slot_to_cell(int slot, int &cx, int &cy) const {
		const int cols = grid_cols();
		const int col = slot % cols;
		const int row = slot / cols;
		const int content_h = display_height() - FL_STATUS_H;
		const int used_w = cols * FL_CELL_W + (cols + 1) * FL_GAP;
		const int used_h = grid_rows() * FL_CELL_H + (grid_rows() + 1) * FL_GAP;
		const int start_x = std::max(FL_GAP, (display_width() - used_w) / 2);
		const int start_y = std::max(FL_GAP, (content_h - used_h) / 2);
		cx = start_x + col * (FL_CELL_W + FL_GAP);
		cy = start_y + row * (FL_CELL_H + FL_GAP);
	}

	void draw_selection_ring(UBYTE *fb, int cx, int cy) {
		int outer_x = cx - FL_SEL_OUTSET;
		int outer_y = cy - FL_SEL_OUTSET;
		int outer_w = FL_CELL_W + FL_SEL_OUTSET * 2;
		int outer_h = FL_CELL_H + FL_SEL_OUTSET * 2;

		int black_th = FL_BORDER / 2;
		for (int t = 0; t < FL_BORDER; t++) {
			uint8_t col = (t < black_th) ? TONE_BLACK : TONE_WHITE;
			border(fb, outer_x + t, outer_y + t, outer_w - t * 2, outer_h - t * 2, col);
		}
	}

	void clear_ring_gap_outside_cell(UBYTE *fb, int cx, int cy) {
		const int oxL = cx - FL_SEL_OUTSET;
		const int oxR = cx + FL_CELL_W + FL_SEL_OUTSET - 1;
		const int oyT = cy - FL_SEL_OUTSET;
		const int oyB = cy + FL_CELL_H + FL_SEL_OUTSET - 1;

		for (int y = cy; y < cy + FL_CELL_H; y++) {
			for (int x = oxL; x < cx; x++) px(fb, x, y, TONE_WHITE);
			for (int x = cx + FL_CELL_W; x <= oxR; x++) px(fb, x, y, TONE_WHITE);
		}

		for (int y = oyT; y < cy; y++) {
			for (int x = cx; x < cx + FL_CELL_W; x++) px(fb, x, y, TONE_WHITE);
		}
		for (int y = cy + FL_CELL_H; y <= oyB; y++) {
			for (int x = cx; x < cx + FL_CELL_W; x++) px(fb, x, y, TONE_WHITE);
		}

		for (int y = oyT; y < cy; y++) {
			for (int x = oxL; x < cx; x++) px(fb, x, y, TONE_WHITE);
			for (int x = cx + FL_CELL_W; x <= oxR; x++) px(fb, x, y, TONE_WHITE);
		}
		for (int y = cy + FL_CELL_H; y <= oyB; y++) {
			for (int x = oxL; x < cx; x++) px(fb, x, y, TONE_WHITE);
			for (int x = cx + FL_CELL_W; x <= oxR; x++) px(fb, x, y, TONE_WHITE);
		}
	}

	void render_status_bar_only(UBYTE *fb) {
		draw_status_bar(fb, m_battery_pct);
	}

	void draw_status_bar(UBYTE *fb, int battery_pct) {
		(void)fb;
		const int w = display_width();
		const int h = display_height();
		const int bar_y = h - FL_STATUS_H;

		Paint_DrawRectangle(0, bar_y, w - 1, h - 1, WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
		Paint_DrawLine(0, bar_y, w - 1, bar_y, BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);

		int bat_pct = battery_pct;
		if (bat_pct < 0) bat_pct = 0;
		if (bat_pct > 100) bat_pct = 100;

		char left[32], right[16];
		snprintf(left, sizeof(left), "Book %d / %d", m_sel + 1, (int)m_entries.size());
		snprintf(right, sizeof(right), "%d%%", bat_pct);

		Paint_DrawString_EN(10, bar_y + 3, left, &Font12, WHITE, BLACK);

		int right_px = (int)strlen(right) * Font12.Width;
		const int icon_w = 16;
		const int icon_h = 8;
		const int icon_tip_w = 2;
		const int icon_gap = 4;
		const int group_w = icon_w + icon_gap + right_px;
		int group_x = w - group_w - 10;

		int icon_x = group_x;
		int icon_y = bar_y + 5;
		int body_x2 = icon_x + icon_w;
		int body_y2 = icon_y + icon_h;
		Paint_DrawRectangle(icon_x, icon_y, body_x2, body_y2, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
		int tip_y = icon_y + (icon_h / 2) - 2;
		Paint_DrawRectangle(body_x2 + 1, tip_y, body_x2 + icon_tip_w, tip_y + 3, BLACK, DOT_PIXEL_1X1, DRAW_FILL_FULL);

		int inner_w = icon_w - 2;
		int fill_w = (inner_w * bat_pct) / 100;
		if (fill_w > 0)
			Paint_DrawRectangle(icon_x + 1, icon_y + 1, icon_x + fill_w, body_y2 - 1, BLACK, DOT_PIXEL_1X1, DRAW_FILL_FULL);

		Paint_DrawString_EN(group_x + icon_w + icon_gap, bar_y + 3, right, &Font12, WHITE, BLACK);
	}

	void blit(UBYTE *fb, const uint8_t *cover, int dx, int dy) {
		for (int y = 0; y < COVER_H; y++) {
			int fy = dy + y;
			if (fy < 0 || fy >= display_height()) continue;

			for (int x = 0; x < COVER_W; x++) {
				int fx = dx + x;
				if (fx < 0 || fx >= display_width()) continue;

				int si = y * COVER_STRIDE + x / 4;
				int ss = 6 - (x % 4) * 2;
				uint8_t g4 = (cover[si] >> ss) & 0x03;

				uint8_t tone = TONE_WHITE;
				if (g4 == 0x00) tone = TONE_BLACK;
				else if (g4 == 0x01) tone = TONE_DGRAY;
				else if (g4 == 0x02) tone = TONE_LGRAY;

				px(fb, fx, fy, tone);
			}
		}
	}

	void px(UBYTE *fb, int x, int y, uint8_t tone) {
		if (x < 0 || x >= display_width() || y < 0 || y >= display_height()) return;

		int phys_x = 0;
		int phys_y = 0;
		if (!map_to_physical(x, y, phys_x, phys_y)) return;

		const int stride = (EPD_3IN97_WIDTH + 7) / 8;
		const int b = phys_y * stride + phys_x / 8;
		const uint8_t mask = (uint8_t)(0x80 >> (phys_x % 8));

		if (gray_pixel_to_black(tone, x, y))
			fb[b] &= (uint8_t)~mask;
		else
			fb[b] |= mask;
	}

	void fill_rect(UBYTE *fb, int x, int y, int w, int h, uint8_t tone) {
		for (int row = 0; row < h; row++)
			for (int col = 0; col < w; col++)
				px(fb, x + col, y + row, tone);
	}

	void border(UBYTE *fb, int x, int y, int w, int h, uint8_t tone) {
		for (int i = 0; i < w; i++) { px(fb, x + i, y, tone); px(fb, x + i, y + h - 1, tone); }
		for (int i = 0; i < h; i++) { px(fb, x, y + i, tone); px(fb, x + w - 1, y + i, tone); }
	}

	void free_offscreen() {
		int pp = per_page();
		int ps = m_page * pp, pe = ps + pp;
		for (int i = 0; i < (int)m_entries.size(); i++) {
			if ((i < ps || i >= pe) && m_entries[i].cover_buf) {
				free(m_entries[i].cover_buf);
				m_entries[i].cover_buf = nullptr;
				m_entries[i].load_attempted = false;
			}
		}
	}

	static int display_width() {
		return WaveshareRenderer::display_width();
	}

	static int display_height() {
		return WaveshareRenderer::display_height();
	}

	static bool map_to_physical(int x, int y, int &phys_x, int &phys_y) {
		switch (WaveshareRenderer::display_rotation()) {
		case ROTATE_0:
			phys_x = x;
			phys_y = y;
			return true;
		case ROTATE_90:
			phys_x = EPD_3IN97_WIDTH - y - 1;
			phys_y = x;
			return true;
		case ROTATE_180:
			phys_x = EPD_3IN97_WIDTH - x - 1;
			phys_y = EPD_3IN97_HEIGHT - y - 1;
			return true;
		case ROTATE_270:
			phys_x = y;
			phys_y = EPD_3IN97_HEIGHT - x - 1;
			return true;
		default:
			return false;
		}
	}

	static int grid_cols() {
		const int w = display_width();
		int cols = (w - FL_GAP) / (FL_CELL_W + FL_GAP);
		if (cols < 1) cols = 1;
		return cols;
	}

	static int grid_rows() {
		const int h = display_height() - FL_STATUS_H;
		int rows = (h - FL_GAP) / (FL_CELL_H + FL_GAP);
		if (rows < 1) rows = 1;
		return rows;
	}

	static int per_page() {
		return grid_cols() * grid_rows();
	}

	void scan_dir(const char *path) {
		File root = SD_MMC.open(path);
		if (!root || !root.isDirectory()) return;

		File entry = root.openNextFile();
		while (entry) {
			std::string base(path);
			if (!base.empty() && base.back() != '/') base += '/';

			std::string raw_name = entry.name();
			std::string fpath;
			if (!raw_name.empty() && raw_name.front() == '/') fpath = raw_name;
			else fpath = base + raw_name;

			size_t slash = fpath.find_last_of('/');
			std::string fname = (slash == std::string::npos) ? fpath : fpath.substr(slash + 1);

			if (entry.isDirectory()) {
				if (fpath.length() < 200) scan_dir(fpath.c_str());
			} else {
				std::string fl = fname;
				std::transform(fl.begin(), fl.end(), fl.begin(), ::tolower);
				bool is_epub = fl.length() >= 5 && fl.compare(fl.length() - 5, 5, ".epub") == 0;
				bool is_txt  = fl.length() >= 4 && fl.compare(fl.length() - 4, 4, ".txt") == 0;
				if ((is_epub || is_txt)
						&& (int)m_entries.size() < FL_MAX_FILES) {
					BookEntry e;
					e.path = fpath;
					e.display_name = fname;
					size_t dot = e.display_name.rfind('.');
					if (dot != std::string::npos) e.display_name = e.display_name.substr(0, dot);
					m_entries.push_back(std::move(e));
				}
			}
			entry = root.openNextFile();
		}
	}
};

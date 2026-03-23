#pragma once

#include <Arduino.h>
#include <SD_MMC.h>
#include <string>
#include "miniz.h"
extern "C" {
#include "tjpgd.h"
}

#define COVER_W 190
#define COVER_H 218

#define COVER_STRIDE ((COVER_W + 3) / 4)
#define COVER_BUF_SZ (COVER_STRIDE * COVER_H)

struct JpegCtx {
	const uint8_t *data; size_t size; size_t pos;
	uint8_t *out;
	int src_w, src_h, tgt_w, tgt_h;
};

static size_t cl_in(JDEC *jd, uint8_t *buf, size_t n) {
	JpegCtx *c = (JpegCtx *)jd->device;
	size_t av = c->size - c->pos; if (n > av) n = av;
	if (buf) memcpy(buf, c->data + c->pos, n);
	c->pos += n; return n;
}

static int cl_out(JDEC *jd, void *bmp, JRECT *r) {
	JpegCtx *c = (JpegCtx *)jd->device;
	const uint8_t *px = (const uint8_t *)bmp;
	int bw = r->right - r->left + 1, bh = r->bottom - r->top + 1;
	for (int row = 0; row < bh; row++) {
		int sy = r->top + row;
		int dy = (int)((long)sy * c->tgt_h / c->src_h);
		if (dy < 0 || dy >= c->tgt_h) { px += bw*3; continue; }
		for (int col = 0; col < bw; col++) {
			int sx = r->left + col;
			int dx = (int)((long)sx * c->tgt_w / c->src_w);
			uint8_t rv=px[0], gv=px[1], bv=px[2]; px+=3;
			if (dx < 0 || dx >= c->tgt_w) continue;
			uint8_t lum = (uint8_t)((rv*77u + gv*150u + bv*29u) >> 8);
			uint8_t pv = (lum >= 192) ? 3 : (lum >= 128) ? 2 : (lum >= 64) ? 1 : 0;
			int bi = dy * COVER_STRIDE + dx/4;
			int sh = 6 - (dx%4)*2;
			c->out[bi] = (c->out[bi] & ~(3<<sh)) | (pv<<sh);
		}
	}
	return 1;
}

class CoverLoader {
public:
	static uint8_t *load(const char *epub_path) {
		File f = SD_MMC.open(epub_path);
		if (!f) return nullptr;
		mz_zip_archive zip; memset(&zip,0,sizeof(zip));
		zip.m_pRead = zread; zip.m_pIO_opaque = &f;
		if (!mz_zip_reader_init(&zip, f.size(), 0)) { f.close(); return nullptr; }
		std::string cp = find_cover(&zip);
		if (cp.empty()) { mz_zip_reader_end(&zip); f.close(); return nullptr; }
		size_t isz; uint8_t *img = (uint8_t*)extract(&zip, cp.c_str(), &isz);
		mz_zip_reader_end(&zip); f.close();
		if (!img) return nullptr;
		uint8_t *result = decode(img, isz);
		free(img); return result;
	}

private:
	static size_t zread(void *op, mz_uint64 ofs, void *buf, size_t n) {
		File *f=(File*)op; if(!f->seek((uint32_t)ofs)) return 0;
		return f->read((uint8_t*)buf,n);
	}
	static void *extract(mz_zip_archive *zip, const char *name, size_t *sz) {
		mz_uint32 idx;
		if (!mz_zip_reader_locate_file_v2(zip,name,nullptr,0,&idx)) return nullptr;
		mz_zip_archive_file_stat st;
		if (!mz_zip_reader_file_stat(zip,idx,&st)) return nullptr;
		size_t n=(size_t)st.m_uncomp_size;
		void *buf=heap_caps_malloc(n+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
		if (!buf) buf=malloc(n+1); if (!buf) return nullptr;
		if (!mz_zip_reader_extract_to_mem(zip,idx,buf,n,0)){free(buf);return nullptr;}
		((uint8_t*)buf)[n]=0; if(sz)*sz=n; return buf;
	}
	static std::string av(const std::string &t, const char *n) {
		std::string s=std::string(n)+"=\"";
		size_t p=t.find(s); if(p==std::string::npos) return "";
		p+=s.size(); size_t e=t.find('"',p);
		return e==std::string::npos?"":t.substr(p,e-p);
	}
	static std::string find_cover(mz_zip_archive *zip) {
		size_t sz; char *c=(char*)extract(zip,"META-INF/container.xml",&sz);
		if(!c) return ""; std::string xml(c,sz); free(c);
		const char *needle="full-path=\""; size_t p=xml.find(needle);
		if(p==std::string::npos) return ""; p+=strlen(needle);
		size_t e=xml.find('"',p); if(e==std::string::npos) return "";
		std::string opf=xml.substr(p,e-p);
		std::string base=opf.substr(0,opf.rfind('/')+1);
		char *od=(char*)extract(zip,opf.c_str(),&sz);
		if(!od) return ""; std::string ox(od,sz); free(od);
		std::string cid;
		for(size_t pos=0;(pos=ox.find("<item ",pos))!=std::string::npos;){
			size_t en=ox.find('>',pos); if(en==std::string::npos) break;
			std::string tag=ox.substr(pos,en-pos+1);
			if(av(tag,"properties").find("cover-image")!=std::string::npos){cid=av(tag,"id");break;}
			pos=en;
		}
		if(cid.empty()) for(size_t pos=0;(pos=ox.find("<meta ",pos))!=std::string::npos;){
			size_t en=ox.find('>',pos); if(en==std::string::npos) break;
			std::string tag=ox.substr(pos,en-pos+1);
			if(av(tag,"name")=="cover"){cid=av(tag,"content");break;}
			pos=en;
		}
		if(cid.empty()) return "";
		for(size_t pos=0;(pos=ox.find("<item ",pos))!=std::string::npos;){
			size_t en=ox.find('>',pos); if(en==std::string::npos) break;
			std::string tag=ox.substr(pos,en-pos+1);
			if(av(tag,"id")==cid) return base+av(tag,"href");
			pos=en;
		}
		return "";
	}
	static uint8_t *decode(const uint8_t *data, size_t size) {
		uint8_t *buf=(uint8_t*)heap_caps_malloc(COVER_BUF_SZ,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
		if(!buf) buf=(uint8_t*)malloc(COVER_BUF_SZ);
		if(!buf) return nullptr;
		memset(buf,0xFF,COVER_BUF_SZ);
		void *pool=malloc(4096); if(!pool){free(buf);return nullptr;}
		JpegCtx ctx={data,size,0,buf,0,0,COVER_W,COVER_H};
		JDEC jd;
		if(jd_prepare(&jd,cl_in,pool,4096,&ctx)!=JDR_OK){free(pool);free(buf);return nullptr;}
		uint8_t scale=0;
		while(scale<3&&(jd.width>>(scale+1))>=COVER_W&&(jd.height>>(scale+1))>=COVER_H) scale++;
		ctx.src_w = (jd.width  >> scale);
		ctx.src_h = (jd.height >> scale);
		if (ctx.src_w < 1) ctx.src_w = 1;
		if (ctx.src_h < 1) ctx.src_h = 1;
		JRESULT rc=jd_decomp(&jd,cl_out,scale);
		free(pool);
		if(rc!=JDR_OK&&rc!=JDR_INTR){free(buf);return nullptr;}
		return buf;
	}
};

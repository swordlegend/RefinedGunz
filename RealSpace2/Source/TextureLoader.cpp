#include "stdafx.h"
#include "TextureLoader.h"
#include "gli/gli.hpp"
#include "RealSpace2.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb/stb_image_resize.h"

constexpr auto pool = D3DPOOL_MANAGED;

static D3DPtr<IDirect3DTexture9> LoadDDS(const void* data, size_t size,
	float sample_ratio, TextureInfo& info)
{
	D3DPtr<IDirect3DTexture9> ret;
	D3DFORMAT format;
	auto tex = gli::load(static_cast<const char*>(data), size);
	switch (tex.format())
	{
	case gli::FORMAT_RGB_DXT1_UNORM:
		format = D3DFMT_DXT1;
		break;
	case gli::FORMAT_RGBA_DXT3_UNORM:
		format = D3DFMT_DXT3;
		break;
	case gli::FORMAT_RGBA_DXT5_UNORM:
		format = D3DFMT_DXT5;
		break;
	case gli::FORMAT_BGRA8_UNORM:
		format = D3DFMT_A8R8G8B8;
		break;
	default:
		MLog("LoadDDS -- Unknown DDS format %d\n", tex.format());
		assert(false);
		return nullptr;
	}
	gli::texture2D tex2D(tex);
	if (tex2D.empty())
		return false;

	auto Width = tex2D[0].dimensions().x;
	auto Height = tex2D[0].dimensions().y;
	auto Levels = tex2D.levels();
	auto Usage = 0;

	if (FAILED(RGetDevice()->CreateTexture(Width, Height, Levels, Usage,
		format, pool, MakeWriteProxy(ret), nullptr)))
		return nullptr;

	// Copy all mipmap levels into the texture
	for (size_t i{}; i < Levels; ++i)
	{
		D3DLOCKED_RECT LockedRect;
		if (FAILED(ret->LockRect(i, &LockedRect, nullptr, 0)))
			return nullptr;
		auto&& img = tex2D[i];
		memcpy(LockedRect.pBits, img.data(), img.size());
		if (FAILED(ret->UnlockRect(i)))
			return nullptr;
	}

	info.Width = Width;
	info.Height = Height;
	info.MipLevels = Levels;
	info.Format = format;

	return ret;
}

static D3DPtr<IDirect3DTexture9> LoadSTB(const void* data, size_t size,
	float sample_ratio, TextureInfo& info)
{
	struct free_deleter {
		void operator()(void* p) const { free(p); } };
	using c_ptr = std::unique_ptr<u8[], free_deleter>;

	D3DPtr<IDirect3DTexture9> ret;
	auto* data_uc = static_cast<const stbi_uc*>(data);

	int Width{}, Height{}, comp{};
	auto Levels = 1;
	auto Usage = 0;

	c_ptr tex_mem = c_ptr{ stbi_load_from_memory(data_uc, size,
		&Width, &Height, &comp, STBI_rgb_alpha) };

	auto* tex_ptr = tex_mem.get();

	if (!(Width && Height && comp && tex_mem))
	{
		assert(false);
		return nullptr;
	}

	c_ptr resized_tex_mem;
	if (!Equals(sample_ratio, 1))
	{
		auto NewWidth = Width * sample_ratio;
		auto NewHeight = Height * sample_ratio;
		resized_tex_mem = c_ptr{ new u8[NewWidth * NewHeight * 4] };
		if (stbir_resize_uint8(tex_ptr, Width, Height, 0,
			resized_tex_mem.get(), NewWidth, NewHeight, 0, 4))
		{
			Width = NewWidth;
			Height = NewHeight;
			tex_ptr = resized_tex_mem.get();
		}
	}

	D3DFORMAT format;
	if (comp == 4)
		format = D3DFMT_A8R8G8B8;
	else
		format = D3DFMT_X8R8G8B8;

	if (FAILED(RGetDevice()->CreateTexture(Width, Height, Levels, Usage,
		format, pool, MakeWriteProxy(ret), nullptr)))
		return nullptr;

	D3DLOCKED_RECT LockedRect;
	if (FAILED(ret->LockRect(0, &LockedRect, nullptr, 0)))
	{
		assert(false);
		return nullptr;
	}
	auto* ptr = static_cast<unsigned char*>(LockedRect.pBits);
	for (int i{}; i < Width * Height; ++i)
	{
#define SET(offset) *ptr = *(tex_ptr + offset); ++ptr
		// RGBA -> BGRA
		SET(2);
		SET(1);
		SET(0);
		SET(3);
		tex_ptr += 4;
#undef SET
	}
	if (FAILED(ret->UnlockRect(0)))
	{
		assert(false);
		return nullptr;
	}

	info.Width = Width;
	info.Height = Height;
	info.MipLevels = 1;
	info.Format = format;

	return ret;
}

// TGA stuff is from the tga.c file in GLFW
struct tga_header_t
{
	int idlen;                 // 1 byte
	int cmaptype;              // 1 byte
	int imagetype;             // 1 byte
	int cmapfirstidx;          // 2 bytes
	int cmaplen;               // 2 bytes
	int cmapentrysize;         // 1 byte
	int xorigin;               // 2 bytes
	int yorigin;               // 2 bytes
	int width;                 // 2 bytes
	int height;                // 2 bytes
	int bitsperpixel;          // 1 byte
	int imageinfo;             // 1 byte
	int _alphabits;            // (derived from imageinfo)
	int _origin;               // (derived from imageinfo)
};

static bool is_tga(const void* data, size_t size)
{
	if (size < 18)
		return false;

	tga_header_t header;
	auto* buf = static_cast<const char*>(data);

	// Interpret header (endian independent parsing)
	header.idlen = (int)buf[0];
	header.cmaptype = (int)buf[1];
	header.imagetype = (int)buf[2];
	header.cmapfirstidx = (int)buf[3] | (((int)buf[4]) << 8);
	header.cmaplen = (int)buf[5] | (((int)buf[6]) << 8);
	header.cmapentrysize = (int)buf[7];
	header.xorigin = (int)buf[8] | (((int)buf[9]) << 8);
	header.yorigin = (int)buf[10] | (((int)buf[11]) << 8);
	header.width = (int)buf[12] | (((int)buf[13]) << 8);
	header.height = (int)buf[14] | (((int)buf[15]) << 8);
	header.bitsperpixel = (int)buf[16];
	header.imageinfo = (int)buf[17];

	return ((header.cmaptype == 0 || header.cmaptype == 1) &&
		(header.imagetype >= 1 && header.imagetype <= 3) ||
		(header.imagetype >= 9 && header.imagetype <= 11)) &&
		(header.bitsperpixel == 8 || header.bitsperpixel == 24 ||
		header.bitsperpixel == 32);
}

enum class FileType
{
	BMP,
	JPG,
	PNG,
	TGA,
	DDS,
	Unknown,
};

static bool IsFileType(FileType Type, const void* data, size_t size)
{
	const char* magics[] = {
		"BM",
		"\xFF\xD8\xFF",
		"\x89\x50\x4E\x47\x0D\x0A\x1A\x0A",
		nullptr,
		"DDS",
	};

	if (static_cast<size_t>(Type) >= ArraySize(magics))
		return false;

	if (Type == FileType::TGA)
		return is_tga(data, size);

	auto* magic = magics[static_cast<size_t>(Type)];
	auto len = strlen(magic);
	return size >= len && !memcmp(data, magic, len);
};

static FileType GetFileType(const void* data, size_t size, const char* ext)
{
	if (ext)
	{
		FileType SupposedType = FileType::Unknown;
		if (!_stricmp(ext, ".bmp"))
			SupposedType = FileType::BMP;
		if (!_stricmp(ext, ".jpg"))
			SupposedType = FileType::JPG;
		if (!_stricmp(ext, ".png"))
			SupposedType = FileType::PNG;
		if (!_stricmp(ext, ".tga"))
			SupposedType = FileType::TGA;
		if (!_stricmp(ext, ".dds"))
			SupposedType = FileType::DDS;

		if (IsFileType(SupposedType, data, size))
			return SupposedType;
	}

	for (size_t i{}; i < static_cast<size_t>(FileType::Unknown); ++i)
	{
		auto ft = static_cast<FileType>(i);
		if (IsFileType(ft, data, size))
			return ft;
	}

	assert(false);
	return FileType::Unknown;
}

namespace RealSpace2
{
D3DPtr<IDirect3DTexture9> LoadTexture(const void * data, size_t size,
	float sample_ratio,
	TextureInfo& info, const char * ext)
{
	auto ft = GetFileType(data, size, ext);
	if (ft == FileType::Unknown)
		return nullptr;

	switch (ft)
	{
	case FileType::BMP:
	case FileType::JPG:
	case FileType::PNG:
	case FileType::TGA:
		return LoadSTB(data, size, sample_ratio, info);
		break;
	case FileType::DDS:
		return LoadDDS(data, size, sample_ratio, info);
		break;
	};

	return nullptr;
}
}
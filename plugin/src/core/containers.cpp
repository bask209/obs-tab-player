// SPDX-License-Identifier: Apache-2.0
// Containers: raw DEFLATE, ZIP (Guitar Pro 7/8) and the Guitar Pro 6 "BCFS/BCFZ" file system.
#include "readers.hpp"
#include "limits.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace tab {

// ------------------------------------------------------------------ inflate

namespace {

struct BitIn {
	const uint8_t *d;
	size_t n, pos = 0; // bit position
	uint32_t bits(int k)
	{
		uint32_t v = 0;
		for (int i = 0; i < k; i++, pos++) {
			if ((pos >> 3) >= n)
				throw std::runtime_error("Truncated deflate stream");
			v |= uint32_t((d[pos >> 3] >> (pos & 7)) & 1) << i;
		}
		return v;
	}
};

struct Huffman {
	std::array<uint16_t, 16> count{};
	std::array<uint16_t, 320> symbol{};
	void build(const uint8_t *lengths, int n)
	{
		count.fill(0);
		for (int i = 0; i < n; i++)
			count[lengths[i]]++;
		count[0] = 0;
		std::array<uint16_t, 16> offs{};
		for (int i = 1; i < 16; i++)
			offs[i] = uint16_t(offs[i - 1] + count[i - 1]);
		for (int i = 0; i < n; i++)
			if (lengths[i])
				symbol[offs[lengths[i]]++] = uint16_t(i);
	}
	int decode(BitIn &in) const
	{
		int code = 0, first = 0, index = 0;
		for (int len = 1; len < 16; len++) {
			code |= int(in.bits(1));
			int c = count[len];
			if (code - c < first)
				return symbol[index + (code - first)];
			index += c;
			first += c;
			first <<= 1;
			code <<= 1;
		}
		throw std::runtime_error("Bad deflate code");
	}
};

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

} // namespace

std::vector<uint8_t> inflateRaw(const uint8_t *d, size_t n, size_t expected)
{
	std::vector<uint8_t> out;
	out.reserve(std::min(expected ? expected : n * 4, limits::kMaxInflatedBytes));
	auto room = [&](size_t k) {
		if (out.size() + k > limits::kMaxInflatedBytes)
			throw std::runtime_error("Decompressed data too large");
	};
	BitIn in{d, n};
	bool last = false;
	while (!last) {
		last = in.bits(1);
		uint32_t type = in.bits(2);
		if (type == 0) {
			in.pos = (in.pos + 7) & ~size_t(7);
			size_t p = in.pos >> 3;
			if (p + 4 > n)
				throw std::runtime_error("Truncated deflate stream");
			uint16_t len = uint16_t(d[p] | (d[p + 1] << 8));
			p += 4;
			if (p + len > n)
				throw std::runtime_error("Truncated deflate stream");
			room(len);
			out.insert(out.end(), d + p, d + p + len);
			in.pos = (p + len) * 8;
			continue;
		}
		if (type == 3)
			throw std::runtime_error("Bad deflate block");
		Huffman lit, dist;
		if (type == 1) {
			uint8_t l[288];
			std::memset(l, 8, 144);
			std::memset(l + 144, 9, 112);
			std::memset(l + 256, 7, 24);
			std::memset(l + 280, 8, 8);
			lit.build(l, 288);
			uint8_t dl[30];
			std::memset(dl, 5, 30);
			dist.build(dl, 30);
		} else {
			int hlit = int(in.bits(5)) + 257, hdist = int(in.bits(5)) + 1, hclen = int(in.bits(4)) + 4;
			static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
			uint8_t cl[19] = {};
			for (int i = 0; i < hclen; i++)
				cl[order[i]] = uint8_t(in.bits(3));
			Huffman clh;
			clh.build(cl, 19);
			uint8_t lens[320] = {};
			int i = 0;
			while (i < hlit + hdist) {
				int sym = clh.decode(in);
				if (sym < 16) {
					lens[i++] = uint8_t(sym);
				} else {
					int rep = 0;
					uint8_t val = 0;
					if (sym == 16) {
						if (!i)
							throw std::runtime_error("Bad deflate lengths");
						val = lens[i - 1];
						rep = 3 + int(in.bits(2));
					} else if (sym == 17) {
						rep = 3 + int(in.bits(3));
					} else {
						rep = 11 + int(in.bits(7));
					}
					if (i + rep > hlit + hdist)
						throw std::runtime_error("Bad deflate lengths");
					while (rep--)
						lens[i++] = val;
				}
			}
			lit.build(lens, hlit);
			dist.build(lens + hlit, hdist);
		}
		for (;;) {
			int sym = lit.decode(in);
			if (sym < 256) {
				room(1);
				out.push_back(uint8_t(sym));
			} else if (sym == 256) {
				break;
			} else {
				sym -= 257;
				if (sym >= 29)
					throw std::runtime_error("Bad deflate length");
				size_t len = kLenBase[sym] + in.bits(kLenExtra[sym]);
				int ds = dist.decode(in);
				if (ds >= 30)
					throw std::runtime_error("Bad deflate distance");
				size_t back = kDistBase[ds] + in.bits(kDistExtra[ds]);
				if (back > out.size())
					throw std::runtime_error("Bad deflate distance");
				room(len);
				size_t from = out.size() - back;
				for (size_t k = 0; k < len; k++)
					out.push_back(out[from + k]);
			}
		}
	}
	return out;
}

// ------------------------------------------------------------------ zip

static uint32_t rd32(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
static uint16_t rd16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }

bool isZip(const uint8_t *d, size_t n)
{
	return n > 4 && d[0] == 'P' && d[1] == 'K' && d[2] == 3 && d[3] == 4;
}

bool readZipEntry(const uint8_t *d, size_t n, const std::string &name, std::vector<uint8_t> &out)
{
	if (n < 22)
		return false;
	size_t eocd = SIZE_MAX;
	size_t stop = n > 65557 ? n - 65557 : 0;
	for (size_t i = n - 22 + 1; i-- > stop;)
		if (rd32(d + i) == 0x06054b50) {
			eocd = i;
			break;
		}
	if (eocd == SIZE_MAX)
		throw std::runtime_error("Invalid zip file");
	unsigned count = rd16(d + eocd + 10);
	size_t p = rd32(d + eocd + 16);
	for (unsigned k = 0; k < count; k++) {
		if (p + 46 > n || rd32(d + p) != 0x02014b50)
			break;
		unsigned method = rd16(d + p + 10);
		size_t csize = rd32(d + p + 20), usize = std::min<size_t>(rd32(d + p + 24), limits::kMaxInflatedBytes);
		size_t nameLen = rd16(d + p + 28), extra = rd16(d + p + 30), comment = rd16(d + p + 32);
		size_t local = rd32(d + p + 42);
		if (p + 46 + nameLen > n)
			break;
		std::string entry(reinterpret_cast<const char *>(d + p + 46), nameLen);
		p += 46 + nameLen + extra + comment;
		if (entry != name)
			continue;
		if (local + 30 > n)
			throw std::runtime_error("Invalid zip file");
		size_t start = local + 30 + rd16(d + local + 26) + rd16(d + local + 28);
		if (start + csize > n)
			throw std::runtime_error("Invalid zip file");
		if (method == 0)
			out.assign(d + start, d + start + csize);
		else if (method == 8)
			out = inflateRaw(d + start, csize, usize);
		else
			throw std::runtime_error("Unsupported zip compression");
		return true;
	}
	return false;
}

// ------------------------------------------------------------------ gpx

bool isGpx(const uint8_t *d, size_t n)
{
	return n > 8 && (std::memcmp(d, "BCFZ", 4) == 0 || std::memcmp(d, "BCFS", 4) == 0);
}

static std::vector<uint8_t> decompressBcfz(const uint8_t *src, size_t n)
{
	int32_t expected = int32_t(rd32(src + 4));
	if (expected < 0 || size_t(expected) > limits::kMaxInflatedBytes)
		throw std::runtime_error("Corrupt GPX file");
	std::vector<uint8_t> out;
	out.reserve(size_t(expected));
	size_t pos = 64, total = n * 8;
	auto bit = [&]() -> int {
		if (pos >= total)
			throw std::out_of_range("eof");
		int b = (src[pos >> 3] >> (7 - (pos & 7))) & 1;
		pos++;
		return b;
	};
	auto bits = [&](int k) { int v = 0; for (int i = 0; i < k; i++) v = (v << 1) | bit(); return v; };
	auto bitsRev = [&](int k) { int v = 0; for (int i = 0; i < k; i++) v |= bit() << i; return v; };
	try {
		while (int(out.size()) < expected) {
			if (bit()) {
				int word = bits(4);
				int offset = bitsRev(word);
				int size = bitsRev(word);
				if (offset > int(out.size()) || offset <= 0)
					throw std::runtime_error("Corrupt GPX file");
				size_t from = out.size() - size_t(offset);
				int k = std::min(offset, size);
				for (int i = 0; i < k && int(out.size()) < expected; i++)
					out.push_back(out[from + i]);
			} else {
				int size = bitsRev(2);
				for (int i = 0; i < size && int(out.size()) < expected; i++)
					out.push_back(uint8_t(bits(8)));
			}
		}
	} catch (const std::out_of_range &) {
	}
	return out;
}

std::map<std::string, std::vector<uint8_t>> readGpxFiles(const uint8_t *d, size_t n)
{
	std::vector<uint8_t> plain;
	const uint8_t *data;
	size_t len;
	if (std::memcmp(d, "BCFZ", 4) == 0) {
		plain = decompressBcfz(d, n);
		if (plain.size() < 4)
			throw std::runtime_error("Corrupt GPX file");
		data = plain.data() + 4;
		len = plain.size() - 4;
	} else {
		data = d + 4;
		len = n - 4;
	}
	std::map<std::string, std::vector<uint8_t>> files;
	const size_t sector = 0x1000;
	size_t offset = sector;
	while (offset + 0x94 + 4 <= len) {
		if (rd32(data + offset) == 2) {
			std::string name;
			for (size_t i = 0; i < 127 && data[offset + 4 + i]; i++)
				name += char(data[offset + 4 + i]);
			size_t size = std::min<size_t>(rd32(data + offset + 0x8c), len);
			std::vector<uint8_t> file;
			file.reserve(size);
			size_t pointers = offset + 0x94;
			for (size_t k = 0; pointers + 4 * k + 4 <= len; k++) {
				uint32_t s = rd32(data + pointers + 4 * k);
				if (!s)
					break;
				offset = size_t(s) * sector;
				if (offset >= len)
					break;
				size_t take = std::min({sector, len - offset, size - file.size()});
				file.insert(file.end(), data + offset, data + offset + take);
				if (file.size() >= size)
					break;
			}
			files[name] = std::move(file);
		}
		offset += sector;
	}
	return files;
}

} // namespace tab

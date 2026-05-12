//
// LightGeo - High-performance IPv4 geolocation database optimized for zero-allocation lookups
// and extreme memory efficiency via memory-mapping and a /16 prefix index.
// While providing substantial performance gains over comprehensive solutions like GeoLite2, it is
// restricted to IPv4 and provides only essential country-level metadata.
// This specialized tool is intended for real-time, resource-constrained systems where latency-sensitive
// country resolution is critical.
//

#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <cstring>

#ifdef _WIN32
	#define NOMINMAX
	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__) || defined(POSIX)
	#include <sys/mman.h>
	#include <sys/stat.h>
	#include <fcntl.h>
	#include <unistd.h>
#else
	#error "LightGeo: Unsupported platform"
#endif

namespace LightGeo {

static constexpr uint32_t kMaxLocales = 16;
static constexpr uint32_t kMaxLocaleNameSize = 8;

#pragma pack(push, 1)
struct LocaleDef {
    uint64_t tag;
    char     code[kMaxLocaleNameSize];
};

struct Header {
	uint32_t  magic;
	uint32_t  version;
	uint32_t  entry_count;
	uint32_t  location_count;
	uint16_t  locale_count;
	LocaleDef locales[kMaxLocales];
	uint16_t  padding;
	uint32_t  checksum;
};

struct Entry {
	uint32_t ip_start;
	uint32_t ip_end;
	uint16_t location_id;
	uint16_t padding;
};

struct Location {
	uint32_t geoname_id;
	char     continent_code[2];
	char     country_iso_code[2];
};

struct LocaleData {
	char continent_name[16];
	char country_name[48];
};

struct IndexRange {
	uint32_t start_index;
	uint32_t end_index;
};

#pragma pack(pop)

class Db;

class LookupResult {
private:
	const Db * db_;
	const Location* loc_;

public:
	LookupResult(const Db* db, const Location* loc = nullptr) noexcept
		: db_(db), loc_(loc) {}

	explicit operator bool() const noexcept { return loc_ != nullptr; }

	const Location* operator->() const noexcept { return loc_; }
	const LocaleData* operator[](const char* lang_code) const noexcept;
	const Location* get() const noexcept { return loc_; }
};

class Db {
public:
	static constexpr const char *kName    = "LightGeo.db";
	static constexpr uint32_t    kMagic   = 0x4F45474C; // "LGEO"
	static constexpr uint32_t    kVersion = 4;

	Db() = default;
	~Db() { Close(); }

	// prevents copying, allow only transferring ownership
	Db(const Db &) = delete;
	Db &operator=(const Db &) = delete;
	Db(Db &&other) noexcept { MoveFrom(std::move(other)); }
	Db &operator=(Db &&other) noexcept {
		if (this != &other) { Close(); MoveFrom(std::move(other)); }
		return *this;
	}

	bool Open(const char *file_path);

	// Closes handles and unmaps memory
	void Close() noexcept;

	bool IsLoaded() const noexcept { return is_loaded_; }
	uint32_t GetEntryCount() const noexcept { return is_loaded_ ? header_->entry_count : 0; }
	uint32_t GetLocationCount() const noexcept { return is_loaded_ ? header_->location_count : 0; }

	LookupResult Lookup(uint32_t ip_host) const noexcept;
	LookupResult Lookup(const char* ip_string) const noexcept;
	int16_t GetLocaleId(const char* lang_code) const noexcept;

	const LocaleData* GetLocaleData(const Location* loc, int16_t locale_id) const noexcept;
	const LocaleData* GetLocaleData(const Location* loc, const char* locale_code) const noexcept;

protected:
	friend class Compiler;

	static uint32_t GetFileSize(uint32_t entries, uint32_t locations, uint16_t locales) noexcept;
	static uint32_t Adler32(uint32_t adler, const void *data, size_t size) noexcept;

	static uint64_t ToTag64(const char* str) noexcept;
	static bool ParseIPv4(const char* str, uint32_t& out_ip) noexcept;

private:
	void* map_view_   = nullptr;
	size_t file_size_ = 0;

	Header* header_          = nullptr;
	IndexRange* indices_     = nullptr;
	Entry* entries_          = nullptr;
	Location* locations_     = nullptr;
	LocaleData *locale_data_ = nullptr;
	bool is_loaded_          = false;

	bool MapFile(const char *file_path) noexcept;
	void UnmapFile() noexcept;

#ifdef _WIN32
	HANDLE file_h_ = INVALID_HANDLE_VALUE;
	HANDLE map_h_ = nullptr;
#else
	int fd = -1;
#endif

	void MoveFrom(Db&& o) noexcept;
};

inline bool Db::Open(const char *file_path) {
	Close();
	if (!MapFile(file_path)) {
		Close(); return false;
	}

	header_ = static_cast<Header *>(map_view_);
	if (header_->magic != kMagic || header_->version != kVersion) {
		Close(); return false;
	}

	if (file_size_ != GetFileSize(header_->entry_count, header_->location_count, header_->locale_count)) {
		Close(); return false;
	}

	uint8_t *ptr = static_cast<uint8_t *>(map_view_) + sizeof(Header);

	indices_ = reinterpret_cast<IndexRange *>(ptr);
	ptr += (65536 * sizeof(IndexRange));

	entries_ = reinterpret_cast<Entry *>(ptr);
	ptr += (header_->entry_count * sizeof(Entry));

	locations_ = reinterpret_cast<Location *>(ptr);
	ptr += (header_->location_count * sizeof(Location));

	locale_data_ = reinterpret_cast<LocaleData *>(ptr);

	// check integrity
	uint32_t hash = Adler32(1, indices_, 65536 * sizeof(IndexRange));
	hash = Adler32(hash, entries_, header_->entry_count * sizeof(Entry));
	hash = Adler32(hash, locations_, header_->location_count * sizeof(Location));
	hash = Adler32(hash, locale_data_, static_cast<size_t>(header_->location_count) * header_->locale_count * sizeof(LocaleData));
	if (hash != header_->checksum) {
		Close();
		return false; // file corrupted
	}

	is_loaded_ = true;
	return true;
}

inline void Db::Close() noexcept {
	UnmapFile();
	header_ = nullptr; indices_ = nullptr;
	entries_ = nullptr; locations_ = nullptr;
	locale_data_ = nullptr;
	file_size_ = 0;
	is_loaded_ = false;
}

inline LookupResult Db::Lookup(uint32_t ip_host) const noexcept {
	if (!is_loaded_ || header_->entry_count == 0) return {this};

	const IndexRange& range = indices_[ip_host >> 16];
	if (range.start_index > range.end_index ||
		range.start_index >= header_->entry_count ||
		range.end_index >= header_->entry_count)
	{
		return {this};
	}

	int left  = static_cast<int>(range.start_index);
	int right = static_cast<int>(range.end_index);

	// Binary search through sorted IP ranges
	while (left <= right) {
		int mid = left + (right - left) / 2;
		const Entry& e = entries_[mid];

		if (ip_host < e.ip_start) right = mid - 1;
		else if (ip_host > e.ip_end) left = mid + 1;
		else {
			if (e.location_id >= header_->location_count)
				return {this};
			return {this, &locations_[e.location_id]};
		}
	}
	return {this};
}

inline LookupResult Db::Lookup(const char* ip_string) const noexcept {
	uint32_t ip_host = 0;
	if (!ParseIPv4(ip_string, ip_host)) return nullptr;
	return Lookup(ip_host);
}

inline int16_t Db::GetLocaleId(const char* lang_code) const noexcept {
	if (!is_loaded_ || !lang_code) return -1;
	uint64_t search_tag = ToTag64(lang_code);
	for (int16_t i = 0; i < header_->locale_count; ++i) {
		if (header_->locales[i].tag == search_tag) {
			return i;
		}
	}
	return -1;
}

inline const LocaleData* Db::GetLocaleData(const Location* loc, int16_t locale_id) const noexcept {
	if (!loc || locale_id < 0 || locale_id >= header_->locale_count) return nullptr;
	ptrdiff_t loc_index = loc - locations_;
	if (loc_index < 0 || loc_index >= static_cast<int>(header_->location_count)) return nullptr;
	return &locale_data_[loc_index * header_->locale_count + locale_id];
}

inline const LocaleData* Db::GetLocaleData(const Location* loc, const char* locale_code) const noexcept {
	return GetLocaleData(loc, GetLocaleId(locale_code));
}

inline void Db::MoveFrom(Db&& o) noexcept {
	map_view_ = o.map_view_; file_size_ = o.file_size_;
	header_ = o.header_; indices_ = o.indices_;
	entries_ = o.entries_; locations_ = o.locations_;
	locale_data_ = o.locale_data_;
	is_loaded_ = o.is_loaded_;

#ifdef _WIN32
	file_h_ = o.file_h_; map_h_ = o.map_h_;
	o.file_h_ = INVALID_HANDLE_VALUE; o.map_h_ = nullptr;
#else
	fd = o.fd; o.fd = -1;
#endif

	o.map_view_ = nullptr; o.header_ = nullptr;
	o.indices_ = nullptr; o.entries_ = nullptr;
	o.locale_data_ = nullptr;
	o.is_loaded_ = false;
	o.file_size_ = 0;
}

inline bool Db::MapFile(const char *file_path) noexcept {
#ifdef _WIN32
	file_h_ = CreateFileA(file_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file_h_ == INVALID_HANDLE_VALUE)
		return false;

	LARGE_INTEGER file_size_info;
	if (!GetFileSizeEx(file_h_, &file_size_info) || file_size_info.QuadPart < sizeof(Header)) {
		return false;
	}

	map_h_ = CreateFileMappingA(file_h_, nullptr, PAGE_READONLY, 0, 0, nullptr);
	if (!map_h_) return false;

	map_view_ = MapViewOfFile(map_h_, FILE_MAP_READ, 0, 0, 0);
	if (!map_view_) return false;

	file_size_ = static_cast<size_t>(file_size_info.QuadPart);
	return true;
#else
	fd = open(file_path, O_RDONLY);
	if (fd < 0) return false;
	struct stat st;
	if (fstat(fd, &st) < 0 || static_cast<size_t>(st.st_size) < sizeof(Header)) { return false; }
	file_size_ = static_cast<size_t>(st.st_size);
	map_view_  = mmap(nullptr, file_size_, PROT_READ, MAP_SHARED, fd, 0);
	return map_view_ != MAP_FAILED;
#endif
}

inline void Db::UnmapFile() noexcept {
#ifdef _WIN32
	if (map_view_) { UnmapViewOfFile(map_view_); map_view_ = nullptr; }
	if (map_h_) { CloseHandle(map_h_); map_h_ = nullptr; }
	if (file_h_ != INVALID_HANDLE_VALUE) { CloseHandle(file_h_); file_h_ = INVALID_HANDLE_VALUE; }
#else
	if (map_view_ && map_view_ != MAP_FAILED) { munmap(map_view_, file_size_); map_view_ = nullptr; }
	if (fd != -1) { close(fd); fd = -1; }
#endif
}

inline uint32_t Db::GetFileSize(uint32_t entries, uint32_t locations, uint16_t locales) noexcept {
	return sizeof(Header) + (65536 * sizeof(IndexRange)) +
		(entries * sizeof(Entry)) + (locations * sizeof(Location)) +
		(locations * locales * sizeof(LocaleData));
}

inline uint32_t Db::Adler32(uint32_t adler, const void *data, size_t size) noexcept {
	const uint8_t *pbuf = static_cast<const uint8_t *>(data);
	uint32_t a = adler & 0xFFFF, b = (adler >> 16) & 0xFFFF;
	while (size > 0) {
		size_t tlen = size > 5550 ? 5550 : size;
		size -= tlen;
		do { a += *pbuf++; b += a; } while (--tlen);
		a %= 65521; b %= 65521;
	}
	return (b << 16) | a;
}

inline uint64_t Db::ToTag64(const char* str) noexcept {
	uint64_t pack = 0;
	for (int i = 0; i < static_cast<int>(sizeof(pack)) && str[i] != '\0'; ++i) {
		char c = str[i];
		if (c >= 'A' && c <= 'Z') c += 32;
		if (c == '_') c = '-';
		pack |= (static_cast<uint64_t>(static_cast<uint8_t>(c)) << (i * 8));
	}
	return pack;
}

inline bool Db::ParseIPv4(const char* str, uint32_t& out_ip) noexcept {
	if (!str) return false;
	uint32_t ip = 0; int octet = 0, dots = 0; bool digit = false;
	for (int i = 0; str[i]; ++i) {
		char c = str[i];
		if (c >= '0' && c <= '9') {
			octet = octet * 10 + (c - '0');
			if (octet > 255) return false;
			digit = true;
		} else if (c == '.') {
			if (!digit || ++dots > 3) return false;
			ip = (ip << 8) | octet;
			octet = 0; digit = false;
		} else return false;
	}
	if (!digit || dots != 3) return false;
	out_ip = (ip << 8) | octet;
	return true;
}

// ============================================================================
// LocaleView: Zero-overhead wrapper for high-throughput systems
// Caches the locale ID upon creation, eliminating string lookups from the hot path
//
// Example:
//   LightGeo::LocaleView view(&db, "en");
//   auto res = view.Lookup(ip);
// ============================================================================
class LocaleView {
private:
	const Db* db_;
	int16_t locale_id_;

public:
	LocaleView() noexcept = default;
	LocaleView(const Db* db, const char* lang_code) noexcept : db_(db) {
		Init(db, lang_code);
	}

	void Init(const Db* db, const char* lang_code) noexcept {
		db_ = db;
		locale_id_ = db_ ? db_->GetLocaleId(lang_code) : -1;
	}

	bool IsReady() const noexcept {
		return db_ != nullptr && locale_id_ != -1;
	}

	struct Result {
		const Location* meta;
		const LocaleData* text;
		explicit operator bool() const noexcept { return meta != nullptr; }
	};

	Result Lookup(uint32_t ip) const noexcept {
		const LookupResult res = db_->Lookup(ip);
		if (!res) return {};
		return {
			res.get(),
			(locale_id_ != -1) ? db_->GetLocaleData(res.get(), locale_id_) : nullptr
		};
	}
};

inline const LocaleData *LookupResult::operator[](const char* lang_code) const noexcept {
	if (!loc_ || !lang_code) return nullptr;
	int16_t locale_id = db_->GetLocaleId(lang_code);
	return db_->GetLocaleData(loc_, locale_id);
}

} // namespace LightGeo

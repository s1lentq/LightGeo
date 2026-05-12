//
// LightGeo Compiler - The database builder for LightGeo
// Parses raw GeoLite2 CSVs (maxmind), applies surgical CIDR patches, and packs the data
// into a highly optimized, dictionary-encoded binary format (.db)
//

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <random>
#include <filesystem>
#include <charconv>
#include <unordered_map>
#include <span>

#include <lightgeo.hpp>

namespace fs = std::filesystem;
namespace LightGeo {

struct RawEntry {
	uint32_t ip_start;
	uint32_t ip_end;
	uint16_t location_id;
};

struct PatchRule {
	uint32_t ip_start;
	uint32_t ip_end;
	uint16_t location_id;
};

class Compiler {
public:
	Compiler() = default;
	~Compiler() = default;

	int Run(const std::vector<std::string>& args);

private:
	void ShowUsage(const std::string& app_name);

	// main parsing and processing
	[[nodiscard]] bool Compile(const std::vector<fs::path>& loc_files, const fs::path &blocks_file, const fs::path &patch_file, const fs::path &out_file);
	[[nodiscard]] bool LoadFileIntoBuffer(const fs::path& path, std::vector<char>& out_buffer);
	[[nodiscard]] int  SplitCSVLine(std::string_view line, std::string_view fields[], int max_fields);
	[[nodiscard]] bool ParseCIDR(std::string_view cidr, uint32_t& start, uint32_t& end);
	[[nodiscard]] std::string ExtractLangFromFilename(const fs::path& path);

	void SubtractPatchFromBlock(const RawEntry &block, const PatchRule &patch, std::vector<RawEntry> &result);
};

bool Compiler::LoadFileIntoBuffer(const fs::path& path, std::vector<char>& out_buffer) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file.is_open()) return false;
	std::streamsize size = file.tellg();
	file.seekg(0, std::ios::beg);
	out_buffer.resize(static_cast<size_t>(size));
	if (file.read(out_buffer.data(), size)) {
		return true;
	}
	return false;
}

int Compiler::SplitCSVLine(std::string_view line, std::string_view fields[], int max_fields) {
	int count = 0;
	bool in_quotes = false;
	size_t start = 0;
	for (size_t i = 0; i < line.length() && count < max_fields; ++i) {
		if (line[i] == '"') {
			in_quotes = !in_quotes;
		} else if (line[i] == ',' && !in_quotes) {
			fields[count++] = line.substr(start, i - start);
			start = i + 1;
		}
	}
	if (count < max_fields) {
		fields[count++] = line.substr(start);
	}
	return count;
}

bool Compiler::ParseCIDR(std::string_view cidr, uint32_t& start, uint32_t& end) {
	uint8_t octets[4];
	auto current = cidr.data();
	const auto last = cidr.data() + cidr.size();
	for (int i = 0; i < 4; ++i) {
		auto [ptr, ec] = std::from_chars(current, last, octets[i]);
		if (ec != std::errc{}) return false;

		current = ptr;
		if (i < 3) {
			if (current == last || *current != '.') return false;
			current++;
		}
	}

	if (current == last || *current != '/') return false;
	current++;

	uint8_t prefix;
	auto [ptr, ec] = std::from_chars(current, last, prefix);
	if (ec != std::errc{} || prefix > 32) return false;

	uint32_t ip_host = (static_cast<uint32_t>(octets[0]) << 24) |
						(static_cast<uint32_t>(octets[1]) << 16) |
						(static_cast<uint32_t>(octets[2]) <<  8) |
						(static_cast<uint32_t>(octets[3]));

	uint32_t mask = (prefix == 0) ? 0 : (0xFFFFFFFFu << (32 - prefix));
	start = ip_host & mask;
	end = start | ~mask;
	return true;
}

std::string Compiler::ExtractLangFromFilename(const fs::path& path) {
	std::string filename = path.filename().string();
	std::string prefix = "Locations-";
	size_t pos = filename.find(prefix);
	if (pos != std::string::npos) {
		size_t start = pos + prefix.length();
		size_t dot = filename.find('.', start);
		if (dot != std::string::npos) {
			std::string lang = filename.substr(start, dot - start);
			if (lang.length() < kMaxLocaleNameSize) {
				return lang;
			}
		}
	}
	return "";
}

void Compiler::SubtractPatchFromBlock(const RawEntry &block, const PatchRule &patch, std::vector<RawEntry> &result)
{
	if (patch.ip_end < block.ip_start || patch.ip_start > block.ip_end) {
		result.push_back(block);
		return;
	}

	if (patch.ip_start > block.ip_start) {
		result.push_back({block.ip_start, patch.ip_start - 1, block.location_id});
	}

	if (patch.ip_end < block.ip_end) {
		result.push_back({patch.ip_end + 1, block.ip_end, block.location_id});
	}
}

bool Compiler::Compile(const std::vector<fs::path>& loc_files, const fs::path& blocks_file, const fs::path& patch_file, const fs::path& out_file) {
	if (loc_files.empty()) {
		std::cerr << "ERROR: No location files provided!" << std::endl;
		return false;
	}

	if (loc_files.size() > kMaxLocales) {
		std::cerr << "ERROR: Maximum " << kMaxLocales << " locales supported!" << std::endl;
		return false;
	}

	std::vector<std::string> lang_codes;
	for (const auto& f : loc_files) {
		std::string lang = ExtractLangFromFilename(f);
		if (lang.empty()) {
			std::cerr << "ERROR: Could not extract 2-letter language code from filename: " << f.filename().string() << std::endl;
			return false;
		}
		lang_codes.push_back(lang);
	}

	std::cout << "Building Dictionary from " << loc_files.size() << " locale files (Primary: " << lang_codes[0] << ")..." << std::endl;

	std::vector<Location> dictionary;
	std::unordered_map<uint32_t, uint16_t> geoname_to_dict;
	std::unordered_map<std::string, uint16_t> iso_to_dict;

	struct ColumnLocation {
		int geoname_id = -1, continent_code = -1, continent_name = -1, country_iso_code = -1,  country_name = -1;
		int max_required() const {
			return std::max({ geoname_id, continent_code, continent_name, country_iso_code, country_name }) + 1;
		}
		void clear() {
			geoname_id =
				continent_code = continent_name =
				country_iso_code = country_name = -1;
		}
	};

	struct ColumnBinding {
		std::string_view name;
		int* index;
	};

	const auto unquote = [](std::string_view s) -> std::string_view {
		if (s.length() >= 2 && s.front() == '"' && s.back() == '"') {
			return s.substr(1, s.size() - 2);
		}
		return s;
	};

	const auto ValidateColumnBindings = [](std::string_view filename, std::span<const ColumnBinding> bindings) -> bool {
		std::string_view sep = "";
		bool has_errors = false;
		for (const auto& [name, index] : bindings) {
			if (*index == -1) {
				if (!has_errors) {
					std::cerr << "ERROR: Invalid file format [" << filename << "]! Missing columns: ";
					has_errors = true;
				}
				std::cerr << sep << name; sep = ", ";
			}
		}
		if (has_errors) {
			std::cerr << std::endl;
			return false;
		}
		return true;
	};

	std::vector<char> buf_loc;
	if (!LoadFileIntoBuffer(loc_files[0], buf_loc)) {
		std::cerr << "ERROR: Failed to open " << loc_files[0].filename().string() << std::endl;
		return false;
	}

	std::string_view loc_content(buf_loc.data(), buf_loc.size());
	bool loc_header_line = true;

	ColumnLocation loc_col_idx;
	const ColumnBinding bindings[] = {
		{ "geoname_id",       &loc_col_idx.geoname_id },
		{ "continent_code",   &loc_col_idx.continent_code },
		{ "continent_name",   &loc_col_idx.continent_name },
		{ "country_iso_code", &loc_col_idx.country_iso_code },
		{ "country_name",     &loc_col_idx.country_name },
	};

	while (!loc_content.empty()) {
		size_t pos = loc_content.find('\n');
		std::string_view line = loc_content.substr(0, pos);
		loc_content = (pos == std::string_view::npos) ? "" : loc_content.substr(pos + 1);

		if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
		if (line.empty() || line.front() == '#' || line.front() == '/') continue;

		if (loc_header_line) {
			loc_header_line = false;
			std::string_view header_fields[64];
			int num_cols = SplitCSVLine(line, header_fields, sizeof(header_fields) / sizeof(header_fields[0]));
			for (int i = 0; i < num_cols; ++i) {
				for (const ColumnBinding &binding : bindings) {
					if (header_fields[i] == binding.name) {
						*binding.index = i;
						break;
					}
				}
			}

			if (!ValidateColumnBindings(loc_files[0].filename().string(), bindings)) {
				return false;
			}

			continue;
		}

		int needed_fields = loc_col_idx.max_required();
		std::vector<std::string_view> fields(needed_fields);
		if (SplitCSVLine(line, fields.data(), needed_fields) >= needed_fields && !fields[loc_col_idx.geoname_id].empty()) {
			uint32_t geoname_id = 0;
			std::from_chars(fields[loc_col_idx.geoname_id].data(), fields[loc_col_idx.geoname_id].data() + fields[loc_col_idx.geoname_id].size(), geoname_id);

			if (geoname_to_dict.find(geoname_id) != geoname_to_dict.end()) {
				std::cerr << "CRITICAL ERROR: Duplicate geoname_id " << geoname_id << " found!\n";
				return false;
			}

			Location loc{};
			loc.geoname_id = geoname_id;

			std::string_view ctry_code = unquote(fields[loc_col_idx.country_iso_code]);
			if (ctry_code.length() >= 2) { loc.country_iso_code[0] = ctry_code[0]; loc.country_iso_code[1] = ctry_code[1]; }

			std::string_view cont_code = unquote(fields[loc_col_idx.continent_code]);
			if (cont_code.length() >= 2) { loc.continent_code[0] = cont_code[0]; loc.continent_code[1] = cont_code[1]; }

			uint16_t dict_idx = static_cast<uint16_t>(dictionary.size());
			dictionary.push_back(loc);
			geoname_to_dict[geoname_id] = dict_idx;

			if (ctry_code.length() == 2) {
				std::string ctry_code_str(ctry_code);
				if (iso_to_dict.find(ctry_code_str) == iso_to_dict.end()) {
					iso_to_dict[ctry_code_str] = dict_idx;
				}
			}
		}
	}

	std::cout << "Dictionary built: " << dictionary.size() << " unique locations." << std::endl;

	std::vector<LocaleData> locale_data(dictionary.size() * loc_files.size());
	memset(locale_data.data(), 0, locale_data.size() * sizeof(LocaleData));

	for (size_t lang_idx = 0; lang_idx < loc_files.size(); ++lang_idx) {
		std::vector<char> buf_lang;
		if (!LoadFileIntoBuffer(loc_files[lang_idx], buf_lang)) continue;

		std::string_view lang_content(buf_lang.data(), buf_lang.size());
		bool lang_header_line = true;

		loc_col_idx.clear();

		while (!lang_content.empty()) {
			size_t pos = lang_content.find('\n');
			std::string_view line = lang_content.substr(0, pos);
			lang_content = (pos == std::string_view::npos) ? "" : lang_content.substr(pos + 1);

			if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
			if (line.empty() || line.front() == '#' || line.front() == '/') continue;

			if (lang_header_line) {
				lang_header_line = false;
				std::string_view header_fields[64];
				int num_cols = SplitCSVLine(line, header_fields, sizeof(header_fields) / sizeof(header_fields[0]));
				for (int i = 0; i < num_cols; ++i) {
					for (const ColumnBinding &binding : bindings) {
						if (header_fields[i] == binding.name) {
							*binding.index = i;
							break;
						}
					}
				}
				continue;
			}

			int needed_fields = loc_col_idx.max_required();
			std::vector<std::string_view> fields(needed_fields);
			if (SplitCSVLine(line, fields.data(), needed_fields) >= needed_fields && !fields[loc_col_idx.geoname_id].empty()) {
				uint32_t geoname_id = 0;
				std::from_chars(fields[loc_col_idx.geoname_id].data(), fields[loc_col_idx.geoname_id].data() + fields[loc_col_idx.geoname_id].size(), geoname_id);

				auto it = geoname_to_dict.find(geoname_id);
				if (it != geoname_to_dict.end()) {
					LocaleData &loc_data = locale_data[it->second * loc_files.size() + lang_idx];

					std::string_view cont_name = unquote(fields[loc_col_idx.continent_name]);
					cont_name.copy(loc_data.continent_name, sizeof(LocaleData::continent_name) - 1);
					loc_data.continent_name[std::min(cont_name.length(), sizeof(loc_data.continent_name) - 1)] = '\0';

					std::string_view ctry_name = unquote(fields[loc_col_idx.country_name]);
					ctry_name.copy(loc_data.country_name, sizeof(LocaleData::country_name) - 1);
					loc_data.country_name[std::min(ctry_name.length(), sizeof(loc_data.country_name) - 1)] = '\0';
				}
			}
		}
		std::cout << "  + Loaded locale data for [" << lang_codes[lang_idx] << "]" << std::endl;
	}

	std::vector<PatchRule> patches;
	std::vector<char> buf_patches;
	if (LoadFileIntoBuffer(patch_file, buf_patches)) {
		std::string_view patches_content(buf_patches.data(), buf_patches.size());
		while (!patches_content.empty()) {
			size_t pos = patches_content.find('\n');
			std::string_view line = patches_content.substr(0, pos);
			patches_content = (pos == std::string_view::npos) ? "" : patches_content.substr(pos + 1);

			if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
			if (line.empty() || line.front() == '#' || line.front() == '/') continue;

			std::string_view fields[2];
			if (SplitCSVLine(line, fields, 2) >= 2) {
				PatchRule p;
				if (ParseCIDR(fields[0], p.ip_start, p.ip_end)) {
					std::string_view ctry_code = fields[1];
					while (!ctry_code.empty() && ctry_code.front() == ' ') ctry_code.remove_prefix(1);

					if (ctry_code.length() >= 2) {
						std::string ctry_code_str(ctry_code.substr(0, 2));
						auto it = iso_to_dict.find(ctry_code_str);
						if (it != iso_to_dict.end()) {
							p.location_id = it->second;
							patches.push_back(p);
						} else {
							std::cerr << "WARNING: Patch for ISO '" << ctry_code_str << "' ignored (ISO not found in Locations)" << std::endl;
						}
					}
				}
			}
		}
	}

	std::cout << "Parsing IPv4 blocks from " << blocks_file.filename().string() << "..." << std::endl;
	std::vector<char> buf_blocks;
	if (!LoadFileIntoBuffer(blocks_file, buf_blocks)) {
		std::cerr << "ERROR: Failed to open " << blocks_file.filename().string() << std::endl;
		return false;
	}

	std::vector<RawEntry> entries;
	std::string_view blocks_content(buf_blocks.data(), buf_blocks.size());
	bool block_header_line = true;
	uint64_t affected_patched_ips = 0;

	struct ColumnBlock
	{
		int network = -1, geoname_id = -1, reg_geoname_id = -1, rep_geoname_id = -1;
		int max_required() const {
			return std::max({ network, geoname_id, reg_geoname_id, rep_geoname_id }) + 1;
		}
	};

	ColumnBlock block_col_idx;
	const ColumnBinding block_bindings[] = {
		{ "network",                         &block_col_idx.network },
		{ "geoname_id",                      &block_col_idx.geoname_id },
		{ "registered_country_geoname_id",   &block_col_idx.reg_geoname_id },
		{ "represented_country_geoname_id",  &block_col_idx.rep_geoname_id }
	};

	while (!blocks_content.empty()) {
		size_t pos = blocks_content.find('\n');
		std::string_view line = blocks_content.substr(0, pos);
		blocks_content = (pos == std::string_view::npos) ? "" : blocks_content.substr(pos + 1);

		if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
		if (line.empty()) continue;

		if (block_header_line) {
			block_header_line = false;
			std::string_view header_fields[64];
			int num_cols = SplitCSVLine(line, header_fields, sizeof(header_fields) / sizeof(header_fields[0]));
			for (int i = 0; i < num_cols; ++i) {
				for (const ColumnBinding &binding : block_bindings) {
					if (header_fields[i] == binding.name) {
						*binding.index = i;
						break;
					}
				}
			}

			if (!ValidateColumnBindings(blocks_file.filename().string(), bindings)) {
				return false;
			}

			continue;
		}

		int needed_fields = block_col_idx.max_required();
		std::vector<std::string_view> fields(needed_fields);
		if (SplitCSVLine(line, fields.data(), needed_fields) >= needed_fields) {
			uint32_t geoname_id = 0, reg_id = 0, rep_id = 0;

			if (block_col_idx.geoname_id != -1 && !fields[block_col_idx.geoname_id].empty()) {
				std::from_chars(fields[block_col_idx.geoname_id].data(), fields[block_col_idx.geoname_id].data() + fields[block_col_idx.geoname_id].size(), geoname_id);
			}

			if (block_col_idx.reg_geoname_id != -1 && !fields[block_col_idx.reg_geoname_id].empty()) {
				std::from_chars(fields[block_col_idx.reg_geoname_id].data(), fields[block_col_idx.reg_geoname_id].data() + fields[block_col_idx.reg_geoname_id].size(), reg_id);
			}

			if (block_col_idx.rep_geoname_id != -1 && !fields[block_col_idx.rep_geoname_id].empty()) {
				std::from_chars(fields[block_col_idx.rep_geoname_id].data(), fields[block_col_idx.rep_geoname_id].data() + fields[block_col_idx.rep_geoname_id].size(), rep_id);
			}

			uint32_t target_id = (geoname_id != 0) ? geoname_id : (reg_id != 0) ? reg_id : rep_id;
			if (target_id != 0) {
				auto dict_it = geoname_to_dict.find(target_id);
				if (dict_it != geoname_to_dict.end()) {
					RawEntry base_rec;
					if (ParseCIDR(fields[block_col_idx.network], base_rec.ip_start, base_rec.ip_end)) {
						base_rec.location_id = dict_it->second;

						for (const PatchRule& p : patches) {
							uint32_t intersect_start = std::max(base_rec.ip_start, p.ip_start);
							uint32_t intersect_end = std::min(base_rec.ip_end, p.ip_end);
							if (intersect_start <= intersect_end) {
								affected_patched_ips += (static_cast<uint64_t>(intersect_end) - intersect_start + 1);
							}
						}

						std::vector<RawEntry> fragments = { base_rec };

						for (const PatchRule& p : patches) {
							std::vector<RawEntry> next_fragments;
							for (const RawEntry& frag : fragments) {
								SubtractPatchFromBlock(frag, p, next_fragments);
							}
							fragments = std::move(next_fragments);
						}

						for (const RawEntry& f : fragments) {
							entries.push_back(f);
						}
					}
				}
			}
		}
	}

	std::cout << "Parsed " << entries.size() << " raw IP ranges. Optimizing dataset..." << std::endl;

	if (affected_patched_ips > 0) {
		std::cout << "Loaded " << patches.size() << " manual patches: "
			<< affected_patched_ips << " total IPs affected." << std::endl;
	}

	for (const PatchRule& p : patches) {
		entries.push_back({ p.ip_start, p.ip_end, p.location_id });
	}

	std::sort(entries.begin(), entries.end(), [](const RawEntry& a, const RawEntry& b) {
		return a.ip_start < b.ip_start;
	});

	std::vector<Entry> optimized;
	if (!entries.empty()) {
		RawEntry current = entries[0];
		for (size_t i = 1; i < entries.size(); i++) {
			const RawEntry& next = entries[i];
			// if blocks are adjacent and belong to the same country, merge them
			if (current.ip_end + 1 >= next.ip_start && current.location_id == next.location_id) {
				if (next.ip_end > current.ip_end) current.ip_end = next.ip_end;
			} else {
				// save current block and start a new one
				optimized.push_back({ current.ip_start, current.ip_end, current.location_id, 0 });
				current = next;
			}
		}
		// push the last block
		optimized.push_back({ current.ip_start, current.ip_end, current.location_id, 0 });
	}
	std::cout << "Optimization complete: reduced from " << entries.size() << " to " << optimized.size() << " entries." << std::endl;

	std::cout << "Running sanity checks..." << std::endl;
	for (size_t i = 0; i < optimized.size(); i++) {
		// end of the range cannot be less than the start
		if (optimized[i].ip_start > optimized[i].ip_end) {
			std::cerr << "CRITICAL ERROR: Range " << i << " is reversed! (Start: " << optimized[i].ip_start << ", End: " << optimized[i].ip_end << ")" << std::endl;
			return false;
		}
		// ranges must be in strictly ascending order and non-overlapping
		// otherwise, our binary search will go into an infinite loop or return garbage
		if (i > 0 && optimized[i - 1].ip_end >= optimized[i].ip_start) {
			std::cerr << "CRITICAL ERROR: Range " << i << " overlaps with previous! (PrevEnd: " << optimized[i - 1].ip_end << ", CurrStart: " << optimized[i].ip_start << ")" << std::endl;
			return false;
		}
	}
	std::cout << "Sanity Checks passed." << std::endl;

	std::cout << "Building LUT Index (/16)..." << std::endl;
	std::vector<IndexRange> lut(65536, {0xFFFFFFFF, 0});
	for (uint32_t i = 0; i < static_cast<uint32_t>(optimized.size()); i++) {
		uint32_t prefix_start = optimized[i].ip_start >> 16;
		uint32_t prefix_end = optimized[i].ip_end >> 16;
		for (uint32_t p = prefix_start; p <= prefix_end; p++) {
			if (lut[p].start_index == 0xFFFFFFFF) lut[p].start_index = i;
			lut[p].end_index = i;
		}
	}

	std::cout << "Writing binary database..." << std::endl;
	std::ofstream fOut(out_file, std::ios::binary);
	if (!fOut.is_open()) {
		std::cerr << "ERROR: Failed to create output file " << out_file.filename().string() << "!" << std::endl;
		return false;
	}

	// compute checksum of LUT + entries for check integrity
	uint32_t checksum = Db::Adler32(1, lut.data(), lut.size() * sizeof(IndexRange));
	checksum = Db::Adler32(checksum, optimized.data(), optimized.size() * sizeof(Entry));
	checksum = Db::Adler32(checksum, dictionary.data(), dictionary.size() * sizeof(Location));
	checksum = Db::Adler32(checksum, locale_data.data(), locale_data.size() * sizeof(LocaleData));

	Header header = {
		Db::kMagic,
		Db::kVersion,
		static_cast<uint32_t>(optimized.size()),
		static_cast<uint32_t>(dictionary.size()),
		static_cast<uint16_t>(loc_files.size()),
		{},	// available locales
		0,	// padding
		checksum
	};

	// fill in available locales in header
	for (size_t i = 0; i < lang_codes.size(); ++i) {
		LocaleDef &lang = header.locales[i];
		lang_codes[i].copy(lang.code, sizeof(lang.code) - 1);
		lang.code[std::min(lang_codes[i].length(), sizeof(lang.code) - 1)] = '\0';
		lang.tag = Db::ToTag64(lang_codes[i].c_str());
	}

	// File layout: [Header] [IndexRange] [IP Ranges] [Geodata Table] [Locale Table]
	fOut.write(reinterpret_cast<const char *>(&header), sizeof(Header));
	fOut.write(reinterpret_cast<const char *>(lut.data()), lut.size() * sizeof(IndexRange));
	fOut.write(reinterpret_cast<const char *>(optimized.data()), optimized.size() * sizeof(Entry));
	fOut.write(reinterpret_cast<const char *>(dictionary.data()), dictionary.size() * sizeof(Location));
	fOut.write(reinterpret_cast<const char *>(locale_data.data()), locale_data.size() * sizeof(LocaleData));
	fOut.close();

	float mb_size = Db::GetFileSize(header.entry_count, header.location_count, header.locale_count) / (1024.0f * 1024.0f);
	std::cout << "Successfully compiled " << out_file.filename().string() << " (" << std::fixed << std::setprecision(2) << mb_size << " MB)" << std::endl;

	return true;
}

void Compiler::ShowUsage(const std::string& app_name) {
	std::cout << "Usage: " << app_name << " [options]" << std::endl << std::endl;
	std::cout << "Options:" << std::endl;
	std::cout << "  -l, --locations <file>  Path to GeoLite2 Locations CSV (can be used multiple times) (default: GeoLite2-Country-Locations-%lang%.csv)" << std::endl;
	std::cout << "  -b, --blocks <file>     Path to GeoLite2 Blocks CSV (default: GeoLite2-Country-Blocks-IPv4.csv)" << std::endl;
	std::cout << "  -p, --patch <file>      Path to manual patches file (default: patches.txt)" << std::endl;
	std::cout << "  -o, --output <file>     Output .db file (default: " << Db::kName << ")" << std::endl;
	std::cout << "  -h, --help              Show this help menu" << std::endl;
}

int Compiler::Run(const std::vector<std::string>& args) {
	std::vector<fs::path> loc_files;
	fs::path blocks_file = "GeoLite2-Country-Blocks-IPv4.csv";
	fs::path patch_file = "patches.txt";
	fs::path out_file = Db::kName;

	for (size_t i = 1; i < args.size(); i++) {
		if (args[i] == "-h" || args[i] == "--help" || args[i] == "/?") {
			ShowUsage(args[0]);
			return 0;
		}
		else if ((args[i] == "-l" || args[i] == "--locations") && i + 1 < args.size()) {
			loc_files.push_back(args[++i]);
		}
		else if ((args[i] == "-b" || args[i] == "--blocks") && i + 1 < args.size()) {
			blocks_file = args[++i];
		}
		else if ((args[i] == "-p" || args[i] == "--patch") && i + 1 < args.size()) {
			patch_file = args[++i];
		}
		else if ((args[i] == "-o" || args[i] == "--output") && i + 1 < args.size()) {
			out_file = args[++i];
		}
	}

	if (loc_files.empty()) {
		loc_files.push_back("GeoLite2-Country-Locations-en.csv");
	}

	return Compile(loc_files, blocks_file, patch_file, out_file) ? 0 : -1;
}

} // namespace LightGeo

std::vector<std::string> GetPlatformArgs(int argc, char* argv[]) {
	std::vector<std::string> args;
	for (int i = 0; i < argc; i++) {
		args.push_back(argv[i]);
	}
	return args;
}

int main(int argc, char* argv[]) {
	std::vector<std::string> args = GetPlatformArgs(argc, argv);
	LightGeo::Compiler app;
	return app.Run(args);
}
